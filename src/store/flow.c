/*
 * flow.c - moving orders through the state machine (see common/order_state.h).
 *
 * Every change runs in one MySQL transaction that:
 *   1. locks the order row (SELECT ... FOR UPDATE), so concurrent changes to the same
 *      order queue up instead of interleaving;
 *   2. checks the state machine and the actor's permission;
 *   3. updates with a guard, UPDATE orders SET status = to WHERE id = ? AND status = from,
 *      and requires exactly one affected row: a transition can happen only once;
 *   4. appends the order_events timeline row and applies the side effects;
 *   5. commits, and only then returns stock to shared memory, so units are never
 *      sellable before MySQL durably has them back.
 */
#include "store/internal.h"

#include "common/log.h"

#include <stdio.h>
#include <string.h>

int order_lock(db_t *db, int64_t order_id, order_state_t *state, int64_t *user_id)
{
    db_res_t r;
    if (db_query(db, &r, "SELECT status, user_id FROM orders WHERE id = ? FOR UPDATE", "i",
                 order_id) != 0)
        return -1;
    int rc = db_fetch(&r);
    if (rc == 1) {
        *state = order_state_parse(db_str(&r, 0));
        if (user_id)
            *user_id = db_int(&r, 1);
    }
    db_res_free(&r);
    return rc;
}

/* Return every unit still held by the order (active or committed reservations). */
static int return_stock(db_t *db, int64_t order_id, order_state_t to, const char *actor,
                        stock_back_t *back)
{
    stock_line_t lines[ORDER_MAX_LINES];
    size_t n = 0;
    db_res_t r;
    if (db_query(db, &r,
                 "SELECT product_id, qty FROM reservations "
                 "WHERE order_id = ? AND state IN ('active', 'committed') "
                 "ORDER BY product_id FOR UPDATE",
                 "i", order_id) != 0)
        return -1;
    while (n < ORDER_MAX_LINES && db_fetch(&r) == 1) {
        lines[n].product_id = (uint64_t)db_int(&r, 0);
        lines[n].qty = (int32_t)db_int(&r, 1);
        n++;
    }
    db_res_free(&r);

    const char *reason = to == ORDER_REFUNDED ? "refund" : "release";
    for (size_t i = 0; i < n; i++) {
        if (db_exec(db, "UPDATE inventory SET stock_on_hand = stock_on_hand + ? WHERE product_id = ?",
                    "ii", (int64_t)lines[i].qty, (int64_t)lines[i].product_id) != 1 ||
            db_exec(db,
                    "INSERT INTO stock_movements (product_id, delta, reason, order_id, actor) "
                    "VALUES (?, ?, ?, ?, ?)",
                    "iisis", (int64_t)lines[i].product_id, (int64_t)lines[i].qty, reason,
                    order_id, actor) != 1)
            return -1;
        if (back && back->n < ORDER_MAX_LINES)
            back->lines[back->n++] = lines[i];
    }
    if (n && db_exec(db,
                     "UPDATE reservations SET state = 'released' "
                     "WHERE order_id = ? AND state IN ('active', 'committed')",
                     "i", order_id) < 0)
        return -1;
    return 0;
}

int order_move(store_t *s, db_t *db, int64_t order_id, order_state_t from, order_state_t to,
               const char *actor, const char *note, stock_back_t *back)
{
    (void)s;
    if (!order_transition_valid(from, to))
        return 0;
    int64_t n = db_exec(db, "UPDATE orders SET status = ? WHERE id = ? AND status = ?", "sis",
                        order_state_name(to), order_id, order_state_name(from));
    if (n < 0)
        return -1;
    if (n == 0)
        return 0; /* someone else moved it first */

    if (order_event(db, order_id, order_state_name(from), order_state_name(to), actor, note) != 0)
        return -1;

    if (to == ORDER_PAID &&
        db_exec(db, "UPDATE reservations SET state = 'committed' WHERE order_id = ? AND state = 'active'",
                "i", order_id) < 0)
        return -1;
    if (order_transition_returns_stock(from, to) && return_stock(db, order_id, to, actor, back) != 0)
        return -1;
    return 1;
}

void order_stock_back(store_t *s, const stock_back_t *back)
{
    /* Only lockfree-shm takes units out of shared memory at checkout, so only it puts
     * them back; in the other modes shared memory is not on the checkout path. */
    if (!back || !s->stock || s->stock_mode != STOCK_LOCKFREE)
        return;
    for (size_t i = 0; i < back->n; i++) {
        stock_slot_t *slot = stock_find(s->stock, back->lines[i].product_id);
        if (slot)
            stock_adjust(slot, back->lines[i].qty);
    }
}

