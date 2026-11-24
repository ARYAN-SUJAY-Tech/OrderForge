/* Payment messages: signing, tampering, wrong key, malformed fields, replays. */
#include "check.h"
#include "common/paymsg.h"

#include <sodium.h>
#include <stdlib.h>

static const char *KEY_HEX = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

static pay_msg_t make(uint8_t key[PAY_KEY_LEN])
{
    pay_msg_t m;
    memset(&m, 0, sizeof m);
    m.kind = PAY_REPLY;
    m.outcome = PAY_SUCCEEDED;
    m.order_id = 42;
    m.amount_paise = 899900;
    snprintf(m.idem_key, sizeof m.idem_key, "pay-42");
    snprintf(m.method, sizeof m.method, "tok_success");
    snprintf(m.provider_ref, sizeof m.provider_ref, "mockpay_abc");
    CHECK(pay_sign(&m, key) == 0);
    return m;
}

static void test_sign_verify(void)
{
    uint8_t key[PAY_KEY_LEN], other[PAY_KEY_LEN];
    CHECK(pay_key_from_hex(KEY_HEX, key) == 0);
    CHECK(pay_key_from_hex("abc", key) == -1);
    CHECK(pay_key_from_hex("zz0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
                           other) == -1);
    CHECK(pay_key_from_hex(KEY_HEX, key) == 0);

    pay_msg_t m = make(key);
    CHECK(pay_verify(&m, PAY_REPLY, key) == 0);
    CHECK(pay_verify(&m, PAY_REQUEST, key) == -1); /* wrong kind */

    memcpy(other, key, sizeof other);
    other[0] ^= 1;
    CHECK(pay_verify(&m, PAY_REPLY, other) == -1); /* wrong key */

    /* Flip any single byte before the MAC: always rejected. */
    int all_rejected = 1;
    for (size_t i = 0; i < offsetof(pay_msg_t, mac); i++) {
        pay_msg_t t = m;
        ((uint8_t *)&t)[i] ^= 0x01;
        if (pay_verify(&t, PAY_REPLY, key) == 0)
            all_rejected = 0;
    }
    CHECK(all_rejected);

    /* The classic forgery: change a decline into a success. */
    pay_msg_t forged = m;
    forged.outcome = PAY_DECLINED;
    CHECK(pay_verify(&forged, PAY_REPLY, key) == -1);

    /* A string with no terminator is rejected before the MAC is even computed. */
    pay_msg_t unterminated = m;
    memset(unterminated.method, 'A', sizeof unterminated.method);
    CHECK(pay_verify(&unterminated, PAY_REPLY, key) == -1);
}

static void test_replay(void)
{
    uint8_t key[PAY_KEY_LEN];
    pay_key_from_hex(KEY_HEX, key);
    static pay_replay_t guard;
    pay_msg_t m = make(key);
    int64_t now = pay_now_ms();

    CHECK(pay_replay_check(&guard, &m, now, 60000) == 0);
    CHECK(pay_replay_check(&guard, &m, now, 60000) == -1); /* same nonce again */

    pay_msg_t fresh = make(key);                             /* new nonce */
    CHECK(fresh.nonce != m.nonce);
    CHECK(pay_replay_check(&guard, &fresh, now, 60000) == 0);

    pay_msg_t old = make(key);
    CHECK(pay_replay_check(&guard, &old, now + 120000, 60000) == -1); /* too old */
    CHECK(pay_replay_check(&guard, &old, now - 120000, 60000) == -1); /* from the future */

    /* The ring forgets the oldest nonce once it has seen PAY_REPLAY_SLOTS newer ones. */
    static pay_replay_t ring;
    pay_msg_t first = make(key);
    CHECK(pay_replay_check(&ring, &first, now, 60000) == 0);
    for (int i = 0; i < PAY_REPLAY_SLOTS; i++) {
        pay_msg_t x = make(key);
        pay_replay_check(&ring, &x, now, 60000);
    }
    CHECK(ring.used == PAY_REPLAY_SLOTS);
}

int main(void)
{
    if (sodium_init() < 0)
        return 1;
    test_sign_verify();
    test_replay();
    CHECK_DONE();
}
