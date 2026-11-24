/*
 * of-report - periodic sales aggregation and CSV reports.
 *
 * Every report.interval_s seconds (a timerfd), and immediately on SIGHUP:
 *   1. In one MySQL transaction, recompute the last report.days days of daily_sales
 *      and product_sales_daily from paid-or-later orders (delete the window, re-insert).
 *   2. Write four CSV files into report.dir, each one atomically:
 *        write a temporary file -> fsync it -> rename() over the old file -> fsync the
 *        directory. rename() within one filesystem is atomic, so a reader (the export
 *        API) always sees either the complete old report or the complete new one, and
 *        the fsyncs make the new one survive a power cut.
 *   3. Record the run in report_meta, which the dashboard shows as "generated at".
 *
 * The low-stock report also reads the live counters from of-core's shared-memory stock
 * table (mapped read-only) when the core is running in lockfree-shm mode.
 *
 *   usage: of-report [-c conf/orderforge.conf] [--once]
 */
#include "common/buf.h"
#include "common/config.h"
#include "common/harden.h"
#include "common/log.h"
#include "common/stock.h"
#include "common/util.h"
#include "store/db.h"
#include "store/store.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

static of_config_t cfg;
static char        report_dir[256];
static int64_t     window_days;

/* ------------------------------------------------------------------ CSV helpers */

static void csv_field(buf_t *b, const char *s)
{
    if (!s)
        s = "";
    if (strpbrk(s, ",\"\r\n") == NULL) {
        buf_puts(b, s);
        return;
    }
    buf_puts(b, "\"");
    for (const char *p = s; *p; p++) {
        if (*p == '"')
            buf_puts(b, "\"\"");
        else
            buf_append(b, p, 1);
    }
    buf_puts(b, "\"");
}

static void csv_rupees(buf_t *b, int64_t paise)
{
    buf_printf(b, "%s%lld.%02lld", paise < 0 ? "-" : "", (long long)llabs(paise / 100),
               (long long)llabs(paise % 100));
}

/* Atomically replace dir/name with the buffer's contents. 0 or -1. */
static int write_atomic(const char *name, const buf_t *b)
{
    char tmp[512], final[512];
    snprintf(final, sizeof final, "%s/%s", report_dir, name);
    snprintf(tmp, sizeof tmp, "%s/.%s.tmp.%d", report_dir, name, (int)getpid());

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0640);
    if (fd < 0)
        return -1;
    size_t off = 0;
    while (off < b->len) {
        ssize_t n = write(fd, b->data + off, b->len - off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            close(fd);
            unlink(tmp);
            return -1;
        }
        off += (size_t)n;
    }
    if (fsync(fd) != 0 || close(fd) != 0 || rename(tmp, final) != 0) {
        unlink(tmp);
        return -1;
    }
    int dfd = open(report_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        fsync(dfd); /* make the rename itself durable */
        close(dfd);
    }
    return 0;
}

/* ------------------------------------------------------------------ one report run */

static int aggregate(db_t *db)
{
    if (db_begin(db) != 0)
        return -1;
    if (db_exec(db, "DELETE FROM daily_sales WHERE day > CURRENT_DATE - INTERVAL ? DAY", "i",
                window_days) < 0 ||
        db_exec(db, "DELETE FROM product_sales_daily WHERE day > CURRENT_DATE - INTERVAL ? DAY",
                "i", window_days) < 0 ||
        db_exec(db,
                "INSERT INTO daily_sales (day, orders, units, revenue_paise) "
                "SELECT DATE(o.created_at), COUNT(DISTINCT o.id), SUM(oi.qty), "
                "       SUM(oi.qty * oi.unit_price_paise) "
                "FROM orders o JOIN order_items oi ON oi.order_id = o.id "
                "WHERE o.status IN ('paid', 'packed', 'shipped', 'delivered') "
                "  AND DATE(o.created_at) > CURRENT_DATE - INTERVAL ? DAY "
                "GROUP BY DATE(o.created_at)",
                "i", window_days) < 0 ||
        db_exec(db,
                "INSERT INTO product_sales_daily (day, product_id, units, revenue_paise) "
                "SELECT DATE(o.created_at), oi.product_id, SUM(oi.qty), "
                "       SUM(oi.qty * oi.unit_price_paise) "
                "FROM orders o JOIN order_items oi ON oi.order_id = o.id "
                "WHERE o.status IN ('paid', 'packed', 'shipped', 'delivered') "
                "  AND DATE(o.created_at) > CURRENT_DATE - INTERVAL ? DAY "
                "GROUP BY DATE(o.created_at), oi.product_id",
                "i", window_days) < 0 ||
        db_commit(db) != 0) {
        (void)db_rollback(db);
        return -1;
    }
    return 0;
}

