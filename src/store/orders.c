/*
 * orders.c - checkout, order history and order detail.
 *
 * Phase 1 checkout uses the NAIVE stock update on purpose. It is the "before" in the
 * before-and-after benchmark:
 *
 *     stock = SELECT stock_on_hand ...         -- plain consistent (snapshot) read
 *     if (stock < qty) fail
 *     UPDATE inventory SET stock_on_hand = stock - qty   -- absolute write
 *
 * Two buyers can both read 1 unit left, both pass the check, and both write 0: two
 * units sold, one removed. The CHECK (stock_on_hand >= 0) never fires because each
 * write is non-negative. The oversell is still visible: every sale is also written to
 * the stock_movements ledger, so SUM(delta) drifts below zero against stock_on_hand.
 * Phase 2 adds the db-lock and lock-free modes that close the race.
 */
#include "store/internal.h"

#include "common/log.h"
#include "common/util.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECKOUT_MAX_LINES 50

typedef struct {
    int64_t product_id;
    int64_t qty;
    int64_t price;
    bool    active;
    char    name[121];
} line_t;

static void fail_tx(ctx_t *c)
{
    (void)db_rollback(c->db);
    if (db_errno(c->db) == DB_ER_LOCK_DEADLOCK || db_errno(c->db) == DB_ER_LOCK_WAIT)
        resp_error(c, 409, "conflict", "the store is busy, please retry");
    else
        resp_db_error(c);
}

/* Naive read-modify-write. Returns 1 ok, 0 not enough stock (*have set), -1 db error. */
static int take_stock_naive(ctx_t *c, const line_t *l, int64_t *have)
{
    int rc = db_query_int(c->db, have, "SELECT stock_on_hand FROM inventory WHERE product_id = ?",
                          "i", l->product_id);
    if (rc <= 0)
        return -1;
    if (*have < l->qty)
        return 0;
    if (c->s->naive_delay_us > 0)
        usleep((useconds_t)c->s->naive_delay_us); /* widen the race window for demos */
    if (db_exec(c->db, "UPDATE inventory SET stock_on_hand = ? WHERE product_id = ?", "ii",
                *have - l->qty, l->product_id) < 0)
        return -1;
    return 1;
}

void h_checkout(ctx_t *c)
{
    static _Thread_local line_t lines[CHECKOUT_MAX_LINES];
    size_t n = 0;
    int64_t total = 0;

    int64_t cid = cart_id(c);
    if (cid < 0 || db_begin(c->db) != 0) {
        resp_db_error(c);
        return;
    }

    db_res_t r;
    if (db_query(c->db, &r,
                 "SELECT ci.product_id, ci.qty, p.price_paise, p.active, p.name "
                 "FROM cart_items ci JOIN products p ON p.id = ci.product_id "
                 "WHERE ci.cart_id = ? ORDER BY ci.product_id",
                 "i", cid) != 0) {
        fail_tx(c);
        return;
    }
    while (n < CHECKOUT_MAX_LINES && db_fetch(&r) == 1) {
        line_t *l = &lines[n++];
        l->product_id = db_int(&r, 0);
        l->qty = db_int(&r, 1);
        l->price = db_int(&r, 2);
        l->active = db_int(&r, 3) != 0;
        snprintf(l->name, sizeof l->name, "%s", db_str(&r, 4));
        total += l->price * l->qty;
    }
    db_res_free(&r);

    if (n == 0) {
        (void)db_rollback(c->db);
        resp_error(c, 400, "cart_empty", "your cart is empty");
        return;
    }
    for (size_t i = 0; i < n; i++) {
        if (!lines[i].active) {
            (void)db_rollback(c->db);
            resp_error(c, 409, "unavailable", "%s is no longer available", lines[i].name);
            return;
        }
    }

    if (db_exec(c->db, "INSERT INTO orders (user_id, status, total_paise) VALUES (?, 'placed', ?)",
                "ii", c->user.id, total) != 1) {
        fail_tx(c);
        return;
    }
    int64_t order_id = (int64_t)db_insert_id(c->db);

    char actor[32];
    snprintf(actor, sizeof actor, "customer:%lld", (long long)c->user.id);

    /* Lines are in ascending product id order (ORDER BY above), so concurrent
     * checkouts always lock inventory rows in the same order. */
    for (size_t i = 0; i < n; i++) {
        const line_t *l = &lines[i];
        int64_t have = 0;
        int rc = take_stock_naive(c, l, &have);
        if (rc < 0) {
            fail_tx(c);
            return;
        }
        if (rc == 0) {
            (void)db_rollback(c->db);
            resp_error(c, 409, "out_of_stock", "only %lld left of %s", (long long)have, l->name);
            return;
        }
        if (db_exec(c->db,
                    "INSERT INTO order_items (order_id, product_id, qty, unit_price_paise) "
                    "VALUES (?, ?, ?, ?)",
                    "iiii", order_id, l->product_id, l->qty, l->price) != 1 ||
            db_exec(c->db,
                    "INSERT INTO stock_movements (product_id, delta, reason, order_id, actor) "
                    "VALUES (?, ?, 'sale', ?, ?)",
                    "iiis", l->product_id, -l->qty, order_id, actor) != 1) {
            fail_tx(c);
            return;
        }
    }

    if (order_event(c->db, order_id, NULL, "placed", actor, "order placed") != 0 ||
        db_exec(c->db, "DELETE FROM cart_items WHERE cart_id = ?", "i", cid) < 0 ||
        db_commit(c->db) != 0) {
        fail_tx(c);
        return;
    }

    LOG_INFO("checkout", "order %lld user %lld total %lld paise lines %zu", (long long)order_id,
             (long long)c->user.id, (long long)total, n);
    c->resp->status = 201;
    if (render_order(c, order_id, 0) != 1)
        resp_db_error(c);
}

