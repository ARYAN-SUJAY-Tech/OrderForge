/*
 * of-api - pre-forked FastCGI front end.
 *
 * of-api holds no business logic and no database connection. Each worker turns one
 * FastCGI request into one binary frame, sends it to of-core over a Unix domain socket,
 * waits for the response frame, and turns that back into an HTTP response. All
 * authentication, authorisation and stock logic run inside of-core, so a bug here can
 * at worst produce a malformed request that the core rejects.
 *
 * Process model
 *   The parent opens the FastCGI listening socket once, then fork()s N workers that all
 *   block in accept() on that same inherited socket; the kernel hands each new Apache
 *   connection to exactly one of them. Each worker keeps one persistent connection to
 *   of-core, so many workers mean many requests in flight at the core.
 *
 *   The parent does no request work. It blocks SIGCHLD/SIGTERM/SIGINT/SIGHUP and
 *   receives them synchronously with sigtimedwait(), which avoids async-signal-safety
 *   problems entirely:
 *     SIGCHLD  reap with waitpid(WNOHANG) and restart the worker after a backoff that
 *              doubles (100 ms .. 5 s) while it keeps dying young
 *     SIGHUP   reopen the log file and forward SIGHUP to the workers
 *     SIGTERM  forward to the workers, wait up to 5 s, then SIGKILL stragglers
 *
 *   usage: of-api [-c conf/orderforge.conf]
 */
#include "common/buf.h"
#include "common/config.h"
#include "common/harden.h"
#include "common/log.h"
#include "common/metrics.h"
#include "common/proto.h"
#include "common/ratelimit.h"
#include "common/stock.h"
#include "common/util.h"

#include <errno.h>
#include <fcgiapp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_WORKERS     256
#define BACKOFF_MIN_MS  100
#define BACKOFF_MAX_MS  5000
#define STABLE_AFTER_US 10000000  /* a worker alive this long resets its backoff */
#define EXIT_INIT       4

typedef struct {
    pid_t   pid;          /* 0 when not running */
    int64_t started_us;
    int64_t restart_at;   /* monotonic us; 0 when no restart is pending */
    int     backoff_ms;
} worker_t;

static of_config_t cfg;
static int         listen_fd = -1;
static worker_t    workers[MAX_WORKERS];
static int         nworkers;

/* ------------------------------------------------------------------ worker: core client */

typedef struct {
    int         status;
    char        set_cookie[OF_MAX_COOKIE + 1];
    const char *content_type; /* NULL: JSON */
    int         retry_after_s; /* > 0: send a Retry-After header (429) */
    buf_t       body;
} api_resp_t;

/* ------------------------------------------------------------------ edge protection */

/* Shared-memory token buckets, created by the parent before fork (workers inherit). */
static rl_table_t rl;
static bool       rl_enabled;
static rl_rule_t  rule_ip, rule_session, rule_auth;

static int      core_fd = -1;
static char     core_path[108];
static int      core_timeout_s;
static uint64_t next_request_id;

static void core_disconnect(void)
{
    if (core_fd >= 0)
        close(core_fd);
    core_fd = -1;
}