static int csv_daily(db_t *db, buf_t *b)
{
    db_res_t r;
    if (db_query(db, &r,
                 "SELECT DATE_FORMAT(day, '%Y-%m-%d'), orders, units, revenue_paise FROM daily_sales "
                 "WHERE day > CURRENT_DATE - INTERVAL ? DAY ORDER BY day",
                 "i", window_days) != 0)
        return -1;
    buf_puts(b, "day,orders,units,revenue_rupees\n");
    while (db_fetch(&r) == 1) {
        buf_printf(b, "%s,%lld,%lld,", db_str(&r, 0), (long long)db_int(&r, 1),
                   (long long)db_int(&r, 2));
        csv_rupees(b, db_int(&r, 3));
        buf_puts(b, "\n");
    }
    db_res_free(&r);
    return 0;
}

static int csv_top(db_t *db, buf_t *b)
{
    db_res_t r;
    if (db_query(db, &r,
                 "SELECT p.sku, p.name, SUM(s.units), SUM(s.revenue_paise) "
                 "FROM product_sales_daily s JOIN products p ON p.id = s.product_id "
                 "WHERE s.day > CURRENT_DATE - INTERVAL ? DAY "
                 "GROUP BY p.id, p.sku, p.name ORDER BY SUM(s.revenue_paise) DESC",
                 "i", window_days) != 0)
        return -1;
    buf_puts(b, "rank,sku,name,units,revenue_rupees\n");
    for (int rank = 1; db_fetch(&r) == 1; rank++) {
        buf_printf(b, "%d,", rank);
        csv_field(b, db_str(&r, 0));
        buf_puts(b, ",");
        csv_field(b, db_str(&r, 1));
        buf_printf(b, ",%lld,", (long long)db_int(&r, 2));
        csv_rupees(b, db_int(&r, 3));
        buf_puts(b, "\n");
    }
    db_res_free(&r);
    return 0;
}

static int csv_low_stock(db_t *db, buf_t *b)
{
    /* Live counters from shared memory, when the core keeps them exact. */
    stock_table_t t;
    bool live = stock_attach(&t, of_config_get(&cfg, "core.shm_name", "/orderforge")) == 0 &&
                t.shm->stock_mode == STOCK_LOCKFREE;

    db_res_t r;
    if (db_query(db, &r,
                 "SELECT p.id, p.sku, p.name, i.stock_on_hand, i.reorder_level "
                 "FROM products p JOIN inventory i ON i.product_id = p.id "
                 "WHERE p.active = 1 AND i.stock_on_hand <= i.reorder_level "
                 "ORDER BY i.stock_on_hand, p.id",
                 "") != 0) {
        if (t.shm)
            stock_close(&t, false);
        return -1;
    }
    buf_puts(b, "sku,name,stock_on_hand,live_available,reorder_level\n");
    while (db_fetch(&r) == 1) {
        csv_field(b, db_str(&r, 1));
        buf_puts(b, ",");
        csv_field(b, db_str(&r, 2));
        buf_printf(b, ",%lld,", (long long)db_int(&r, 3));
        stock_slot_t *s = live ? stock_find(&t, (uint64_t)db_int(&r, 0)) : NULL;
        if (s)
            buf_printf(b, "%d", (int)atomic_load(&s->available));
        buf_printf(b, ",%lld\n", (long long)db_int(&r, 4));
    }
    db_res_free(&r);
    if (t.shm)
        stock_close(&t, false);
    return 0;
}

static int csv_status(db_t *db, buf_t *b)
{
    db_res_t r;
    if (db_query(db, &r, "SELECT status, COUNT(*), COALESCE(SUM(total_paise), 0) "
                         "FROM orders GROUP BY status ORDER BY status",
                 "") != 0)
        return -1;
    buf_puts(b, "status,orders,value_rupees\n");
    while (db_fetch(&r) == 1) {
        buf_printf(b, "%s,%lld,", db_str(&r, 0), (long long)db_int(&r, 1));
        csv_rupees(b, db_int(&r, 2));
        buf_puts(b, "\n");
    }
    db_res_free(&r);
    return 0;
}

