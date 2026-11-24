/*
 * of-bench - load generator and invariant checker.
 *
 *   of-bench [-c config] oversell [options]
 *
 * The oversell experiment: STOCK units of one product, BUYERS customers who each have
 * that product in their cart, and every checkout fired at the same instant.
 * A correct engine sells exactly min(STOCK, BUYERS) units; anything more is oversold.
 *
 * How the rush is made truly simultaneous
 *   1. Setup (not timed): stock is set through the admin API, so of-core's shared memory
 *      stays in step. Buyers, sessions and carts are inserted straight into MySQL: each
 *      buyer gets a random session token (only its SHA-256 is stored, exactly as a real
 *      login would), so the rush needs no Argon2 logins.
 *   2. T threads each own an epoll instance and BUYERS/T non-blocking TCP sockets. Every
 *      thread first connects all of its sockets, then waits at a pthread barrier.
 *   3. When the last thread arrives, all threads write all their requests at once and
 *      collect the responses with epoll, timing each one with CLOCK_MONOTONIC.
 *   4. The result is checked against MySQL (units sold, ledger) and, in lockfree-shm
 *      mode, against the shared-memory counters.
 *
 * Options
 *   --buyers N         concurrent buyers (default 1000)
 *   --stock S          units available (default 50)
 *   --product ID       product to rush (default 1, HOT-001)
 *   --qty Q            units per buyer (default 1)
 *   --threads T        client threads (default 8)
 *   --url URL          http://host:port of Apache (default http://127.0.0.1:8080)
 *   --admin EMAIL      admin account (default admin@orderforge.local)
 *   --csv FILE         append one result row to FILE
 *   admin password: env OF_ADMIN_PASSWORD (default admin12345)
 *
 * Exit status: 0 when nothing was oversold and every invariant holds, 1 otherwise.
 */
#include "common/buf.h"
#include "common/config.h"
#include "common/log.h"
#include "common/stock.h"
#include "common/util.h"
#include "store/db.h"
#include "store/store.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RESP_MAX 8192

static of_config_t cfg;

/* ------------------------------------------------------------------ options */

typedef struct {
    int         buyers, stock, threads, qty;
    int64_t     product;
    char        host[128];
    char        port[8];
    const char *admin_email;
    const char *admin_password;
    const char *csv;
} opts_t;

static opts_t o = {1000, 50, 8, 1, 1, "127.0.0.1", "8080", "admin@orderforge.local", NULL, NULL};

static void usage(void)
{
    fprintf(stderr,
            "usage: of-bench [-c config] oversell [--buyers N] [--stock S] [--product ID]\n"
            "                [--qty Q] [--threads T] [--url http://host:port] [--admin EMAIL]\n"
            "                [--csv FILE]        (admin password: $OF_ADMIN_PASSWORD)\n");
    exit(2);
}

static int parse_url(const char *url)
{
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0)
        p += 7;
    const char *colon = strchr(p, ':');
    const char *slash = strchr(p, '/');
    size_t hlen = colon ? (size_t)(colon - p) : slash ? (size_t)(slash - p) : strlen(p);
    if (hlen == 0 || hlen >= sizeof o.host)
        return -1;
    memcpy(o.host, p, hlen);
    o.host[hlen] = '\0';
    if (colon) {
        size_t plen = slash ? (size_t)(slash - colon - 1) : strlen(colon + 1);
        if (plen == 0 || plen >= sizeof o.port)
            return -1;
        memcpy(o.port, colon + 1, plen);
        o.port[plen] = '\0';
    } else {
        snprintf(o.port, sizeof o.port, "80");
    }
    return 0;
}

/* ------------------------------------------------------------------ tiny HTTP client */

static struct addrinfo *server;

static int resolve(void)
{
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    int rc = getaddrinfo(o.host, o.port, &hints, &server);
    if (rc != 0) {
        fprintf(stderr, "of-bench: %s:%s: %s\n", o.host, o.port, gai_strerror(rc));
        return -1;
    }
    return 0;
}

typedef struct {
    int  status;
    char cookie[160]; /* of_session=... value */
    char body[RESP_MAX];
} http_resp_t;

