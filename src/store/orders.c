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
#include "common/paymsg.h"
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

/* Mock payment-method tokens understood by of-pay. No card data is ever handled. */
static const char *const pay_methods[] = {"tok_success", "tok_decline", "tok_timeout",
                                          "tok_duplicate", "tok_slow", "tok_random"};

static bool valid_method(const char *m)
{
    for (size_t i = 0; i < sizeof pay_methods / sizeof pay_methods[0]; i++)
        if (strcmp(m, pay_methods[i]) == 0)
            return true;
    return false;
}

/* Idempotency-Key: 8..64 of [A-Za-z0-9_-]. */
static bool valid_idem_key(const char *k)
{
    size_t n = strlen(k);
    if (n < 8 || n > 64)
        return false;
    for (size_t i = 0; i < n; i++)
        if (!((k[i] >= 'a' && k[i] <= 'z') || (k[i] >= 'A' && k[i] <= 'Z') ||
              (k[i] >= '0' && k[i] <= '9') || k[i] == '-' || k[i] == '_'))
            return false;
    return true;
}

/* An order already created with this key: answer with it (200), create nothing. */
static bool replay_existing(ctx_t *c, const char *scoped_key)
{
    int64_t existing;
    if (db_query_int(c->db, &existing, "SELECT id FROM orders WHERE idempotency_key = ?", "s",
                     scoped_key) != 1)
        return false;
    LOG_INFO("checkout_replay", "idempotent retry for order %lld", (long long)existing);
    c->resp->status = 200;
    if (render_order(c, existing, c->user.id) != 1)
        resp_db_error(c);
    return true;
}

/* ---- the shared-memory reservation and its write-ahead log records -------------------
 * Order of events for a lockfree-shm checkout:
 *   CAS-reserve in shared memory -> RESERVE(txid) durable in the WAL -> MySQL transaction
 *   (the order row stores wal_txid) -> COMMIT(txid) or RELEASE(txid).
 * If of-core dies anywhere in between, recovery finds the RESERVE without an outcome and
 * looks up orders.wal_txid: found means MySQL committed (keep the units), not found means
 * MySQL rolled back (give them back). */

static void release_held(ctx_t *c, stock_line_t *held, size_t nheld, uint64_t txid)
{
    if (!nheld)
        return;
    stock_release_all(c->s->stock, held, nheld);
    if (txid && c->s->wal) {
        wal_rec_t r = {.type = WAL_RELEASE, .txid = txid};
        wal_append(c->s->wal, &r); /* no wait: a lost RELEASE is resolved by recovery */
    }
}

static void commit_held(ctx_t *c, stock_line_t *held, size_t nheld, uint64_t txid,
                        int64_t order_id)
{
    if (!nheld)
        return;
    stock_commit_all(c->s->stock, held, nheld);
    if (txid && c->s->wal) {
        wal_rec_t r = {.type = WAL_COMMIT, .txid = txid, .order_id = (uint64_t)order_id};
        wal_append(c->s->wal, &r); /* no wait: MySQL already holds the truth */
    }
}

/* Make the reservation durable before MySQL is touched. 0, or -1 if the log failed. */
static int log_reserve(ctx_t *c, const stock_line_t *held, size_t nheld, uint64_t *txid)
{
    *txid = 0;
    if (!c->s->wal || !c->s->next_txid)
        return 0;
    wal_rec_t r = {.type = WAL_RESERVE, .nlines = (uint32_t)nheld};
    r.txid = atomic_fetch_add(c->s->next_txid, 1);
    for (size_t i = 0; i < nheld && i < WAL_MAX_LINES; i++)
        r.lines[i] = held[i];
    uint64_t seq = wal_append(c->s->wal, &r);
    if (seq == 0 || wal_wait(c->s->wal, seq) != 0)
        return -1;
    *txid = r.txid;
    return 0;
}

/*
 * POST /api/checkout   body (optional): {"payment_method": "tok_success"}
 *                      header (optional): Idempotency-Key: <8..64 chars>
 *
 * One transaction creates the order with its items, takes the stock, records the
 * reservations (with a deadline) and a pending payment, and moves the order to
 * payment_pending. After the commit the payment request is handed to of-pay. The
 * order then waits for the gateway's reply, a cancellation, or the expiry reaper.
 */