static int run_once(db_t *db)
{
    int64_t t0 = of_now_us();
    if (aggregate(db) != 0) {
        LOG_ERROR("aggregate", "%s", db_error(db));
        return -1;
    }

    static const struct {
        const char *file;
        int (*fn)(db_t *, buf_t *);
    } reports[] = {
        {"daily_sales.csv", csv_daily},
        {"top_products.csv", csv_top},
        {"low_stock.csv", csv_low_stock},
        {"order_status.csv", csv_status},
    };
    int written = 0;
    for (size_t i = 0; i < sizeof reports / sizeof reports[0]; i++) {
        buf_t b;
        buf_init(&b);
        if (reports[i].fn(db, &b) == 0 && !buf_failed(&b) && write_atomic(reports[i].file, &b) == 0)
            written++;
        else
            LOG_ERROR("csv", "%s: %s", reports[i].file, strerror(errno));
        buf_free(&b);
    }

    int64_t ms = (of_now_us() - t0) / 1000;
    char note[64];
    snprintf(note, sizeof note, "%d CSV files, %lld-day window", written, (long long)window_days);
    if (db_exec(db,
                "REPLACE INTO report_meta (name, generated_at, duration_ms, note) "
                "VALUES ('sales', NOW(3), ?, ?)",
                "is", ms, note) < 0)
        LOG_WARN("report_meta", "%s", db_error(db));
    LOG_INFO("report", "generated in %lld ms: %s", (long long)ms, note);
    return 0;
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    const char *conf_path = "conf/orderforge.conf";
    bool once = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            conf_path = argv[++i];
        else if (strcmp(argv[i], "--once") == 0)
            once = true;
        else {
            fprintf(stderr, "usage: of-report [-c config] [--once]\n");
            return 2;
        }
    }
    char err[256];
    if (of_config_load(&cfg, conf_path, err, sizeof err) != 0) {
        fprintf(stderr, "of-report: %s\n", err);
        return 1;
    }
    /* Before opening any file: leave root if started as root, then harden. */
    if (of_drop_privileges(&cfg, "of-report", err, sizeof err) < 0) {
        fprintf(stderr, "of-report: %s\n", err);
        return 1;
    }
    of_harden(conf_path, &cfg, "of-report");
    if (of_log_open_config(&cfg, "of-report") != 0)
        return 1;

    int ok1, ok2;
    long interval = of_config_get_long(&cfg, "report.interval_s", 10, 1, 86400, &ok1);
    window_days = of_config_get_long(&cfg, "report.days", 30, 1, 366, &ok2);
    snprintf(report_dir, sizeof report_dir, "%s", of_config_get(&cfg, "report.dir", "run/reports"));
    if (!ok1 || !ok2 || of_mkdir_p(report_dir, 0750) != 0) {
        fprintf(stderr, "of-report: bad report.* settings or cannot create %s\n", report_dir);
        return 1;
    }

    char pid_path[512];
    pid_t holder = 0;
    of_pidfile_path(&cfg, "of-report", pid_path, sizeof pid_path);
    int pid_fd = once ? 0 : of_pidfile_acquire(pid_path, &holder);
    if (pid_fd < 0) {
        fprintf(stderr, "of-report: already running (pid %d holds %s)\n", (int)holder, pid_path);
        return 1;
    }

    db_t *db = db_connect(&cfg, err, sizeof err);
    if (!db) {
        fprintf(stderr, "of-report: database: %s\n", err);
        return 1;
    }
    if (once) {
        int rc = run_once(db);
        db_close(db);
        return rc == 0 ? 0 : 1;
    }

    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGTERM);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGHUP);
    sigprocmask(SIG_BLOCK, &sigs, NULL);
    int sfd = signalfd(-1, &sigs, SFD_CLOEXEC);
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    struct itimerspec its = {{interval, 0}, {interval, 0}};
    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (sfd < 0 || tfd < 0 || ep < 0 || timerfd_settime(tfd, 0, &its, NULL) != 0) {
        fprintf(stderr, "of-report: %s\n", strerror(errno));
        return 1;
    }
    struct epoll_event ev = {.events = EPOLLIN};
    ev.data.u32 = 1;
    epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev);
    ev.data.u32 = 2;
    epoll_ctl(ep, EPOLL_CTL_ADD, sfd, &ev);

    LOG_INFO("start", "of-report pid %d: every %ld s into %s", (int)getpid(), interval, report_dir);
    fprintf(stderr, "of-report: every %ld s into %s (pid %d)\n", interval, report_dir,
            (int)getpid());
    run_once(db);

    for (;;) {
        struct epoll_event evs[2];
        int n = epoll_wait(ep, evs, 2, -1);
        if (n < 0 && errno == EINTR)
            continue;
        for (int i = 0; i < n; i++) {
            if (evs[i].data.u32 == 1) {
                uint64_t ticks;
                if (read(tfd, &ticks, sizeof ticks) != sizeof ticks)
                    continue;
            } else {
                struct signalfd_siginfo si;
                if (read(sfd, &si, sizeof si) != sizeof si)
                    continue;
                if (si.ssi_signo != SIGHUP) {
                    LOG_INFO("shutdown", "of-report stopped");
                    db_close(db);
                    return 0;
                }
                of_log_reopen();
                of_config_t fresh;
                int okr1, okr2;
                if (of_config_load(&fresh, conf_path, err, sizeof err) == 0) {
                    long iv = of_config_get_long(&fresh, "report.interval_s", 10, 1, 86400, &okr1);
                    long dd = of_config_get_long(&fresh, "report.days", 30, 1, 366, &okr2);
                    if (okr1 && okr2) {
                        struct itimerspec nt = {{iv, 0}, {iv, 0}};
                        timerfd_settime(tfd, 0, &nt, NULL);
                        window_days = dd;
                    }
                    LOG_INFO("reload", "SIGHUP: every %ld s, %lld-day window; regenerating now",
                             iv, (long long)window_days);
                }
            }
            if (db_broken(db)) {
                db_t *fresh = db_connect(&cfg, err, sizeof err);
                if (!fresh) {
                    LOG_WARN("db_reconnect", "%s", err);
                    continue;
                }
                db_close(db);
                db = fresh;
            }
            run_once(db);
        }
    }
}
