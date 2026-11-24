/*
 * paymsg.h - messages between of-core and of-pay (the mock payment gateway).
 *
 * Transport: two POSIX message queues, one per direction. A message queue keeps
 * message boundaries (one mq_send = one mq_receive), and it lives in the kernel, so a
 * request sent while of-pay is restarting waits in the queue instead of being lost.
 *
 * Every message is a fixed-size struct authenticated with HMAC-SHA256 over all fields
 * before `mac`, using a shared secret (pay.hmac_key). The receiver verifies the MAC
 * with a constant-time comparison, so a forged or modified reply ("this order was
 * paid") is rejected, and a timing side channel cannot be used to guess a valid MAC.
 *
 * Replays are rejected too: each message carries a random 64-bit nonce and a
 * timestamp. A receiver accepts a message only if its timestamp is within the window
 * and its nonce has not been seen before (pay_replay_check).
 */
#ifndef OF_PAYMSG_H
#define OF_PAYMSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAY_MAGIC   0x5941504Fu /* "OPAY" */
#define PAY_VERSION 1
#define PAY_KEY_LEN 32
#define PAY_MAC_LEN 32

typedef enum { PAY_REQUEST = 1, PAY_REPLY = 2 } pay_kind_t;
typedef enum { PAY_OUTCOME_NONE = 0, PAY_SUCCEEDED = 1, PAY_DECLINED = 2 } pay_outcome_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t kind;          /* pay_kind_t */
    uint32_t outcome;       /* pay_outcome_t, replies only */
    uint32_t reserved;
    uint64_t order_id;
    int64_t  amount_paise;
    int64_t  ts_ms;         /* sender's wall clock, milliseconds */
    uint64_t nonce;         /* random, unique per message */
    char     idem_key[48];  /* the payment's idempotency key */
    char     method[24];    /* mock payment-method token, e.g. "tok_success" */
    char     provider_ref[32];
    uint8_t  mac[PAY_MAC_LEN];
} pay_msg_t;

_Static_assert(sizeof(pay_msg_t) == 184, "pay_msg_t layout changed");

/* Parse a 64-hex-character key. 0 or -1. */
int  pay_key_from_hex(const char *hex, uint8_t key[PAY_KEY_LEN]);

/* Fill magic/version/nonce/ts and compute the MAC. 0 or -1 (no randomness). */
int  pay_sign(pay_msg_t *m, const uint8_t key[PAY_KEY_LEN]);

/* Structural checks (magic, version, kind, NUL-terminated strings) and the MAC.
 * 0 when authentic, -1 otherwise. */
int  pay_verify(const pay_msg_t *m, pay_kind_t expected, const uint8_t key[PAY_KEY_LEN]);

/* Replay protection: remembers the last PAY_REPLAY_SLOTS nonces. */
#define PAY_REPLAY_SLOTS 4096
typedef struct {
    uint64_t seen[PAY_REPLAY_SLOTS];
    size_t   next;
    size_t   used;
} pay_replay_t;

/* 0 when the message is fresh (and records its nonce); -1 when its timestamp is
 * outside +/- window_ms of now_ms or its nonce was seen before. */
int  pay_replay_check(pay_replay_t *r, const pay_msg_t *m, int64_t now_ms, int64_t window_ms);

/* Wall-clock milliseconds. */
int64_t pay_now_ms(void);

#endif
