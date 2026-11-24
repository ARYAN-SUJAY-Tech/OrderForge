/*
 * of-supervisor - parent of every OrderForge daemon.
 *
 *   usage: of-supervisor [-c conf/orderforge.conf] [-m naive|db-lock|lockfree-shm]
 *
 * Start-up: of-core first (it creates the socket, shared memory and payment queues and
 * may need to recover from a crash), and only once its socket accepts connections the
 * others: of-pay, of-report, of-api. The binaries are taken from the directory of this
 * executable, so a debug supervisor runs debug daemons.
 *
 * Each child gets its own process group (setpgid). A Ctrl-C in the terminal therefore
 * reaches only the supervisor, which then stops the children in a controlled order;
 * and a stuck child can be killed together with its own children (kill(-pgid)).
 *
 * Events arrive on one epoll loop: a signalfd (SIGCHLD, SIGTERM, SIGINT, SIGHUP) and a
 * timerfd armed for the next scheduled restart.
 *   SIGCHLD  reap with waitpid(WNOHANG); a child that exits unexpectedly is restarted
 *            after a backoff (100 ms doubling to 5 s, reset after 10 s of uptime). More
 *            than supervisor.max_restarts crashes within supervisor.window_s marks the
 *            child failed and the system "degraded": restarting stops.
 *   SIGHUP   forwarded to every child (each reopens its log and reloads what it can);
 *            failed children get a fresh chance.
 *   SIGTERM  ordered drain: of-api, of-report, of-pay, then of-core (which finishes its
 *   SIGINT   in-flight requests); a child that does not exit in time gets SIGKILL.
 *
 * A pid file locked with flock() prevents two supervisors. The current state is written
 * to <run.dir>/supervisor.status (atomically, with rename) for `of-ctl status`.
 */
#include "common/config.h"
#include "common/harden.h"
#include "common/log.h"
#include "common/util.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NCHILD          4
#define BACKOFF_MIN_MS  100
#define BACKOFF_MAX_MS  5000
#define STABLE_AFTER_US 10000000

typedef enum { ST_STOPPED, ST_RUNNING, ST_BACKOFF, ST_FAILED } state_t;
static const char *const state_names[] = {"stopped", "running", "backoff", "failed"};

typedef struct {
    const char *name;
    pid_t       pid;
    state_t     state;
    int64_t     started_us;
    int64_t     started_unix;
    int64_t     restart_at_us;
    int         backoff_ms;
    int         restarts;           /* total */
    int64_t     recent[32];         /* times of recent crashes (us) */
    int         nrecent;
    char        last_exit[64];
} child_t;

/* Start order; shutdown runs it backwards. */
static child_t children[NCHILD] = {
    {.name = "of-core"}, {.name = "of-pay"}, {.name = "of-report"}, {.name = "of-api"},
};

static of_config_t cfg;
static const char *conf_path = "conf/orderforge.conf";
static const char *mode_arg;
static char        bin_dir[PATH_MAX];
static char        status_path[512];
static int         max_restarts, window_s, stop_timeout_s;
static bool        degraded;
static int64_t     sup_started_unix;

/* ------------------------------------------------------------------ status file */

static void write_status(void)
{
    char tmp[600], buf[2048];
    int n = snprintf(buf, sizeof buf, "supervisor_pid=%d\nstarted_unix=%lld\ndegraded=%d\n",
                     (int)getpid(), (long long)sup_started_unix, degraded ? 1 : 0);
    for (int i = 0; i < NCHILD && n > 0 && (size_t)n < sizeof buf; i++) {
        const child_t *c = &children[i];
        n += snprintf(buf + n, sizeof buf - (size_t)n,
                      "child=%s pid=%d state=%s restarts=%d started_unix=%lld last_exit=%s\n",
                      c->name, (int)c->pid, state_names[c->state], c->restarts,
                      (long long)c->started_unix, c->last_exit[0] ? c->last_exit : "-");
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", status_path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    if (write(fd, buf, (size_t)n) != n || close(fd) != 0 || rename(tmp, status_path) != 0)
        unlink(tmp);
}

/* ------------------------------------------------------------------ children */

static int spawn(child_t *c)
{
    char path[PATH_MAX + 32];
    snprintf(path, sizeof path, "%s/%s", bin_dir, c->name);
    char *argv[6];
    int a = 0;
    argv[a++] = (char *)c->name;
    argv[a++] = "-c";
    argv[a++] = (char *)conf_path;
    if (strcmp(c->name, "of-core") == 0 && mode_arg) {
        argv[a++] = "-m";
        argv[a++] = (char *)mode_arg;
    }
    argv[a] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("fork", "%s: %s", c->name, strerror(errno));
        return -1;
    }
    if (pid == 0) {
        /* Child: its own process group, default signal state, then become the daemon. */
        setpgid(0, 0);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        execv(path, argv);
        fprintf(stderr, "of-supervisor: exec %s: %s\n", path, strerror(errno));
        _exit(127);
    }
    setpgid(pid, pid); /* also from the parent, so there is no race with kill(-pid) */
    c->pid = pid;
    c->state = ST_RUNNING;
    c->started_us = of_now_us();
    c->started_unix = time(NULL);
    LOG_INFO("spawn", "%s started (pid %d)", c->name, (int)pid);
    write_status();
    return 0;
}

