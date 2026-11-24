/*
 * of-core - the OrderForge engine.
 *
 * Threads
 *   reactor (main thread)  one epoll loop over: the listening Unix socket, every
 *                          of-api connection, an eventfd (completions) and a signalfd.
 *                          It only moves bytes: it reads frames, decodes them, pushes
 *                          jobs onto the bounded queue, and writes finished responses.
 *   workers (core.workers) each pops a job, runs store_handle() with its OWN MySQL
 *                          connection (no connection-pool contention), encodes the
 *                          response, appends it to the completion list and pokes the
 *                          eventfd so the reactor wakes up and sends it.
 *
 *   of-api ──frame──▶ reactor ──queue──▶ worker ──completion list + eventfd──▶ reactor ──▶ of-api
 *
 * Backpressure
 *   The queue is bounded (core.queue_size). When it is full the reactor answers 503
 *   "busy" at once instead of buffering without limit; busy_rejects counts it.
 *   A job that waited longer than core.job_timeout_ms is answered 503 without running.
 *
 * Connections and lifetimes
 *   Connections live in a fixed table. Each slot has a generation number that is bumped
 *   when the slot is closed; a job remembers (slot, generation), so a response for a
 *   connection that closed (and whose slot may have been reused) is simply dropped.
 *
 * Signals (signalfd, so they arrive as ordinary readable events in the epoll loop)
 *   SIGTERM/SIGINT  drain: stop accepting, finish queued and in-flight jobs (10 s max),
 *                   then exit cleanly
 *   SIGHUP          reopen the log file
 *
 * Security
 *   The socket is created mode 0660 and every connection's peer uid is checked with
 *   SO_PEERCRED: only our own uid (or core.allow_uid) may talk to the core.
 *
 *   usage: of-core [-c conf/orderforge.conf] [-m naive|db-lock|lockfree-shm]
 */
#include "common/config.h"
#include "common/log.h"
#include "common/proto.h"
#include "common/queue.h"
#include "common/stock.h"
#include "common/util.h"
#include "store/store.h"

#include <errno.h>
#include <libgen.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_CONNS      1024
#define MAX_EVENTS     256
#define DRAIN_LIMIT_US 10000000
#define TAG_LISTEN     (1ull << 32)
#define TAG_EVENTFD    (2ull << 32)
#define TAG_SIGNALFD   (3ull << 32)
#define TAG_CONN       (4ull << 32)

typedef struct {
    int      fd;       /* -1 when the slot is free */
    uint32_t gen;
    uint8_t *rbuf;
    size_t   rlen, rcap;
    buf_t    wbuf;     /* encoded frames waiting to be written */
    size_t   woff;
    bool     want_out; /* EPOLLOUT registered */
    int      inflight;
} conn_t;

typedef struct job {
    uint32_t    slot, gen;
    uint64_t    request_id;
    int64_t     enq_us;
    of_req_t    req;
    char       *arena;  /* owns req's strings */
    buf_t       out;    /* the encoded response frame */
    struct job *next;
} job_t;

/* ------------------------------------------------------------------ globals */

static of_config_t   cfg;
static stock_table_t stock;
static int           stock_mode = -1;
static queue_t       jobs;
static conn_t        conns[MAX_CONNS];
static int           epfd = -1, listen_fd = -1, efd = -1, sfd = -1;
static char          sock_path[108];
static uid_t         allow_uid;
static int64_t       job_timeout_us, slow_us;
static int           nworkers;

/* Completion list: workers append, the reactor drains. */
static pthread_mutex_t done_lock = PTHREAD_MUTEX_INITIALIZER;
static job_t          *done_head, *done_tail;

/* Worker start-up handshake. */
static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  init_cond = PTHREAD_COND_INITIALIZER;
static int             workers_ready, workers_failed;

#define METRIC(field) (stock.shm->metrics.field)

/* ------------------------------------------------------------------ jobs */

static void job_free(job_t *j)
{
    free(j->arena);
    buf_free(&j->out);
    free(j);
}

