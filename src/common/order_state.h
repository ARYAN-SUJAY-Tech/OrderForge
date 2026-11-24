/*
 * order_state.h - the order state machine (pure logic, no I/O).
 *
 *   placed ──▶ payment_pending ──▶ paid ──▶ packed ──▶ shipped ──▶ delivered
 *     │              │               │        │
 *     └──────────────┴──▶ cancelled  └────────┴──▶ refunded
 *
 *   cancelled  before payment: declined, timed out, expired, or cancelled by a person;
 *              the reserved stock goes back on the shelf
 *   refunded   after payment but before shipping; the stock goes back and the payment
 *              is refunded
 *   delivered, cancelled and refunded are final.
 *
 * The database enforces "each transition happens once": every change is a guarded
 * UPDATE ... SET status = <to> WHERE id = ? AND status = <from>, so when a late payment
 * reply and the expiry reaper race, exactly one of them matches the row.
 */
#ifndef OF_ORDER_STATE_H
#define OF_ORDER_STATE_H

#include <stdbool.h>

typedef enum {
    ORDER_PLACED = 0,
    ORDER_PAYMENT_PENDING,
    ORDER_PAID,
    ORDER_PACKED,
    ORDER_SHIPPED,
    ORDER_DELIVERED,
    ORDER_CANCELLED,
    ORDER_REFUNDED,
    ORDER_STATE_COUNT,
    ORDER_STATE_INVALID = -1,
} order_state_t;

/* Who asks for a transition; some moves are reserved to some actors. */
typedef enum {
    ACTOR_SYSTEM,   /* checkout, payment replies, the expiry reaper */
    ACTOR_CUSTOMER, /* the order's owner */
    ACTOR_ADMIN,
} order_actor_t;

const char   *order_state_name(order_state_t s);
order_state_t order_state_parse(const char *name); /* ORDER_STATE_INVALID if unknown */

/* Is from -> to an edge of the machine at all? */
bool order_transition_valid(order_state_t from, order_state_t to);

/* Is this actor allowed to make that move? (implies order_transition_valid) */
bool order_transition_allowed(order_state_t from, order_state_t to, order_actor_t actor);

bool order_state_final(order_state_t s);

/* Does entering `to` from `from` give stock back? (cancelled, refunded) */
bool order_transition_returns_stock(order_state_t from, order_state_t to);

#endif
