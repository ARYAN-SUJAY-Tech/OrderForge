/*
 * wal.h - write-ahead log for in-flight stock reservations.
 *
 * Why: in lockfree-shm mode a checkout first takes units with a CAS in shared memory,
 * then commits the order in MySQL. Shared memory survives a crash of of-core (it lives
 * in /dev/shm), so if of-core dies between those two steps the units would stay taken
 * in shared memory although MySQL never sold them. The WAL closes that window: before
 * the checkout touches MySQL it durably records RESERVE(txid, lines); afterwards it
 * records COMMIT(txid, order) or RELEASE(txid). After a crash, recovery replays the log
 * and, for every RESERVE without an outcome, asks MySQL whether the order with that
 * txid committed, then keeps or returns the units in shared memory accordingly.
 *
 * Record on disk (little-endian, same host):
 *   magic u32 "OFWL" | length u32 (payload bytes) | type u16 | reserved u16 | seq u64
 *   | payload | crc32 u32 over everything after magic+length up to the end of payload
 * Payloads:
 *   RESERVE     txid u64, n u32, n x (product_id u64, qty i32)
 *   COMMIT      txid u64, order_id u64
 *   RELEASE     txid u64
 *   CHECKPOINT  next_txid u64 (a hint), in_flight u32
 *
 * Group commit: appenders copy encoded records into a shared buffer under a mutex; one
 * flusher thread swaps the buffer out, write()s it and fdatasync()s once for the whole
 * batch, then wakes every waiter whose record is now durable. Under load many
 * reservations share one fsync, which is what makes durability affordable.
 *
 * Recovery reads records until the first bad magic, impossible length or CRC mismatch:
 * that is the torn tail of the crash, and the file is truncated there.
 *
 * Compaction: when the file passes max_bytes, the flusher writes a new file holding a
 * CHECKPOINT plus a RESERVE for every reservation still in flight, fsyncs it, and
 * rename()s it over the old one (then fsyncs the directory). rename is atomic, so a
 * crash leaves either the complete old log or the complete new one.
 */
#ifndef OF_WAL_H
#define OF_WAL_H

#include "common/stock.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WAL_MAGIC     0x4C57464Fu /* "OFWL" */
#define WAL_HDR_SIZE  20
#define WAL_MAX_LINES 50
#define WAL_MAX_PAYLOAD (12 + WAL_MAX_LINES * 12)

typedef enum {
    WAL_RESERVE = 1,
    WAL_COMMIT = 2,
    WAL_RELEASE = 3,
    WAL_CHECKPOINT = 4,
} wal_type_t;

typedef struct {
    wal_type_t   type;
    uint64_t     seq;       /* assigned by wal_append */
    uint64_t     txid;      /* RESERVE/COMMIT/RELEASE; CHECKPOINT: next txid hint */
    uint64_t     order_id;  /* COMMIT */
    uint32_t     nlines;    /* RESERVE; CHECKPOINT: records in flight */
    stock_line_t lines[WAL_MAX_LINES];
} wal_rec_t;

/* Counters: wal_stats_t (common/stock.h), normally the one inside the shared-memory
 * metrics so of-ctl and the metrics endpoint can see them. */

/* ---- pure encoding (unit-tested) */

/* Append one encoded record to out (cap bytes). Returns bytes written, 0 if it won't fit. */
size_t wal_encode(const wal_rec_t *r, uint8_t *out, size_t cap);

/* Decode one record at p (avail bytes). Returns its size, 0 when the bytes are not a
 * complete, valid record (torn or corrupt). */
size_t wal_decode(const uint8_t *p, size_t avail, wal_rec_t *r);

/* ---- recovery */

typedef struct {
    uint64_t records;
    uint64_t valid_bytes;
    uint64_t torn_bytes;   /* discarded tail */
    uint64_t last_seq;
    uint64_t max_txid;
} wal_scan_t;

/* Read every valid record of path, calling fn for each (fn returning non-zero stops).
 * Truncates a torn tail. A missing file is an empty log. 0 or -1 with errno. */
int wal_replay(const char *path, int (*fn)(const wal_rec_t *, void *), void *ctx,
               wal_scan_t *scan);

/* ---- the live log */

typedef struct wal wal_t;

/*
 * Start a fresh log at path containing just a CHECKPOINT (written atomically with
 * rename), then start the flusher thread. first_seq continues the sequence numbers;
 * group_us is how long the flusher lingers to gather a batch; max_bytes triggers
 * compaction. stats may be NULL.
 */
wal_t *wal_start(const char *path, uint64_t first_seq, uint64_t next_txid, int64_t group_us,
                 size_t max_bytes, wal_stats_t *stats, char *err, size_t errlen);

/* Queue a record (its seq is filled in). Returns the seq, or 0 on failure. */
uint64_t wal_append(wal_t *w, wal_rec_t *r);

/* Block until the record with this seq is on disk. 0, or -1 if the log failed. */
int wal_wait(wal_t *w, uint64_t seq);

/* Records currently in flight (RESERVE without COMMIT/RELEASE). */
size_t wal_in_flight(wal_t *w);

/* Flush everything, stop the flusher, close. */
void wal_stop(wal_t *w);

#endif
