/*
 * of-ctl - operator command line tool.
 *
 *   of-ctl [-c config] useradd [--admin] EMAIL NAME
 *        Create or update an account. The password is read from the first line of
 *        stdin (so it never appears in `ps` or shell history), e.g.
 *            echo 'secret123' | of-ctl useradd --admin admin@orderforge.local Admin
 *
 *   of-ctl [-c config] invariants
 *        Every invariant: the stock ledger (SUM(stock_movements) == stock_on_hand, none
 *        negative), shared memory == MySQL (lockfree-shm), and the order checks
 *        (payments, timelines, reservations, nothing stuck past its deadline).
 *
 *   of-ctl [-c config] status     supervisor and daemons, engine metrics, WAL, recovery
 *   of-ctl [-c config] metrics    the Prometheus metrics (same as GET /api/metrics)
 *   of-ctl [-c config] stuck      orders that should have moved on but have not
 *   of-ctl [-c config] reload     SIGHUP: reopen logs, reload settings, retry failed daemons
 *   of-ctl [-c config] stop       SIGTERM: ordered drain and shutdown
 *
 * status and metrics read the shared-memory table through a read-only mapping and the
 * supervisor's status file: they never send a request to of-core.
 */
#include "common/config.h"
#include "common/log.h"
#include "common/metrics.h"
#include "common/util.h"
#include "store/db.h"
#include "store/store.h"

#include <ctype.h>
#include <signal.h>
#include <sodium.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static of_config_t cfg;

static void usage(void)
{
    fprintf(stderr,
            "usage: of-ctl [-c config] useradd [--admin] EMAIL NAME   (password on stdin)\n"
            "       of-ctl [-c config] invariants | status | metrics | stuck | reload | stop\n");
    exit(2);
}

static db_t *connect_db(void)
{
    char err[256];
    db_t *db = db_connect(&cfg, err, sizeof err);
    if (!db)
        fprintf(stderr, "of-ctl: database: %s\n", err);
    return db;
}