static void encode_error(buf_t *out, uint64_t rid, int status, const char *code,
                         const char *msg)
{
    buf_t body;
    buf_init(&body);
    buf_puts(&body, "{\"error\":{");
    buf_json_kv_str(&body, "code", code);
    buf_puts(&body, ",");
    buf_json_kv_str(&body, "message", msg);
    buf_puts(&body, "}}");
    (void)proto_encode_response(out, rid, status, "", body.data, body.len);
    buf_free(&body);
}

/* ------------------------------------------------------------------ workers */

static void complete(job_t *j)
{
    j->next = NULL;
    pthread_mutex_lock(&done_lock);
    if (done_tail)
        done_tail->next = j;
    else
        done_head = j;
    done_tail = j;
    pthread_mutex_unlock(&done_lock);

    uint64_t one = 1;
    ssize_t w;
    do {
        w = write(efd, &one, sizeof one); /* wakes the reactor's epoll_wait */
    } while (w < 0 && errno == EINTR);
}

static void *worker_main(void *arg)
{
    long id = (long)arg;
    store_t store;
    char err[256];

    int rc = store_init(&store, &cfg, &stock, stock_mode, err, sizeof err);
    pthread_mutex_lock(&init_lock);
    if (rc == 0)
        workers_ready++;
    else
        workers_failed++;
    pthread_cond_signal(&init_cond);
    pthread_mutex_unlock(&init_lock);
    if (rc != 0) {
        LOG_ERROR("worker_init", "worker %ld: %s", id, err);
        return NULL;
    }

    int64_t next_reconnect = 0;
    job_t *j;
    while ((j = queue_pop(&jobs)) != NULL) {
        atomic_store(&METRIC(queue_depth), queue_depth(&jobs));
        atomic_fetch_add(&METRIC(active_workers), 1);
        int64_t start = of_now_us();

        /* A worker whose MySQL connection broke reconnects (at most once a second);
         * meanwhile requests fail fast with 503 from the store. */
        if (db_broken(store.db) && start >= next_reconnect) {
            next_reconnect = start + 1000000;
            if (store_reconnect(&store, err, sizeof err) == 0)
                LOG_INFO("db_reconnect", "worker %ld reconnected to MySQL", id);
            else
                LOG_WARN("db_reconnect", "worker %ld: %s", id, err);
        }

        if (start - j->enq_us > job_timeout_us) {
            encode_error(&j->out, j->request_id, 503, "timeout",
                         "the store is overloaded, please retry");
        } else {
            of_resp_t resp;
            of_resp_init(&resp);
            store_handle(&store, &j->req, &resp);
            if (proto_encode_response(&j->out, j->request_id, resp.status, resp.set_cookie,
                                      resp.body.data, resp.body.len) != 0)
                encode_error(&j->out, j->request_id, 500, "internal", "response too large");
            of_resp_free(&resp);
        }

        int64_t took = of_now_us() - start;
        if (took > slow_us)
            LOG_WARN("slow_request", "trace=%llu %s took %lld ms (queued %lld ms)",
                     (unsigned long long)j->request_id, j->req.path, (long long)(took / 1000),
                     (long long)((start - j->enq_us) / 1000));
        atomic_fetch_sub(&METRIC(active_workers), 1);
        complete(j);
    }
    store_close(&store);
    return NULL;
}

/* ------------------------------------------------------------------ connections */

static void conn_close(uint32_t slot)
{
    conn_t *c = &conns[slot];
    if (c->fd < 0)
        return;
    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    c->fd = -1;
    c->gen++; /* responses still in flight for this slot will be dropped */
    free(c->rbuf);
    c->rbuf = NULL;
    c->rlen = c->rcap = 0;
    buf_free(&c->wbuf);
    c->woff = 0;
    c->want_out = false;
    c->inflight = 0;
}

