#include "common/wal.h"

#include "common/buf.h"
#include "common/crc32.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BATCH_SOFT_LIMIT (64 * 1024) /* flush early once this much is waiting */

/* ------------------------------------------------------------------ encoding */

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t get64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

static size_t payload_len(const wal_rec_t *r)
{
    switch (r->type) {
    case WAL_RESERVE:    return 12 + (size_t)r->nlines * 12;
    case WAL_COMMIT:     return 16;
    case WAL_RELEASE:    return 8;
    case WAL_CHECKPOINT: return 12;
    }
    return 0;
}

size_t wal_encode(const wal_rec_t *r, uint8_t *out, size_t cap)
{
    if (r->type == WAL_RESERVE && r->nlines > WAL_MAX_LINES)
        return 0;
    size_t plen = payload_len(r);
    size_t total = WAL_HDR_SIZE + plen + 4;
    if (plen == 0 || total > cap)
        return 0;

    put32(out, WAL_MAGIC);
    put32(out + 4, (uint32_t)plen);
    uint16_t type = (uint16_t)r->type, reserved = 0;
    memcpy(out + 8, &type, 2);
    memcpy(out + 10, &reserved, 2);
    put64(out + 12, r->seq);

    uint8_t *p = out + WAL_HDR_SIZE;
    switch (r->type) {
    case WAL_RESERVE:
        put64(p, r->txid);
        put32(p + 8, r->nlines);
        for (uint32_t i = 0; i < r->nlines; i++) {
            put64(p + 12 + i * 12, r->lines[i].product_id);
            put32(p + 20 + i * 12, (uint32_t)r->lines[i].qty);
        }
        break;
    case WAL_COMMIT:
        put64(p, r->txid);
        put64(p + 8, r->order_id);
        break;
    case WAL_RELEASE:
        put64(p, r->txid);
        break;
    case WAL_CHECKPOINT:
        put64(p, r->txid);
        put32(p + 8, r->nlines);
        break;
    }
    /* The CRC covers type, seq and payload: a torn or bit-flipped record never decodes. */
    put32(out + WAL_HDR_SIZE + plen, of_crc32(0, out + 8, WAL_HDR_SIZE - 8 + plen));
    return total;
}

size_t wal_decode(const uint8_t *p, size_t avail, wal_rec_t *r)
{
    if (avail < WAL_HDR_SIZE + 4 || get32(p) != WAL_MAGIC)
        return 0;
    uint32_t plen = get32(p + 4);
    if (plen > WAL_MAX_PAYLOAD || avail < WAL_HDR_SIZE + (size_t)plen + 4)
        return 0;
    if (of_crc32(0, p + 8, WAL_HDR_SIZE - 8 + plen) != get32(p + WAL_HDR_SIZE + plen))
        return 0;

    uint16_t type;
    memcpy(&type, p + 8, 2);
    memset(r, 0, sizeof *r);
    r->type = (wal_type_t)type;
    r->seq = get64(p + 12);
    const uint8_t *q = p + WAL_HDR_SIZE;
    switch (r->type) {
    case WAL_RESERVE:
        if (plen < 12)
            return 0;
        r->txid = get64(q);
        r->nlines = get32(q + 8);
        if (r->nlines > WAL_MAX_LINES || plen != 12 + r->nlines * 12)
            return 0;
        for (uint32_t i = 0; i < r->nlines; i++) {
            r->lines[i].product_id = get64(q + 12 + i * 12);
            r->lines[i].qty = (int32_t)get32(q + 20 + i * 12);
        }
        break;
    case WAL_COMMIT:
        if (plen != 16)
            return 0;
        r->txid = get64(q);
        r->order_id = get64(q + 8);
        break;
    case WAL_RELEASE:
        if (plen != 8)
            return 0;
        r->txid = get64(q);
        break;
    case WAL_CHECKPOINT:
        if (plen != 12)
            return 0;
        r->txid = get64(q);
        r->nlines = get32(q + 8);
        break;
    default:
        return 0;
    }
    return WAL_HDR_SIZE + plen + 4;
}