static int core_connect(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    memcpy(addr.sun_path, core_path, strlen(core_path) + 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    /* A hung core must not hang Apache forever. */
    struct timeval tv = {core_timeout_s, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    core_fd = fd;
    return 0;
}

/* The core may have restarted since our last request: an idle connection that now
 * reads as EOF is stale. Check without blocking before reusing it. */
static void core_drop_if_stale(void)
{
    if (core_fd < 0)
        return;
    char c;
    ssize_t n = recv(core_fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) || n > 0)
        core_disconnect(); /* EOF, error, or unexpected unsolicited bytes */
}

static int write_all(int fd, const void *p, size_t len)
{
    const char *s = p;
    while (len > 0) {
        ssize_t n = send(fd, s, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        s += n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_all(int fd, void *p, size_t len)
{
    char *s = p;
    while (len > 0) {
        ssize_t n = read(fd, s, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1; /* EOF, error or SO_RCVTIMEO timeout */
        s += n;
        len -= (size_t)n;
    }
    return 0;
}

static void api_error(api_resp_t *r, int status, const char *code, const char *msg)
{
    r->status = status;
    buf_reset(&r->body);
    buf_puts(&r->body, "{\"error\":{");
    buf_json_kv_str(&r->body, "code", code);
    buf_puts(&r->body, ",");
    buf_json_kv_str(&r->body, "message", msg);
    buf_puts(&r->body, "}}");
}

/* Send one request to of-core and wait for its response. Always fills r. */
static void core_call(const of_req_t *req, api_resp_t *r)
{
    static buf_t frame;
    static char *payload, *arena;
    static size_t payload_cap;

    buf_reset(&frame);
    uint64_t rid = ++next_request_id;
    if (proto_encode_request(&frame, rid, req) != 0) {
        api_error(r, 413, "too_large", "request too large");
        return;
    }

    /* Send. A send failure means the core never got the request, so one retry on a fresh
     * connection is safe. Once it is sent we never retry: the core may have acted on it. */
    core_drop_if_stale();
    for (int attempt = 0;; attempt++) {
        if (core_fd < 0 && core_connect() != 0) {
            LOG_WARN("core_unavailable", "connect %s: %s", core_path, strerror(errno));
            api_error(r, 503, "core_unavailable", "the store engine is not running");
            return;
        }
        if (write_all(core_fd, frame.data, frame.len) == 0)
            break;
        core_disconnect();
        if (attempt == 1) {
            api_error(r, 503, "core_unavailable", "the store engine is not reachable");
            return;
        }
    }

    of_frame_hdr_t h;
    if (read_all(core_fd, &h, sizeof h) != 0 || proto_check_header(&h, OF_MSG_RESPONSE) != 0 ||
        h.request_id != rid) {
        LOG_WARN("core_error", "no valid response header for request %llu",
                 (unsigned long long)rid);
        core_disconnect();
        api_error(r, 502, "core_error", "the store engine did not answer");
        return;
    }
    if (h.length > payload_cap) {
        char *p = realloc(payload, h.length);
        char *a = realloc(arena, (size_t)h.length + 8);
        if (!p || !a) {
            free(p ? p : payload);
            free(a ? a : arena);
            payload = arena = NULL;
            payload_cap = 0;
            core_disconnect();
            api_error(r, 502, "core_error", "out of memory");
            return;
        }
        payload = p;
        arena = a;
        payload_cap = h.length;
    }
    int status;
    const char *cookie, *body;
    size_t body_len;
    if (read_all(core_fd, payload, h.length) != 0 ||
        proto_decode_response(payload, h.length, &status, &cookie, &body, &body_len, arena,
                              (size_t)h.length + 8) != 0) {
        core_disconnect();
        api_error(r, 502, "core_error", "malformed response from the store engine");
        return;
    }
    r->status = status;
    snprintf(r->set_cookie, sizeof r->set_cookie, "%s", cookie);
    buf_reset(&r->body);
    buf_append(&r->body, body, body_len);
}

/* ------------------------------------------------------------------ worker: FastCGI side */

static volatile sig_atomic_t worker_hup;

static void on_worker_hup(int sig)
{
    (void)sig;
    worker_hup = 1;
}

static of_method_t parse_method(const char *m)
{
    if (!m)
        return OF_METHOD_OTHER;
    if (strcmp(m, "GET") == 0 || strcmp(m, "HEAD") == 0)
        return OF_GET;
    if (strcmp(m, "POST") == 0)
        return OF_POST;
    if (strcmp(m, "PUT") == 0)
        return OF_PUT;
    if (strcmp(m, "DELETE") == 0)
        return OF_DELETE;
    return OF_METHOD_OTHER;
}

static void send_response(FCGX_Request *rq, const api_resp_t *resp)
{
    /* Headers are formatted with snprintf: libfcgi's own printf does not know %zu. */
    char retry[48] = "";
    if (resp->retry_after_s > 0)
        snprintf(retry, sizeof retry, "Retry-After: %d\r\n", resp->retry_after_s);
    char head[512 + OF_MAX_COOKIE];
    int n = snprintf(head, sizeof head,
                     "Status: %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Cache-Control: no-store\r\n"
                     "X-Content-Type-Options: nosniff\r\n"
                     "%s%s%s%s"
                     "Content-Length: %zu\r\n\r\n",
                     resp->status, of_status_text(resp->status),
                     resp->content_type ? resp->content_type : "application/json; charset=utf-8",
                     resp->set_cookie[0] ? "Set-Cookie: " : "", resp->set_cookie,
                     resp->set_cookie[0] ? "\r\n" : "", retry, resp->body.len);
    if (n < 0 || (size_t)n >= sizeof head)
        return;
    FCGX_PutStr(head, n, rq->out);
    if (resp->body.len)
        FCGX_PutStr(resp->body.data, (int)resp->body.len, rq->out);
}

/* Read exactly len body bytes into buf. 0 or -1. */
static int read_body(FCGX_Request *rq, char *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int n = FCGX_GetStr(buf + got, (int)(len - got), rq->in);
        if (n <= 0)
            return -1;
        got += (size_t)n;
    }
    buf[len] = '\0';
    return 0;
}

/*
 * GET /api/metrics: Prometheus text format, rendered straight from of-core's shared
 * memory (mapped read-only per scrape), so it works even while the core is busy.
 * Only clients on this host may scrape.
 */
static void serve_metrics(const char *ip, api_resp_t *resp)
{
    if (!ip || (strcmp(ip, "127.0.0.1") != 0 && strcmp(ip, "::1") != 0)) {
        api_error(resp, 403, "forbidden", "metrics are only served to localhost");
        return;
    }
    stock_table_t t;
    if (stock_attach(&t, of_config_get(&cfg, "core.shm_name", "/orderforge")) != 0) {
        resp->status = 503;
        resp->content_type = "text/plain; version=0.0.4";
        buf_reset(&resp->body);
        buf_puts(&resp->body, "# of-core shared memory not available\norderforge_core_up 0\n");
        return;
    }
    resp->status = 200;
    resp->content_type = "text/plain; version=0.0.4";
    buf_reset(&resp->body);
    metrics_format(&t, &resp->body);
    stock_close(&t, false);
    if (rl_enabled)
        buf_printf(&resp->body,
                   "# HELP orderforge_ratelimit_allowed_total Requests within the rate limits.\n"
                   "# TYPE orderforge_ratelimit_allowed_total counter\n"
                   "orderforge_ratelimit_allowed_total %llu\n"
                   "# HELP orderforge_ratelimit_limited_total Requests refused with 429.\n"
                   "# TYPE orderforge_ratelimit_limited_total counter\n"
                   "orderforge_ratelimit_limited_total %llu\n"
                   "# HELP orderforge_ratelimit_table_full_total Requests allowed because the "
                   "bucket table was full.\n"
                   "# TYPE orderforge_ratelimit_table_full_total counter\n"
                   "orderforge_ratelimit_table_full_total %llu\n",
                   (unsigned long long)atomic_load(&rl.shm->allowed),
                   (unsigned long long)atomic_load(&rl.shm->limited),
                   (unsigned long long)atomic_load(&rl.shm->table_full));
}

/*
 * Checks made at the edge, before a request reaches of-core (cheapest first):
 *  1. Rate limits (token buckets in shared memory, shared by every worker process):
 *     per client IP, per session, and a strict one for login/registration per IP
 *     against password guessing. Over the limit: 429 with Retry-After.
 *  2. Cross-site requests: a state-changing request whose Origin header names another
 *     site is refused (403). Browsers always send Origin on cross-site POSTs.
 *  3. Request bodies must be JSON. A cross-site HTML form can only send
 *     application/x-www-form-urlencoded, multipart/form-data or text/plain; with
 *     enctype=text/plain an attacker can even shape the body into valid JSON
 *     ({"email":"..","password":"..","x":"=.."}) to log a victim into the attacker's
 *     account (login CSRF). Requiring application/json closes that: a page from another
 *     site cannot send it without a CORS preflight, which we never grant.
 * Returns 0 to continue, or fills resp and returns -1.
 */
static int edge_checks(FCGX_Request *rq, const of_req_t *req, const char *path, int64_t clen,
                       const char *session, api_resp_t *resp)
{
    if (rl_enabled) {
        char key[OF_MAX_TOKEN + 16];
        int64_t now = rl_now_ms(), wait = 0;
        snprintf(key, sizeof key, "ip:%s", req->ip);
        wait = rl_take(&rl, key, &rule_ip, now);
        if (!wait && session[0]) {
            snprintf(key, sizeof key, "s:%.32s", session);
            wait = rl_take(&rl, key, &rule_session, now);
        }
        if (!wait && req->method == OF_POST &&
            (strcmp(path, "/api/auth/login") == 0 || strcmp(path, "/api/auth/register") == 0)) {
            snprintf(key, sizeof key, "auth:%s", req->ip);
            wait = rl_take(&rl, key, &rule_auth, now);
        }
        if (wait) {
            api_error(resp, 429, "rate_limited", "too many requests, slow down");
            resp->retry_after_s = (int)((wait + 999) / 1000);
            LOG_INFO("rate_limited", "%s %s from %s", req->path, session[0] ? "(session)" : "",
                     req->ip);
            return -1;
        }
    }

    if (req->method != OF_GET) {
        const char *origin = FCGX_GetParam("HTTP_ORIGIN", rq->envp);
        const char *host = FCGX_GetParam("HTTP_HOST", rq->envp);
        if (origin) {
            const char *o = strstr(origin, "://");
            o = o ? o + 3 : origin;
            if (strcmp(origin, "null") == 0 || !host || strcmp(o, host) != 0) {
                api_error(resp, 403, "cross_origin", "cross-site requests are not allowed");
                LOG_WARN("cross_origin", "%s from Origin %s refused (Host %s)", path, origin,
                         host ? host : "-");
                return -1;
            }
        }
        if (clen > 0) {
            const char *ct = FCGX_GetParam("CONTENT_TYPE", rq->envp);
            if (!ct || strncasecmp(ct, "application/json", 16) != 0) {
                api_error(resp, 415, "unsupported_media_type",
                          "request bodies must be application/json");
                return -1;
            }
        }
    }
    return 0;
}

static void serve_one(FCGX_Request *rq, api_resp_t *resp)
{
    static char body[OF_MAX_BODY + 1];
    char path[OF_MAX_PATH + 1];
    char session[OF_MAX_TOKEN + 1] = "";
    int64_t t0 = of_now_us();

    const char *method_s = FCGX_GetParam("REQUEST_METHOD", rq->envp);
    const char *uri = FCGX_GetParam("REQUEST_URI", rq->envp);
    const char *query = FCGX_GetParam("QUERY_STRING", rq->envp);
    const char *clen_s = FCGX_GetParam("CONTENT_LENGTH", rq->envp);
    const char *csrf = FCGX_GetParam("HTTP_X_CSRF_TOKEN", rq->envp);
    const char *ip = FCGX_GetParam("REMOTE_ADDR", rq->envp);
    const char *idem = FCGX_GetParam("HTTP_IDEMPOTENCY_KEY", rq->envp);

    of_req_t req = {
        .method = parse_method(method_s),
        .path = path,
        .query = query ? query : "",
        .body = body,
        .body_len = 0,
        .session_token = session,
        .csrf_token = csrf ? csrf : "",
        .ip = ip ? ip : "",
        .idem_key = idem ? idem : "",
    };

    resp->status = 0;
    resp->set_cookie[0] = '\0';
    resp->content_type = NULL;
    resp->retry_after_s = 0;
    buf_reset(&resp->body);

    /* Validate at the edge what the protocol caps anyway, with friendlier errors. */
    size_t plen = uri ? strcspn(uri, "?") : 0;
    if (!uri || plen == 0 || plen >= sizeof path) {
        api_error(resp, 400, "bad_request", "bad path");
    } else {
        memcpy(path, uri, plen);
        path[plen] = '\0';
    }
    if (resp->status == 0 &&
        (strlen(req.query) > OF_MAX_QUERY || strlen(req.csrf_token) > OF_MAX_TOKEN ||
         strlen(req.idem_key) > OF_MAX_IDEM))
        api_error(resp, 400, "bad_request", "query string or header too long");

    int64_t clen = 0;
    if (resp->status == 0 && clen_s && *clen_s && of_parse_i64(clen_s, 0, INT64_MAX, &clen) != 0)
        api_error(resp, 400, "bad_request", "bad Content-Length");
    if (resp->status == 0) {
        (void)of_cookie_get(FCGX_GetParam("HTTP_COOKIE", rq->envp), "of_session", session,
                            sizeof session);
        (void)edge_checks(rq, &req, path, clen, session, resp);
    }
    if (resp->status == 0 && clen > 0) {
        if ((size_t)clen > sizeof body - 1)
            api_error(resp, 413, "too_large", "request body too large");
        else if (read_body(rq, body, (size_t)clen) != 0)
            api_error(resp, 400, "bad_request", "truncated body");
        else
            req.body_len = (size_t)clen;
    }
    if (req.body_len == 0)
        body[0] = '\0';

    if (resp->status == 0 && req.method == OF_GET && strcmp(path, "/api/metrics") == 0) {
        serve_metrics(req.ip, resp);
    } else if (resp->status == 0) {
        core_call(&req, resp);
    }

    send_response(rq, resp);
    LOG_DEBUG("request", "%s %s %d %lldus", method_s ? method_s : "?", plen ? path : "?",
              resp->status, (long long)(of_now_us() - t0));
}

static void worker_main(void)
{
    /* Undo the parent's signal setup: workers die on SIGTERM (default action), use a
     * flag for SIGHUP, and ignore SIGPIPE so a client hang-up is just a write error. */
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_worker_hup;
    sa.sa_flags = SA_RESTART; /* accept() resumes instead of failing with EINTR */
    sigaction(SIGHUP, &sa, NULL);

    /* Connect lazily: the core may start after us; a missing core just means 503s. */
    if (core_connect() != 0)
        LOG_WARN("core_unavailable", "of-core not reachable at %s yet", core_path);

    FCGX_Request rq;
    if (FCGX_InitRequest(&rq, listen_fd, 0) != 0) {
        LOG_ERROR("worker_init", "FCGX_InitRequest failed");
        _exit(EXIT_INIT);
    }
    LOG_DEBUG("worker_ready", "worker accepting requests");

    api_resp_t resp;
    buf_init(&resp.body);
    while (FCGX_Accept_r(&rq) >= 0) {
        serve_one(&rq, &resp);
        FCGX_Finish_r(&rq);
        if (worker_hup) {
            worker_hup = 0;
            of_log_reopen();
        }
    }
    LOG_ERROR("worker_exit", "FCGX_Accept_r failed");
    _exit(1);
}

/* ------------------------------------------------------------------ parent */

static int spawn(worker_t *w)
{
    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("fork", "%s", strerror(errno));
        w->pid = 0;
        w->restart_at = of_now_us() + (int64_t)w->backoff_ms * 1000; /* try again later */
        return -1;
    }
    if (pid == 0)
        worker_main(); /* never returns */
    w->pid = pid;
    w->started_us = of_now_us();
    w->restart_at = 0;
    return 0;
}

static void describe_exit(int status, char *out, size_t outlen)
{
    if (WIFEXITED(status))
        snprintf(out, outlen, "exited with status %d", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        snprintf(out, outlen, "killed by signal %d (%s)%s", WTERMSIG(status),
                 strsignal(WTERMSIG(status)), WCOREDUMP(status) ? ", core dumped" : "");
    else
        snprintf(out, outlen, "status 0x%x", status);
}

static void reap(void)
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < nworkers; i++) {
            worker_t *w = &workers[i];
            if (w->pid != pid)
                continue;
            char why[96];
            describe_exit(status, why, sizeof why);
            int64_t lived = of_now_us() - w->started_us;
            if (lived > STABLE_AFTER_US)
                w->backoff_ms = BACKOFF_MIN_MS;
            LOG_WARN("worker_died", "worker %d %s after %lld ms; restart in %d ms", (int)pid, why,
                     (long long)(lived / 1000), w->backoff_ms);
            w->pid = 0;
            w->restart_at = of_now_us() + (int64_t)w->backoff_ms * 1000;
            w->backoff_ms = w->backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : w->backoff_ms * 2;
        }
    }
}

static void signal_workers(int sig)
{
    for (int i = 0; i < nworkers; i++)
        if (workers[i].pid > 0)
            kill(workers[i].pid, sig);
}

static void shutdown_workers(void)
{
    signal_workers(SIGTERM);
    int64_t deadline = of_now_us() + 5000000;
    for (;;) {
        int alive = 0;
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
            for (int i = 0; i < nworkers; i++)
                if (workers[i].pid == pid)
                    workers[i].pid = 0;
        for (int i = 0; i < nworkers; i++)
            alive += workers[i].pid > 0;
        if (alive == 0)
            return;
        if (of_now_us() > deadline) {
            LOG_WARN("shutdown", "%d workers did not exit, sending SIGKILL", alive);
            signal_workers(SIGKILL);
            deadline = INT64_MAX;
        }
        struct timespec ts = {0, 50 * 1000000};
        nanosleep(&ts, NULL);
    }
}

static void usage(void)
{
    fprintf(stderr, "usage: of-api [-c config]\n");
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
        fprintf(stderr, "of-api: %s\n", err);
        return 1;
    }
    /* Before opening any file: leave root if started as root, then harden. */
    if (of_drop_privileges(&cfg, "of-api", err, sizeof err) < 0) {
        fprintf(stderr, "of-api: %s\n", err);
        return 1;
    }
    of_harden(conf_path, &cfg, "of-api");
    if (of_log_open_config(&cfg, "of-api") != 0)
        return 1;
    char pid_path[512];
    pid_t holder = 0;
    of_pidfile_path(&cfg, "of-api", pid_path, sizeof pid_path);
    if (of_pidfile_acquire(pid_path, &holder) < 0) {
        fprintf(stderr, "of-api: already running (pid %d holds %s)\n", (int)holder, pid_path);
        return 1;
    }

    int ok, ok2;
    nworkers = (int)of_config_get_long(&cfg, "api.workers", 16, 1, MAX_WORKERS, &ok);
    core_timeout_s = (int)of_config_get_long(&cfg, "api.core_timeout_s", 30, 1, 600, &ok2);
    if (!ok || !ok2) {
        LOG_ERROR("config", "api.workers must be 1..%d, api.core_timeout_s 1..600", MAX_WORKERS);
        return 1;
    }
    const char *cpath = of_config_get(&cfg, "core.socket", "run/of-core.sock");
    if (strlen(cpath) >= sizeof core_path) {
        LOG_ERROR("config", "core.socket path too long");
        return 1;
    }
    snprintf(core_path, sizeof core_path, "%s", cpath);

    /* Rate limits. Rates are per second except the auth rule (per minute). */
    int r[7];
    rl_enabled = of_config_get_long(&cfg, "api.rate_limit", 1, 0, 1, &r[0]) == 1;
    rule_ip.rate_milli_per_s =
        (uint32_t)of_config_get_long(&cfg, "api.rate_ip_per_s", 500, 1, 1000000, &r[1]) * 1000;
    rule_ip.burst = (uint32_t)of_config_get_long(&cfg, "api.rate_ip_burst", 2000, 1, RL_MAX_BURST, &r[2]);
    rule_session.rate_milli_per_s =
        (uint32_t)of_config_get_long(&cfg, "api.rate_session_per_s", 20, 1, 1000000, &r[3]) * 1000;
    rule_session.burst =
        (uint32_t)of_config_get_long(&cfg, "api.rate_session_burst", 40, 1, RL_MAX_BURST, &r[4]);
    rule_auth.rate_milli_per_s =
        (uint32_t)(of_config_get_long(&cfg, "api.rate_auth_per_min", 30, 1, 60000, &r[5]) * 1000 / 60);
    rule_auth.burst = (uint32_t)of_config_get_long(&cfg, "api.rate_auth_burst", 30, 1, RL_MAX_BURST, &r[6]);
    for (int i = 0; i < 7; i++)
        if (!r[i]) {
            fprintf(stderr, "of-api: an api.rate_* setting is out of range\n");
            return 1;
        }
    if (rl_enabled &&
        rl_create(&rl, of_config_get(&cfg, "api.rate_shm_name", "/orderforge-rl"), 65536) != 0) {
        fprintf(stderr, "of-api: cannot create the rate-limit table: %s\n", strerror(errno));
        return 1;
    }
    const char *bind = of_config_get(&cfg, "api.listen", "127.0.0.1:9000");

    if (FCGX_Init() != 0) {
        LOG_ERROR("init", "FCGX_Init failed");
        return 1;
    }
    listen_fd = FCGX_OpenSocket(bind, 1024);
    if (listen_fd < 0) {
        LOG_ERROR("listen", "cannot listen on %s", bind);
        fprintf(stderr, "of-api: cannot listen on %s\n", bind);
        return 1;
    }

    /* Block the signals we handle before forking, so none is lost in between; the
     * parent then takes them one at a time with sigtimedwait(). */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGHUP);
    sigprocmask(SIG_BLOCK, &set, NULL);
    signal(SIGPIPE, SIG_IGN);

    for (int i = 0; i < nworkers; i++) {
        workers[i].backoff_ms = BACKOFF_MIN_MS;
        if (spawn(&workers[i]) != 0)
            return 1;
    }
    LOG_INFO("start", "of-api pid %d listening on %s with %d workers", (int)getpid(), bind,
             nworkers);
    fprintf(stderr, "of-api: listening on %s with %d workers (pid %d)\n", bind, nworkers,
            (int)getpid());

    for (;;) {
        /* Sleep until a signal arrives or the earliest pending restart is due. */
        int64_t now = of_now_us(), next = now + 1000000;
        for (int i = 0; i < nworkers; i++)
            if (workers[i].pid == 0 && workers[i].restart_at && workers[i].restart_at < next)
                next = workers[i].restart_at;
        int64_t wait_us = next > now ? next - now : 0;
        struct timespec ts = {wait_us / 1000000, (wait_us % 1000000) * 1000};

        siginfo_t si;
        int sig = sigtimedwait(&set, &si, &ts);
        if (sig == SIGTERM || sig == SIGINT) {
            LOG_INFO("shutdown", "received %s, stopping workers", strsignal(sig));
            shutdown_workers();
            if (rl_enabled)
                rl_close(&rl, true);
            LOG_INFO("shutdown", "of-api stopped");
            return 0;
        }
        if (sig == SIGHUP) {
            of_log_reopen();
            signal_workers(SIGHUP);
            LOG_INFO("reload", "log reopened");
        }
        if (sig == SIGCHLD)
            reap();

        now = of_now_us();
        for (int i = 0; i < nworkers; i++) {
            worker_t *w = &workers[i];
            if (w->pid == 0 && w->restart_at && w->restart_at <= now)
                (void)spawn(w);
        }
    }
}
