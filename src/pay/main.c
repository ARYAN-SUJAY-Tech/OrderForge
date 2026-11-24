/*
 * of-pay - mock payment gateway.
 *
 * No card data exists anywhere in OrderForge: the checkout sends a payment-method
 * token, and the token decides what this gateway does with it.
 *
 *   tok_success    approve after the normal latency
 *   tok_decline    decline after the normal latency
 *   tok_timeout    never answer (the order expires through the reservation TTL)
 *   tok_duplicate  approve, then send the same approval again (tests exactly-once)
 *   tok_slow       approve after pay.slow_ms, longer than the reservation TTL, so the
 *                  order expires first (tests the automatic refund of a late success)
 *   tok_random     approve, decline, time out or duplicate at the configured rates
 *
 * Design: one thread, one epoll loop over
 *   - the request queue (a POSIX message queue is a pollable descriptor on Linux),
 *   - a timerfd armed for the earliest pending reply (replies sit in a min-heap by due
 *     time, so hundreds of payments can be "processing" at once with no extra threads),
 *   - a signalfd (SIGTERM/SIGINT stop, SIGHUP reopens the log).
 *
 * Every request's HMAC and nonce are checked before anything else, and every reply is
 * signed, so neither side can be fooled by a forged or replayed message.
 * Idempotency: a payment key seen before gets the same outcome and reference again.
 *
 *   usage: of-pay [-c conf/orderforge.conf]
 */
#include "common/config.h"
#include "common/harden.h"
#include "common/log.h"
#include "common/paymsg.h"
#include "common/util.h"

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <signal.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define HEAP_MAX  65536
#define IDEM_SLOTS 65536 /* power of two */

/* ------------------------------------------------------------------ state */

typedef struct {
    int64_t   due_us;   /* CLOCK_MONOTONIC */
    pay_msg_t reply;    /* unsigned; signed just before sending */
} pending_t;

static pending_t *heap;
static size_t     heap_len;

typedef struct {
    char     key[48];
    uint32_t outcome;
    char     ref[32];
} idem_entry_t;

static idem_entry_t *idem;

static of_config_t cfg;
static uint8_t     key[PAY_KEY_LEN];
static mqd_t       mq_req, mq_rep;
static int         tfd;
static int64_t     lat_min_ms, lat_max_ms, slow_ms;
static int         decline_pct, timeout_pct, duplicate_pct;
static pay_replay_t replay;

static struct {
    uint64_t requests, approved, declined, silent, duplicates, rejected, idempotent_hits;
} stats;

/* ------------------------------------------------------------------ min-heap by due time */

static void heap_push(const pending_t *p)
{
    if (heap_len == HEAP_MAX) {
        LOG_WARN("overload", "reply heap full, dropping reply for %s", p->reply.idem_key);
        return;
    }
    size_t i = heap_len++;
    heap[i] = *p;
    while (i > 0 && heap[(i - 1) / 2].due_us > heap[i].due_us) {
        pending_t t = heap[i];
        heap[i] = heap[(i - 1) / 2];
        heap[(i - 1) / 2] = t;
        i = (i - 1) / 2;
    }
}

static pending_t heap_pop(void)
{
    pending_t top = heap[0];
    heap[0] = heap[--heap_len];
    size_t i = 0;
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < heap_len && heap[l].due_us < heap[m].due_us)
            m = l;
        if (r < heap_len && heap[r].due_us < heap[m].due_us)
            m = r;
        if (m == i)
            break;
        pending_t t = heap[i];
        heap[i] = heap[m];
        heap[m] = t;
        i = m;
    }
    return top;
}

/* Arm the timerfd for the earliest reply (or disarm when there is none). */
static void arm_timer(void)
{
    struct itimerspec its = {0};
    if (heap_len > 0) {
        int64_t wait = heap[0].due_us - of_now_us();
        if (wait < 1)
            wait = 1; /* 0 would disarm the timer */
        its.it_value.tv_sec = wait / 1000000;
        its.it_value.tv_nsec = (wait % 1000000) * 1000;
    }
    timerfd_settime(tfd, 0, &its, NULL);
}

/* ------------------------------------------------------------------ idempotency */