/* Blocking request for setup steps. 0 on any HTTP answer, -1 on network failure. */
static int http_call(const char *method, const char *path, const char *body, const char *session,
                     const char *csrf, http_resp_t *r)
{
    int fd = socket(server->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, server->ai_addr, server->ai_addrlen) != 0) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    char req[2048];
    int n = snprintf(req, sizeof req,
                     "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
                     "Content-Type: application/json\r\nContent-Length: %zu\r\n"
                     "%s%s%s%s%s%s\r\n%s",
                     method, path, o.host, body ? strlen(body) : 0,
                     session ? "Cookie: of_session=" : "", session ? session : "",
                     session ? "\r\n" : "", csrf ? "X-CSRF-Token: " : "", csrf ? csrf : "",
                     csrf ? "\r\n" : "", body ? body : "");
    if (n < 0 || (size_t)n >= sizeof req || write(fd, req, (size_t)n) != n) {
        close(fd);
        return -1;
    }
    static char raw[64 * 1024];
    size_t got = 0;
    ssize_t k;
    while (got < sizeof raw - 1 && (k = read(fd, raw + got, sizeof raw - 1 - got)) > 0)
        got += (size_t)k;
    raw[got] = '\0';
    close(fd);

    memset(r, 0, sizeof *r);
    if (sscanf(raw, "HTTP/1.%*d %d", &r->status) != 1)
        return -1;
    const char *c = strstr(raw, "of_session=");
    if (c) {
        c += strlen("of_session=");
        size_t len = strcspn(c, ";\r\n");
        if (len < sizeof r->cookie) {
            memcpy(r->cookie, c, len);
            r->cookie[len] = '\0';
        }
    }
    const char *b = strstr(raw, "\r\n\r\n");
    if (b)
        snprintf(r->body, sizeof r->body, "%s", b + 4);
    return 0;
}

/* Pull "key":"value" or "key":number out of a JSON body (good enough for known keys). */
static int json_find(const char *body, const char *key, char *out, size_t outlen)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(body, pat);
    if (!p)
        return -1;
    p += strlen(pat);
    if (*p == '"')
        p++;
    size_t len = strcspn(p, "\",}");
    if (len >= outlen)
        return -1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ buyers */

typedef struct {
    char    token[65];
    char    csrf[65];
    int     fd;
    char    req[512];
    size_t  req_len, sent;
    char    resp[512];
    size_t  resp_len;
    int     status;    /* 0 = no answer */
    int64_t t_send, t_done;
} buyer_t;

static buyer_t *buyers;

static int create_buyers(db_t *db, const char *run_id)
{
    char hash[crypto_pwhash_STRBYTES];
    if (store_hash_password("bench-password", hash, sizeof hash) != 0)
        return -1;

    for (int i = 0; i < o.buyers; i++) {
        if (i % 250 == 0) { /* commit in batches of 250 buyers */
            if (i > 0 && db_commit(db) != 0)
                return -1;
            if (db_begin(db) != 0)
                return -1;
        }
        buyer_t *b = &buyers[i];
        unsigned char raw[32];
        if (of_random_bytes(raw, sizeof raw) != 0)
            return -1;
        of_hex(b->token, raw, sizeof raw);
        if (of_random_bytes(raw, sizeof raw) != 0)
            return -1;
        of_hex(b->csrf, raw, sizeof raw);

        unsigned char h[crypto_hash_sha256_BYTES];
        char token_hash[65];
        crypto_hash_sha256(h, (const unsigned char *)b->token, 64);
        of_hex(token_hash, h, sizeof h);

        char email[96], name[48];
        snprintf(email, sizeof email, "bench-%s-%d@bench.local", run_id, i);
        snprintf(name, sizeof name, "Bench Buyer %d", i);
        if (db_exec(db, "INSERT INTO users (email, name, pw_hash) VALUES (?, ?, ?)", "sss", email,
                    name, hash) != 1)
            return -1;
        int64_t uid = (int64_t)db_insert_id(db);
        if (db_exec(db,
                    "INSERT INTO sessions (token_hash, user_id, csrf_token, ip, expires_at) "
                    "VALUES (?, ?, ?, 'bench', NOW(3) + INTERVAL 1 HOUR)",
                    "sis", token_hash, uid, b->csrf) != 1 ||
            db_exec(db, "INSERT INTO carts (user_id) VALUES (?)", "i", uid) != 1)
            return -1;
        int64_t cart = (int64_t)db_insert_id(db);
        if (db_exec(db, "INSERT INTO cart_items (cart_id, product_id, qty) VALUES (?, ?, ?)", "iii",
                    cart, o.product, (int64_t)o.qty) != 1)
            return -1;

        b->req_len = (size_t)snprintf(b->req, sizeof b->req,
                                      "POST /api/checkout HTTP/1.1\r\nHost: %s\r\n"
                                      "Cookie: of_session=%s\r\nX-CSRF-Token: %s\r\n"
                                      "Content-Length: 0\r\nConnection: close\r\n\r\n",
                                      o.host, b->token, b->csrf);
    }
    return db_commit(db);
}

