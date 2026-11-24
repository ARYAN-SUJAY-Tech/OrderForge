/*
 * of-api - pre-forked FastCGI front end.
 *
 * Process model
 *   The parent opens the FastCGI listening socket once, then fork()s N workers that all
 *   block in accept() on that same inherited socket; the kernel hands each new Apache
 *   connection to exactly one of them. Each worker owns a private MySQL connection, so
 *   workers share nothing but the socket and the O_APPEND log file.
 *
 *   The parent does no request work. It blocks SIGCHLD/SIGTERM/SIGINT/SIGHUP and
 *   receives them synchronously with sigtimedwait(), which avoids async-signal-safety
 *   problems entirely:
 *     SIGCHLD  reap with waitpid(WNOHANG) and restart the worker after a backoff that
 *              doubles (100 ms .. 5 s) while it keeps dying young
 *     SIGHUP   reopen the log file and forward SIGHUP to the workers
 *     SIGTERM  forward to the workers, wait up to 5 s, then SIGKILL stragglers
 *
 *   A worker whose MySQL connection breaks finishes its current request and exits with
 *   status 3; the parent's restart gives it a fresh connection.
 *
 *   usage: of-api [-c conf/orderforge.conf]
 */
#include "common/config.h"
#include "common/log.h"
#include "common/util.h"
#include "store/store.h"

#include <errno.h>
#include <fcgiapp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_WORKERS     64
#define BACKOFF_MIN_MS  100
#define BACKOFF_MAX_MS  5000
#define STABLE_AFTER_US 10000000  /* a worker alive this long resets its backoff */
#define PATH_MAX_LEN    1024
#define EXIT_DB_BROKEN  3
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

/* ------------------------------------------------------------------ worker */

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

static void send_response(FCGX_Request *rq, const of_resp_t *resp)
{
    /* Headers are formatted with snprintf: libfcgi's own printf does not know %zu. */
    char head[512 + OF_COOKIE_MAX];
    int n = snprintf(head, sizeof head,
                     "Status: %d %s\r\n"
                     "Content-Type: application/json; charset=utf-8\r\n"
                     "Cache-Control: no-store\r\n"
                     "X-Content-Type-Options: nosniff\r\n"
                     "%s%s%s"
                     "Content-Length: %zu\r\n\r\n",
                     resp->status, of_status_text(resp->status),
                     resp->set_cookie[0] ? "Set-Cookie: " : "", resp->set_cookie,
                     resp->set_cookie[0] ? "\r\n" : "", resp->body.len);
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

static void serve_one(store_t *store, FCGX_Request *rq, of_resp_t *resp)
{
    static char body[16384 + 1];
    char path[PATH_MAX_LEN];
    char session[80] = "";
    int64_t t0 = of_now_us();

    const char *method_s = FCGX_GetParam("REQUEST_METHOD", rq->envp);
    const char *uri = FCGX_GetParam("REQUEST_URI", rq->envp);
    const char *query = FCGX_GetParam("QUERY_STRING", rq->envp);
    const char *clen_s = FCGX_GetParam("CONTENT_LENGTH", rq->envp);
    const char *csrf = FCGX_GetParam("HTTP_X_CSRF_TOKEN", rq->envp);
    const char *ip = FCGX_GetParam("REMOTE_ADDR", rq->envp);

    of_req_t req = {
        .method = parse_method(method_s),
        .path = path,
        .query = query ? query : "",
        .body = body,
        .body_len = 0,
        .session_token = session,
        .csrf_token = csrf ? csrf : "",
        .ip = ip ? ip : "",
    };

    of_resp_init(resp);
    resp->status = 0;

    /* Path: REQUEST_URI without its query string. */
    size_t plen = uri ? strcspn(uri, "?") : 0;
    if (!uri || plen == 0 || plen >= sizeof path) {
        resp->status = 400;
        buf_puts(&resp->body, "{\"error\":{\"code\":\"bad_request\",\"message\":\"bad path\"}}");
    } else {
        memcpy(path, uri, plen);
        path[plen] = '\0';
    }

    int64_t clen = 0;
    if (resp->status == 0 && clen_s && *clen_s) {
        if (of_parse_i64(clen_s, 0, INT64_MAX, &clen) != 0) {
            resp->status = 400;
            buf_puts(&resp->body,
                     "{\"error\":{\"code\":\"bad_request\",\"message\":\"bad Content-Length\"}}");
        } else if ((size_t)clen > sizeof body - 1) {
            resp->status = 413;
            buf_puts(&resp->body,
                     "{\"error\":{\"code\":\"too_large\",\"message\":\"request body too large\"}}");
        } else if (read_body(rq, body, (size_t)clen) != 0) {
            resp->status = 400;
            buf_puts(&resp->body,
                     "{\"error\":{\"code\":\"bad_request\",\"message\":\"truncated body\"}}");
        } else {
            req.body_len = (size_t)clen;
        }
    }
    if (req.body_len == 0)
        body[0] = '\0';

    if (resp->status == 0) {
        (void)of_cookie_get(FCGX_GetParam("HTTP_COOKIE", rq->envp), "of_session", session,
                            sizeof session);
        store_handle(store, &req, resp);
    }

    send_response(rq, resp);
    LOG_INFO("request", "%s %s %d %lldus", method_s ? method_s : "?", plen ? path : "?",
             resp->status, (long long)(of_now_us() - t0));
    of_resp_free(resp);
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

    static store_t store;
    char err[256];
    if (store_init(&store, &cfg, err, sizeof err) != 0) {
        LOG_ERROR("worker_init", "%s", err);
        _exit(EXIT_INIT);
    }
    LOG_INFO("worker_ready", "worker accepting requests");

    FCGX_Request rq;
    if (FCGX_InitRequest(&rq, listen_fd, 0) != 0) {
        LOG_ERROR("worker_init", "FCGX_InitRequest failed");
        _exit(EXIT_INIT);
    }

    of_resp_t resp;
    while (FCGX_Accept_r(&rq) >= 0) {
        serve_one(&store, &rq, &resp);
        FCGX_Finish_r(&rq);

        if (worker_hup) {
            worker_hup = 0;
            of_log_reopen();
        }
        if (db_broken(store.db)) {
            LOG_WARN("worker_exit", "database connection lost, exiting for a fresh one");
            store_close(&store);
            _exit(EXIT_DB_BROKEN);
        }
    }
    LOG_ERROR("worker_exit", "FCGX_Accept_r failed");
    store_close(&store);
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

static int open_log(const char *proc)
{
    const char *dir = of_config_get(&cfg, "log.dir", "run/log");
    int lvl = of_log_level_parse(of_config_get(&cfg, "log.level", "info"));
    if (lvl < 0)
        lvl = OF_LOG_INFO;
    if (strcmp(dir, "-") == 0)
        return of_log_open(NULL, proc, (of_log_level_t)lvl);

    char path[512];
    if (of_mkdir_p(dir, 0750) != 0) {
        fprintf(stderr, "of-api: mkdir %s: %s\n", dir, strerror(errno));
        return -1;
    }
    snprintf(path, sizeof path, "%s/%s.log", dir, proc);
    if (of_log_open(path, proc, (of_log_level_t)lvl) != 0) {
        fprintf(stderr, "of-api: open %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
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
    if (open_log("of-api") != 0)
        return 1;

    int ok;
    nworkers = (int)of_config_get_long(&cfg, "api.workers", 4, 1, MAX_WORKERS, &ok);
    if (!ok) {
        LOG_ERROR("config", "api.workers must be 1..%d", MAX_WORKERS);
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
