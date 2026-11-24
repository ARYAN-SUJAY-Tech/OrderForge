/*
 * stock.h - live stock table in POSIX shared memory.
 *
 * of-core owns the table and changes it; of-ctl (and later of-report and of-bench)
 * map it read-only to see the same live numbers without calling the core.
 *
 * Layout: a 64-byte header with metrics, then an open-addressing hash table of slots.
 * Each slot is exactly one cache line, so threads updating neighbouring products never
 * contend for the same line (no false sharing).
 *
 * Concurrency
 *   - Lookup is lock-free: probe from hash(product_id) until the slot whose
 *     product_id matches (acquire load) or an empty slot.
 *   - Insert (a rare admin action) takes a process-local mutex, initialises the
 *     counters, then publishes the slot by storing product_id with release order,
 *     so a reader that sees the id also sees initialised counters. Slots are never
 *     removed (products are soft-deleted), so a published slot never moves.
 *   - Reserve is a compare-and-swap loop on `available`: read it, fail if it is below
 *     the quantity, otherwise try to swap in available - qty; if another thread changed
 *     it first, the CAS fails, reloads the fresh value and retries. No locks, so no
 *     deadlock; each failed CAS means another thread made progress, so no livelock.
 *
 * Counters per product:   available = on_hand - reserved
 *   reserve:  available -= q, reserved += q     (checkout starts)
 *   commit:   reserved -= q                     (sale is durable; on_hand fell in MySQL)
 *   release:  available += q, reserved -= q     (checkout failed / payment declined)
 *   adjust:   available += delta                (restock, recount)
 */
#ifndef OF_STOCK_H
#define OF_STOCK_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OF_STOCK_MAGIC 0x4F465354u /* "OFST" */
#define OF_STOCK_ABI   2

typedef struct {
    _Atomic int32_t  available;   /* on hand minus reserved */
    _Atomic int32_t  reserved;    /* held by checkouts in progress */
    _Atomic uint64_t product_id;  /* 0 = empty slot; written once, release order */
    _Atomic int64_t  price_paise;
    _Atomic uint32_t version;     /* bumped on every change */
    uint8_t          pad[36];
} stock_slot_t;

/* Process-wide counters, readable by of-ctl. */
typedef struct {
    _Atomic uint64_t requests_total;
    _Atomic uint64_t busy_rejects;     /* queue full: answered "busy" */
    _Atomic uint64_t queue_depth;
    _Atomic uint64_t orders_total;
    _Atomic uint64_t orders_failed;    /* out of stock */
    _Atomic uint64_t cas_retries;      /* lost CAS races in stock_reserve */
    _Atomic uint64_t active_workers;
    _Atomic uint64_t payments_sent;
    _Atomic uint64_t payments_succeeded;
    _Atomic uint64_t payments_declined;
    _Atomic uint64_t payments_ignored;    /* duplicate or replayed replies */
    _Atomic uint64_t payments_rejected;   /* bad MAC, stale or replayed nonce */
    _Atomic uint64_t late_refunds;        /* success arrived after the order expired */
    _Atomic uint64_t reservations_expired;
} stock_metrics_t;

typedef struct {
    uint32_t         magic;
    uint32_t         abi_version;
    uint32_t         capacity;         /* power of two */
    uint32_t         stock_mode;       /* informational, set by of-core */
    _Atomic uint64_t count;            /* products in the table */
    _Atomic int64_t  started_unix;     /* of-core start time, 0 while loading */
    stock_metrics_t  metrics;
    uint8_t          pad[64 - (32 + sizeof(stock_metrics_t)) % 64];
    stock_slot_t     slots[];
} stock_shm_t;

_Static_assert(sizeof(stock_slot_t) == 64, "a stock slot must be one cache line");
_Static_assert(sizeof(stock_shm_t) % 64 == 0, "slots must start on a cache line");

/* Process-local handle around the mapping. */
typedef struct {
    stock_shm_t    *shm;
    size_t          map_size;
    uint64_t        mask;
    pthread_mutex_t insert_lock;
    bool            owner;     /* created by this process (of-core) */
    char            name[64];
} stock_table_t;

/* One cart line for multi-item operations. */
typedef struct {
    uint64_t product_id;
    int32_t  qty;
} stock_line_t;

/* of-core: create (or reset) the named object with room for capacity products
 * (rounded up to a power of two). 0 or -1 with errno. */
int  stock_create(stock_table_t *t, const char *name, uint32_t capacity);

/* Readers: map an existing table read-only. 0 or -1 (errno ENOENT if absent). */
int  stock_attach(stock_table_t *t, const char *name);

/* Anonymous table for unit tests and single-process use (no name). */
int  stock_create_anon(stock_table_t *t, uint32_t capacity);

/* Unmap; the owner also removes the name when unlink is true. */
void stock_close(stock_table_t *t, bool unlink_name);

/* Lock-free lookup. NULL when the product is not in the table. */
stock_slot_t *stock_find(const stock_table_t *t, uint64_t product_id);

/* Insert or overwrite a product's counters (available = on_hand, reserved = 0).
 * -1 with errno ENOSPC when the table is full. */
stock_slot_t *stock_upsert(stock_table_t *t, uint64_t product_id, int32_t on_hand,
                           int64_t price_paise);

/* CAS reserve on one slot. true when qty units were taken; *retries counts lost races. */
bool stock_reserve(stock_slot_t *s, int32_t qty, uint64_t *retries);
void stock_commit(stock_slot_t *s, int32_t qty);
void stock_release(stock_slot_t *s, int32_t qty);

/* Add delta to available (negative allowed). Fails (false) instead of going below 0. */
bool stock_adjust(stock_slot_t *s, int32_t delta);

/*
 * Reserve every line or none. Lines are sorted by product id first, so every caller
 * touches products in the same order. On failure the lines already taken are given
 * back and *failed_index names the line (in the caller's sorted array) that was short:
 * its product is missing (slot absent) or has too little available.
 * Returns true when everything was reserved.
 */
bool stock_reserve_all(stock_table_t *t, stock_line_t *lines, size_t n, size_t *failed_index,
                       uint64_t *retries);
void stock_commit_all(stock_table_t *t, const stock_line_t *lines, size_t n);
void stock_release_all(stock_table_t *t, const stock_line_t *lines, size_t n);

#endif