/* ------------------------------------------------------------------ file helpers */

static int write_all(int fd, const void *p, size_t len)
{
    const uint8_t *s = p;
    while (len > 0) {
        ssize_t n = write(fd, s, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        s += n;
        len -= (size_t)n;
    }
    return 0;
}

static void fsync_dir_of(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    int dfd = open(dirname(tmp), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
}

/* Atomically replace path with the given bytes: temp file, fsync, rename, fsync dir. */
static int replace_file(const char *path, const uint8_t *data, size_t len)
{
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    if (write_all(fd, data, len) != 0 || fdatasync(fd) != 0 || close(fd) != 0 ||
        rename(tmp, path) != 0) {
        int e = errno;
        unlink(tmp);
        errno = e;
        return -1;
    }
    fsync_dir_of(path);
    return 0;
}

/* ------------------------------------------------------------------ recovery */

int wal_replay(const char *path, int (*fn)(const wal_rec_t *, void *), void *ctx,
               wal_scan_t *scan)
{
    memset(scan, 0, sizeof *scan);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return -1;
    }
    size_t size = (size_t)st.st_size;
    uint8_t *data = size ? malloc(size) : NULL;
    if (size && !data) {
        close(fd);
        return -1;
    }
    size_t got = 0;
    while (got < size) {
        ssize_t n = read(fd, data + got, size - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        got += (size_t)n;
    }

    size_t off = 0;
    wal_rec_t r;
    while (off < got) {
        size_t n = wal_decode(data + off, got - off, &r);
        if (n == 0)
            break; /* the torn tail of a crash (or corruption): stop here */
        scan->records++;
        scan->last_seq = r.seq;
        if (r.type != WAL_CHECKPOINT && r.txid > scan->max_txid)
            scan->max_txid = r.txid;
        if (r.type == WAL_CHECKPOINT && r.txid > scan->max_txid + 1)
            scan->max_txid = r.txid - 1;
        off += n;
        if (fn && fn(&r, ctx) != 0)
            break;
    }
    scan->valid_bytes = off;
    scan->torn_bytes = size - off;
    if (off < size && ftruncate(fd, (off_t)off) == 0)
        fdatasync(fd);
    free(data);
    close(fd);
    return 0;
}

/* ------------------------------------------------------------------ the live log */

struct wal {
    char            path[512];
    int             fd;
    pthread_mutex_t lock;
    pthread_cond_t  work;    /* flusher: records are waiting */
    pthread_cond_t  durable; /* appenders: durable_seq moved */
    buf_t           pending;
    uint64_t        pending_records;
    uint64_t        next_seq;     /* last seq handed out */
    uint64_t        durable_seq;  /* everything <= this is on disk */
    uint64_t        next_txid;    /* hint written into checkpoints */
    bool            stopping, failed;
    pthread_t       flusher;
    int64_t         group_us;
    size_t          max_bytes;
    size_t          file_size;
    wal_stats_t    *stats;
    wal_stats_t     own_stats;
    /* RESERVE records whose outcome is not logged yet (rewritten on compaction). */
    wal_rec_t      *inflight;
    size_t          ninflight, cap_inflight;
};

static void track(wal_t *w, const wal_rec_t *r)
{
    if (r->type == WAL_RESERVE) {
        if (w->ninflight == w->cap_inflight) {
            size_t cap = w->cap_inflight ? w->cap_inflight * 2 : 64;
            wal_rec_t *p = realloc(w->inflight, cap * sizeof *p);
            if (!p)
                return; /* only compaction loses precision; replay still sees the record */
            w->inflight = p;
            w->cap_inflight = cap;
        }
        w->inflight[w->ninflight++] = *r;
    } else if (r->type == WAL_COMMIT || r->type == WAL_RELEASE) {
        for (size_t i = 0; i < w->ninflight; i++)
            if (w->inflight[i].txid == r->txid) {
                w->inflight[i] = w->inflight[--w->ninflight];
                break;
            }
    }
    if (r->txid >= w->next_txid && r->type != WAL_CHECKPOINT)
        w->next_txid = r->txid + 1;
}

/* Rewrite the log as CHECKPOINT + the RESERVEs still in flight. Caller holds the lock.
 * Everything appended so far is represented, so it all becomes durable. */
static int compact_locked(wal_t *w)
{
    buf_t b;
    buf_init(&b);
    uint8_t rec[WAL_HDR_SIZE + WAL_MAX_PAYLOAD + 4];
    wal_rec_t cp = {.type = WAL_CHECKPOINT, .seq = ++w->next_seq, .txid = w->next_txid,
                    .nlines = (uint32_t)w->ninflight};
    buf_append(&b, rec, wal_encode(&cp, rec, sizeof rec));
    for (size_t i = 0; i < w->ninflight; i++)
        buf_append(&b, rec, wal_encode(&w->inflight[i], rec, sizeof rec));
    if (buf_failed(&b)) {
        buf_free(&b);
        return -1;
    }

    int rc = replace_file(w->path, (const uint8_t *)b.data, b.len);
    if (rc == 0) {
        int fd = open(w->path, O_WRONLY | O_APPEND | O_CLOEXEC);
        if (fd < 0) {
            rc = -1;
        } else {
            if (w->fd >= 0)
                close(w->fd);
            w->fd = fd;
            w->file_size = b.len;
            buf_reset(&w->pending);
            w->pending_records = 0;
            w->durable_seq = w->next_seq;
            atomic_fetch_add(&w->stats->compactions, 1);
            pthread_cond_broadcast(&w->durable);
        }
    }
    buf_free(&b);
    return rc;
}

static void *flusher_main(void *arg)
{
    wal_t *w = arg;
    buf_t batch;
    buf_init(&batch);

    pthread_mutex_lock(&w->lock);
    for (;;) {
        while (w->pending.len == 0 && !w->stopping)
            pthread_cond_wait(&w->work, &w->lock);
        if (w->pending.len == 0 && w->stopping)
            break;

        /* Linger briefly so more appenders can join this fsync (group commit). */
        if (w->group_us > 0 && !w->stopping && w->pending.len < BATCH_SOFT_LIMIT) {
            struct timespec until;
            clock_gettime(CLOCK_MONOTONIC, &until);
            until.tv_nsec += (long)(w->group_us % 1000000) * 1000;
            until.tv_sec += w->group_us / 1000000 + until.tv_nsec / 1000000000;
            until.tv_nsec %= 1000000000;
            pthread_cond_timedwait(&w->work, &w->lock, &until);
        }

        /* Take the batch; appenders keep filling a fresh buffer meanwhile. */
        buf_t tmp = batch;
        batch = w->pending;
        w->pending = tmp;
        buf_reset(&w->pending);
        uint64_t last = w->next_seq, count = w->pending_records;
        w->pending_records = 0;
        int fd = w->fd;
        pthread_mutex_unlock(&w->lock);

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int rc = write_all(fd, batch.data, batch.len) == 0 && fdatasync(fd) == 0 ? 0 : -1;
        clock_gettime(CLOCK_MONOTONIC, &t1);

        pthread_mutex_lock(&w->lock);
        if (rc != 0) {
            w->failed = true; /* never pretend a record is durable */
        } else {
            w->durable_seq = last;
            w->file_size += batch.len;
            atomic_fetch_add(&w->stats->flushes, 1);
            atomic_fetch_add(&w->stats->bytes, batch.len);
            atomic_fetch_add(&w->stats->flush_us_total,
                             (uint64_t)((t1.tv_sec - t0.tv_sec) * 1000000 +
                                        (t1.tv_nsec - t0.tv_nsec) / 1000));
            if (count > atomic_load(&w->stats->max_batch))
                atomic_store(&w->stats->max_batch, count);
        }
        buf_reset(&batch);
        pthread_cond_broadcast(&w->durable);

        if (!w->failed && w->file_size > w->max_bytes && compact_locked(w) != 0)
            w->failed = true;
    }
    pthread_mutex_unlock(&w->lock);
    buf_free(&batch);
    return NULL;
}

wal_t *wal_start(const char *path, uint64_t first_seq, uint64_t next_txid, int64_t group_us,
                 size_t max_bytes, wal_stats_t *stats, char *err, size_t errlen)
{
    wal_t *w = calloc(1, sizeof *w);
    if (!w || strlen(path) >= sizeof w->path) {
        snprintf(err, errlen, "wal: bad path or out of memory");
        free(w);
        return NULL;
    }
    snprintf(w->path, sizeof w->path, "%s", path);
    w->fd = -1;
    w->next_seq = first_seq;
    w->next_txid = next_txid;
    w->group_us = group_us;
    w->max_bytes = max_bytes;
    w->stats = stats ? stats : &w->own_stats;
    buf_init(&w->pending);
    pthread_mutex_init(&w->lock, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&w->work, &ca);
    pthread_cond_init(&w->durable, NULL);
    pthread_condattr_destroy(&ca);

    /* A fresh log: just a checkpoint. Recovery has already dealt with the old one. */
    pthread_mutex_lock(&w->lock);
    int rc = compact_locked(w);
    pthread_mutex_unlock(&w->lock);
    if (rc != 0 || pthread_create(&w->flusher, NULL, flusher_main, w) != 0) {
        snprintf(err, errlen, "wal %s: %s", path, strerror(errno));
        if (w->fd >= 0)
            close(w->fd);
        buf_free(&w->pending);
        free(w);
        return NULL;
    }
    return w;
}

uint64_t wal_append(wal_t *w, wal_rec_t *r)
{
    uint8_t rec[WAL_HDR_SIZE + WAL_MAX_PAYLOAD + 4];
    pthread_mutex_lock(&w->lock);
    if (w->failed || w->stopping) {
        pthread_mutex_unlock(&w->lock);
        return 0;
    }
    r->seq = ++w->next_seq;
    size_t n = wal_encode(r, rec, sizeof rec);
    if (n == 0) {
        w->next_seq--;
        pthread_mutex_unlock(&w->lock);
        return 0;
    }
    bool was_empty = w->pending.len == 0;
    buf_append(&w->pending, rec, n);
    w->pending_records++;
    track(w, r);
    atomic_fetch_add(&w->stats->appends, 1);
    if (was_empty || w->pending.len >= BATCH_SOFT_LIMIT)
        pthread_cond_signal(&w->work);
    uint64_t seq = r->seq;
    pthread_mutex_unlock(&w->lock);
    return seq;
}

int wal_wait(wal_t *w, uint64_t seq)
{
    pthread_mutex_lock(&w->lock);
    while (w->durable_seq < seq && !w->failed)
        pthread_cond_wait(&w->durable, &w->lock);
    int rc = w->durable_seq >= seq ? 0 : -1;
    pthread_mutex_unlock(&w->lock);
    return rc;
}

size_t wal_in_flight(wal_t *w)
{
    pthread_mutex_lock(&w->lock);
    size_t n = w->ninflight;
    pthread_mutex_unlock(&w->lock);
    return n;
}

void wal_stop(wal_t *w)
{
    if (!w)
        return;
    pthread_mutex_lock(&w->lock);
    w->stopping = true;
    pthread_cond_signal(&w->work);
    pthread_mutex_unlock(&w->lock);
    pthread_join(w->flusher, NULL);
    if (w->fd >= 0)
        close(w->fd);
    buf_free(&w->pending);
    free(w->inflight);
    pthread_cond_destroy(&w->work);
    pthread_cond_destroy(&w->durable);
    pthread_mutex_destroy(&w->lock);
    free(w);
}
