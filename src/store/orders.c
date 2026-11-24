/*
 * orders.c - checkout, order history and order detail.
 *
 * Checkout runs in one of three stock modes (store.stock_mode), the before-and-after
 * of the oversell benchmark:
 *
 * naive (baseline, unsafe on purpose)
 *     stock = SELECT stock_on_hand ...         -- plain consistent (snapshot) read
 *     if (stock < qty) fail
 *     UPDATE inventory SET stock_on_hand = stock - qty   -- absolute write
 *   Two buyers both read 1 left, both pass the check, both write 0: two sold, one
 *   removed. The CHECK (stock_on_hand >= 0) never fires because each write is
 *   non-negative; the stock_movements ledger still records both sales, so SUM(delta)
 *   drifts below stock_on_hand and the invariant checker sees the oversell.
 *
 * db-lock
 *     SELECT stock_on_hand ... FOR UPDATE      -- row lock held until COMMIT
 *     UPDATE ... SET stock_on_hand = stock_on_hand - n WHERE ... AND stock_on_hand >= n
 *   Rows are locked in ascending product id, so two carts can never wait on each other
 *   in a cycle. Correct, but every buyer of a hot product queues on one row lock.
 *
 * lockfree-shm (default)
 *     reserve all lines with CAS on the shared-memory table (layer 1); a sold-out buyer
 *     is rejected here, without a single MySQL round trip
 *     UPDATE ... guarded as above (layer 2), then COMMIT; on any failure the
 *     reservation is released, on success it is committed
 *   The CHECK constraint (layer 3) and the invariant checker (layer 4) back both up.
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

/*
 * Take l->qty units in MySQL according to the stock mode.
 * Returns 1 ok, 0 not enough stock (*have set when known), -1 db error.
 */
static int take_stock(ctx_t *c, const line_t *l, int64_t *have)
{
    switch (c->s->stock_mode) {
    case STOCK_NAIVE: {
        int rc = db_query_int(c->db, have,
                              "SELECT stock_on_hand FROM inventory WHERE product_id = ?", "i",
                              l->product_id);
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
    case STOCK_DBLOCK: {
        int rc = db_query_int(c->db, have,
                              "SELECT stock_on_hand FROM inventory WHERE product_id = ? "
                              "FOR UPDATE",
                              "i", l->product_id);
        if (rc <= 0)
            return -1;
        if (*have < l->qty)
            return 0;
        break; /* fall through to the guarded update */
    }
    case STOCK_LOCKFREE:
        *have = -1; /* already reserved in shared memory; the update below is layer 2 */
        break;
    }

    int64_t n = db_exec(c->db,
                        "UPDATE inventory SET stock_on_hand = stock_on_hand - ? "
                        "WHERE product_id = ? AND stock_on_hand >= ?",
                        "iii", l->qty, l->product_id, l->qty);
    if (n < 0)
        return -1;
    if (n == 0) {
        /* Only reachable if shared memory and MySQL disagree: log it loudly. */
        LOG_ERROR("stock_drift", "guarded update refused product %lld qty %lld",
                  (long long)l->product_id, (long long)l->qty);
        return 0;
    }
    return 1;
}

void h_checkout(ctx_t *c)
{
    static _Thread_local line_t lines[CHECKOUT_MAX_LINES];
    static _Thread_local stock_line_t held[CHECKOUT_MAX_LINES];
    size_t n = 0, nheld = 0;
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

    /* Layer 1 (lockfree-shm): reserve everything in shared memory, or nothing. The lines
     * are already in ascending product id, which stock_reserve_all keeps. */
    if (c->s->stock_mode == STOCK_LOCKFREE) {
        for (size_t i = 0; i < n; i++) {
            held[i].product_id = (uint64_t)lines[i].product_id;
            held[i].qty = (int32_t)lines[i].qty;
        }
        size_t bad = 0;
        uint64_t retries = 0;
        bool ok = stock_reserve_all(c->s->stock, held, n, &bad, &retries);
        STORE_METRIC_ADD(c->s, cas_retries, retries);
        if (!ok) {
            (void)db_rollback(c->db);
            stock_slot_t *s = stock_find(c->s->stock, held[bad].product_id);
            long long left = s ? atomic_load(&s->available) : 0;
            STORE_METRIC_ADD(c->s, orders_failed, 1);
            resp_error(c, 409, "out_of_stock", "only %lld left of %s", left < 0 ? 0 : left,
                       lines[bad].name);
            return;
        }
        nheld = n;
    }

    if (db_exec(c->db, "INSERT INTO orders (user_id, status, total_paise) VALUES (?, 'placed', ?)",
                "ii", c->user.id, total) != 1)
        goto db_failed;
    int64_t order_id = (int64_t)db_insert_id(c->db);

    char actor[32];
    snprintf(actor, sizeof actor, "customer:%lld", (long long)c->user.id);

    for (size_t i = 0; i < n; i++) {
        const line_t *l = &lines[i];
        int64_t have = 0;
        int rc = take_stock(c, l, &have);
        if (rc < 0)
            goto db_failed;
        if (rc == 0) {
            (void)db_rollback(c->db);
            if (nheld)
                stock_release_all(c->s->stock, held, nheld);
            STORE_METRIC_ADD(c->s, orders_failed, 1);
            if (have >= 0)
                resp_error(c, 409, "out_of_stock", "only %lld left of %s", (long long)have,
                           l->name);
            else
                resp_error(c, 409, "out_of_stock", "%s is out of stock", l->name);
            return;
        }
        if (db_exec(c->db,
                    "INSERT INTO order_items (order_id, product_id, qty, unit_price_paise) "
                    "VALUES (?, ?, ?, ?)",
                    "iiii", order_id, l->product_id, l->qty, l->price) != 1 ||
            db_exec(c->db,
                    "INSERT INTO stock_movements (product_id, delta, reason, order_id, actor) "
                    "VALUES (?, ?, 'sale', ?, ?)",
                    "iiis", l->product_id, -l->qty, order_id, actor) != 1)
            goto db_failed;
    }

    if (order_event(c->db, order_id, NULL, "placed", actor, "order placed") != 0 ||
        db_exec(c->db, "DELETE FROM cart_items WHERE cart_id = ?", "i", cid) < 0 ||
        db_commit(c->db) != 0)
        goto db_failed;

    /* Durable in MySQL: the reservation becomes a sale. */
    if (nheld)
        stock_commit_all(c->s->stock, held, nheld);
    STORE_METRIC_ADD(c->s, orders_total, 1);

    LOG_DEBUG("checkout", "order %lld user %lld total %lld paise lines %zu", (long long)order_id,
              (long long)c->user.id, (long long)total, n);
    c->resp->status = 201;
    if (render_order(c, order_id, 0) != 1)
        resp_db_error(c);
    return;

db_failed:
    /* fail_tx rolls back; whatever was reserved in shared memory goes back on the shelf. */
    fail_tx(c);
    if (nheld)
        stock_release_all(c->s->stock, held, nheld);
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