static void set_out_interest(uint32_t slot, bool want)
{
    conn_t *c = &conns[slot];
    if (c->want_out == want)
        return;
    struct epoll_event ev = {.events = EPOLLIN | EPOLLRDHUP | (want ? EPOLLOUT : 0),
                             .data.u64 = TAG_CONN | slot};
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev) == 0)
        c->want_out = want;
}

static void conn_flush(uint32_t slot)
{
    conn_t *c = &conns[slot];
    while (c->woff < c->wbuf.len) {
        ssize_t n = write(c->fd, c->wbuf.data + c->woff, c->wbuf.len - c->woff);
        if (n > 0) {
            c->woff += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            set_out_interest(slot, true); /* the kernel buffer is full: wait for EPOLLOUT */
            return;
        }
        conn_close(slot);
        return;
    }
    buf_reset(&c->wbuf);
    c->woff = 0;
    set_out_interest(slot, false);
}

static void dispatch(uint32_t slot, const of_frame_hdr_t *h, const uint8_t *payload,
                     bool draining)
{
    conn_t *c = &conns[slot];
    atomic_fetch_add(&METRIC(requests_total), 1);

    job_t *j = calloc(1, sizeof *j);
    char *arena = j ? malloc(h->length + 8) : NULL;
    if (!j || !arena) {
        free(j);
        encode_error(&c->wbuf, h->request_id, 503, "busy", "out of memory");
        return;
    }
    j->arena = arena;
    if (proto_decode_request(payload, h->length, &j->req, arena, h->length + 8) != 0) {
        LOG_WARN("protocol", "malformed request frame, closing connection");
        job_free(j);
        conn_close(slot);
        return;
    }
    j->slot = slot;
    j->gen = c->gen;
    j->request_id = h->request_id;
    j->enq_us = of_now_us();

    if (draining) {
        encode_error(&c->wbuf, h->request_id, 503, "shutting_down", "the store is restarting");
        job_free(j);
        return;
    }
    if (!queue_try_push(&jobs, j)) {
        /* Backpressure: refuse now rather than queue without limit. */
        atomic_fetch_add(&METRIC(busy_rejects), 1);
        encode_error(&c->wbuf, h->request_id, 503, "busy", "the store is busy, please retry");
        job_free(j);
        return;
    }
    c->inflight++;
    atomic_store(&METRIC(queue_depth), queue_depth(&jobs));
}