static int cmd_useradd(int argc, char **argv)
{
    bool admin = false;
    int i = 0;
    if (i < argc && strcmp(argv[i], "--admin") == 0) {
        admin = true;
        i++;
    }
    if (argc - i != 2)
        usage();

    char email[255];
    snprintf(email, sizeof email, "%s", argv[i]);
    for (char *p = email; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    const char *name = argv[i + 1];
    if (!strchr(email, '@') || strlen(name) == 0 || strlen(name) > 100) {
        fprintf(stderr, "of-ctl: invalid email or name\n");
        return 1;
    }

    char password[160];
    if (isatty(STDIN_FILENO))
        fprintf(stderr, "password: ");
    if (!fgets(password, sizeof password, stdin)) {
        fprintf(stderr, "of-ctl: no password on stdin\n");
        return 1;
    }
    password[strcspn(password, "\r\n")] = '\0';
    if (strlen(password) < 8 || strlen(password) > 128) {
        fprintf(stderr, "of-ctl: password must be 8 to 128 characters\n");
        return 1;
    }

    if (sodium_init() < 0) {
        fprintf(stderr, "of-ctl: libsodium init failed\n");
        return 1;
    }
    char hash[crypto_pwhash_STRBYTES];
    int rc = store_hash_password(password, hash, sizeof hash);
    sodium_memzero(password, sizeof password);
    if (rc != 0) {
        fprintf(stderr, "of-ctl: hashing failed\n");
        return 1;
    }

    db_t *db = connect_db();
    if (!db)
        return 1;
    int64_t n = db_exec(db,
                        "INSERT INTO users (email, name, pw_hash, role) VALUES (?, ?, ?, ?) AS new "
                        "ON DUPLICATE KEY UPDATE name = new.name, pw_hash = new.pw_hash, "
                        "  role = new.role, failed_logins = 0, locked_until = NULL",
                        "ssss", email, name, hash, admin ? "admin" : "customer");
    if (n < 0) {
        fprintf(stderr, "of-ctl: %s\n", db_error(db));
        db_close(db);
        return 1;
    }
    printf("%s %s (%s)\n", n == 1 ? "created" : "updated", email, admin ? "admin" : "customer");
    db_close(db);
    return 0;
}

/*
 * Ledger check in MySQL, plus (when of-core runs in lockfree-shm mode) a comparison of
 * the live shared-memory counters with MySQL. The table is mapped read-only: of-ctl can
 * look but can never change stock.
 */
static int cmd_invariants(void)
{
    db_t *db = connect_db();
    if (!db)
        return 1;

    stock_table_t t;
    const stock_table_t *tp = NULL;
    const char *shm_name = of_config_get(&cfg, "core.shm_name", "/orderforge");
    if (stock_attach(&t, shm_name) == 0) {
        if (t.shm->stock_mode == STOCK_LOCKFREE && atomic_load(&t.shm->started_unix) != 0)
            tp = &t;
        printf("of-core shared memory: mode %s, %llu products%s\n",
               stock_mode_name((stock_mode_t)t.shm->stock_mode),
               (unsigned long long)atomic_load(&t.shm->count),
               tp ? " (compared with MySQL)" : " (not compared: only lockfree-shm keeps it exact)");
    } else {
        printf("of-core shared memory: not found (core not running), checking MySQL only\n");
    }

    buf_t out;
    buf_init(&out);
    int64_t oversold = 0;
    int bad = store_check_invariants(db, tp, &out, &oversold);
    if (out.data)
        fputs(out.data, stdout);
    buf_reset(&out);
    int bad_orders = bad < 0 ? -1 : store_check_orders(db, &out, 30);
    printf("\norders\n");
    if (out.data)
        fputs(out.data, stdout);
    buf_free(&out);
    if (t.shm)
        stock_close(&t, false);
    if (bad < 0 || bad_orders < 0)
        fprintf(stderr, "of-ctl: %s\n", db_error(db));
    db_close(db);
    return bad == 0 && bad_orders == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ status & friends */

static void fmt_age(char *out, size_t n, int64_t secs)
{
    if (secs < 0)
        snprintf(out, n, "-");
    else if (secs < 60)
        snprintf(out, n, "%llds", (long long)secs);
    else if (secs < 3600)
        snprintf(out, n, "%lldm %02llds", (long long)secs / 60, (long long)secs % 60);
    else
        snprintf(out, n, "%lldh %02lldm", (long long)secs / 3600, (long long)secs % 3600 / 60);
}

/* Approximate a percentile from the cumulative histogram (upper bound of its bucket). */
static const char *pct(const stock_metrics_t *m, double q, char *buf, size_t n)
{
    static const int bounds[OF_LAT_BUCKETS] = OF_LAT_BOUNDS_MS;
    uint64_t total = atomic_load(&m->latency_count), cum = 0;
    if (!total) {
        snprintf(buf, n, "-");
        return buf;
    }
    for (int i = 0; i < OF_LAT_BUCKETS; i++) {
        cum += atomic_load(&m->latency_bucket[i]);
        if ((double)cum >= q * (double)total) {
            if (bounds[i] < 0)
                snprintf(buf, n, ">1s");
            else
                snprintf(buf, n, "<=%dms", bounds[i]);
            return buf;
        }
    }
    snprintf(buf, n, "?");
    return buf;
}

static void print_daemons(void)
{
    char path[512];
    snprintf(path, sizeof path, "%s/supervisor.status", of_config_get(&cfg, "run.dir", "run"));
    char sup_pid_path[512];
    of_pidfile_path(&cfg, "of-supervisor", sup_pid_path, sizeof sup_pid_path);
    pid_t sup = 0;
    bool sup_alive = of_pidfile_read(sup_pid_path, &sup) == 0;
    FILE *f = sup_alive ? fopen(path, "r") : NULL;
    int64_t now = time(NULL);

    if (f) {
        char line[512];
        long long started = 0;
        int degraded = 0;
        printf("%-12s %-8s %-9s %-9s %-10s %s\n", "daemon", "pid", "state", "restarts", "up",
               "last exit");
        while (fgets(line, sizeof line, f)) {
            char name[32], state[16], last[64];
            int pid, restarts;
            long long st;
            if (sscanf(line, "started_unix=%lld", &started) == 1 ||
                sscanf(line, "degraded=%d", &degraded) == 1)
                continue;
            if (sscanf(line, "child=%31s pid=%d state=%15s restarts=%d started_unix=%lld last_exit=%63s",
                       name, &pid, state, &restarts, &st, last) == 6) {
                char up[32];
                fmt_age(up, sizeof up, strcmp(state, "running") == 0 ? now - st : -1);
                printf("%-12s %-8d %-9s %-9d %-10s %s\n", name, pid, state, restarts, up, last);
            }
        }
        fclose(f);
        char up[32];
        fmt_age(up, sizeof up, now - started);
        printf("supervisor pid %d, up %s: %s\n", (int)sup, up,
               degraded ? "DEGRADED (a daemon keeps crashing; see the logs, then `of-ctl reload`)"
                        : "all daemons supervised");
        return;
    }

    printf("supervisor: not running; daemons found through their pid files:\n");
    static const char *const names[] = {"of-core", "of-pay", "of-report", "of-api"};
    for (size_t i = 0; i < 4; i++) {
        char pp[512];
        pid_t pid = 0;
        of_pidfile_path(&cfg, names[i], pp, sizeof pp);
        if (of_pidfile_read(pp, &pid) == 0)
            printf("  %-12s pid %d\n", names[i], (int)pid);
        else
            printf("  %-12s not running\n", names[i]);
    }
}

static int cmd_status(void)
{
    printf("== daemons\n");
    print_daemons();

    stock_table_t t;
    const char *shm_name = of_config_get(&cfg, "core.shm_name", "/orderforge");
    printf("\n== engine (shared memory %s)\n", shm_name);
    if (stock_attach(&t, shm_name) != 0) {
        printf("not available: of-core is not running\n");
        return 1;
    }
    const stock_shm_t *h = t.shm;
    const stock_metrics_t *m = &h->metrics;
    static const char *const modes[] = {"naive", "db-lock", "lockfree-shm"};
    int64_t started = atomic_load(&h->started_unix);
    char up[32], p50[16], p95[16], p99[16];
    fmt_age(up, sizeof up, started ? time(NULL) - started : -1);
    printf("mode %s, %s, %llu products\n", h->stock_mode < 3 ? modes[h->stock_mode] : "?",
           started ? up : "starting / recovering", (unsigned long long)atomic_load(&h->count));
    printf("requests %llu   queue %llu   active workers %llu   busy rejects %llu\n",
           (unsigned long long)atomic_load(&m->requests_total),
           (unsigned long long)atomic_load(&m->queue_depth),
           (unsigned long long)atomic_load(&m->active_workers),
           (unsigned long long)atomic_load(&m->busy_rejects));
    printf("latency  p50 %s  p95 %s  p99 %s   (%llu requests)\n", pct(m, .50, p50, sizeof p50),
           pct(m, .95, p95, sizeof p95), pct(m, .99, p99, sizeof p99),
           (unsigned long long)atomic_load(&m->latency_count));
    printf("orders %llu   sold out %llu   CAS retries %llu\n",
           (unsigned long long)atomic_load(&m->orders_total),
           (unsigned long long)atomic_load(&m->orders_failed),
           (unsigned long long)atomic_load(&m->cas_retries));
    printf("payments sent %llu  approved %llu  declined %llu  duplicates ignored %llu  "
           "rejected %llu  late refunds %llu   expired reservations %llu\n",
           (unsigned long long)atomic_load(&m->payments_sent),
           (unsigned long long)atomic_load(&m->payments_succeeded),
           (unsigned long long)atomic_load(&m->payments_declined),
           (unsigned long long)atomic_load(&m->payments_ignored),
           (unsigned long long)atomic_load(&m->payments_rejected),
           (unsigned long long)atomic_load(&m->late_refunds),
           (unsigned long long)atomic_load(&m->reservations_expired));

    uint64_t ap = atomic_load(&m->wal.appends), fl = atomic_load(&m->wal.flushes);
    printf("\n== write-ahead log\n");
    printf("records %llu in %llu fsyncs (avg batch %.1f, max %llu), avg fsync %.2f ms, "
           "%llu bytes, %llu compactions\n",
           (unsigned long long)ap, (unsigned long long)fl, fl ? (double)ap / (double)fl : 0.0,
           (unsigned long long)atomic_load(&m->wal.max_batch),
           fl ? atomic_load(&m->wal.flush_us_total) / 1000.0 / (double)fl : 0.0,
           (unsigned long long)atomic_load(&m->wal.bytes),
           (unsigned long long)atomic_load(&m->wal.compactions));

    printf("\n== crash recovery\n");
    printf("starts %llu, recoveries from a crash %llu, last start-up recovery %llu ms\n",
           (unsigned long long)atomic_load(&m->starts),
           (unsigned long long)atomic_load(&m->recoveries),
           (unsigned long long)atomic_load(&m->recovery_ms_last));
    printf("in-flight reservations resolved: %llu committed, %llu released; "
           "reconciliation drift %llu products / %llu units; torn log bytes %llu\n",
           (unsigned long long)atomic_load(&m->recovered_committed),
           (unsigned long long)atomic_load(&m->recovered_released),
           (unsigned long long)atomic_load(&m->drift_products),
           (unsigned long long)atomic_load(&m->drift_units),
           (unsigned long long)atomic_load(&m->wal_torn_bytes));
    stock_close(&t, false);
    return 0;
}

static int cmd_metrics(void)
{
    stock_table_t t;
    if (stock_attach(&t, of_config_get(&cfg, "core.shm_name", "/orderforge")) != 0) {
        fprintf(stderr, "of-ctl: of-core shared memory not available\n");
        return 1;
    }
    buf_t b;
    buf_init(&b);
    metrics_format(&t, &b);
    if (b.data)
        fputs(b.data, stdout);
    buf_free(&b);
    stock_close(&t, false);
    return 0;
}

static int cmd_stuck(void)
{
    db_t *db = connect_db();
    if (!db)
        return 1;
    buf_t out;
    buf_init(&out);
    int bad = store_check_orders(db, &out, 30);
    if (out.data)
        fputs(out.data, stdout);
    buf_free(&out);
    if (bad < 0)
        fprintf(stderr, "of-ctl: %s\n", db_error(db));
    db_close(db);
    return bad == 0 ? 0 : 1;
}

/* Signal the supervisor, or (without one) every daemon directly. */
static int signal_stack(int sig, const char *what)
{
    char pp[512];
    pid_t pid = 0;
    of_pidfile_path(&cfg, "of-supervisor", pp, sizeof pp);
    if (of_pidfile_read(pp, &pid) == 0) {
        if (kill(pid, sig) != 0) {
            perror("of-ctl: kill");
            return 1;
        }
        printf("%s sent to of-supervisor (pid %d)\n", what, (int)pid);
        if (sig != SIGTERM)
            return 0;
        for (int i = 0; i < 600 && of_pidfile_read(pp, &pid) == 0; i++) {
            struct timespec ts = {0, 100 * 1000000};
            nanosleep(&ts, NULL);
        }
        printf("%s\n", of_pidfile_read(pp, &pid) == 0 ? "still stopping (see the logs)"
                                                     : "all daemons stopped");
        return 0;
    }
    /* No supervisor: front to back, so the engine drains last. */
    static const char *const names[] = {"of-api", "of-report", "of-pay", "of-core"};
    int sent = 0;
    for (size_t i = 0; i < 4; i++) {
        of_pidfile_path(&cfg, names[i], pp, sizeof pp);
        if (of_pidfile_read(pp, &pid) == 0 && kill(pid, sig) == 0) {
            printf("%s sent to %s (pid %d)\n", what, names[i], (int)pid);
            sent++;
            if (sig == SIGTERM)
                for (int k = 0; k < 300 && of_pidfile_read(pp, &pid) == 0; k++) {
                    struct timespec ts = {0, 100 * 1000000};
                    nanosleep(&ts, NULL);
                }
        }
    }
    if (!sent)
        printf("nothing running\n");
    return 0;
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
        fprintf(stderr, "of-ctl: %s\n", err);
        return 1;
    }
    of_log_open(NULL, "of-ctl", OF_LOG_ERROR);

    const char *cmd = argv[optind];
    if (strcmp(cmd, "useradd") == 0)
        return cmd_useradd(argc - optind - 1, argv + optind + 1);
    if (strcmp(cmd, "invariants") == 0)
        return cmd_invariants();
    if (strcmp(cmd, "status") == 0)
        return cmd_status();
    if (strcmp(cmd, "metrics") == 0)
        return cmd_metrics();
    if (strcmp(cmd, "stuck") == 0)
        return cmd_stuck();
    if (strcmp(cmd, "reload") == 0)
        return signal_stack(SIGHUP, "SIGHUP (reload)");
    if (strcmp(cmd, "stop") == 0)
        return signal_stack(SIGTERM, "SIGTERM (stop)");
    usage();
    return 2;
}
