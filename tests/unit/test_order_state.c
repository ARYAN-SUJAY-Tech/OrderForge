/* The order state machine: every edge, every non-edge, and who may take each edge. */
#include "check.h"
#include "common/order_state.h"

static void test_names(void)
{
    for (int s = 0; s < ORDER_STATE_COUNT; s++)
        CHECK(order_state_parse(order_state_name((order_state_t)s)) == s);
    CHECK_STR(order_state_name(ORDER_PAYMENT_PENDING), "payment_pending");
    CHECK(order_state_parse("lost") == ORDER_STATE_INVALID);
    CHECK(order_state_parse(NULL) == ORDER_STATE_INVALID);
    CHECK_STR(order_state_name(ORDER_STATE_INVALID), "invalid");
}

static void test_edges(void)
{
    /* The complete list of legal edges. Everything else must be rejected. */
    static const int edges[][2] = {
        {ORDER_PLACED, ORDER_PAYMENT_PENDING}, {ORDER_PLACED, ORDER_CANCELLED},
        {ORDER_PAYMENT_PENDING, ORDER_PAID},   {ORDER_PAYMENT_PENDING, ORDER_CANCELLED},
        {ORDER_PAID, ORDER_PACKED},            {ORDER_PAID, ORDER_REFUNDED},
        {ORDER_PACKED, ORDER_SHIPPED},         {ORDER_PACKED, ORDER_REFUNDED},
        {ORDER_SHIPPED, ORDER_DELIVERED},
    };
    int legal = 0;
    for (int f = 0; f < ORDER_STATE_COUNT; f++) {
        for (int t = 0; t < ORDER_STATE_COUNT; t++) {
            bool expect = false;
            for (size_t i = 0; i < sizeof edges / sizeof edges[0]; i++)
                if (edges[i][0] == f && edges[i][1] == t)
                    expect = true;
            bool got = order_transition_valid((order_state_t)f, (order_state_t)t);
            if (got != expect)
                fprintf(stderr, "  edge %s -> %s: expected %d\n", order_state_name(f),
                        order_state_name(t), expect);
            CHECK(got == expect);
            legal += got;
        }
    }
    CHECK(legal == 9);
    CHECK(!order_transition_valid(ORDER_STATE_INVALID, ORDER_PAID));
    CHECK(!order_transition_valid(ORDER_PAID, (order_state_t)42));
}

static void test_final_and_stock(void)
{
    CHECK(order_state_final(ORDER_DELIVERED));
    CHECK(order_state_final(ORDER_CANCELLED));
    CHECK(order_state_final(ORDER_REFUNDED));
    CHECK(!order_state_final(ORDER_PAID));
    /* No edge leaves a final state. */
    for (int t = 0; t < ORDER_STATE_COUNT; t++) {
        CHECK(!order_transition_valid(ORDER_DELIVERED, (order_state_t)t));
        CHECK(!order_transition_valid(ORDER_CANCELLED, (order_state_t)t));
        CHECK(!order_transition_valid(ORDER_REFUNDED, (order_state_t)t));
    }
    CHECK(order_transition_returns_stock(ORDER_PAYMENT_PENDING, ORDER_CANCELLED));
    CHECK(order_transition_returns_stock(ORDER_PACKED, ORDER_REFUNDED));
    CHECK(!order_transition_returns_stock(ORDER_PAID, ORDER_PACKED));
    CHECK(!order_transition_returns_stock(ORDER_SHIPPED, ORDER_DELIVERED));
}

static void test_actors(void)
{
    /* Only the system confirms payment; nobody else can mark an order paid. */
    CHECK(order_transition_allowed(ORDER_PAYMENT_PENDING, ORDER_PAID, ACTOR_SYSTEM));
    CHECK(!order_transition_allowed(ORDER_PAYMENT_PENDING, ORDER_PAID, ACTOR_ADMIN));
    CHECK(!order_transition_allowed(ORDER_PAYMENT_PENDING, ORDER_PAID, ACTOR_CUSTOMER));
    CHECK(!order_transition_allowed(ORDER_PLACED, ORDER_PAYMENT_PENDING, ACTOR_ADMIN));

    /* Fulfilment is the admin's job. */
    CHECK(order_transition_allowed(ORDER_PAID, ORDER_PACKED, ACTOR_ADMIN));
    CHECK(order_transition_allowed(ORDER_PACKED, ORDER_SHIPPED, ACTOR_ADMIN));
    CHECK(order_transition_allowed(ORDER_SHIPPED, ORDER_DELIVERED, ACTOR_ADMIN));
    CHECK(!order_transition_allowed(ORDER_PAID, ORDER_PACKED, ACTOR_CUSTOMER));
    CHECK(!order_transition_allowed(ORDER_PAID, ORDER_PACKED, ACTOR_SYSTEM));

    /* A customer may back out before the order is packed, not after. */
    CHECK(order_transition_allowed(ORDER_PLACED, ORDER_CANCELLED, ACTOR_CUSTOMER));
    CHECK(order_transition_allowed(ORDER_PAYMENT_PENDING, ORDER_CANCELLED, ACTOR_CUSTOMER));
    CHECK(order_transition_allowed(ORDER_PAID, ORDER_REFUNDED, ACTOR_CUSTOMER));
    CHECK(!order_transition_allowed(ORDER_PACKED, ORDER_REFUNDED, ACTOR_CUSTOMER));
    CHECK(order_transition_allowed(ORDER_PACKED, ORDER_REFUNDED, ACTOR_ADMIN));

    /* The system cancels unpaid orders (decline, timeout) but never refunds on its own. */
    CHECK(order_transition_allowed(ORDER_PAYMENT_PENDING, ORDER_CANCELLED, ACTOR_SYSTEM));
    CHECK(!order_transition_allowed(ORDER_PAID, ORDER_REFUNDED, ACTOR_SYSTEM));

    /* Never through an invalid edge, whoever asks. */
    CHECK(!order_transition_allowed(ORDER_SHIPPED, ORDER_REFUNDED, ACTOR_ADMIN));
    CHECK(!order_transition_allowed(ORDER_DELIVERED, ORDER_SHIPPED, ACTOR_ADMIN));
}

int main(void)
{
    test_names();
    test_edges();
    test_final_and_stock();
    test_actors();
    CHECK_DONE();
}
