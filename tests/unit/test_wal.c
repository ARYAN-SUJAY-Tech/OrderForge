/*
 * Write-ahead log: encoding, CRC, torn tails, group commit under concurrency, and
 * compaction keeping exactly the reservations still in flight.
 */
#include "check.h"
#include "common/wal.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char dir[] = "/tmp/of_test_wal_XXXXXX";
static char path[128];

static wal_rec_t reserve(uint64_t txid, int n)
{
    wal_rec_t r = {.type = WAL_RESERVE, .txid = txid, .nlines = (uint32_t)n};
    for (int i = 0; i < n; i++) {
        r.lines[i].product_id = (uint64_t)(i + 1) * 7;
        r.lines[i].qty = i + 1;
    }
    return r;
}

static void test_encoding(void)
{
    uint8_t buf[2048];
    wal_rec_t in = reserve(42, 3), out;
    in.seq = 9;
    size_t n = wal_encode(&in, buf, sizeof buf);
    CHECK(n == WAL_HDR_SIZE + 12 + 3 * 12 + 4);
    CHECK(wal_decode(buf, n, &out) == n);
    CHECK(out.type == WAL_RESERVE && out.seq == 9 && out.txid == 42 && out.nlines == 3);
    CHECK(out.lines[2].product_id == 21 && out.lines[2].qty == 3);

    wal_rec_t c = {.type = WAL_COMMIT, .seq = 10, .txid = 42, .order_id = 777};
    n = wal_encode(&c, buf, sizeof buf);
    CHECK(wal_decode(buf, n, &out) == n && out.type == WAL_COMMIT && out.order_id == 777);

    /* Every single-byte corruption and every truncation is rejected. */
    n = wal_encode(&in, buf, sizeof buf);
    int bad_accepted = 0;
    for (size_t i = 0; i < n; i++) {
        buf[i] ^= 0x10;
        if (wal_decode(buf, n, &out) != 0)
            bad_accepted++;
        buf[i] ^= 0x10;
    }
    CHECK(bad_accepted == 0);
    for (size_t cut = 0; cut < n; cut++)
        if (wal_decode(buf, cut, &out) != 0)
            bad_accepted++;
    CHECK(bad_accepted == 0);

    wal_rec_t too_many = reserve(1, 1);
    too_many.nlines = WAL_MAX_LINES + 1;
    CHECK(wal_encode(&too_many, buf, sizeof buf) == 0);
    CHECK(wal_encode(&in, buf, 10) == 0); /* does not fit */
}

typedef struct {
    int      reserves, commits, releases, checkpoints;
    uint64_t last_seq;
    int      out_of_order;
} counts_t;

static int count(const wal_rec_t *r, void *ctx)
{
    counts_t *c = ctx;
    if (r->seq <= c->last_seq)
        c->out_of_order++;
    c->last_seq = r->seq;
    c->reserves += r->type == WAL_RESERVE;
    c->commits += r->type == WAL_COMMIT;
    c->releases += r->type == WAL_RELEASE;
    c->checkpoints += r->type == WAL_CHECKPOINT;
    return 0;
}

static void test_append_replay_torn(void)
{
    char err[128];
    wal_stats_t st = {0};
    wal_t *w = wal_start(path, 100, 1, 0, 1 << 20, &st, err, sizeof err);
    CHECK(w != NULL);
    wal_rec_t r = reserve(1, 2);
    uint64_t s1 = wal_append(w, &r);
    wal_rec_t c = {.type = WAL_COMMIT, .txid = 1, .order_id = 5};
    uint64_t s2 = wal_append(w, &c);
    r = reserve(2, 1);
    uint64_t s3 = wal_append(w, &r);
    CHECK(s1 > 100 && s2 == s1 + 1 && s3 == s2 + 1);
    CHECK(wal_wait(w, s3) == 0);
    CHECK(wal_in_flight(w) == 1); /* txid 2 has no outcome yet */
    wal_stop(w);

    counts_t k = {0};
    wal_scan_t scan;
    CHECK(wal_replay(path, count, &k, &scan) == 0);
    CHECK(k.checkpoints == 1 && k.reserves == 2 && k.commits == 1 && k.out_of_order == 0);
    CHECK(scan.torn_bytes == 0 && scan.last_seq == s3 && scan.max_txid == 2);

    /* Simulate a crash mid-write: half a record at the end of the file. */
    struct stat sb;
    stat(path, &sb);
    uint8_t rec[256];
    wal_rec_t half = reserve(3, 4);
    half.seq = s3 + 1;
    size_t n = wal_encode(&half, rec, sizeof rec);
    int fd = open(path, O_WRONLY | O_APPEND);
    CHECK(write(fd, rec, n / 2) == (ssize_t)(n / 2));
    close(fd);

    memset(&k, 0, sizeof k);
    CHECK(wal_replay(path, count, &k, &scan) == 0);
    CHECK(k.reserves == 2 && scan.torn_bytes == n / 2);
    struct stat after;
    stat(path, &after);
    CHECK(after.st_size == sb.st_size); /* the torn tail was cut off */

    /* A missing file is just an empty log. */
    CHECK(wal_replay("/tmp/of_no_such_wal", count, &k, &scan) == 0 && scan.records == 0);
}