static idem_entry_t *idem_slot(const char *k)
{
    uint64_t h = 1469598103934665603ull; /* FNV-1a */
    for (const char *p = k; *p; p++)
        h = (h ^ (unsigned char)*p) * 1099511628211ull;
    for (size_t probe = 0; probe < IDEM_SLOTS; probe++) {
        idem_entry_t *e = &idem[(h + probe) & (IDEM_SLOTS - 1)];
        if (e->key[0] == '\0' || strcmp(e->key, k) == 0)
            return e;
    }
    return &idem[h & (IDEM_SLOTS - 1)]; /* full: overwrite (a mock may forget) */
}

/* ------------------------------------------------------------------ requests */

static int64_t latency_us(void)
{
    int64_t span = lat_max_ms - lat_min_ms;
    int64_t ms = lat_min_ms + (span > 0 ? (int64_t)randombytes_uniform((uint32_t)span + 1) : 0);
    return ms * 1000;
}

static void schedule(const pay_msg_t *req, uint32_t outcome, const char *ref, int64_t delay_us)
{
    pending_t p;
    memset(&p, 0, sizeof p);
    p.due_us = of_now_us() + delay_us;
    p.reply.kind = PAY_REPLY;
    p.reply.outcome = outcome;
    p.reply.order_id = req->order_id;
    p.reply.amount_paise = req->amount_paise;
    snprintf(p.reply.idem_key, sizeof p.reply.idem_key, "%s", req->idem_key);
    snprintf(p.reply.method, sizeof p.reply.method, "%s", req->method);
    snprintf(p.reply.provider_ref, sizeof p.reply.provider_ref, "%s", ref);
    heap_push(&p);
}

static void handle_request(const pay_msg_t *req)
{
    if (pay_verify(req, PAY_REQUEST, key) != 0 ||
        pay_replay_check(&replay, req, pay_now_ms(), 300000) != 0) {
        stats.rejected++;
        LOG_WARN("request_rejected", "request with a bad MAC, or replayed/stale, dropped");
        return;
    }
    stats.requests++;

    /* Same idempotency key, same answer: a retried request cannot pay twice. */
    idem_entry_t *e = idem_slot(req->idem_key);
    if (e->key[0] && strcmp(e->key, req->idem_key) == 0) {
        stats.idempotent_hits++;
        LOG_INFO("idempotent", "%s seen before; repeating outcome %u", req->idem_key, e->outcome);
        if (e->outcome != PAY_OUTCOME_NONE)
            schedule(req, e->outcome, e->ref, latency_us());
        return;
    }

    const char *tok = req->method;
    bool duplicate = false, silent = false;
    uint32_t outcome = PAY_SUCCEEDED;
    int64_t delay = latency_us();

    if (strcmp(tok, "tok_decline") == 0) {
        outcome = PAY_DECLINED;
    } else if (strcmp(tok, "tok_timeout") == 0) {
        silent = true;
    } else if (strcmp(tok, "tok_duplicate") == 0) {
        duplicate = true;
    } else if (strcmp(tok, "tok_slow") == 0) {
        delay = slow_ms * 1000;
    } else if (strcmp(tok, "tok_random") == 0) {
        uint32_t roll = randombytes_uniform(100);
        if (roll < (uint32_t)decline_pct)
            outcome = PAY_DECLINED;
        else if (roll < (uint32_t)(decline_pct + timeout_pct))
            silent = true;
        else if (roll < (uint32_t)(decline_pct + timeout_pct + duplicate_pct))
            duplicate = true;
    } /* tok_success and anything else: approve */

    char ref[32];
    unsigned char rnd[6];
    randombytes_buf(rnd, sizeof rnd);
    char hex[13];
    of_hex(hex, rnd, sizeof rnd);
    snprintf(ref, sizeof ref, "mockpay_%s", hex);

    snprintf(e->key, sizeof e->key, "%s", req->idem_key);
    e->outcome = silent ? PAY_OUTCOME_NONE : outcome;
    snprintf(e->ref, sizeof e->ref, "%s", ref);

    if (silent) {
        stats.silent++;
        LOG_INFO("payment", "order %llu %s: no reply (simulated timeout)",
                 (unsigned long long)req->order_id, req->idem_key);
        return;
    }
    outcome == PAY_SUCCEEDED ? stats.approved++ : stats.declined++;
    schedule(req, outcome, ref, delay);
    if (duplicate) {
        stats.duplicates++;
        schedule(req, outcome, ref, delay + 50000); /* the same callback, 50 ms later */
    }
    LOG_INFO("payment", "order %llu %s via %s: %s in %lld ms%s",
             (unsigned long long)req->order_id, req->idem_key, tok,
             outcome == PAY_SUCCEEDED ? "approve" : "decline", (long long)(delay / 1000),
             duplicate ? " (+duplicate)" : "");
}