static child_t *by_pid(pid_t pid)
{
    for (int i = 0; i < NCHILD; i++)
        if (children[i].pid == pid)
            return &children[i];
    return NULL;
}

static void describe(int status, char *out, size_t outlen)
{
    if (WIFEXITED(status))
        snprintf(out, outlen, "exit:%d", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        snprintf(out, outlen, "signal:%d(%s)%s", WTERMSIG(status), strsignal(WTERMSIG(status)),
                 WCOREDUMP(status) ? ",core" : "");
    else
        snprintf(out, outlen, "status:0x%x", status);
}

/* A child died while we wanted it running: schedule a restart, or give up. */
static void on_crash(child_t *c, int status)
{
    int64_t now = of_now_us();
    describe(status, c->last_exit, sizeof c->last_exit);
    int64_t lived = now - c->started_us;
    c->pid = 0;

    /* Keep only the crashes inside the window. */
    int k = 0;
    for (int i = 0; i < c->nrecent; i++)
        if (now - c->recent[i] < (int64_t)window_s * 1000000)
            c->recent[k++] = c->recent[i];
    c->nrecent = k;
    if (c->nrecent < (int)(sizeof c->recent / sizeof c->recent[0]))
        c->recent[c->nrecent++] = now;

    if (c->nrecent > max_restarts) {
        c->state = ST_FAILED;
        degraded = true;
        LOG_ERROR("degraded", "%s crashed %d times within %d s (last: %s); not restarting it. "
                              "Fix the cause, then `of-ctl reload` to retry.",
                  c->name, c->nrecent, window_s, c->last_exit);
        fprintf(stderr, "of-supervisor: %s keeps crashing; system DEGRADED (see the logs)\n",
                c->name);
        write_status();
        return;
    }
    if (lived > STABLE_AFTER_US)
        c->backoff_ms = BACKOFF_MIN_MS;
    if (c->backoff_ms == 0)
        c->backoff_ms = BACKOFF_MIN_MS;
    c->state = ST_BACKOFF;
    c->restart_at_us = now + (int64_t)c->backoff_ms * 1000;
    LOG_WARN("crash", "%s (ran %lld ms) %s; restart in %d ms", c->name,
             (long long)(lived / 1000), c->last_exit, c->backoff_ms);
    fprintf(stderr, "of-supervisor: %s died (%s), restarting in %d ms\n", c->name, c->last_exit,
            c->backoff_ms);
    c->backoff_ms = c->backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : c->backoff_ms * 2;
    write_status();
}

static void reap(bool stopping)
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        child_t *c = by_pid(pid);
        if (!c)
            continue;
        if (stopping) {
            describe(status, c->last_exit, sizeof c->last_exit);
            c->pid = 0;
            c->state = ST_STOPPED;
            LOG_INFO("stopped", "%s %s", c->name, c->last_exit);
            continue;
        }
        c->restarts++;
        on_crash(c, status);
    }
}

static void arm_restart_timer(int tfd)
{
    int64_t next = 0;
    for (int i = 0; i < NCHILD; i++)
        if (children[i].state == ST_BACKOFF && (!next || children[i].restart_at_us < next))
            next = children[i].restart_at_us;
    struct itimerspec its = {0};
    if (next) {
        int64_t wait = next - of_now_us();
        if (wait < 1000)
            wait = 1000;
        its.it_value.tv_sec = wait / 1000000;
        its.it_value.tv_nsec = (wait % 1000000) * 1000;
    }
    timerfd_settime(tfd, 0, &its, NULL);
}