#define THREADS 8
#define PER 500

static wal_t *shared;

static void *worker(void *arg)
{
    uint64_t base = (uint64_t)(uintptr_t)arg * 100000;
    for (int i = 0; i < PER; i++) {
        wal_rec_t r = reserve(base + (uint64_t)i, 1);
        uint64_t s = wal_append(shared, &r);
        if (s == 0 || wal_wait(shared, s) != 0)
            abort();
        wal_rec_t done = {.type = (i % 2) ? WAL_COMMIT : WAL_RELEASE, .txid = r.txid};
        wal_append(shared, &done);
    }
    return NULL;
}

static void test_group_commit(void)
{
    char err[128];
    wal_stats_t st = {0};
    shared = wal_start(path, 0, 1, 200, 1 << 26, &st, err, sizeof err);
    CHECK(shared != NULL);
    pthread_t t[THREADS];
    for (uintptr_t i = 0; i < THREADS; i++)
        pthread_create(&t[i], NULL, worker, (void *)i);
    for (int i = 0; i < THREADS; i++)
        pthread_join(t[i], NULL);
    CHECK(wal_in_flight(shared) == 0);
    wal_stop(shared);

    uint64_t appends = atomic_load(&st.appends), flushes = atomic_load(&st.flushes);
    CHECK(appends == THREADS * PER * 2);
    /* Group commit: far fewer fsyncs than records. */
    CHECK(flushes < appends / 2);
    CHECK(atomic_load(&st.max_batch) > 1);
    printf("  group commit: %llu records, %llu fsyncs, largest batch %llu\n",
           (unsigned long long)appends, (unsigned long long)flushes,
           (unsigned long long)atomic_load(&st.max_batch));

    counts_t k = {0};
    wal_scan_t scan;
    CHECK(wal_replay(path, count, &k, &scan) == 0);
    CHECK(k.reserves == THREADS * PER && k.commits + k.releases == THREADS * PER);
    CHECK(k.out_of_order == 0 && scan.torn_bytes == 0);
}

typedef struct {
    uint64_t live[64];
    int      n;
} live_t;

/* Rebuild the in-flight set the way recovery does. */
static int collect(const wal_rec_t *r, void *ctx)
{
    live_t *l = ctx;
    if (r->type == WAL_RESERVE && l->n < 64)
        l->live[l->n++] = r->txid;
    if (r->type == WAL_COMMIT || r->type == WAL_RELEASE)
        for (int i = 0; i < l->n; i++)
            if (l->live[i] == r->txid) {
                l->live[i] = l->live[--l->n];
                break;
            }
    return 0;
}

static void test_compaction(void)
{
    char err[128];
    wal_stats_t st = {0};
    wal_t *w = wal_start(path, 0, 1, 0, 4096, &st, err, sizeof err); /* tiny: compacts often */
    CHECK(w != NULL);
    for (uint64_t tx = 1; tx <= 2000; tx++) {
        wal_rec_t r = reserve(tx, 3);
        uint64_t s = wal_append(w, &r);
        wal_wait(w, s);
        if (tx % 500 != 0) { /* every 500th stays in flight */
            wal_rec_t done = {.type = WAL_RELEASE, .txid = tx};
            wal_append(w, &done);
        }
    }
    CHECK(wal_in_flight(w) == 4);
    wal_stop(w);
    CHECK(atomic_load(&st.compactions) > 0);

    struct stat sb;
    stat(path, &sb);
    CHECK(sb.st_size < 8192); /* compaction keeps the file small */

    live_t l = {0};
    wal_scan_t scan;
    CHECK(wal_replay(path, collect, &l, &scan) == 0);
    CHECK(l.n == 4);
    int found = 0;
    for (int i = 0; i < l.n; i++)
        found += l.live[i] % 500 == 0;
    CHECK(found == 4);
    CHECK(scan.max_txid >= 2000);
}

int main(void)
{
    if (!mkdtemp(dir))
        return 1;
    snprintf(path, sizeof path, "%s/core.wal", dir);
    test_encoding();
    test_append_replay_torn();
    test_group_commit();
    test_compaction();
    unlink(path);
    rmdir(dir);
    CHECK_DONE();
}
