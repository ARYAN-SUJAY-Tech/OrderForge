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
#include "common/paymsg.h"
#include "common/stock.h"
#include "common/util.h"
#include "store/db.h"
#include "store/store.h"

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
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
            "                [--csv FILE]        (admin password: $OF_ADMIN_PASSWORD)\n"
            "       of-bench [-c config] load [--clients N] [--duration S] [--restock N]\n"
            "                [--method tok_success] [--url http://host:port] [--csv FILE]\n"
            "       of-bench [-c config] inject ORDER_ID   (forged/stale/replayed payment replies)\n");
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

/* Blocking request, thread-safe. idem may be NULL. 0 on any HTTP answer, -1 on a network
 * failure or a 15 s timeout. */
static int http_call_idem(const char *method, const char *path, const char *body,
                          const char *session, const char *csrf, const char *idem,
                          http_resp_t *r)
{
    int fd = socket(server->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct timeval tv = {15, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(fd, server->ai_addr, server->ai_addrlen) != 0) {
        close(fd);
        return -1;
    }
    char req[2048];
    int n = snprintf(req, sizeof req,
                     "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
                     "Content-Type: application/json\r\nContent-Length: %zu\r\n"
                     "%s%s%s%s%s%s%s%s%s\r\n%s",
                     method, path, o.host, body ? strlen(body) : 0,
                     session ? "Cookie: of_session=" : "", session ? session : "",
                     session ? "\r\n" : "", csrf ? "X-CSRF-Token: " : "", csrf ? csrf : "",
                     csrf ? "\r\n" : "", idem ? "Idempotency-Key: " : "", idem ? idem : "",
                     idem ? "\r\n" : "", body ? body : "");
    if (n < 0 || (size_t)n >= sizeof req || write(fd, req, (size_t)n) != n) {
        close(fd);
        return -1;
    }
    char raw[32 * 1024];
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

static int http_call(const char *method, const char *path, const char *body, const char *session,
                     const char *csrf, http_resp_t *r)
{
    return http_call_idem(method, path, body, session, csrf, NULL, r);
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

static int create_buyers(db_t *db, const char *run_id, bool with_cart)
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
        if (with_cart &&
            db_exec(db, "INSERT INTO cart_items (cart_id, product_id, qty) VALUES (?, ?, ?)", "iii",
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
    if (create_buyers(db, run_id, true) != 0) {
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
    int bad_orders = bad < 0 ? -1 : store_check_orders(db, &inv, 30);
    if (bad >= 0 && bad_orders != 0)
        bad += bad_orders < 0 ? 1 : bad_orders;

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

/* ------------------------------------------------------------------ sustained load */
/*
 * of-bench load: CLIENTS customers shop continuously for DURATION seconds (add a random
 * product, check out, repeat). Built for the chaos test: when of-core is killed, the
 * requests in flight fail (502/503 or a dropped connection) and the client retries the
 * same checkout with the SAME Idempotency-Key, so a checkout that actually committed
 * just before the crash is answered 200 with the existing order instead of being
 * placed twice. Every second's successes and failures are recorded, so the outage is
 * visible; at the end the run waits for payments to settle and checks every invariant.
 */
#define LOAD_MAX_SECONDS 3600
#define LOAD_MAX_PRODUCTS 512

static int64_t          load_products[LOAD_MAX_PRODUCTS];
static int              load_nproducts;
static int              load_duration_s = 20;
static const char      *load_method = "tok_success";
static int64_t          load_t0;
static _Atomic uint32_t sec_ok[LOAD_MAX_SECONDS], sec_fail[LOAD_MAX_SECONDS];
static _Atomic uint64_t n_created, n_replayed, n_soldout, n_failed_attempts, n_gave_up,
                        n_other, n_checkouts;

typedef struct {
    int      idx;
    int64_t *lat;
    size_t   nlat, cap;
} client_t;

static void tick(bool ok)
{
    int64_t sec = (of_now_us() - load_t0) / 1000000;
    if (sec >= 0 && sec < LOAD_MAX_SECONDS)
        atomic_fetch_add(ok ? &sec_ok[sec] : &sec_fail[sec], 1);
}

static void pause_ms(int ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static void *load_client(void *arg)
{
    client_t *cl = arg;
    buyer_t *b = &buyers[cl->idx];
    unsigned seed = (unsigned)(cl->idx * 7919 + 17);
    int64_t deadline = load_t0 + (int64_t)load_duration_s * 1000000;
    http_resp_t r;
    char body[96], path[64];

    while (of_now_us() < deadline) {
        int64_t pid = load_products[rand_r(&seed) % load_nproducts];
        snprintf(body, sizeof body, "{\"product_id\":%lld,\"qty\":1}", (long long)pid);
        if (http_call("POST", "/api/cart/items", body, b->token, b->csrf, &r) != 0 ||
            r.status != 200) {
            tick(false);
            pause_ms(200);
            continue;
        }
        tick(true);

        /* One checkout attempt = one Idempotency-Key, however many retries it takes. */
        unsigned char rnd[16];
        char key[33];
        of_random_bytes(rnd, sizeof rnd);
        of_hex(key, rnd, sizeof rnd);
        snprintf(body, sizeof body, "{\"payment_method\":\"%s\"}", load_method);
        atomic_fetch_add(&n_checkouts, 1);
        int64_t t0 = of_now_us();
        bool done = false;
        for (int attempt = 0; attempt < 40 && !done; attempt++) {
            int rc = http_call_idem("POST", "/api/checkout", body, b->token, b->csrf, key, &r);
            bool conflict = rc == 0 && r.status == 409 && strstr(r.body, "\"conflict\"");
            if (rc != 0 || r.status == 502 || r.status == 503 || conflict) {
                /* Engine restarting, or a MySQL deadlock ("please retry"): same key again. */
                tick(false);
                atomic_fetch_add(&n_failed_attempts, 1);
                pause_ms(200);
                continue;
            }
            tick(true);
            done = true;
            if (r.status == 201) {
                atomic_fetch_add(&n_created, 1);
            } else if (r.status == 200) {
                atomic_fetch_add(&n_replayed, 1); /* it had committed before the crash */
            } else if (r.status == 409) {
                atomic_fetch_add(&n_soldout, 1);
            } else {
                atomic_fetch_add(&n_other, 1);
            }
            if (r.status == 201 || r.status == 200) {
                if (cl->nlat == cl->cap) {
                    size_t cap = cl->cap ? cl->cap * 2 : 256;
                    int64_t *p = realloc(cl->lat, cap * sizeof *p);
                    if (p) {
                        cl->lat = p;
                        cl->cap = cap;
                    }
                }
                if (cl->nlat < cl->cap)
                    cl->lat[cl->nlat++] = of_now_us() - t0;
            }
        }
        if (!done)
            atomic_fetch_add(&n_gave_up, 1);
        /* Leave the cart empty for the next round, whatever happened. */
        snprintf(path, sizeof path, "/api/cart/items/%lld", (long long)pid);
        http_call("DELETE", path, NULL, b->token, b->csrf, &r);
    }
    return NULL;
}

static int cmd_load(int argc, char **argv)
{
    int clients = 50, restock = 1000;
    const char *csv = NULL;
    for (int i = 0; i + 1 < argc; i += 2) {
        int64_t n;
        if (strcmp(argv[i], "--clients") == 0 && of_parse_i64(argv[i + 1], 1, 2000, &n) == 0)
            clients = (int)n;
        else if (strcmp(argv[i], "--duration") == 0 &&
                 of_parse_i64(argv[i + 1], 1, LOAD_MAX_SECONDS - 60, &n) == 0)
            load_duration_s = (int)n;
        else if (strcmp(argv[i], "--restock") == 0 && of_parse_i64(argv[i + 1], 0, 1000000, &n) == 0)
            restock = (int)n;
        else if (strcmp(argv[i], "--method") == 0)
            load_method = argv[i + 1];
        else if (strcmp(argv[i], "--url") == 0 && parse_url(argv[i + 1]) == 0)
            ;
        else if (strcmp(argv[i], "--csv") == 0)
            csv = argv[i + 1];
        else
            usage();
    }
    if (argc % 2)
        usage();
    o.admin_password = getenv("OF_ADMIN_PASSWORD") ? getenv("OF_ADMIN_PASSWORD") : "admin12345";
    o.buyers = clients;
    if (sodium_init() < 0 || resolve() != 0)
        return 1;

    char err[256];
    db_t *db = db_connect(&cfg, err, sizeof err);
    if (!db) {
        fprintf(stderr, "of-bench: database: %s\n", err);
        return 1;
    }

    /* Admin session: mode, TTL, and restocking through the API (keeps shm exact). */
    http_resp_t r;
    char body[512], csrf[80], mode[32] = "?", ttl_s[16] = "120";
    snprintf(body, sizeof body, "{\"email\":\"%s\",\"password\":\"%s\"}", o.admin_email,
             o.admin_password);
    if (http_call("POST", "/api/auth/login", body, NULL, NULL, &r) != 0 || r.status != 200 ||
        json_find(r.body, "csrf_token", csrf, sizeof csrf) != 0) {
        fprintf(stderr, "of-bench: admin login failed (is the stack running?)\n");
        return 1;
    }
    char admin_cookie[160];
    snprintf(admin_cookie, sizeof admin_cookie, "%s", r.cookie);
    if (http_call("GET", "/api/admin/system", NULL, admin_cookie, NULL, &r) == 0 && r.status == 200) {
        json_find(r.body, "stock_mode", mode, sizeof mode);
        json_find(r.body, "reservation_ttl_s", ttl_s, sizeof ttl_s);
    }

    db_res_t res;
    if (db_query(db, &res,
                 "SELECT p.id, i.stock_on_hand FROM products p JOIN inventory i "
                 "ON i.product_id = p.id WHERE p.active = 1 ORDER BY p.id", "") != 0)
        return 1;
    while (load_nproducts < LOAD_MAX_PRODUCTS && db_fetch(&res) == 1) {
        int64_t id = db_int(&res, 0), have = db_int(&res, 1);
        load_products[load_nproducts++] = id;
        if (restock > 0 && have < restock) {
            char path[96];
            snprintf(path, sizeof path, "/api/admin/products/%lld/stock", (long long)id);
            snprintf(body, sizeof body, "{\"delta\":%lld,\"reason\":\"restock\"}",
                     (long long)(restock - have));
            http_call("POST", path, body, admin_cookie, csrf, &r);
        }
    }
    db_res_free(&res);
    if (load_nproducts == 0) {
        fprintf(stderr, "of-bench: no active products\n");
        return 1;
    }

    char run_id[24];
    snprintf(run_id, sizeof run_id, "L%lld", (long long)time(NULL));
    buyers = calloc((size_t)clients, sizeof *buyers);
    if (create_buyers(db, run_id, false) != 0) {
        fprintf(stderr, "of-bench: creating buyers: %s\n", db_error(db));
        return 1;
    }
    printf("of-bench load: mode %s, %d clients, %d s, %d products (restocked to %d), "
           "payment %s\n", mode, clients, load_duration_s, load_nproducts, restock, load_method);
    fflush(stdout);

    load_t0 = of_now_us();
    pthread_t *th = calloc((size_t)clients, sizeof *th);
    client_t *cl = calloc((size_t)clients, sizeof *cl);
    for (int i = 0; i < clients; i++) {
        cl[i].idx = i;
        pthread_create(&th[i], NULL, load_client, &cl[i]);
    }
    for (int i = 0; i < clients; i++)
        pthread_join(th[i], NULL);
    int seconds = (int)((of_now_us() - load_t0) / 1000000) + 1;
    if (seconds > LOAD_MAX_SECONDS)
        seconds = LOAD_MAX_SECONDS;

    /* Outage: the longest run of seconds with failures and no success. */
    int longest = 0, run = 0, out_start = -1, best_start = -1;
    for (int s2 = 0; s2 < seconds; s2++) {
        bool down = atomic_load(&sec_fail[s2]) > 0 && atomic_load(&sec_ok[s2]) == 0;
        if (down) {
            if (run == 0)
                out_start = s2;
            run++;
            if (run > longest) {
                longest = run;
                best_start = out_start;
            }
        } else {
            run = 0;
        }
    }

    size_t nlat = 0;
    for (int i = 0; i < clients; i++)
        nlat += cl[i].nlat;
    int64_t *lat = calloc(nlat ? nlat : 1, sizeof *lat);
    size_t k = 0;
    for (int i = 0; i < clients; i++)
        for (size_t j = 0; j < cl[i].nlat; j++)
            lat[k++] = cl[i].lat[j];
    qsort(lat, nlat, sizeof *lat, cmp_i64);

    printf("\n  results\n");
    printf("    checkouts                %llu  (%.1f per second)\n",
           (unsigned long long)atomic_load(&n_checkouts),
           (double)atomic_load(&n_checkouts) / load_duration_s);
    printf("    orders created (201)     %llu\n", (unsigned long long)atomic_load(&n_created));
    printf("    replayed by key (200)    %llu   (committed just before a crash; not duplicated)\n",
           (unsigned long long)atomic_load(&n_replayed));
    printf("    sold out (409)           %llu\n", (unsigned long long)atomic_load(&n_soldout));
    printf("    failed attempts retried  %llu   (502/503/connection errors during an outage)\n",
           (unsigned long long)atomic_load(&n_failed_attempts));
    printf("    gave up / other          %llu / %llu\n", (unsigned long long)atomic_load(&n_gave_up),
           (unsigned long long)atomic_load(&n_other));
    if (nlat)
        printf("    checkout latency ms      p50 %.1f  p95 %.1f  p99 %.1f  max %.1f\n",
               lat[nlat * 50 / 100] / 1000.0, lat[nlat * 95 / 100] / 1000.0,
               lat[nlat * 99 / 100] / 1000.0, lat[nlat - 1] / 1000.0);
    if (longest)
        printf("    longest outage           %d s (from second %d)\n", longest, best_start);
    else
        printf("    longest outage           none\n");
    printf("    timeline (ok/failed per second):");
    for (int s2 = 0; s2 < seconds; s2++)
        printf("%s%u/%u", s2 % 10 ? " " : "\n      ", atomic_load(&sec_ok[s2]),
               atomic_load(&sec_fail[s2]));
    printf("\n");

    /* Let payments settle: every bench order paid or cancelled (bounded by the TTL). */
    char like[64];
    snprintf(like, sizeof like, "bench-%s-%%", run_id);
    int64_t wait_until = of_now_us() + (atoll(ttl_s) + 15) * 1000000LL, pending = 0;
    printf("\n  waiting for payments to settle...");
    fflush(stdout);
    do {
        if (db_query_int(db, &pending,
                         "SELECT COUNT(*) FROM orders o JOIN users u ON u.id = o.user_id "
                         "WHERE u.email LIKE ? AND o.status IN ('placed', 'payment_pending')",
                         "s", like) < 0)
            break;
        if (pending)
            pause_ms(500);
    } while (pending && of_now_us() < wait_until);
    printf(" %s\n", pending ? "timed out" : "done");

    stock_table_t t;
    const stock_table_t *tp = NULL;
    if (strcmp(mode, "lockfree-shm") == 0 &&
        stock_attach(&t, of_config_get(&cfg, "core.shm_name", "/orderforge")) == 0)
        tp = &t;
    buf_t inv;
    buf_init(&inv);
    int64_t oversold = 0;
    int bad = store_check_invariants(db, tp, &inv, &oversold);
    int bad_orders = bad < 0 ? -1 : store_check_orders(db, &inv, 30);
    printf("\n  invariants%s\n", tp ? " (MySQL + shared memory + orders)" : " (MySQL + orders)");
    if (inv.data)
        for (char *line = strtok(inv.data, "\n"); line; line = strtok(NULL, "\n"))
            printf("    %s\n", line);
    bool pass = bad == 0 && bad_orders == 0 && oversold == 0;
    printf("\n  %s\n", pass ? "PASS: every invariant holds" : "FAIL: see above");

    if (csv) {
        FILE *f = fopen(csv, "a");
        if (f) {
            fseek(f, 0, SEEK_END);
            if (ftell(f) == 0)
                fprintf(f, "mode,clients,duration_s,checkouts,created,replayed,sold_out,"
                           "failed_attempts,gave_up,p50_ms,p99_ms,longest_outage_s,pass\n");
            fprintf(f, "%s,%d,%d,%llu,%llu,%llu,%llu,%llu,%llu,%.1f,%.1f,%d,%d\n", mode, clients,
                    load_duration_s, (unsigned long long)atomic_load(&n_checkouts),
                    (unsigned long long)atomic_load(&n_created),
                    (unsigned long long)atomic_load(&n_replayed),
                    (unsigned long long)atomic_load(&n_soldout),
                    (unsigned long long)atomic_load(&n_failed_attempts),
                    (unsigned long long)atomic_load(&n_gave_up),
                    nlat ? lat[nlat / 2] / 1000.0 : 0, nlat ? lat[nlat * 99 / 100] / 1000.0 : 0,
                    longest, pass);
            fclose(f);
        }
    }
    buf_free(&inv);
    if (tp)
        stock_close(&t, false);
    for (int i = 0; i < clients; i++)
        free(cl[i].lat);
    free(cl);
    free(th);
    free(lat);
    db_close(db);
    return pass ? 0 : 1;
}

/* ------------------------------------------------------------------ message injection */
/*
 * of-bench inject ORDER_ID: attack the payment channel directly, the way a hostile
 * process on the same machine could, by writing into of-core's reply queue:
 *   1. a forged approval, signed with a random key      -> must be rejected (bad MAC)
 *   2. a correctly signed approval stamped 10 minutes ago -> must be rejected (stale)
 *   3. a correctly signed approval                       -> accepted: the order is paid
 *   4. the very same message again                       -> must be rejected (replayed nonce)
 * The caller checks the order and the payments_rejected metric afterwards. Use an order
 * whose gateway never answers (tok_timeout) so only these messages can decide it.
 */
static int send_reply(mqd_t q, const pay_msg_t *m, const char *what)
{
    if (mq_send(q, (const char *)m, sizeof *m, 0) != 0) {
        fprintf(stderr, "of-bench: mq_send (%s): %s\n", what, strerror(errno));
        return -1;
    }
    printf("  sent %s\n", what);
    fflush(stdout);
    pause_ms(400);
    return 0;
}

static int cmd_inject(int argc, char **argv)
{
    int64_t order_id;
    if (argc != 1 || of_parse_i64(argv[0], 1, INT64_MAX, &order_id) != 0)
        usage();
    uint8_t key[PAY_KEY_LEN], bad[PAY_KEY_LEN];
    if (sodium_init() < 0 || pay_key_from_hex(of_config_get(&cfg, "pay.hmac_key", ""), key) != 0) {
        fprintf(stderr, "of-bench: pay.hmac_key missing or invalid\n");
        return 1;
    }
    char err[256];
    db_t *db = db_connect(&cfg, err, sizeof err);
    if (!db) {
        fprintf(stderr, "of-bench: database: %s\n", err);
        return 1;
    }
    db_res_t r;
    pay_msg_t m;
    memset(&m, 0, sizeof m);
    if (db_query(db, &r, "SELECT idem_key, amount_paise FROM payments WHERE order_id = ? "
                         "ORDER BY id DESC LIMIT 1", "i", order_id) != 0 || db_fetch(&r) != 1) {
        fprintf(stderr, "of-bench: order %lld has no payment\n", (long long)order_id);
        return 1;
    }
    snprintf(m.idem_key, sizeof m.idem_key, "%s", db_str(&r, 0));
    m.amount_paise = db_int(&r, 1);
    db_res_free(&r);
    db_close(db);

    m.kind = PAY_REPLY;
    m.outcome = PAY_SUCCEEDED;
    m.order_id = (uint64_t)order_id;
    snprintf(m.method, sizeof m.method, "tok_injected");
    mqd_t q = mq_open(of_config_get(&cfg, "pay.reply_queue", "/orderforge-pay-rep"),
                      O_WRONLY | O_CLOEXEC);
    if (q == (mqd_t)-1) {
        fprintf(stderr, "of-bench: mq_open: %s\n", strerror(errno));
        return 1;
    }
    printf("of-bench inject: order %lld, payment %s\n", (long long)order_id, m.idem_key);

    pay_msg_t forged = m;
    randombytes_buf(bad, sizeof bad);
    snprintf(forged.provider_ref, sizeof forged.provider_ref, "inject_forged");
    pay_sign(&forged, bad);
    int rc = send_reply(q, &forged, "1. forged approval (wrong key)");

    pay_msg_t stale = m;
    snprintf(stale.provider_ref, sizeof stale.provider_ref, "inject_stale");
    pay_sign(&stale, key);
    stale.ts_ms -= 10 * 60 * 1000; /* then re-MAC, so only the timestamp is wrong */
    crypto_auth_hmacsha256(stale.mac, (const unsigned char *)&stale, offsetof(pay_msg_t, mac),
                           key);
    rc |= send_reply(q, &stale, "2. signed approval, 10 minutes old");

    pay_msg_t valid = m;
    snprintf(valid.provider_ref, sizeof valid.provider_ref, "inject_valid");
    pay_sign(&valid, key);
    rc |= send_reply(q, &valid, "3. genuine signed approval");
    rc |= send_reply(q, &valid, "4. the same message again (replay)");
    mq_close(q);
    return rc ? 1 : 0;
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
    if (strcmp(argv[optind], "load") == 0)
        return cmd_load(argc - optind - 1, argv + optind + 1);
    if (strcmp(argv[optind], "inject") == 0)
        return cmd_inject(argc - optind - 1, argv + optind + 1);
    usage();
    return 2;
}