/* Send every reply that is due. A full reply queue just postpones it. */
static void send_due(void)
{
    int64_t now = of_now_us();
    while (heap_len > 0 && heap[0].due_us <= now) {
        pending_t p = heap_pop();
        if (pay_sign(&p.reply, key) != 0)
            continue;
        if (mq_send(mq_rep, (const char *)&p.reply, sizeof p.reply, 0) != 0) {
            if (errno == EAGAIN) { /* of-core is behind (or down): retry in 20 ms */
                p.due_us = now + 20000;
                heap_push(&p);
                break;
            }
            LOG_ERROR("mq_send", "%s", strerror(errno));
        }
    }
    arm_timer();
}

/* ------------------------------------------------------------------ main */

/* Latency and outcome mix; re-read on SIGHUP. 0, or -1 (nothing changed) if invalid. */
static int load_behaviour(const of_config_t *c)
{
    int ok[6];
    int64_t mn = of_config_get_long(c, "pay.latency_min_ms", 20, 0, 60000, &ok[0]);
    int64_t mx = of_config_get_long(c, "pay.latency_max_ms", 150, 0, 60000, &ok[1]);
    int64_t sl = of_config_get_long(c, "pay.slow_ms", 180000, 0, 3600000, &ok[2]);
    int de = (int)of_config_get_long(c, "pay.decline_pct", 10, 0, 100, &ok[3]);
    int to = (int)of_config_get_long(c, "pay.timeout_pct", 5, 0, 100, &ok[4]);
    int du = (int)of_config_get_long(c, "pay.duplicate_pct", 5, 0, 100, &ok[5]);
    for (int i = 0; i < 6; i++)
        if (!ok[i])
            return -1;
    if (mx < mn || de + to + du > 100)
        return -1;
    lat_min_ms = mn;
    lat_max_ms = mx;
    slow_ms = sl;
    decline_pct = de;
    timeout_pct = to;
    duplicate_pct = du;
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: of-pay [-c config]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *conf_path = "conf/orderforge.conf";
    int opt;
    while ((opt = getopt(argc, argv, "c:h")) != -1) {
        if (opt == 'c')
            conf_path = optarg;
        else
            usage();
    }
    char err[256];
    if (of_config_load(&cfg, conf_path, err, sizeof err) != 0) {
        fprintf(stderr, "of-pay: %s\n", err);
        return 1;
    }
    /* Before opening any file: leave root if started as root, then harden. */
    if (of_drop_privileges(&cfg, "of-pay", err, sizeof err) < 0) {
        fprintf(stderr, "of-pay: %s\n", err);
        return 1;
    }
    of_harden(conf_path, &cfg, "of-pay");
    if (of_log_open_config(&cfg, "of-pay") != 0)
        return 1;
    if (sodium_init() < 0 || pay_key_from_hex(of_config_get(&cfg, "pay.hmac_key", ""), key) != 0) {
        fprintf(stderr, "of-pay: pay.hmac_key missing or invalid (64 hex characters)\n");
        return 1;
    }

    char pid_path[512];
    pid_t holder = 0;
    of_pidfile_path(&cfg, "of-pay", pid_path, sizeof pid_path);
    if (of_pidfile_acquire(pid_path, &holder) < 0) {
        fprintf(stderr, "of-pay: already running (pid %d holds %s)\n", (int)holder, pid_path);
        return 1;
    }
    if (load_behaviour(&cfg) != 0) {
        fprintf(stderr, "of-pay: a pay.* setting is out of range\n");
        return 1;
    }
    int okm;
    long maxmsg = of_config_get_long(&cfg, "pay.mq_maxmsg", 10, 1, 1000, &okm);
    if (!okm) {
        fprintf(stderr, "of-pay: pay.mq_maxmsg out of range\n");
        return 1;
    }

    heap = calloc(HEAP_MAX, sizeof *heap);
    idem = calloc(IDEM_SLOTS, sizeof *idem);
    if (!heap || !idem)
        return 1;

    const char *req_name = of_config_get(&cfg, "pay.request_queue", "/orderforge-pay-req");
    const char *rep_name = of_config_get(&cfg, "pay.reply_queue", "/orderforge-pay-rep");
    struct mq_attr attr = {.mq_maxmsg = maxmsg, .mq_msgsize = sizeof(pay_msg_t)};
    mq_req = mq_open(req_name, O_CREAT | O_RDONLY | O_NONBLOCK | O_CLOEXEC, 0600, &attr);
    mq_rep = mq_open(rep_name, O_CREAT | O_WRONLY | O_NONBLOCK | O_CLOEXEC, 0600, &attr);
    if (mq_req == (mqd_t)-1 || mq_rep == (mqd_t)-1) {
        fprintf(stderr, "of-pay: mq_open: %s\n", strerror(errno));
        return 1;
    }

    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGTERM);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGHUP);
    sigprocmask(SIG_BLOCK, &sigs, NULL);
    int sfd = signalfd(-1, &sigs, SFD_CLOEXEC | SFD_NONBLOCK);
    tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (sfd < 0 || tfd < 0 || ep < 0) {
        fprintf(stderr, "of-pay: %s\n", strerror(errno));
        return 1;
    }
    struct epoll_event ev = {.events = EPOLLIN};
    ev.data.u32 = 1;
    epoll_ctl(ep, EPOLL_CTL_ADD, (int)mq_req, &ev);
    ev.data.u32 = 2;
    epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev);
    ev.data.u32 = 3;
    epoll_ctl(ep, EPOLL_CTL_ADD, sfd, &ev);

    LOG_INFO("start", "of-pay pid %d: latency %lld-%lld ms, slow %lld ms, random mix "
                      "decline %d%% timeout %d%% duplicate %d%%",
             (int)getpid(), (long long)lat_min_ms, (long long)lat_max_ms, (long long)slow_ms,
             decline_pct, timeout_pct, duplicate_pct);
    fprintf(stderr, "of-pay: listening on %s, replying on %s (pid %d)\n", req_name, rep_name,
            (int)getpid());

    for (;;) {
        struct epoll_event evs[8];
        int n = epoll_wait(ep, evs, 8, -1);
        if (n < 0 && errno == EINTR)
            continue;
        for (int i = 0; i < n; i++) {
            if (evs[i].data.u32 == 1) {
                pay_msg_t m;
                ssize_t got;
                while ((got = mq_receive(mq_req, (char *)&m, sizeof m, NULL)) >= 0) {
                    if ((size_t)got == sizeof m)
                        handle_request(&m);
                }
                send_due(); /* also re-arms the timer for anything newly scheduled */
            } else if (evs[i].data.u32 == 2) {
                uint64_t ticks;
                if (read(tfd, &ticks, sizeof ticks) < 0 && errno != EAGAIN)
                    LOG_WARN("timerfd", "%s", strerror(errno));
                send_due();
            } else {
                struct signalfd_siginfo si;
                while (read(sfd, &si, sizeof si) == sizeof si) {
                    if (si.ssi_signo == SIGHUP) {
                        of_log_reopen();
                        of_config_t fresh;
                        char e2[256];
                        if (of_config_load(&fresh, conf_path, e2, sizeof e2) == 0 &&
                            load_behaviour(&fresh) == 0)
                            LOG_INFO("reload", "SIGHUP: latency %lld-%lld ms, slow %lld ms, "
                                               "decline %d%% timeout %d%% duplicate %d%%",
                                     (long long)lat_min_ms, (long long)lat_max_ms,
                                     (long long)slow_ms, decline_pct, timeout_pct, duplicate_pct);
                        else
                            LOG_WARN("reload", "SIGHUP: config not reloaded (invalid)");
                        continue;
                    }
                    LOG_INFO("shutdown", "requests %llu approved %llu declined %llu silent %llu "
                                         "duplicates %llu rejected %llu idempotent %llu; "
                                         "%zu replies unsent",
                             (unsigned long long)stats.requests, (unsigned long long)stats.approved,
                             (unsigned long long)stats.declined, (unsigned long long)stats.silent,
                             (unsigned long long)stats.duplicates,
                             (unsigned long long)stats.rejected,
                             (unsigned long long)stats.idempotent_hits, heap_len);
                    mq_close(mq_req);
                    mq_close(mq_rep);
                    return 0;
                }
            }
        }
    }
}