int render_order(ctx_t *c, int64_t order_id, int64_t owner_only)
{
    buf_t *b = &c->resp->body;
    db_res_t r;
    if (db_query(c->db, &r,
                 "SELECT o.id, o.user_id, u.email, o.status, o.total_paise, "
                 ISO("o.created_at") ", " ISO("o.updated_at")
                 " FROM orders o JOIN users u ON u.id = o.user_id WHERE o.id = ?",
                 "i", order_id) != 0)
        return -1;
    int rc = db_fetch(&r);
    if (rc != 1 || (owner_only && db_int(&r, 1) != owner_only)) {
        db_res_free(&r);
        return rc < 0 ? -1 : 0;
    }

    buf_reset(b);
    buf_puts(b, "{\"order\":{");
    buf_json_kv_int(b, "id", db_int(&r, 0));
    buf_puts(b, ",");
    buf_json_kv_int(b, "user_id", db_int(&r, 1));
    buf_puts(b, ",");
    buf_json_kv_str(b, "email", db_str(&r, 2));
    buf_puts(b, ",");
    buf_json_kv_str(b, "status", db_str(&r, 3));
    buf_puts(b, ",");
    buf_json_kv_int(b, "total_paise", db_int(&r, 4));
    buf_puts(b, ",");
    buf_json_kv_str(b, "created_at", db_str(&r, 5));
    buf_puts(b, ",");
    buf_json_kv_str(b, "updated_at", db_str(&r, 6));
    db_res_free(&r);

    if (db_query(c->db, &r,
                 "SELECT oi.product_id, p.sku, p.name, oi.qty, oi.unit_price_paise "
                 "FROM order_items oi JOIN products p ON p.id = oi.product_id "
                 "WHERE oi.order_id = ? ORDER BY oi.product_id",
                 "i", order_id) != 0)
        return -1;
    buf_puts(b, ",\"items\":[");
    for (int n = 0; db_fetch(&r) == 1; n++) {
        buf_puts(b, n ? ",{" : "{");
        buf_json_kv_int(b, "product_id", db_int(&r, 0));
        buf_puts(b, ",");
        buf_json_kv_str(b, "sku", db_str(&r, 1));
        buf_puts(b, ",");
        buf_json_kv_str(b, "name", db_str(&r, 2));
        buf_puts(b, ",");
        buf_json_kv_int(b, "qty", db_int(&r, 3));
        buf_puts(b, ",");
        buf_json_kv_int(b, "unit_price_paise", db_int(&r, 4));
        buf_puts(b, "}");
    }
    db_res_free(&r);

    if (db_query(c->db, &r,
                 "SELECT from_state, to_state, actor, note, " ISO("at")
                 " FROM order_events WHERE order_id = ? ORDER BY id",
                 "i", order_id) != 0)
        return -1;
    buf_puts(b, "],\"events\":[");
    for (int n = 0; db_fetch(&r) == 1; n++) {
        buf_puts(b, n ? ",{" : "{");
        buf_json_kv_str(b, "from", db_str(&r, 0));
        buf_puts(b, ",");
        buf_json_kv_str(b, "to", db_str(&r, 1));
        buf_puts(b, ",");
        buf_json_kv_str(b, "actor", db_str(&r, 2));
        buf_puts(b, ",");
        buf_json_kv_str(b, "note", db_str(&r, 3));
        buf_puts(b, ",");
        buf_json_kv_str(b, "at", db_str(&r, 4));
        buf_puts(b, "}");
    }
    db_res_free(&r);
    buf_puts(b, "]}}");
    return 1;
}

void h_orders_mine(ctx_t *c)
{
    int64_t page = of_query_get_int(c->req->query, "page", 1, 1, 100000);
    int64_t per_page = 20;
    db_res_t r;
    if (db_query(c->db, &r,
                 "SELECT o.id, o.status, o.total_paise, " ISO("o.created_at") ", "
                 "  (SELECT COALESCE(SUM(qty), 0) FROM order_items WHERE order_id = o.id) "
                 "FROM orders o WHERE o.user_id = ? ORDER BY o.id DESC LIMIT ? OFFSET ?",
                 "iii", c->user.id, per_page, (page - 1) * per_page) != 0) {
        resp_db_error(c);
        return;
    }
    buf_t *b = &c->resp->body;
    buf_puts(b, "{\"orders\":[");
    for (int n = 0; db_fetch(&r) == 1; n++) {
        buf_puts(b, n ? ",{" : "{");
        buf_json_kv_int(b, "id", db_int(&r, 0));
        buf_puts(b, ",");
        buf_json_kv_str(b, "status", db_str(&r, 1));
        buf_puts(b, ",");
        buf_json_kv_int(b, "total_paise", db_int(&r, 2));
        buf_puts(b, ",");
        buf_json_kv_str(b, "created_at", db_str(&r, 3));
        buf_puts(b, ",");
        buf_json_kv_int(b, "units", db_int(&r, 4));
        buf_puts(b, "}");
    }
    buf_puts(b, "],");
    buf_json_kv_int(b, "page", page);
    buf_puts(b, "}");
    db_res_free(&r);
}

void h_order_get(ctx_t *c)
{
    /* Customers see only their own orders; a 404 (not 403) avoids confirming that
     * someone else's order id exists. Admins see every order. */
    int rc = render_order(c, c->id, c->user.is_admin ? 0 : c->user.id);
    if (rc == 0)
        resp_error(c, 404, "not_found", "order not found");
    else if (rc < 0)
        resp_db_error(c);
}
