/*
 * payments.c - applying gateway replies, and the reservation expiry reaper.
 *
 * Both run on of-core's background thread with its own MySQL connection. They race with
 * each other only in time (the thread handles one event at a time), but they race with
 * customers and admins cancelling from worker threads. Every path therefore locks
 * the order row first (then the payment row) and moves the order with a guarded
 * update, so exactly one of "paid", "declined", "expired" and "cancelled" can win.
 *
 * Payment row states: pending -> succeeded | declined | timed_out | cancelled -> refunded
 * Only a reply that finds the payment 'pending' may act on the order. A duplicated or
 * replayed reply finds it already decided and is ignored.
 */
#include "store/internal.h"

#include "common/log.h"
#include "common/paymsg.h"

#include <stdio.h>
#include <string.h>

static void audit_system(db_t *db, const char *action, int64_t order_id, const char *detail)
{
    char target[48];
    snprintf(target, sizeof target, "order:%lld", (long long)order_id);
    if (db_exec(db, "INSERT INTO audit_log (actor_id, action, target, detail, ip) "
                    "VALUES (NULL, ?, ?, ?, 'of-core')",
                "sss", action, target, detail) < 0)
        LOG_WARN("audit_failed", "could not record %s for order %lld", action, (long long)order_id);
}

int store_payment_reply(store_t *s, const void *reply_p)
{
    const pay_msg_t *m = reply_p;
    db_t *db = s->db;

    /* Which payment? Looked up by its idempotency key, which the gateway echoes. */
    int64_t pay_id = 0, order_id = 0;
    db_res_t r;
    if (db_query(db, &r, "SELECT id, order_id FROM payments WHERE idem_key = ?", "s",
                 m->idem_key) != 0)
        return -1;
    int found = db_fetch(&r);
    if (found == 1) {
        pay_id = db_int(&r, 0);
        order_id = db_int(&r, 1);
    }
    db_res_free(&r);
    if (found != 1 || (uint64_t)order_id != m->order_id) {
        LOG_WARN("payment_unknown", "reply for unknown payment %s (order %llu) ignored",
                 m->idem_key, (unsigned long long)m->order_id);
        STORE_METRIC_ADD(s, payments_ignored, 1);
        return 0;
    }

    if (db_begin(db) != 0)
        return -1;
    order_state_t state;
    if (order_lock(db, order_id, &state, NULL) != 1)
        goto fail;
    char pstatus[16] = "";
    if (db_query(db, &r, "SELECT status FROM payments WHERE id = ? FOR UPDATE", "i", pay_id) != 0)
        goto fail;
    if (db_fetch(&r) == 1)
        snprintf(pstatus, sizeof pstatus, "%s", db_str(&r, 0));
    db_res_free(&r);

    stock_back_t back = {0};
    int changed = 0;
    char note[128];

    if (strcmp(pstatus, "pending") == 0 && state == ORDER_PAYMENT_PENDING) {
        if (m->outcome == PAY_SUCCEEDED) {
            snprintf(note, sizeof note, "payment approved (ref %s)", m->provider_ref);
            if (db_exec(db, "UPDATE payments SET status = 'succeeded', provider_ref = ? WHERE id = ?",
                        "si", m->provider_ref, pay_id) != 1 ||
                order_move(s, db, order_id, state, ORDER_PAID, "of-pay", note, NULL) != 1)
                goto fail;
            STORE_METRIC_ADD(s, payments_succeeded, 1);
        } else {
            snprintf(note, sizeof note, "payment declined (ref %s)", m->provider_ref);
            if (db_exec(db, "UPDATE payments SET status = 'declined', provider_ref = ?, note = ? "
                            "WHERE id = ?",
                        "ssi", m->provider_ref, "declined by the gateway", pay_id) != 1 ||
                order_move(s, db, order_id, state, ORDER_CANCELLED, "of-pay", note, &back) != 1)
                goto fail;
            STORE_METRIC_ADD(s, payments_declined, 1);
        }
        changed = 1;
    } else if (m->outcome == PAY_SUCCEEDED &&
               (strcmp(pstatus, "timed_out") == 0 || strcmp(pstatus, "cancelled") == 0)) {
        /* The money arrived after we gave up on it: give it back, and say so. */
        if (db_exec(db, "UPDATE payments SET status = 'refunded', provider_ref = ?, note = ? "
                        "WHERE id = ?",
                    "ssi", m->provider_ref,
                    "late success after the order was cancelled: refunded automatically",
                    pay_id) != 1)
            goto fail;
        snprintf(note, sizeof note, "late payment (ref %s) refunded automatically",
                 m->provider_ref);
        if (order_event(db, order_id, order_state_name(state), order_state_name(state), "of-pay",
                        note) != 0)
            goto fail;
        audit_system(db, "auto_refund", order_id, note);
        STORE_METRIC_ADD(s, late_refunds, 1);
        LOG_WARN("late_payment", "order %lld: %s", (long long)order_id, note);
        changed = 1;
    } else {
        /* Already decided: a duplicate or replayed reply. Exactly-once means ignoring it. */
        LOG_INFO("payment_duplicate", "order %lld payment %s already %s; reply ignored",
                 (long long)order_id, m->idem_key, pstatus);
        STORE_METRIC_ADD(s, payments_ignored, 1);
    }

    if (db_commit(db) != 0)
        goto fail;
    order_stock_back(s, &back);
    return changed;

fail:
    (void)db_rollback(db);
    return -1;
}

int store_expire_reservations(store_t *s, int limit)
{
    db_t *db = s->db;
    int64_t ids[256];
    int n = 0;
    if (limit > 256)
        limit = 256;

    db_res_t r;
    if (db_query(db, &r,
                 "SELECT DISTINCT order_id FROM reservations "
                 "WHERE state = 'active' AND expires_at <= NOW(3) ORDER BY order_id LIMIT ?",
                 "i", (int64_t)limit) != 0)
        return -1;
    while (n < limit && db_fetch(&r) == 1)
        ids[n++] = db_int(&r, 0);
    db_res_free(&r);

    int expired = 0;
    for (int i = 0; i < n; i++) {
        if (db_begin(db) != 0)
            return -1;
        order_state_t state;
        if (order_lock(db, ids[i], &state, NULL) != 1) {
            (void)db_rollback(db);
            continue;
        }
        if (state != ORDER_PLACED && state != ORDER_PAYMENT_PENDING) {
            (void)db_rollback(db); /* decided meanwhile (paid or cancelled): nothing to do */
            continue;
        }
        stock_back_t back = {0};
        if (db_exec(db, "UPDATE payments SET status = 'timed_out', note = ? "
                        "WHERE order_id = ? AND status = 'pending'",
                    "si", "no reply from the gateway before the reservation expired", ids[i]) < 0 ||
            order_move(s, db, ids[i], state, ORDER_CANCELLED, "reaper",
                       "payment not received in time: reservation expired", &back) != 1 ||
            db_commit(db) != 0) {
            (void)db_rollback(db);
            return -1;
        }
        order_stock_back(s, &back);
        STORE_METRIC_ADD(s, reservations_expired, 1);
        expired++;
        LOG_INFO("reservation_expired", "order %lld cancelled, stock returned", (long long)ids[i]);
    }
    return expired;
}
