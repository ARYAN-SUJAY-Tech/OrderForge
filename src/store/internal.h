/*
 * internal.h - shared by the store's handler files. Not part of the public API.
 */
#ifndef OF_STORE_INTERNAL_H
#define OF_STORE_INTERNAL_H

#include "common/json.h"
#include "common/order_state.h"
#include "store/store.h"

#include <stdbool.h>
#include <stdint.h>

/* Passing DB_NULL_INT for an 'i' parameter binds SQL NULL (used for partial updates). */
#define DB_NULL_INT INT64_MIN

typedef struct {
    int64_t id;
    char    email[255];
    char    name[101];
    bool    is_admin;
    char    csrf[65];
    char    token_hash[65];
} user_t;

typedef struct {
    store_t        *s;
    db_t           *db;
    const of_req_t *req;
    of_resp_t      *resp;
    bool            authed;
    user_t          user;
    int64_t         id;    /* the ":id" captured from the route, if any */
    json_obj_t     *json;  /* parsed body, set by ctx_json() */
} ctx_t;

typedef void (*handler_fn)(ctx_t *c);

/* ---- responses */
void resp_error(ctx_t *c, int status, const char *code, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
void resp_db_error(ctx_t *c);                  /* 503 if the db is down, else 500 */
void resp_ok_empty(ctx_t *c);                  /* 200 {"ok":true} */

/* Parse the request body as a JSON object. On failure writes a 400 and returns -1. */
int ctx_json(ctx_t *c);

/* Audit log row; failures are logged, never fatal. */
void audit(ctx_t *c, const char *action, const char *target, const char *detail);

/* Insert one order_events row. -1 on db error. */
int order_event(db_t *db, int64_t order_id, const char *from, const char *to,
                const char *actor, const char *note);

/* Render one product row (columns from PRODUCT_COLS) as a JSON object. */
#define PRODUCT_COLS                                                                   \
    "p.id, p.sku, p.name, p.description, p.price_paise, p.category_id, c.name, "       \
    "p.active, i.stock_on_hand, i.reorder_level"
#define PRODUCT_FROM                                                                   \
    " FROM products p JOIN categories c ON c.id = p.category_id"                       \
    " JOIN inventory i ON i.product_id = p.id"
void render_product(buf_t *b, const db_res_t *r);

/* SQL expression formatting a DATETIME column as ISO-8601 UTC. */
#define ISO(col) "DATE_FORMAT(" col ", '%Y-%m-%dT%TZ')"

/* Write {"order":{...,"items":[...],"events":[...]}} for an order. When owner_only is
 * non-zero the order must belong to that user. 1 rendered, 0 not found, -1 db error. */
int render_order(ctx_t *c, int64_t order_id, int64_t owner_only);

/* Find or create the current user's cart. Returns its id, or -1 on db error. */
int64_t cart_id(ctx_t *c);

/* ---- order flow (flow.c) ------------------------------------------------------------
 * Lock order inside every transaction that changes an order, to rule out deadlocks:
 *     orders row  ->  payments row  ->  reservations  ->  inventory rows (ascending id)
 */
#define ORDER_MAX_LINES 50

typedef struct {
    size_t       n;
    stock_line_t lines[ORDER_MAX_LINES];
} stock_back_t;

/* In an open transaction: lock the order row (SELECT ... FOR UPDATE).
 * 1 found, 0 no such order, -1 db error. */
int order_lock(db_t *db, int64_t order_id, order_state_t *state, int64_t *user_id);

/*
 * In an open transaction, with the order locked and known to be in `from`:
 * guarded UPDATE, timeline event, and the side effects of entering `to`:
 *   paid                 reservations active -> committed
 *   cancelled, refunded  reservations -> released, stock_on_hand += qty, ledger row;
 *                        the units are collected in *back for shared memory
 * 1 done, 0 lost a race (status no longer `from`), -1 db error.
 */
int order_move(store_t *s, db_t *db, int64_t order_id, order_state_t from, order_state_t to,
               const char *actor, const char *note, stock_back_t *back);

/* After COMMIT: make returned units sellable again in shared memory (lockfree mode). */
void order_stock_back(store_t *s, const stock_back_t *back);

/*
 * A complete, self-contained change for API callers (own transaction). `to` may be
 * ORDER_STATE_INVALID for "cancel", which picks cancelled (unpaid) or refunded (paid).
 * owner_only > 0 restricts the change to that user's order.
 * Returns 1 done, 0 not allowed (*cur holds the current state), -2 not found, -1 error.
 */
int order_change(store_t *s, int64_t order_id, order_state_t to, order_actor_t actor,
                 int64_t owner_only, const char *actor_name, const char *note,
                 order_state_t *cur, order_state_t *result);

void h_order_cancel(ctx_t *c);
void h_admin_order_status(ctx_t *c);
void h_admin_reports(ctx_t *c);
void h_admin_report_export(ctx_t *c);

/* ---- session lookup (auth.c), called by the router before protected handlers */
int auth_load_session(ctx_t *c);

/* ---- handlers */
void h_register(ctx_t *c);
void h_login(ctx_t *c);
void h_logout(ctx_t *c);
void h_me(ctx_t *c);

void h_categories(ctx_t *c);
void h_products(ctx_t *c);
void h_product(ctx_t *c);

void h_cart_get(ctx_t *c);
void h_cart_set_item(ctx_t *c);
void h_cart_delete_item(ctx_t *c);

void h_checkout(ctx_t *c);
void h_orders_mine(ctx_t *c);
void h_order_get(ctx_t *c);

void h_admin_products(ctx_t *c);
void h_admin_product_create(ctx_t *c);
void h_admin_product_update(ctx_t *c);
void h_admin_product_delete(ctx_t *c);
void h_admin_stock(ctx_t *c);
void h_admin_orders(ctx_t *c);
void h_admin_system(ctx_t *c);

/* Shared-memory metrics helper: no-op when the store has no stock table. */
#define STORE_METRIC_ADD(s, field, n)                                                    \
    do {                                                                                 \
        if ((s)->stock && (s)->stock->shm)                                               \
            atomic_fetch_add_explicit(&(s)->stock->shm->metrics.field, (n),              \
                                      memory_order_relaxed);                             \
    } while (0)

#endif
