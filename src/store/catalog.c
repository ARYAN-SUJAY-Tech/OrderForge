/*
 * catalog.c - public product catalogue: categories, search, filter, pagination.
 */
#include "store/internal.h"

#include "common/util.h"

#include <stdio.h>
#include <string.h>

#define SEARCH_MAX 64

/* Build a LIKE pattern "%term%" with %, _ and \ in the user's term escaped. */
static int like_pattern(const char *term, char *out, size_t outlen)
{
    size_t o = 0;
    out[o++] = '%';
    for (const char *p = term; *p; p++) {
        if (*p == '%' || *p == '_' || *p == '\\') {
            if (o + 2 >= outlen)
                return -1;
            out[o++] = '\\';
        }
        if (o + 2 >= outlen)
            return -1;
        out[o++] = *p;
    }
    out[o++] = '%';
    out[o] = '\0';
    return 0;
}

void h_categories(ctx_t *c)
{
    db_res_t r;
    if (db_query(c->db, &r,
                 "SELECT c.id, c.name, COUNT(p.id) FROM categories c "
                 "LEFT JOIN products p ON p.category_id = c.id AND p.active = 1 "
                 "GROUP BY c.id, c.name ORDER BY c.name",
                 "") != 0) {
        resp_db_error(c);
        return;
    }
    buf_t *b = &c->resp->body;
    buf_puts(b, "{\"categories\":[");
    for (int n = 0; db_fetch(&r) == 1; n++) {
        buf_puts(b, n ? ",{" : "{");
        buf_json_kv_int(b, "id", db_int(&r, 0));
        buf_puts(b, ",");
        buf_json_kv_str(b, "name", db_str(&r, 1));
        buf_puts(b, ",");
        buf_json_kv_int(b, "products", db_int(&r, 2));
        buf_puts(b, "}");
    }
    buf_puts(b, "]}");
    db_res_free(&r);
}

void h_products(ctx_t *c)
{
    const char *qs = c->req->query;
    char term[SEARCH_MAX + 1] = "";
    if (of_query_get(qs, "q", term, sizeof term) != 0)
        term[0] = '\0';
    int64_t category = of_query_get_int(qs, "category", 0, 0, INT32_MAX);
    int64_t page = of_query_get_int(qs, "page", 1, 1, 100000);
    int64_t per_page = of_query_get_int(qs, "per_page", 12, 1, 50);
    int64_t offset = (page - 1) * per_page;

    char pattern[2 * SEARCH_MAX + 4];
    if (like_pattern(term, pattern, sizeof pattern) != 0) {
        resp_error(c, 400, "bad_query", "search term too long");
        return;
    }

    int64_t total = 0;
    if (db_query_int(c->db, &total,
                     "SELECT COUNT(*) FROM products p WHERE p.active = 1 "
                     "AND (? = 0 OR p.category_id = ?) AND (p.name LIKE ? OR p.sku LIKE ?)",
                     "iiss", category, category, pattern, pattern) < 0) {
        resp_db_error(c);
        return;
    }

    db_res_t r;
    if (db_query(c->db, &r,
                 "SELECT " PRODUCT_COLS PRODUCT_FROM
                 " WHERE p.active = 1 AND (? = 0 OR p.category_id = ?)"
                 " AND (p.name LIKE ? OR p.sku LIKE ?)"
                 " ORDER BY p.id LIMIT ? OFFSET ?",
                 "iissii", category, category, pattern, pattern, per_page, offset) != 0) {
        resp_db_error(c);
        return;
    }

    buf_t *b = &c->resp->body;
    buf_puts(b, "{\"products\":[");
    for (int n = 0; db_fetch(&r) == 1; n++) {
        if (n)
            buf_puts(b, ",");
        render_product(b, &r);
    }
    buf_puts(b, "],");
    buf_json_kv_int(b, "page", page);
    buf_puts(b, ",");
    buf_json_kv_int(b, "per_page", per_page);
    buf_puts(b, ",");
    buf_json_kv_int(b, "total", total);
    buf_puts(b, "}");
    db_res_free(&r);
}

void h_product(ctx_t *c)
{
    db_res_t r;
    if (db_query(c->db, &r, "SELECT " PRODUCT_COLS PRODUCT_FROM " WHERE p.id = ? AND p.active = 1",
                 "i", c->id) != 0) {
        resp_db_error(c);
        return;
    }
    int rc = db_fetch(&r);
    if (rc == 1) {
        buf_puts(&c->resp->body, "{\"product\":");
        render_product(&c->resp->body, &r);
        buf_puts(&c->resp->body, "}");
    }
    db_res_free(&r);
    if (rc == 0)
        resp_error(c, 404, "not_found", "product not found");
    else if (rc < 0)
        resp_db_error(c);
}