/* of-core is ready once its socket accepts a connection. */
static bool core_ready(void)
{
    const char *path = of_config_get(&cfg, "core.socket", "run/of-core.sock");
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof addr.sun_path)
        return false;
    memcpy(addr.sun_path, path, strlen(path) + 1);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;
    bool ok = connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0;
    close(fd);
    return ok;
}

static void stop_all(void)
{
    /* Front to back: stop taking requests first, the engine (which drains) last. */
    for (int i = NCHILD - 1; i >= 0; i--) {
        child_t *c = &children[i];
        if (c->pid <= 0) {
            c->state = ST_STOPPED;
            continue;
        }
        kill(c->pid, SIGTERM);
        int64_t deadline = of_now_us() + (int64_t)stop_timeout_s * 1000000;
        for (;;) {
            int status;
            pid_t r = waitpid(c->pid, &status, WNOHANG);
            if (r == c->pid) {
                describe(status, c->last_exit, sizeof c->last_exit);
                break;
            }
            if (of_now_us() > deadline) {
                LOG_WARN("shutdown", "%s did not stop in %d s: SIGKILL", c->name, stop_timeout_s);
                kill(-c->pid, SIGKILL); /* its whole process group */
                waitpid(c->pid, &status, 0);
                snprintf(c->last_exit, sizeof c->last_exit, "killed");
                break;
            }
            struct timespec ts = {0, 50 * 1000000};
            nanosleep(&ts, NULL);
        }
        LOG_INFO("shutdown", "%s stopped (%s)", c->name, c->last_exit);
        fprintf(stderr, "of-supervisor: %s stopped\n", c->name);
        c->pid = 0;
        c->state = ST_STOPPED;
        write_status();
    }
}

static int load_policy(const of_config_t *c)
{
    int ok1, ok2, ok3;
    int mr = (int)of_config_get_long(c, "supervisor.max_restarts", 5, 1, 1000, &ok1);
    int ws = (int)of_config_get_long(c, "supervisor.window_s", 60, 1, 86400, &ok2);
    int st = (int)of_config_get_long(c, "supervisor.stop_timeout_s", 15, 1, 600, &ok3);
    if (!ok1 || !ok2 || !ok3)
        return -1;
    max_restarts = mr;
    window_s = ws;
    stop_timeout_s = st;
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: of-supervisor [-c config] [-m naive|db-lock|lockfree-shm]\n");
    exit(2);
}