static void conn_read(uint32_t slot, bool draining)
{
    conn_t *c = &conns[slot];
    for (;;) {
        if (c->rcap - c->rlen < 4096) {
            size_t cap = c->rcap ? c->rcap * 2 : 8192;
            size_t max = OF_PROTO_HDR_SIZE + OF_MAX_REQ_PAYLOAD + 4096;
            if (cap > max)
                cap = max;
            if (cap <= c->rlen) { /* a frame larger than any legal one: drop the peer */
                conn_close(slot);
                return;
            }
            uint8_t *p = realloc(c->rbuf, cap);
            if (!p) {
                conn_close(slot);
                return;
            }
            c->rbuf = p;
            c->rcap = cap;
        }
        ssize_t n = read(c->fd, c->rbuf + c->rlen, c->rcap - c->rlen);
        if (n > 0) {
            c->rlen += (size_t)n;
        } else if (n == 0) {
            conn_close(slot); /* peer hung up; its in-flight responses are dropped */
            return;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else {
            conn_close(slot);
            return;
        }

        /* Peel off every complete frame. */
        size_t off = 0;
        while (c->rlen - off >= OF_PROTO_HDR_SIZE) {
            of_frame_hdr_t h;
            memcpy(&h, c->rbuf + off, sizeof h);
            if (proto_check_header(&h, OF_MSG_REQUEST) != 0) {
                LOG_WARN("protocol", "bad frame header, closing connection");
                conn_close(slot);
                return;
            }
            if (c->rlen - off < OF_PROTO_HDR_SIZE + h.length)
                break; /* the rest of this frame has not arrived yet */
            dispatch(slot, &h, c->rbuf + off + OF_PROTO_HDR_SIZE, draining);
            if (c->fd < 0)
                return;
            off += OF_PROTO_HDR_SIZE + h.length;
        }
        if (off) {
            memmove(c->rbuf, c->rbuf + off, c->rlen - off);
            c->rlen -= off;
        }
    }
    if (c->wbuf.len > c->woff)
        conn_flush(slot); /* immediate replies (busy, draining) */
}

static void accept_all(void)
{
    for (;;) {
        int fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                LOG_WARN("accept", "%s", strerror(errno));
            return;
        }

        /* Who is on the other end? The kernel tells us, it cannot be forged. */
        struct ucred cred;
        socklen_t len = sizeof cred;
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 ||
            (cred.uid != getuid() && cred.uid != allow_uid)) {
            LOG_WARN("peer_rejected", "connection from uid %d pid %d refused", (int)cred.uid,
                     (int)cred.pid);
            close(fd);
            continue;
        }

        uint32_t slot = MAX_CONNS;
        for (uint32_t i = 0; i < MAX_CONNS; i++)
            if (conns[i].fd < 0) {
                slot = i;
                break;
            }
        if (slot == MAX_CONNS) {
            LOG_WARN("accept", "connection table full (%d)", MAX_CONNS);
            close(fd);
            continue;
        }
        conn_t *c = &conns[slot];
        c->fd = fd;
        buf_init(&c->wbuf);
        struct epoll_event ev = {.events = EPOLLIN | EPOLLRDHUP, .data.u64 = TAG_CONN | slot};
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
            conn_close(slot);
            continue;
        }
        LOG_DEBUG("accept", "of-api pid %d connected (slot %u)", (int)cred.pid, slot);
    }
}

/* Move finished jobs onto their connections' write buffers. */
static void drain_completions(void)
{
    uint64_t count;
    while (read(efd, &count, sizeof count) < 0 && errno == EINTR)
        ;

    pthread_mutex_lock(&done_lock);
    job_t *j = done_head;
    done_head = done_tail = NULL;
    pthread_mutex_unlock(&done_lock);

    while (j) {
        job_t *next = j->next;
        conn_t *c = &conns[j->slot];
        if (c->fd >= 0 && c->gen == j->gen) {
            c->inflight--;
            buf_append(&c->wbuf, j->out.data, j->out.len);
            conn_flush(j->slot);
        }
        job_free(j);
        j = next;
    }
}

static int total_inflight(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CONNS; i++)
        if (conns[i].fd >= 0)
            n += conns[i].inflight;
    return n;
}

/* ------------------------------------------------------------------ setup */