void h_checkout(ctx_t *c)
{
    static _Thread_local line_t lines[CHECKOUT_MAX_LINES];
    static _Thread_local stock_line_t held[CHECKOUT_MAX_LINES];
    size_t n = 0, nheld = 0;
    int64_t total = 0;
    uint64_t txid = 0;

    const char *method = "tok_success";
    if (c->req->body_len > 0) {
        if (ctx_json(c))
            return;
        const char *m = json_get_str(c->json, "payment_method");
        if (m)
            method = m;
    }
    if (!valid_method(method)) {
        resp_error(c, 400, "invalid_payment_method", "unknown payment method");
        return;
    }

    /* Keys are scoped to the user, so two customers can never collide. */
    char scoped_key[96] = "";
    const char *key = c->req->idem_key ? c->req->idem_key : "";
    if (*key) {
        if (!valid_idem_key(key)) {
            resp_error(c, 400, "invalid_idempotency_key",
                       "Idempotency-Key must be 8-64 letters, digits, '-' or '_'");
            return;
        }
        snprintf(scoped_key, sizeof scoped_key, "u%lld:%s", (long long)c->user.id, key);
        if (replay_existing(c, scoped_key))
            return;
    }

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
        if (log_reserve(c, held, nheld, &txid) != 0) {
            (void)db_rollback(c->db);
            stock_release_all(c->s->stock, held, nheld);
            LOG_ERROR("wal", "write-ahead log unavailable; refusing checkout");
            resp_error(c, 503, "durability", "the store cannot record orders right now");
            return;
        }
    }

    if (db_exec(c->db,
                "INSERT INTO orders (user_id, status, total_paise, idempotency_key, wal_txid) "
                "VALUES (?, 'placed', ?, ?, ?)",
                "iisi", c->user.id, total, scoped_key[0] ? scoped_key : NULL,
                txid ? (int64_t)txid : DB_NULL_INT) != 1) {
        if (db_errno(c->db) == DB_ER_DUP_ENTRY && scoped_key[0]) {
            /* The same key won a race on another worker: give back, answer with theirs. */
            (void)db_rollback(c->db);
            release_held(c, held, nheld, txid);
            if (!replay_existing(c, scoped_key))
                resp_error(c, 409, "conflict", "please retry");
            return;
        }
        goto db_failed;
    }
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
            release_held(c, held, nheld, txid);
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
                    "iiis", l->product_id, -l->qty, order_id, actor) != 1 ||
            db_exec(c->db,
                    "INSERT INTO reservations (order_id, product_id, qty, expires_at) "
                    "VALUES (?, ?, ?, NOW(3) + INTERVAL ? SECOND)",
                    "iiii", order_id, l->product_id, l->qty, c->s->reservation_ttl_s) != 1)
            goto db_failed;
    }

    char pay_key[48];
    snprintf(pay_key, sizeof pay_key, "pay-%lld-1", (long long)order_id);
    if (db_exec(c->db,
                "INSERT INTO payments (order_id, idem_key, method, amount_paise) VALUES (?, ?, ?, ?)",
                "issi", order_id, pay_key, method, total) != 1 ||
        order_event(c->db, order_id, NULL, "placed", actor, "order placed") != 0 ||
        order_move(c->s, c->db, order_id, ORDER_PLACED, ORDER_PAYMENT_PENDING, "of-core",
                   "sent to the payment gateway", NULL) != 1 ||
        db_exec(c->db, "DELETE FROM cart_items WHERE cart_id = ?", "i", cid) < 0 ||
        db_commit(c->db) != 0)
        goto db_failed;

    /* Durable in MySQL: the shared-memory reservation is now held by the order itself. */
    commit_held(c, held, nheld, txid, order_id);
    STORE_METRIC_ADD(c->s, orders_total, 1);

    /* Hand the payment to of-pay. If that fails the order stays payment_pending and the
     * reaper cancels it when the reservation expires: no stock is ever stranded. */
    pay_msg_t req;
    memset(&req, 0, sizeof req);
    req.kind = PAY_REQUEST;
    req.order_id = (uint64_t)order_id;
    req.amount_paise = total;
    snprintf(req.idem_key, sizeof req.idem_key, "%s", pay_key);
    snprintf(req.method, sizeof req.method, "%s", method);
    if (!c->s->pay_submit || c->s->pay_submit(c->s->pay_ctx, &req) != 0)
        LOG_WARN("payment_not_sent", "order %lld: payment outbox full or gateway disabled; "
                                     "it will expire",
                 (long long)order_id);
    else
        STORE_METRIC_ADD(c->s, payments_sent, 1);

    LOG_DEBUG("checkout", "order %lld user %lld total %lld paise lines %zu", (long long)order_id,
              (long long)c->user.id, (long long)total, n);
    c->resp->status = 201;
    if (render_order(c, order_id, 0) != 1)
        resp_db_error(c);
    return;

db_failed:
    /* fail_tx rolls back; whatever was reserved in shared memory goes back on the shelf. */
    fail_tx(c);
    release_held(c, held, nheld, txid);
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
    buf_puts(b, "]");

    /* The latest payment attempt, if any. */
    if (db_query(c->db, &r,
                 "SELECT status, method, provider_ref, note, amount_paise FROM payments "
                 "WHERE order_id = ? ORDER BY id DESC LIMIT 1",
                 "i", order_id) != 0)
        return -1;
    if (db_fetch(&r) == 1) {
        buf_puts(b, ",\"payment\":{");
        buf_json_kv_str(b, "status", db_str(&r, 0));
        buf_puts(b, ",");
        buf_json_kv_str(b, "method", db_str(&r, 1));
        buf_puts(b, ",");
        buf_json_kv_str(b, "provider_ref", db_str(&r, 2));
        buf_puts(b, ",");
        buf_json_kv_str(b, "note", db_str(&r, 3));
        buf_puts(b, ",");
        buf_json_kv_int(b, "amount_paise", db_int(&r, 4));
        buf_puts(b, "}");
    }
    db_res_free(&r);

    /* While unpaid: when the reservation runs out (the UI shows a countdown). */
    if (db_query(c->db, &r,
                 "SELECT " ISO("MIN(expires_at)") ", "
                 "  GREATEST(0, TIMESTAMPDIFF(SECOND, NOW(3), MIN(expires_at))) "
                 "FROM reservations WHERE order_id = ? AND state = 'active'",
                 "i", order_id) != 0)
        return -1;
    if (db_fetch(&r) == 1 && !db_null(&r, 0)) {
        buf_puts(b, ",");
        buf_json_kv_str(b, "reservation_expires_at", db_str(&r, 0));
        buf_puts(b, ",");
        buf_json_kv_int(b, "reservation_seconds_left", db_int(&r, 1));
    }
    db_res_free(&r);
    buf_puts(b, "}}");
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
