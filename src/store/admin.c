/*
 * admin.c - product management, stock adjustment and the order board (read-only in
 * phase 1; transitions arrive with the state machine in phase 3).
 *
 * Every change is written to audit_log. Stock changes made here are atomic and
 * relative (stock = stock + delta, guarded against going negative) and each one is
 * recorded in the stock_movements ledger.
 */
#include "store/internal.h"

#include "common/util.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define PRICE_MAX  10000000000LL  /* Rs 10 crore, in paise */
#define STOCK_MAX  1000000

static bool valid_sku(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n < 1 || n > 32)
        return false;
    for (size_t i = 0; i < n; i++)
        if (!isupper((unsigned char)s[i]) && !isdigit((unsigned char)s[i]) && s[i] != '-')
            return false;
    return true;
}

static bool valid_text(const char *s, size_t min, size_t max)
{
    size_t n = s ? strlen(s) : 0;
    if (n < min || n > max)
        return false;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] < 0x20 && s[i] != '\n')
            return false;
    return true;
}

static void render_one(ctx_t *c, int64_t id)
{
    db_res_t r;
    if (db_query(c->db, &r, "SELECT " PRODUCT_COLS PRODUCT_FROM " WHERE p.id = ?", "i", id) != 0) {
        resp_db_error(c);
        return;
    }
    int rc = db_fetch(&r);
    if (rc == 1) {
        buf_reset(&c->resp->body);
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

void h_admin_products(ctx_t *c)
{
    int64_t low_only = of_query_get_int(c->req->query, "low_stock", 0, 0, 1);
    db_res_t r;
    if (db_query(c->db, &r,
                 "SELECT " PRODUCT_COLS PRODUCT_FROM
                 " WHERE (? = 0 OR (p.active = 1 AND i.stock_on_hand <= i.reorder_level))"
                 " ORDER BY p.active DESC, p.id",
                 "i", low_only) != 0) {
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
    buf_puts(b, "]}");
    db_res_free(&r);
}

/* POST {"sku","name","description","price_paise","category_id","stock","reorder_level"} */
void h_admin_product_create(ctx_t *c)
{
    if (ctx_json(c))
        return;
    const char *sku = json_get_str(c->json, "sku");
    const char *name = json_get_str(c->json, "name");
    const char *desc = json_get_str(c->json, "description");
    int64_t price, category, stock = 0, reorder = 10;

    if (!valid_sku(sku)) {
        resp_error(c, 400, "invalid_sku", "sku must be 1-32 characters of A-Z, 0-9 and '-'");
        return;
    }
    if (!valid_text(name, 1, 120)) {
        resp_error(c, 400, "invalid_name", "name must be 1 to 120 characters");
        return;
    }
    if (!desc)
        desc = "";
    if (!valid_text(desc, 0, 2000)) {
        resp_error(c, 400, "invalid_description", "description is limited to 2000 characters");
        return;
    }
    if (json_get_int(c->json, "price_paise", &price) != 0 || price < 0 || price > PRICE_MAX) {
        resp_error(c, 400, "invalid_price", "price_paise must be a non-negative integer");
        return;
    }
    if (json_get_int(c->json, "category_id", &category) != 0 || category <= 0) {
        resp_error(c, 400, "invalid_category", "category_id is required");
        return;
    }
    if (json_get(c->json, "stock") &&
        (json_get_int(c->json, "stock", &stock) != 0 || stock < 0 || stock > STOCK_MAX)) {
        resp_error(c, 400, "invalid_stock", "stock must be 0 to %d", STOCK_MAX);
        return;
    }
    if (json_get(c->json, "reorder_level") &&
        (json_get_int(c->json, "reorder_level", &reorder) != 0 || reorder < 0 ||
         reorder > STOCK_MAX)) {
        resp_error(c, 400, "invalid_reorder_level", "reorder_level must be 0 to %d", STOCK_MAX);
        return;
    }

    if (db_begin(c->db) != 0) {
        resp_db_error(c);
        return;
    }
    if (db_exec(c->db,
                "INSERT INTO products (sku, name, description, price_paise, category_id) "
                "VALUES (?, ?, ?, ?, ?)",
                "sssii", sku, name, desc, price, category) != 1) {
        unsigned e = db_errno(c->db);
        (void)db_rollback(c->db);
        if (e == DB_ER_DUP_ENTRY)
            resp_error(c, 409, "sku_taken", "a product with sku %s already exists", sku);
        else if (e == DB_ER_NO_REFERENCED)
            resp_error(c, 400, "invalid_category", "no such category");
        else
            resp_db_error(c);
        return;
    }
    int64_t id = (int64_t)db_insert_id(c->db);
    char actor[32];
    snprintf(actor, sizeof actor, "admin:%lld", (long long)c->user.id);

    if (db_exec(c->db,
                "INSERT INTO inventory (product_id, stock_on_hand, reorder_level) VALUES (?, ?, ?)",
                "iii", id, stock, reorder) != 1 ||
        db_exec(c->db,
                "INSERT INTO stock_movements (product_id, delta, reason, actor) "
                "VALUES (?, ?, 'initial', ?)",
                "iis", id, stock, actor) != 1 ||
        db_commit(c->db) != 0) {
        (void)db_rollback(c->db);
        resp_db_error(c);
        return;
    }

    char target[48];
    snprintf(target, sizeof target, "product:%lld", (long long)id);
    audit(c, "product_create", target, sku);
    render_one(c, id);
    if (c->resp->status == 200)
        c->resp->status = 201;
}

/* PUT with any subset of {"name","description","price_paise","category_id",
 * "reorder_level","active"}. Absent fields keep their value (bound as NULL + COALESCE). */
void h_admin_product_update(ctx_t *c)
{
    if (ctx_json(c))
        return;
    const char *name = json_get_str(c->json, "name");
    const char *desc = json_get_str(c->json, "description");
    int64_t price = DB_NULL_INT, category = DB_NULL_INT, reorder = DB_NULL_INT,
            active = DB_NULL_INT;

    if (json_get(c->json, "name") && !valid_text(name, 1, 120)) {
        resp_error(c, 400, "invalid_name", "name must be 1 to 120 characters");
        return;
    }
    if (json_get(c->json, "description") && !valid_text(desc, 0, 2000)) {
        resp_error(c, 400, "invalid_description", "description is limited to 2000 characters");
        return;
    }
    if (json_get(c->json, "price_paise") &&
        (json_get_int(c->json, "price_paise", &price) != 0 || price < 0 || price > PRICE_MAX)) {
        resp_error(c, 400, "invalid_price", "price_paise must be a non-negative integer");
        return;
    }
    if (json_get(c->json, "category_id") &&
        (json_get_int(c->json, "category_id", &category) != 0 || category <= 0)) {
        resp_error(c, 400, "invalid_category", "category_id must be positive");
        return;
    }
    if (json_get(c->json, "reorder_level") &&
        (json_get_int(c->json, "reorder_level", &reorder) != 0 || reorder < 0 ||
         reorder > STOCK_MAX)) {
        resp_error(c, 400, "invalid_reorder_level", "reorder_level must be 0 to %d", STOCK_MAX);
        return;
    }
    if (json_get(c->json, "active")) {
        bool a;
        if (json_get_bool(c->json, "active", &a) != 0) {
            resp_error(c, 400, "invalid_active", "active must be true or false");
            return;
        }
        active = a ? 1 : 0;
    }

    int64_t exists;
    int rc = db_query_int(c->db, &exists, "SELECT 1 FROM products WHERE id = ?", "i", c->id);
    if (rc == 0) {
        resp_error(c, 404, "not_found", "product not found");
        return;
    }
    if (rc < 0 || db_begin(c->db) != 0) {
        resp_db_error(c);
        return;
    }
    if (db_exec(c->db,
                "UPDATE products SET name = COALESCE(?, name), "
                "  description = COALESCE(?, description), "
                "  price_paise = COALESCE(?, price_paise), "
                "  category_id = COALESCE(?, category_id), "
                "  active = COALESCE(?, active) WHERE id = ?",
                "ssiiii", name, desc, price, category, active, c->id) < 0 ||
        db_exec(c->db,
                "UPDATE inventory SET reorder_level = COALESCE(?, reorder_level) "
                "WHERE product_id = ?",
                "ii", reorder, c->id) < 0) {
        unsigned e = db_errno(c->db);
        (void)db_rollback(c->db);
        if (e == DB_ER_NO_REFERENCED)
            resp_error(c, 400, "invalid_category", "no such category");
        else
            resp_db_error(c);
        return;
    }
    if (db_commit(c->db) != 0) {
        resp_db_error(c);
        return;
    }

    char target[48];
    snprintf(target, sizeof target, "product:%lld", (long long)c->id);
    audit(c, "product_update", target, "");
    render_one(c, c->id);
}

/* Soft delete: the row stays so past orders keep a valid product reference. */
void h_admin_product_delete(ctx_t *c)
{
    int64_t n = db_exec(c->db, "UPDATE products SET active = 0 WHERE id = ?", "i", c->id);
    if (n < 0) {
        resp_db_error(c);
        return;
    }
    int64_t exists;
    int rc = db_query_int(c->db, &exists, "SELECT 1 FROM products WHERE id = ?", "i", c->id);
    if (rc == 0) {
        resp_error(c, 404, "not_found", "product not found");
        return;
    }
    if (rc < 0) {
        resp_db_error(c);
        return;
    }
    /* Remove it from every cart so nobody can check it out. */
    (void)db_exec(c->db, "DELETE FROM cart_items WHERE product_id = ?", "i", c->id);

    char target[48];
    snprintf(target, sizeof target, "product:%lld", (long long)c->id);
    audit(c, "product_delete", target, "soft delete");
    render_one(c, c->id);
}

/* POST {"delta": 25, "reason": "restock" | "adjust"}. Negative deltas are allowed for
 * "adjust" (breakage, recount) but can never take stock below zero. */
void h_admin_stock(ctx_t *c)
{
    if (ctx_json(c))
        return;
    int64_t delta;
    const char *reason = json_get_str(c->json, "reason");
    if (json_get_int(c->json, "delta", &delta) != 0 || delta == 0 || delta < -STOCK_MAX ||
        delta > STOCK_MAX) {
        resp_error(c, 400, "invalid_delta", "delta must be a non-zero integer");
        return;
    }
    if (!reason)
        reason = delta > 0 ? "restock" : "adjust";
    if (strcmp(reason, "restock") != 0 && strcmp(reason, "adjust") != 0) {
        resp_error(c, 400, "invalid_reason", "reason must be restock or adjust");
        return;
    }
    if (strcmp(reason, "restock") == 0 && delta < 0) {
        resp_error(c, 400, "invalid_delta", "a restock must add stock");
        return;
    }

    char actor[32];
    snprintf(actor, sizeof actor, "admin:%lld", (long long)c->user.id);

    if (db_begin(c->db) != 0) {
        resp_db_error(c);
        return;
    }
    /* Relative, guarded update: atomic in InnoDB, so it is safe against concurrent sales. */
    int64_t n = db_exec(c->db,
                        "UPDATE inventory SET stock_on_hand = stock_on_hand + ? "
                        "WHERE product_id = ? AND stock_on_hand + ? >= 0",
                        "iii", delta, c->id, delta);
    if (n < 0) {
        (void)db_rollback(c->db);
        resp_db_error(c);
        return;
    }
    if (n == 0) {
        (void)db_rollback(c->db);
        int64_t exists;
        if (db_query_int(c->db, &exists, "SELECT 1 FROM inventory WHERE product_id = ?", "i",
                         c->id) == 1)
            resp_error(c, 409, "insufficient_stock", "stock cannot go below zero");
        else
            resp_error(c, 404, "not_found", "product not found");
        return;
    }
    if (db_exec(c->db,
                "INSERT INTO stock_movements (product_id, delta, reason, actor) VALUES (?, ?, ?, ?)",
                "iiss", c->id, delta, reason, actor) != 1 ||
        db_commit(c->db) != 0) {
        (void)db_rollback(c->db);
        resp_db_error(c);
        return;
    }

    char target[48], detail[64];
    snprintf(target, sizeof target, "product:%lld", (long long)c->id);
    snprintf(detail, sizeof detail, "%s %+lld", reason, (long long)delta);
    audit(c, "stock_change", target, detail);
    render_one(c, c->id);
}

void h_admin_orders(ctx_t *c)
{
    char status[24] = "";
    if (of_query_get(c->req->query, "status", status, sizeof status) != 0)
        status[0] = '\0';
    int64_t page = of_query_get_int(c->req->query, "page", 1, 1, 100000);
    int64_t per_page = of_query_get_int(c->req->query, "per_page", 25, 1, 100);

    int64_t total;
    if (db_query_int(c->db, &total, "SELECT COUNT(*) FROM orders WHERE (? = '' OR status = ?)",
                     "ss", status, status) < 0) {
        resp_db_error(c);
        return;
    }

    db_res_t r;
    if (db_query(c->db, &r,
                 "SELECT o.id, u.email, o.status, o.total_paise, " ISO("o.created_at") ", "
                 ISO("o.updated_at") ", "
                 "  (SELECT COALESCE(SUM(qty), 0) FROM order_items WHERE order_id = o.id) "
                 "FROM orders o JOIN users u ON u.id = o.user_id "
                 "WHERE (? = '' OR o.status = ?) ORDER BY o.id DESC LIMIT ? OFFSET ?",
                 "ssii", status, status, per_page, (page - 1) * per_page) != 0) {
        resp_db_error(c);
        return;
    }
    buf_t *b = &c->resp->body;
    buf_puts(b, "{\"orders\":[");
    for (int n = 0; db_fetch(&r) == 1; n++) {
        buf_puts(b, n ? ",{" : "{");
        buf_json_kv_int(b, "id", db_int(&r, 0));
        buf_puts(b, ",");
        buf_json_kv_str(b, "email", db_str(&r, 1));
        buf_puts(b, ",");
        buf_json_kv_str(b, "status", db_str(&r, 2));
        buf_puts(b, ",");
        buf_json_kv_int(b, "total_paise", db_int(&r, 3));
        buf_puts(b, ",");
        buf_json_kv_str(b, "created_at", db_str(&r, 4));
        buf_puts(b, ",");
        buf_json_kv_str(b, "updated_at", db_str(&r, 5));
        buf_puts(b, ",");
        buf_json_kv_int(b, "units", db_int(&r, 6));
        buf_puts(b, "}");
    }
    buf_puts(b, "],");
    buf_json_kv_int(b, "page", page);
    buf_puts(b, ",");
    buf_json_kv_int(b, "total", total);
    buf_puts(b, "}");
    db_res_free(&r);
}