static int open_socket(void)
{
    const char *path = of_config_get(&cfg, "core.socket", "run/of-core.sock");
    if (strlen(path) >= sizeof sock_path) {
        LOG_ERROR("socket", "core.socket path too long (max %zu)", sizeof sock_path - 1);
        return -1;
    }
    snprintf(sock_path, sizeof sock_path, "%s", path);

    char dir[sizeof sock_path];
    snprintf(dir, sizeof dir, "%s", path);
    if (of_mkdir_p(dirname(dir), 0750) != 0) {
        LOG_ERROR("socket", "mkdir for %s: %s", path, strerror(errno));
        return -1;
    }

    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    memcpy(addr.sun_path, sock_path, strlen(sock_path) + 1);

    /* A leftover socket file: if something answers, another core is running. */
    int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (probe >= 0 && connect(probe, (struct sockaddr *)&addr, sizeof addr) == 0) {
        close(probe);
        LOG_ERROR("socket", "another of-core is already listening on %s", sock_path);
        fprintf(stderr, "of-core: another of-core is already listening on %s\n", sock_path);
        return -1;
    }
    if (probe >= 0)
        close(probe);
    unlink(sock_path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    mode_t old = umask(0117); /* socket file mode 0660 */
    int rc = bind(fd, (struct sockaddr *)&addr, sizeof addr);
    umask(old);
    if (rc != 0 || listen(fd, 512) != 0) {
        LOG_ERROR("socket", "bind/listen %s: %s", sock_path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int setup_stock(void)
{
    const char *name = of_config_get(&cfg, "core.shm_name", "/orderforge");
    int ok;
    long cap = of_config_get_long(&cfg, "core.stock_capacity", 4096, 64, 1 << 24, &ok);
    if (!ok || stock_create(&stock, name, (uint32_t)cap) != 0) {
        LOG_ERROR("shm", "cannot create shared memory %s: %s", name, strerror(errno));
        return -1;
    }

    char err[256];
    db_t *db = db_connect(&cfg, err, sizeof err);
    if (!db) {
        LOG_ERROR("shm", "loading stock: %s", err);
        return -1;
    }
    int n = store_load_stock(db, &stock, err, sizeof err);
    db_close(db);
    if (n < 0) {
        LOG_ERROR("shm", "%s", err);
        return -1;
    }
    LOG_INFO("shm", "stock table %s: %d products, capacity %u, %zu bytes", name, n,
             stock.shm->capacity, stock.map_size);
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: of-core [-c config] [-m naive|db-lock|lockfree-shm]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *conf_path = "conf/orderforge.conf";
    const char *mode_arg = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "c:m:h")) != -1) {
        if (opt == 'c')
            conf_path = optarg;
        else if (opt == 'm')
            mode_arg = optarg;
        else
            usage();
    }

    char err[256];
    if (of_config_load(&cfg, conf_path, err, sizeof err) != 0) {
        fprintf(stderr, "of-core: %s\n", err);
        return 1;
    }
    if (of_log_open_config(&cfg, "of-core") != 0)
        return 1;

    stock_mode = stock_mode_parse(mode_arg ? mode_arg
                                           : of_config_get(&cfg, "store.stock_mode",
                                                           "lockfree-shm"));
    if (stock_mode < 0) {
        fprintf(stderr, "of-core: stock mode must be naive, db-lock or lockfree-shm\n");
        return 1;
    }

    int ok1, ok2, ok3, ok4, ok5;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    nworkers = (int)of_config_get_long(&cfg, "core.workers", ncpu > 0 ? ncpu : 4, 1, 256, &ok1);
    long qsize = of_config_get_long(&cfg, "core.queue_size", 1024, 1, 1 << 20, &ok2);
    job_timeout_us = of_config_get_long(&cfg, "core.job_timeout_ms", 10000, 1, 600000, &ok3) * 1000;
    slow_us = of_config_get_long(&cfg, "core.slow_ms", 500, 1, 600000, &ok4) * 1000;
    allow_uid = (uid_t)of_config_get_long(&cfg, "core.allow_uid", (long)getuid(), 0, 1 << 30, &ok5);
    if (!(ok1 && ok2 && ok3 && ok4 && ok5)) {
        fprintf(stderr, "of-core: a core.* setting is out of range\n");
        return 1;
    }

    for (int i = 0; i < MAX_CONNS; i++)
        conns[i].fd = -1;

    /* The socket first: it detects an already-running core before we touch (and would
     * otherwise reset) its shared-memory table. */
    if ((listen_fd = open_socket()) < 0)
        return 1;
    if (setup_stock() != 0 || queue_init(&jobs, (size_t)qsize) != 0) {
        unlink(sock_path);
        return 1;
    }
    stock.shm->stock_mode = (uint32_t)stock_mode;

    /* Block the signals for every thread (workers inherit the mask), then receive them
     * as data through a signalfd in the reactor. */
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGTERM);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &sigs, NULL);
    signal(SIGPIPE, SIG_IGN);

    sfd = signalfd(-1, &sigs, SFD_NONBLOCK | SFD_CLOEXEC);
    efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (sfd < 0 || efd < 0 || epfd < 0) {
        LOG_ERROR("init", "signalfd/eventfd/epoll: %s", strerror(errno));
        return 1;
    }
    struct epoll_event ev = {.events = EPOLLIN};
    ev.data.u64 = TAG_LISTEN;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);
    ev.data.u64 = TAG_EVENTFD;
    epoll_ctl(epfd, EPOLL_CTL_ADD, efd, &ev);
    ev.data.u64 = TAG_SIGNALFD;
    epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &ev);

    pthread_t *threads = calloc((size_t)nworkers, sizeof *threads);
    for (long i = 0; i < nworkers; i++)
        if (pthread_create(&threads[i], NULL, worker_main, (void *)i) != 0) {
            LOG_ERROR("init", "pthread_create failed");
            return 1;
        }
    pthread_mutex_lock(&init_lock);
    while (workers_ready + workers_failed < nworkers)
        pthread_cond_wait(&init_cond, &init_lock);
    pthread_mutex_unlock(&init_lock);
    if (workers_failed) {
        fprintf(stderr, "of-core: %d workers could not start (see the log)\n", workers_failed);
        return 1;
    }

    atomic_store(&stock.shm->started_unix, (int64_t)time(NULL));
    LOG_INFO("start", "of-core pid %d on %s: %d workers, queue %ld, stock mode %s",
             (int)getpid(), sock_path, nworkers, qsize, stock_mode_name(stock_mode));
    fprintf(stderr, "of-core: listening on %s (%d workers, mode %s, pid %d)\n", sock_path,
            nworkers, stock_mode_name(stock_mode), (int)getpid());

    /* ---------------------------------------------------------------- reactor */
    bool draining = false;
    int64_t drain_deadline = 0;
    struct epoll_event evs[MAX_EVENTS];

    for (;;) {
        if (draining && (total_inflight() == 0 || of_now_us() > drain_deadline))
            break;
        int n = epoll_wait(epfd, evs, MAX_EVENTS, draining ? 100 : -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            LOG_ERROR("epoll", "%s", strerror(errno));
            break;
        }
        for (int i = 0; i < n; i++) {
            uint64_t tag = evs[i].data.u64 & ~0xFFFFFFFFull;
            uint32_t slot = (uint32_t)(evs[i].data.u64 & 0xFFFFFFFFu);

            if (tag == TAG_LISTEN) {
                accept_all();
            } else if (tag == TAG_EVENTFD) {
                drain_completions();
            } else if (tag == TAG_SIGNALFD) {
                struct signalfd_siginfo si;
                while (read(sfd, &si, sizeof si) == sizeof si) {
                    if (si.ssi_signo == SIGHUP) {
                        of_log_reopen();
                        LOG_INFO("reload", "log reopened");
                    } else if (!draining) {
                        draining = true;
                        drain_deadline = of_now_us() + DRAIN_LIMIT_US;
                        epoll_ctl(epfd, EPOLL_CTL_DEL, listen_fd, NULL);
                        close(listen_fd);
                        unlink(sock_path);
                        LOG_INFO("shutdown", "%s: draining %d in-flight requests",
                                 strsignal((int)si.ssi_signo), total_inflight());
                    }
                }
            } else if (tag == TAG_CONN && slot < MAX_CONNS && conns[slot].fd >= 0) {
                if (evs[i].events & EPOLLOUT)
                    conn_flush(slot);
                if (conns[slot].fd >= 0 && (evs[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)))
                    conn_read(slot, draining);
                else if (conns[slot].fd >= 0 && (evs[i].events & EPOLLERR))
                    conn_close(slot);
            }
        }
    }

    /* Workers finish whatever is still queued, then see NULL and exit. */
    queue_close(&jobs);
    for (int i = 0; i < nworkers; i++)
        pthread_join(threads[i], NULL);
    drain_completions();
    for (uint32_t i = 0; i < MAX_CONNS; i++)
        conn_close(i);
    free(threads);
    queue_destroy(&jobs);
    stock_close(&stock, true);
    LOG_INFO("shutdown", "of-core stopped");
    return 0;
}
