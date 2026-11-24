#include "common/paymsg.h"

#include "common/util.h"

#include <sodium.h>
#include <string.h>
#include <time.h>

int pay_key_from_hex(const char *hex, uint8_t key[PAY_KEY_LEN])
{
    size_t bin_len = 0;
    if (!hex || strlen(hex) != 2 * PAY_KEY_LEN)
        return -1;
    if (sodium_hex2bin(key, PAY_KEY_LEN, hex, strlen(hex), NULL, &bin_len, NULL) != 0 ||
        bin_len != PAY_KEY_LEN)
        return -1;
    return 0;
}

int64_t pay_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void compute_mac(const pay_msg_t *m, const uint8_t key[PAY_KEY_LEN], uint8_t out[PAY_MAC_LEN])
{
    crypto_auth_hmacsha256(out, (const unsigned char *)m, offsetof(pay_msg_t, mac), key);
}

int pay_sign(pay_msg_t *m, const uint8_t key[PAY_KEY_LEN])
{
    m->magic = PAY_MAGIC;
    m->version = PAY_VERSION;
    m->reserved = 0;
    m->ts_ms = pay_now_ms();
    if (of_random_bytes(&m->nonce, sizeof m->nonce) != 0)
        return -1;
    compute_mac(m, key, m->mac);
    return 0;
}

static bool terminated(const char *s, size_t cap)
{
    return memchr(s, '\0', cap) != NULL;
}

int pay_verify(const pay_msg_t *m, pay_kind_t expected, const uint8_t key[PAY_KEY_LEN])
{
    if (m->magic != PAY_MAGIC || m->version != PAY_VERSION || m->kind != expected ||
        !terminated(m->idem_key, sizeof m->idem_key) || !terminated(m->method, sizeof m->method) ||
        !terminated(m->provider_ref, sizeof m->provider_ref))
        return -1;
    /* crypto_auth_hmacsha256_verify compares in constant time. */
    return crypto_auth_hmacsha256_verify(m->mac, (const unsigned char *)m,
                                         offsetof(pay_msg_t, mac), key) == 0
               ? 0
               : -1;
}

int pay_replay_check(pay_replay_t *r, const pay_msg_t *m, int64_t now_ms, int64_t window_ms)
{
    int64_t skew = m->ts_ms - now_ms;
    if (skew > window_ms || skew < -window_ms)
        return -1;
    for (size_t i = 0; i < r->used; i++)
        if (r->seen[i] == m->nonce)
            return -1;
    r->seen[r->next] = m->nonce;
    r->next = (r->next + 1) % PAY_REPLAY_SLOTS;
    if (r->used < PAY_REPLAY_SLOTS)
        r->used++;
    return 0;
}