int main(int argc, char **argv)
{
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
        fprintf(stderr, "of-supervisor: %s\n", err);
        return 1;
    }
    /* Before opening any file: leave root if started as root, then harden. */
    if (of_drop_privileges(&cfg, "of-supervisor", err, sizeof err) < 0) {
        fprintf(stderr, "of-supervisor: %s\n", err);
        return 1;
    }
    of_harden(conf_path, &cfg, "of-supervisor");
    if (of_log_open_config(&cfg, "of-supervisor") != 0 || load_policy(&cfg) != 0) {
        fprintf(stderr, "of-supervisor: bad log or supervisor.* settings\n");
        return 1;
    }

    char pid_path[512];
    pid_t holder = 0;
    of_pidfile_path(&cfg, "of-supervisor", pid_path, sizeof pid_path);
    if (of_pidfile_acquire(pid_path, &holder) < 0) {
        fprintf(stderr, "of-supervisor: already running (pid %d holds %s)\n", (int)holder,
                pid_path);
        return 1;
    }
    snprintf(status_path, sizeof status_path, "%s/supervisor.status",
             of_config_get(&cfg, "run.dir", "run"));

    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) {
        fprintf(stderr, "of-supervisor: cannot find own executable\n");
        return 1;
    }
    self[n] = '\0';
    snprintf(bin_dir, sizeof bin_dir, "%s", dirname(self));

    /* Signals as data: blocked here (children unblock them before exec). */
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGCHLD);
    sigaddset(&sigs, SIGTERM);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGHUP);
    sigprocmask(SIG_BLOCK, &sigs, NULL);
    int sfd = signalfd(-1, &sigs, SFD_NONBLOCK | SFD_CLOEXEC);
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (sfd < 0 || tfd < 0 || ep < 0) {
        fprintf(stderr, "of-supervisor: %s\n", strerror(errno));
        return 1;
    }
    struct epoll_event ev = {.events = EPOLLIN};
    ev.data.u32 = 1;
    epoll_ctl(ep, EPOLL_CTL_ADD, sfd, &ev);
    ev.data.u32 = 2;
    epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev);

    sup_started_unix = time(NULL);
    LOG_INFO("start", "of-supervisor pid %d, binaries in %s, config %s", (int)getpid(), bin_dir,
             conf_path);

    /* of-core first, and wait until it serves (recovery may take a moment). */
    if (spawn(&children[0]) != 0)
        return 1;
    int64_t deadline = of_now_us() + 60 * 1000000LL;
    while (!core_ready()) {
        int status;
        if (waitpid(children[0].pid, &status, WNOHANG) == children[0].pid) {
            describe(status, children[0].last_exit, sizeof children[0].last_exit);
            fprintf(stderr, "of-supervisor: of-core failed to start (%s); see %s/of-core.log\n",
                    children[0].last_exit, of_config_get(&cfg, "log.dir", "run/log"));
            children[0].pid = 0;
            children[0].state = ST_FAILED;
            write_status();
            return 1;
        }
        if (of_now_us() > deadline) {
            fprintf(stderr, "of-supervisor: of-core not ready after 60 s\n");
            stop_all();
            return 1;
        }
        struct timespec ts = {0, 100 * 1000000};
        nanosleep(&ts, NULL);
    }
    for (int i = 1; i < NCHILD; i++)
        spawn(&children[i]);
    fprintf(stderr, "of-supervisor: all daemons running (pid %d). Ctrl-C to stop; "
                    "`of-ctl status` for details.\n",
            (int)getpid());

    for (;;) {
        struct epoll_event evs[4];
        int nev = epoll_wait(ep, evs, 4, -1);
        if (nev < 0 && errno == EINTR)
            continue;
        for (int i = 0; i < nev; i++) {
            if (evs[i].data.u32 == 2) { /* a restart is due */
                uint64_t ticks;
                if (read(tfd, &ticks, sizeof ticks) < 0 && errno != EAGAIN)
                    continue;
                int64_t now = of_now_us();
                for (int k = 0; k < NCHILD; k++)
                    if (children[k].state == ST_BACKOFF && children[k].restart_at_us <= now)
                        if (spawn(&children[k]) != 0) {
                            children[k].state = ST_BACKOFF;
                            children[k].restart_at_us = now + BACKOFF_MAX_MS * 1000LL;
                        }
                arm_restart_timer(tfd);
                continue;
            }
            struct signalfd_siginfo si;
            while (read(sfd, &si, sizeof si) == sizeof si) {
                if (si.ssi_signo == SIGCHLD) {
                    reap(false);
                    arm_restart_timer(tfd);
                } else if (si.ssi_signo == SIGHUP) {
                    of_log_reopen();
                    of_config_t fresh;
                    if (of_config_load(&fresh, conf_path, err, sizeof err) == 0 &&
                        load_policy(&fresh) == 0)
                        cfg = fresh;
                    for (int k = 0; k < NCHILD; k++) {
                        child_t *c = &children[k];
                        if (c->pid > 0)
                            kill(c->pid, SIGHUP);
                        if (c->state == ST_FAILED) { /* a fresh chance */
                            c->nrecent = 0;
                            c->backoff_ms = BACKOFF_MIN_MS;
                            spawn(c);
                        }
                    }
                    degraded = false;
                    for (int k = 0; k < NCHILD; k++)
                        degraded |= children[k].state == ST_FAILED;
                    LOG_INFO("reload", "SIGHUP forwarded to the daemons");
                    write_status();
                } else { /* SIGTERM / SIGINT */
                    LOG_INFO("shutdown", "%s: stopping the daemons", strsignal((int)si.ssi_signo));
                    fprintf(stderr, "of-supervisor: stopping (of-api, of-report, of-pay, "
                                    "then of-core)\n");
                    stop_all();
                    reap(true);
                    LOG_INFO("shutdown", "of-supervisor stopped");
                    return 0;
                }
            }
        }
    }
}
