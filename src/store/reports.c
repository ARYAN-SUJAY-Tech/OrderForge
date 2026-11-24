/*
 * reports.c - the admin reporting API.
 *
 * of-report (a separate process) aggregates sales into daily_sales and
 * product_sales_daily every few seconds and writes CSV files with an atomic rename().
 * This file only reads: the aggregate tables for the dashboard, live counts for the
 * things that must be current (order states, payments, low stock), and the CSV files
 * for export. Because of-report replaces each CSV with rename(), a reader here always
 * sees either the old file or the new one, never a half-written one.
 */
#include "store/internal.h"

#include "common/util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define EXPORT_MAX (1024 * 1024)

/* GET /api/admin/reports?days=30 */
void h_admin_reports(ctx_t *c)
{
    int64_t days = of_query_get_int(c->req->query, "days", 30, 1, 366);
    buf_t *b = &c->resp->body;
    db_res_t r;

    buf_puts(b, "{");
    buf_json_kv_int(b, "days", days);

    /* When of-report last ran. */
    if (db_query(c->db, &r,
                 "SELECT " ISO("generated_at") ", duration_ms, "
                 "  TIMESTAMPDIFF(SECOND, generated_at, NOW(3)) "
                 "FROM report_meta WHERE name = 'sales'",
                 "") != 0)
        goto db_failed;
    if (db_fetch(&r) == 1) {
        buf_puts(b, ",");
        buf_json_kv_str(b, "generated_at", db_str(&r, 0));
        buf_puts(b, ",");
        buf_json_kv_int(b, "generation_ms", db_int(&r, 1));
        buf_puts(b, ",");
        buf_json_kv_int(b, "age_seconds", db_int(&r, 2));
    }
    db_res_free(&r);

    /* Daily series from of-report's aggregate table. */
    if (db_query(c->db, &r,
                 "SELECT DATE_FORMAT(day, '%Y-%m-%d'), orders, units, revenue_paise FROM daily_sales "
                 "WHERE day > CURRENT_DATE - INTERVAL ? DAY ORDER BY day",
                 "i", days) != 0)
        goto db_failed;
    int64_t t_orders = 0, t_units = 0, t_rev = 0;
    buf_puts(b, ",\"daily\":[");
    for (int n = 0; db_fetch(&r) == 1; n++) {
        t_orders += db_int(&r, 1);
        t_units += db_int(&r, 2);
        t_rev += db_int(&r, 3);
        buf_puts(b, n ? ",{" : "{");
        buf_json_kv_str(b, "day", db_str(&r, 0));
        buf_puts(b, ",");
        buf_json_kv_int(b, "orders", db_int(&r, 1));
        buf_puts(b, ",");
        buf_json_kv_int(b, "units", db_int(&r, 2));
        buf_puts(b, ",");
        buf_json_kv_int(b, "revenue_paise", db_int(&r, 3));
        buf_puts(b, "}");
    }
    db_res_free(&r);
    buf_puts(b, "],\"totals\":{");
    buf_json_kv_int(b, "orders", t_orders);
    buf_puts(b, ",");
    buf_json_kv_int(b, "units", t_units);
    buf_puts(b, ",");
    buf_json_kv_int(b, "revenue_paise", t_rev);
    buf_puts(b, "}");

    /* Top products over the same window. */
    if (db_query(c->db, &r,
                 "SELECT p.id, p.sku, p.name, SUM(s.units), SUM(s.revenue_paise) "
                 "FROM product_sales_daily s JOIN products p ON p.id = s.product_id "
                 "WHERE s.day > CURRENT_DATE - INTERVAL ? DAY "
                 "GROUP BY p.id, p.sku, p.name ORDER BY SUM(s.revenue_paise) DESC LIMIT 10",
                 "i", days) != 0)
        goto db_failed;
    buf_puts(b, ",\"top_products\":[");
    for (int n = 0; db_fetch(&r) == 1; n++) {
        buf_puts(b, n ? ",{" : "{");
        buf_json_kv_int(b, "product_id", db_int(&r, 0));
        buf_puts(b, ",");
        buf_json_kv_str(b, "sku", db_str(&r, 1));
        buf_puts(b, ",");
        buf_json_kv_str(b, "name", db_str(&r, 2));
        buf_puts(b, ",");
        buf_json_kv_int(b, "units", db_int(&r, 3));
        buf_puts(b, ",");
        buf_json_kv_int(b, "revenue_paise", db_int(&r, 4));
        buf_puts(b, "}");
    }
    db_res_free(&r);
    buf_puts(b, "]");

    /* Live counts: order states and payment outcomes. */
    if (db_query(c->db, &r, "SELECT status, COUNT(*) FROM orders GROUP BY status", "") != 0)
        goto db_failed;
    buf_puts(b, ",\"order_status\":{");
    for (int n = 0; db_fetch(&r) == 1; n++) {
        if (n)
            buf_puts(b, ",");
        buf_json_kv_int(b, db_str(&r, 0), db_int(&r, 1));
    }
    db_res_free(&r);
    if (db_query(c->db, &r, "SELECT status, COUNT(*) FROM payments GROUP BY status", "") != 0)
        goto db_failed;
    buf_puts(b, "},\"payments\":{");
    for (int n = 0; db_fetch(&r) == 1; n++) {
        if (n)
            buf_puts(b, ",");
        buf_json_kv_int(b, db_str(&r, 0), db_int(&r, 1));
    }
    db_res_free(&r);

    /* Live low-stock list. */
    if (db_query(c->db, &r,
                 "SELECT p.id, p.sku, p.name, i.stock_on_hand, i.reorder_level "
                 "FROM products p JOIN inventory i ON i.product_id = p.id "
                 "WHERE p.active = 1 AND i.stock_on_hand <= i.reorder_level "
                 "ORDER BY i.stock_on_hand, p.id",
                 "") != 0)
        goto db_failed;
    buf_puts(b, "},\"low_stock\":[");
    for (int n = 0; db_fetch(&r) == 1; n++) {
        buf_puts(b, n ? ",{" : "{");
        buf_json_kv_int(b, "id", db_int(&r, 0));
        buf_puts(b, ",");
        buf_json_kv_str(b, "sku", db_str(&r, 1));
        buf_puts(b, ",");
        buf_json_kv_str(b, "name", db_str(&r, 2));
        buf_puts(b, ",");
        buf_json_kv_int(b, "stock", db_int(&r, 3));
        buf_puts(b, ",");
        buf_json_kv_int(b, "reorder_level", db_int(&r, 4));
        buf_puts(b, "}");
    }
    db_res_free(&r);
    buf_puts(b, "]}");
    return;

db_failed:
    resp_db_error(c);
}