int order_change(store_t *s, int64_t order_id, order_state_t to, order_actor_t actor,
                 int64_t owner_only, const char *actor_name, const char *note,
                 order_state_t *cur, order_state_t *result)
{
    db_t *db = s->db;
    if (db_begin(db) != 0)
        return -1;

    order_state_t from;
    int64_t owner = 0;
    int rc = order_lock(db, order_id, &from, &owner);
    if (rc <= 0 || (owner_only > 0 && owner != owner_only)) {
        (void)db_rollback(db);
        return rc < 0 ? -1 : -2;
    }
    if (cur)
        *cur = from;

    if (to == ORDER_STATE_INVALID) /* "cancel": whichever exit fits the current state */
        to = (from == ORDER_PAID || from == ORDER_PACKED) ? ORDER_REFUNDED : ORDER_CANCELLED;
    if (result)
        *result = to;
    if (!order_transition_allowed(from, to, actor)) {
        (void)db_rollback(db);
        return 0;
    }

    /* The payment follows the order out: an unpaid one is cancelled, a paid one refunded. */
    if (to == ORDER_CANCELLED &&
        db_exec(db, "UPDATE payments SET status = 'cancelled', note = ? "
                    "WHERE order_id = ? AND status = 'pending'",
                "si", note ? note : "", order_id) < 0)
        goto fail;
    if (to == ORDER_REFUNDED &&
        db_exec(db, "UPDATE payments SET status = 'refunded', note = ? "
                    "WHERE order_id = ? AND status = 'succeeded'",
                "si", note ? note : "", order_id) < 0)
        goto fail;

    stock_back_t back = {0};
    rc = order_move(s, db, order_id, from, to, actor_name, note, &back);
    if (rc < 0)
        goto fail;
    if (rc == 0) {
        (void)db_rollback(db);
        return 0;
    }
    if (db_commit(db) != 0)
        goto fail;
    order_stock_back(s, &back);
    return 1;

fail:
    (void)db_rollback(db);
    return -1;
}

/* ---------------------------------------------------------------- API handlers */

/* POST /api/orders/{id}/cancel : the customer backs out (cancel if unpaid, refund if paid). */
void h_order_cancel(ctx_t *c)
{
    char actor[32];
    snprintf(actor, sizeof actor, "customer:%lld", (long long)c->user.id);
    order_state_t cur = ORDER_STATE_INVALID, to = ORDER_STATE_INVALID;
    int rc = order_change(c->s, c->id, ORDER_STATE_INVALID, ACTOR_CUSTOMER,
                          c->user.is_admin ? 0 : c->user.id, actor, "cancelled by the customer",
                          &cur, &to);
    if (rc == -2) {
        resp_error(c, 404, "not_found", "order not found");
    } else if (rc < 0) {
        resp_db_error(c);
    } else if (rc == 0) {
        resp_error(c, 409, "not_cancellable", "an order that is %s can no longer be cancelled",
                   order_state_name(cur));
    } else {
        char target[48];
        snprintf(target, sizeof target, "order:%lld", (long long)c->id);
        audit(c, to == ORDER_REFUNDED ? "order_refund" : "order_cancel", target, "by customer");
        if (render_order(c, c->id, 0) != 1)
            resp_db_error(c);
    }
}

/* POST /api/admin/orders/{id}/status {"status": "packed", "note": "..."} */
void h_admin_order_status(ctx_t *c)
{
    if (ctx_json(c))
        return;
    order_state_t to = order_state_parse(json_get_str(c->json, "status"));
    const char *note = json_get_str(c->json, "note");
    if (to == ORDER_STATE_INVALID) {
        resp_error(c, 400, "invalid_status", "status must be one of the order states");
        return;
    }
    if (note && strlen(note) > 200) {
        resp_error(c, 400, "invalid_note", "note is limited to 200 characters");
        return;
    }
    char actor[32], default_note[64];
    snprintf(actor, sizeof actor, "admin:%lld", (long long)c->user.id);
    snprintf(default_note, sizeof default_note, "marked %s by an administrator",
             order_state_name(to));

    order_state_t cur = ORDER_STATE_INVALID;
    int rc = order_change(c->s, c->id, to, ACTOR_ADMIN, 0, actor,
                          note && *note ? note : default_note, &cur, NULL);
    if (rc == -2) {
        resp_error(c, 404, "not_found", "order not found");
    } else if (rc < 0) {
        resp_db_error(c);
    } else if (rc == 0) {
        resp_error(c, 409, "invalid_transition", "an order cannot go from %s to %s",
                   order_state_name(cur), order_state_name(to));
    } else {
        char target[48], detail[64];
        snprintf(target, sizeof target, "order:%lld", (long long)c->id);
        snprintf(detail, sizeof detail, "%s -> %s", order_state_name(cur), order_state_name(to));
        audit(c, "order_status", target, detail);
        if (render_order(c, c->id, 0) != 1)
            resp_db_error(c);
    }
}
