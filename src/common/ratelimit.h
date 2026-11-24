/*
 * ratelimit.h - token-bucket rate limiting shared by every of-api worker process.
 *
 * of-api runs N pre-forked worker processes; a limit kept in one process's memory would
 * let a client get N times the limit. So the buckets live in POSIX shared memory,
 * created by the of-api parent before it forks (the workers inherit the mapping).
 *
 * Each bucket is two 64-bit words: the hash of its key ("ip:1.2.3.4", "s:<session>",
 * "auth:1.2.3.4") and its state, packed as
 *     high 40 bits: time of the last refill (ms since the table was created)
 *     low 24 bits:  tokens, in thousandths of a token
 * Because the whole state is one word, taking a token is a single compare-and-swap:
 * read the state, refill it for the time elapsed, fail if it holds less than one token,
 * otherwise try to swap in (now, tokens - 1). A concurrent taker in another process
 * makes the CAS fail and we retry with its fresh state. No locks, shared by processes.
 *
 * Keys are placed by open addressing (a CAS claims an empty slot). If no slot is free
 * within the probe window the request is allowed (fail open) and counted, so a flood of
 * distinct keys can never lock legitimate users out.
 */
#ifndef OF_RATELIMIT_H
#define OF_RATELIMIT_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RL_MAGIC    0x4C52464Fu /* "OFRL" */
#define RL_ABI      1
#define RL_PROBES   32
#define RL_MAX_BURST 16000       /* 24 bits of thousandths */

typedef struct {
    _Atomic uint64_t key;   /* hash of the key, 0 = empty */
    _Atomic uint64_t state; /* (refill time << 24) | thousandths of tokens; 0 = new */
} rl_slot_t;

typedef struct {
    uint32_t         magic;
    uint32_t         abi;
    uint32_t         capacity; /* power of two */
    uint32_t         reserved;
    int64_t          base_ms;  /* CLOCK_MONOTONIC ms at creation */
    _Atomic uint64_t allowed;
    _Atomic uint64_t limited;
    _Atomic uint64_t table_full; /* allowed because no slot was free */
    uint8_t          pad[16];
    rl_slot_t        slots[];
} rl_shm_t;

_Static_assert(sizeof(rl_shm_t) == 64, "rate-limit header is one cache line");

typedef struct {
    rl_shm_t *shm;
    size_t    size;
    uint64_t  mask;
    bool      owner;
    char      name[64];
} rl_table_t;

/* A rule: steady rate in thousandths of a token per second, and the bucket size. */
typedef struct {
    uint32_t rate_milli_per_s; /* e.g. 20 tokens/s = 20000; 30 per minute = 500 */
    uint32_t burst;            /* tokens a full bucket holds (<= RL_MAX_BURST) */
} rl_rule_t;

/* of-api parent: create (or reset) the named table. 0 or -1 with errno. */
int  rl_create(rl_table_t *t, const char *name, uint32_t capacity);
/* Tests: an anonymous MAP_SHARED table (shared with children after fork). */
int  rl_create_anon(rl_table_t *t, uint32_t capacity);
/* Readers (of-ctl): read-only mapping of an existing table. 0 or -1. */
int  rl_attach(rl_table_t *t, const char *name);
void rl_close(rl_table_t *t, bool unlink_name);

/* Milliseconds on the table's clock (CLOCK_MONOTONIC based). */
int64_t rl_now_ms(void);

/*
 * Take one token for key under rule at time now_ms. Returns 0 when allowed; otherwise
 * the number of milliseconds until a token will be available (>= 1).
 */
int64_t rl_take(rl_table_t *t, const char *key, const rl_rule_t *rule, int64_t now_ms);

#endif