/* GET /api/admin/reports/export?name=daily_sales
 * Returns {"name", "filename", "csv"}; the browser turns it into a download. */
void h_admin_report_export(ctx_t *c)
{
    static const char *const names[] = {"daily_sales", "top_products", "low_stock",
                                        "order_status"};
    char name[32];
    if (of_query_get(c->req->query, "name", name, sizeof name) != 0)
        name[0] = '\0';
    bool known = false;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(name, names[i]) == 0)
            known = true;
    if (!known) { /* a fixed list: no path from the request ever reaches open() */
        resp_error(c, 400, "invalid_report",
                   "name must be daily_sales, top_products, low_stock or order_status");
        return;
    }

    char path[512];
    snprintf(path, sizeof path, "%s/%s.csv",
             of_config_get(c->s->cfg, "report.dir", "run/reports"), name);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        resp_error(c, 404, "no_report", "no %s report yet: is of-report running?", name);
        return;
    }
    struct stat st;
    char *data = NULL;
    if (fstat(fd, &st) != 0 || st.st_size > EXPORT_MAX ||
        !(data = malloc((size_t)st.st_size + 1))) {
        close(fd);
        resp_error(c, 500, "internal", "report unreadable");
        return;
    }
    size_t got = 0;
    while (got < (size_t)st.st_size) {
        ssize_t n = read(fd, data + got, (size_t)st.st_size - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(fd);
    data[got] = '\0';

    buf_t *b = &c->resp->body;
    buf_puts(b, "{");
    buf_json_kv_str(b, "name", name);
    buf_puts(b, ",");
    char filename[64];
    snprintf(filename, sizeof filename, "orderforge-%s.csv", name);
    buf_json_kv_str(b, "filename", filename);
    buf_puts(b, ",");
    buf_json_kv_str(b, "csv", data);
    buf_puts(b, "}");
    free(data);
}