/* ------------------------------------------------------------------ the rush */

typedef struct {
    int first, count;
    int connect_failures;
} shard_t;

static pthread_barrier_t start_line;

static void *rush(void *arg)
{
    shard_t *sh = arg;
    int ep = epoll_create1(EPOLL_CLOEXEC);

    /* Phase 1: connect every socket of this shard (not timed). */
    int pending = 0;
    for (int i = sh->first; i < sh->first + sh->count; i++) {
        buyer_t *b = &buyers[i];
        b->fd = socket(server->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        int one = 1;
        setsockopt(b->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        if (connect(b->fd, server->ai_addr, server->ai_addrlen) != 0 && errno != EINPROGRESS) {
            close(b->fd);
            b->fd = -1;
            sh->connect_failures++;
            continue;
        }
        struct epoll_event ev = {.events = EPOLLOUT, .data.u32 = (uint32_t)i};
        epoll_ctl(ep, EPOLL_CTL_ADD, b->fd, &ev);
        pending++;
    }
    struct epoll_event evs[256];
    int64_t deadline = of_now_us() + 30000000;
    while (pending > 0 && of_now_us() < deadline) {
        int n = epoll_wait(ep, evs, 256, 1000);
        for (int k = 0; k < n; k++) {
            buyer_t *b = &buyers[evs[k].data.u32];
            int err = 0;
            socklen_t len = sizeof err;
            getsockopt(b->fd, SOL_SOCKET, SO_ERROR, &err, &len);
            epoll_ctl(ep, EPOLL_CTL_DEL, b->fd, NULL);
            if (err) {
                close(b->fd);
                b->fd = -1;
                sh->connect_failures++;
            }
            pending--;
        }
    }

    /* Phase 2: everybody goes at once. */
    pthread_barrier_wait(&start_line);

    int open = 0;
    for (int i = sh->first; i < sh->first + sh->count; i++) {
        buyer_t *b = &buyers[i];
        if (b->fd < 0)
            continue;
        b->t_send = of_now_us();
        ssize_t w = write(b->fd, b->req, b->req_len);
        b->sent = w > 0 ? (size_t)w : 0;
        struct epoll_event ev = {.events = EPOLLIN | (b->sent < b->req_len ? EPOLLOUT : 0),
                                 .data.u32 = (uint32_t)i};
        epoll_ctl(ep, EPOLL_CTL_ADD, b->fd, &ev);
        open++;
    }

    /* Phase 3: collect responses (each connection closes after its response). */
    deadline = of_now_us() + 120000000;
    while (open > 0 && of_now_us() < deadline) {
        int n = epoll_wait(ep, evs, 256, 1000);
        for (int k = 0; k < n; k++) {
            buyer_t *b = &buyers[evs[k].data.u32];
            if ((evs[k].events & EPOLLOUT) && b->sent < b->req_len) {
                ssize_t w = write(b->fd, b->req + b->sent, b->req_len - b->sent);
                if (w > 0)
                    b->sent += (size_t)w;
                if (b->sent == b->req_len) {
                    struct epoll_event ev = {.events = EPOLLIN, .data.u32 = evs[k].data.u32};
                    epoll_ctl(ep, EPOLL_CTL_MOD, b->fd, &ev);
                }
            }
            if (!(evs[k].events & (EPOLLIN | EPOLLHUP | EPOLLERR)))
                continue;
            for (;;) {
                char tmp[2048];
                ssize_t r = read(b->fd, tmp, sizeof tmp);
                if (r > 0) {
                    size_t keep = sizeof b->resp - 1 - b->resp_len;
                    if ((size_t)r < keep)
                        keep = (size_t)r;
                    memcpy(b->resp + b->resp_len, tmp, keep);
                    b->resp_len += keep;
                    continue;
                }
                if (r < 0 && errno == EINTR)
                    continue;
                if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    break;
                /* EOF or error: this request is finished. */
                b->resp[b->resp_len] = '\0';
                b->t_done = of_now_us();
                if (sscanf(b->resp, "HTTP/1.%*d %d", &b->status) != 1)
                    b->status = 0;
                epoll_ctl(ep, EPOLL_CTL_DEL, b->fd, NULL);
                close(b->fd);
                b->fd = -1;
                open--;
                break;
            }
        }
    }
    for (int i = sh->first; i < sh->first + sh->count; i++)
        if (buyers[i].fd >= 0)
            close(buyers[i].fd);
    close(ep);
    return NULL;
}

/* ------------------------------------------------------------------ reporting */

static int cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

static int cmd_oversell(int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        int64_t n;
        if (!v)
            usage();
        if (strcmp(a, "--buyers") == 0 && of_parse_i64(v, 1, 100000, &n) == 0)
            o.buyers = (int)n;
        else if (strcmp(a, "--stock") == 0 && of_parse_i64(v, 0, 10000000, &n) == 0)
            o.stock = (int)n;
        else if (strcmp(a, "--product") == 0 && of_parse_i64(v, 1, INT64_MAX, &n) == 0)
            o.product = n;
        else if (strcmp(a, "--qty") == 0 && of_parse_i64(v, 1, 100, &n) == 0)
            o.qty = (int)n;
        else if (strcmp(a, "--threads") == 0 && of_parse_i64(v, 1, 256, &n) == 0)
            o.threads = (int)n;
        else if (strcmp(a, "--url") == 0 && parse_url(v) == 0)
            ;
        else if (strcmp(a, "--admin") == 0)
            o.admin_email = v;
        else if (strcmp(a, "--csv") == 0)
            o.csv = v;
        else
            usage();
        i++;
    }
    o.admin_password = getenv("OF_ADMIN_PASSWORD") ? getenv("OF_ADMIN_PASSWORD") : "admin12345";
    if (o.threads > o.buyers)
        o.threads = o.buyers;
    if (sodium_init() < 0 || resolve() != 0)
        return 1;

    char err[256];
    db_t *db = db_connect(&cfg, err, sizeof err);
    if (!db) {
        fprintf(stderr, "of-bench: database: %s\n", err);
        return 1;
    }

    /* ---- admin session: stock mode and stock level go through the API */
    http_resp_t r;
    char body[512], csrf[80], mode[32] = "?", val[32];
    snprintf(body, sizeof body, "{\"email\":\"%s\",\"password\":\"%s\"}", o.admin_email,
             o.admin_password);
    if (http_call("POST", "/api/auth/login", body, NULL, NULL, &r) != 0) {
        fprintf(stderr, "of-bench: cannot reach http://%s:%s (is Apache running?)\n", o.host,
                o.port);
        return 1;
    }
    if (r.status != 200 || json_find(r.body, "csrf_token", csrf, sizeof csrf) != 0) {
        fprintf(stderr, "of-bench: admin login failed (%d): %s\n", r.status, r.body);
        return 1;
    }
    char admin_cookie[160];
    snprintf(admin_cookie, sizeof admin_cookie, "%s", r.cookie);

    if (http_call("GET", "/api/admin/system", NULL, admin_cookie, NULL, &r) == 0 && r.status == 200)
        json_find(r.body, "stock_mode", mode, sizeof mode);

    char path[96];
    snprintf(path, sizeof path, "/api/products/%lld", (long long)o.product);
    if (http_call("GET", path, NULL, NULL, NULL, &r) != 0 || r.status != 200 ||
        json_find(r.body, "stock", val, sizeof val) != 0) {
        fprintf(stderr, "of-bench: product %lld not found\n", (long long)o.product);
        return 1;
    }
    int64_t delta = o.stock - atoll(val);
    if (delta != 0) {
        snprintf(path, sizeof path, "/api/admin/products/%lld/stock", (long long)o.product);
        snprintf(body, sizeof body, "{\"delta\":%lld,\"reason\":\"adjust\"}", (long long)delta);
        if (http_call("POST", path, body, admin_cookie, csrf, &r) != 0 || r.status != 200) {
            fprintf(stderr, "of-bench: could not set stock (%d): %s\n", r.status, r.body);
            return 1;
        }
    }

    /* ---- buyers */
    char run_id[24];
    snprintf(run_id, sizeof run_id, "%lld", (long long)time(NULL));
    buyers = calloc((size_t)o.buyers, sizeof *buyers);
    printf("of-bench oversell: mode %s, %d buyers x %d unit%s of product %lld, stock %d, "
           "%d threads\n",
           mode, o.buyers, o.qty, o.qty > 1 ? "s" : "", (long long)o.product, o.stock, o.threads);
    printf("  setting up buyers, sessions and carts...\n");
    fflush(stdout);
    if (create_buyers(db, run_id) != 0) {
        fprintf(stderr, "of-bench: creating buyers: %s\n", db_error(db));
        return 1;
    }

    /* ---- rush */
    printf("  connecting %d sockets, then all checkouts at once\n", o.buyers);
    fflush(stdout);
    pthread_barrier_init(&start_line, NULL, (unsigned)o.threads);
    shard_t *shards = calloc((size_t)o.threads, sizeof *shards);
    pthread_t *th = calloc((size_t)o.threads, sizeof *th);
    int per = o.buyers / o.threads, extra = o.buyers % o.threads, next = 0;
    int64_t t0 = of_now_us();
    for (int i = 0; i < o.threads; i++) {
        shards[i].first = next;
        shards[i].count = per + (i < extra);
        next += shards[i].count;
        pthread_create(&th[i], NULL, rush, &shards[i]);
    }
    int connect_failures = 0;
    for (int i = 0; i < o.threads; i++) {
        pthread_join(th[i], NULL);
        connect_failures += shards[i].connect_failures;
    }
    int64_t first_send = INT64_MAX, last_done = 0;

    /* ---- tally */
    int ok = 0, sold_out = 0, busy = 0, other = 0, no_answer = 0;
    int64_t *lat = calloc((size_t)o.buyers, sizeof *lat);
    int nlat = 0;
    for (int i = 0; i < o.buyers; i++) {
        buyer_t *b = &buyers[i];
        if (b->t_send && b->t_send < first_send)
            first_send = b->t_send;
        if (b->t_done > last_done)
            last_done = b->t_done;
        if (b->status == 201)
            ok++;
        else if (b->status == 409)
            sold_out++;
        else if (b->status == 503)
            busy++;
        else if (b->status == 0)
            no_answer++;
        else
            other++;
        if (b->status && b->t_done)
            lat[nlat++] = b->t_done - b->t_send;
    }
    qsort(lat, (size_t)nlat, sizeof *lat, cmp_i64);
    double p50 = nlat ? lat[nlat * 50 / 100] / 1000.0 : 0;
    double p95 = nlat ? lat[nlat * 95 / 100] / 1000.0 : 0;
    double p99 = nlat ? lat[nlat * 99 / 100] / 1000.0 : 0;
    double pmax = nlat ? lat[nlat - 1] / 1000.0 : 0;
    double wall_ms = (last_done > first_send ? last_done - first_send : of_now_us() - t0) / 1000.0;

    /* ---- ground truth from MySQL */
    int64_t sold = 0, on_hand = 0;
    char like[64];
    snprintf(like, sizeof like, "bench-%s-%%", run_id);
    db_query_int(db, &sold,
                 "SELECT COALESCE(SUM(oi.qty), 0) FROM order_items oi "
                 "JOIN orders o ON o.id = oi.order_id JOIN users u ON u.id = o.user_id "
                 "WHERE oi.product_id = ? AND u.email LIKE ?",
                 "is", o.product, like);
    db_query_int(db, &on_hand, "SELECT stock_on_hand FROM inventory WHERE product_id = ?", "i",
                 o.product);
    int64_t demand = (int64_t)o.buyers * o.qty;
    int64_t expect = demand < o.stock ? demand : (o.stock / o.qty) * o.qty;
    int64_t oversold = sold > o.stock ? sold - o.stock : 0;

    stock_table_t t;
    const stock_table_t *tp = NULL;
    if (strcmp(mode, "lockfree-shm") == 0 &&
        stock_attach(&t, of_config_get(&cfg, "core.shm_name", "/orderforge")) == 0)
        tp = &t;
    buf_t inv;
    buf_init(&inv);
    int64_t ledger_oversold = 0;
    int bad = store_check_invariants(db, tp, &inv, &ledger_oversold);

    printf("\n  results\n");
    printf("    orders accepted (201)    %d\n", ok);
    printf("    sold out (409)           %d\n", sold_out);
    printf("    busy (503)               %d\n", busy);
    printf("    other / no answer        %d / %d   (connect failures %d)\n", other, no_answer,
           connect_failures);
    printf("    units sold (MySQL)       %lld   expected %lld\n", (long long)sold,
           (long long)expect);
    printf("    stock_on_hand after      %lld\n", (long long)on_hand);
    printf("    OVERSOLD                 %lld\n", (long long)oversold);
    printf("    latency ms  p50 %.1f  p95 %.1f  p99 %.1f  max %.1f   wall %.0f ms\n", p50, p95,
           p99, pmax, wall_ms);
    printf("\n  invariants%s\n", tp ? " (MySQL + shared memory)" : " (MySQL)");
    if (inv.data) {
        /* indent the checker's report */
        for (char *line = strtok(inv.data, "\n"); line; line = strtok(NULL, "\n"))
            printf("    %s\n", line);
    }

    /* Correctness: never sell more than exists, and the ledgers must balance.
     * Liveness: sell everything, unless some buyers were turned away by backpressure
     * (503 busy) or got no answer, in which case selling less is legitimate. */
    bool shed = busy > 0 || no_answer > 0;
    bool sold_ok = sold == expect || (shed && sold < expect);
    bool pass = oversold == 0 && bad == 0 && sold_ok;
    printf("\n");
    if (pass && sold == expect)
        printf("  PASS: sold exactly the available stock, all invariants hold\n");
    else if (pass)
        printf("  PASS: nothing oversold, invariants hold; %lld of %lld units unsold because "
               "%d requests were shed (busy/no answer)\n",
               (long long)(expect - sold), (long long)expect, busy + no_answer);
    else if (oversold == 0 && sold_ok && bad > 0 && ledger_oversold > 0)
        printf("  FAIL: this run sold correctly, but the database already holds %lld oversold "
               "units from an earlier run; reset it with scripts/load-db.sh\n",
               (long long)ledger_oversold);
    else
        printf("  FAIL: see above\n");

    if (o.csv) {
        FILE *f = fopen(o.csv, "a");
        if (f) {
            fseek(f, 0, SEEK_END);
            if (ftell(f) == 0)
                fprintf(f, "mode,buyers,stock,qty,accepted,sold_out,busy,other,no_answer,"
                           "units_sold,oversold,p50_ms,p95_ms,p99_ms,max_ms,wall_ms,"
                           "invariant_failures\n");
            fprintf(f, "%s,%d,%d,%d,%d,%d,%d,%d,%d,%lld,%lld,%.2f,%.2f,%.2f,%.2f,%.1f,%d\n",
                    mode, o.buyers, o.stock, o.qty, ok, sold_out, busy, other, no_answer,
                    (long long)sold, (long long)oversold, p50, p95, p99, pmax, wall_ms, bad);
            fclose(f);
        }
    }

    buf_free(&inv);
    if (tp)
        stock_close(&t, false);
    db_close(db);
    free(lat);
    free(shards);
    free(th);
    free(buyers);
    freeaddrinfo(server);
    return pass ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *conf_path = "conf/orderforge.conf";
    int opt;
    while ((opt = getopt(argc, argv, "+c:h")) != -1) {
        if (opt == 'c')
            conf_path = optarg;
        else
            usage();
    }
    if (optind >= argc)
        usage();

    char err[256];
    if (of_config_load(&cfg, conf_path, err, sizeof err) != 0) {
        fprintf(stderr, "of-bench: %s\n", err);
        return 1;
    }
    of_log_open(NULL, "of-bench", OF_LOG_ERROR);

    if (strcmp(argv[optind], "oversell") == 0)
        return cmd_oversell(argc - optind - 1, argv + optind + 1);
    usage();
    return 2;
}
