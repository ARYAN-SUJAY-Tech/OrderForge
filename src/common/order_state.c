#include "common/order_state.h"

#include <string.h>

static const char *const names[ORDER_STATE_COUNT] = {
    "placed", "payment_pending", "paid", "packed", "shipped", "delivered", "cancelled", "refunded",
};

const char *order_state_name(order_state_t s)
{
    return (unsigned)s < ORDER_STATE_COUNT ? names[s] : "invalid";
}

order_state_t order_state_parse(const char *name)
{
    for (int i = 0; name && i < ORDER_STATE_COUNT; i++)
        if (strcmp(name, names[i]) == 0)
            return (order_state_t)i;
    return ORDER_STATE_INVALID;
}

/* who[from][to]: bit set of actors allowed to take the edge; 0 = no such edge. */
#define SYS  (1u << ACTOR_SYSTEM)
#define CUST (1u << ACTOR_CUSTOMER)
#define ADM  (1u << ACTOR_ADMIN)

static const unsigned who[ORDER_STATE_COUNT][ORDER_STATE_COUNT] = {
    [ORDER_PLACED][ORDER_PAYMENT_PENDING]    = SYS,
    [ORDER_PLACED][ORDER_CANCELLED]          = SYS | CUST | ADM,
    [ORDER_PAYMENT_PENDING][ORDER_PAID]      = SYS,
    [ORDER_PAYMENT_PENDING][ORDER_CANCELLED] = SYS | CUST | ADM,
    [ORDER_PAID][ORDER_PACKED]               = ADM,
    [ORDER_PAID][ORDER_REFUNDED]             = CUST | ADM,
    [ORDER_PACKED][ORDER_SHIPPED]            = ADM,
    [ORDER_PACKED][ORDER_REFUNDED]           = ADM,
    [ORDER_SHIPPED][ORDER_DELIVERED]         = ADM,
};

static bool in_range(order_state_t s)
{
    return (unsigned)s < ORDER_STATE_COUNT;
}

bool order_transition_valid(order_state_t from, order_state_t to)
{
    return in_range(from) && in_range(to) && who[from][to] != 0;
}

bool order_transition_allowed(order_state_t from, order_state_t to, order_actor_t actor)
{
    return order_transition_valid(from, to) && (who[from][to] & (1u << actor)) != 0;
}

bool order_state_final(order_state_t s)
{
    return s == ORDER_DELIVERED || s == ORDER_CANCELLED || s == ORDER_REFUNDED;
}

bool order_transition_returns_stock(order_state_t from, order_state_t to)
{
    return order_transition_valid(from, to) && (to == ORDER_CANCELLED || to == ORDER_REFUNDED);
}
