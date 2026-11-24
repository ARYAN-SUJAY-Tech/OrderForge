/*
 * Tests for the shared-memory stock table and the CAS reservation.
 * The concurrency tests hammer one hot product from many threads (and, separately,
 * from forked processes through a named shm object) and check that exactly the
 * available stock is sold: never more, never less.
 */
#include "check.h"
#include "common/stock.h"

#include <pthread.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static void test_layout(void)
{
    CHECK(sizeof(stock_slot_t) == 64);
    CHECK(sizeof(stock_shm_t) % 64 == 0);

    stock_table_t t;
    CHECK(stock_create_anon(&t, 100) == 0);
    CHECK(t.shm->capacity == 128); /* rounded up to a power of two */
    CHECK(((uintptr_t)&t.shm->slots[0]) % 64 == 0);
    CHECK(((uintptr_t)&t.shm->slots[1]) % 64 == 0);
    stock_close(&t, false);
}

static void test_basic(void)
{
    stock_table_t t;
    CHECK(stock_create_anon(&t, 64) == 0);
    CHECK(stock_find(&t, 7) == NULL);
    CHECK(stock_find(&t, 0) == NULL);

    stock_slot_t *s = stock_upsert(&t, 7, 10, 999);
    CHECK(s != NULL && stock_find(&t, 7) == s);
    CHECK(atomic_load(&t.shm->count) == 1);

    CHECK(stock_reserve(s, 4, NULL));
    CHECK(s->available == 6 && s->reserved == 4);
    CHECK(!stock_reserve(s, 7, NULL));          /* not enough */
    CHECK(!stock_reserve(s, 0, NULL));          /* nonsense quantity */
    CHECK(!stock_reserve(s, -1, NULL));
    stock_commit(s, 3);                         /* 3 of the 4 sold */
    CHECK(s->available == 6 && s->reserved == 1);
    stock_release(s, 1);                        /* the last one given back */
    CHECK(s->available == 7 && s->reserved == 0);

    CHECK(stock_adjust(s, 5) && s->available == 12);
    CHECK(!stock_adjust(s, -13) && s->available == 12);
    CHECK(stock_adjust(s, -12) && s->available == 0);

    /* Upsert of an existing id resets its counters and keeps the count. */
    CHECK(stock_upsert(&t, 7, 50, 1) == s && s->available == 50);
    CHECK(atomic_load(&t.shm->count) == 1);
    stock_close(&t, false);
}

static void test_probing_and_full(void)
{
    stock_table_t t;
    CHECK(stock_create_anon(&t, 64) == 0);
    for (uint64_t id = 1; id <= 64; id++)
        CHECK(stock_upsert(&t, id, (int32_t)id, 0) != NULL);
    for (uint64_t id = 1; id <= 64; id++) {
        stock_slot_t *s = stock_find(&t, id);
        CHECK(s != NULL && s->product_id == id && s->available == (int32_t)id);
    }
    CHECK(stock_upsert(&t, 65, 1, 0) == NULL); /* full */
    CHECK(stock_find(&t, 65) == NULL);
    stock_close(&t, false);
}

static void test_reserve_all(void)
{
    stock_table_t t;
    CHECK(stock_create_anon(&t, 64) == 0);
    stock_upsert(&t, 1, 5, 0);
    stock_upsert(&t, 2, 1, 0);
    stock_upsert(&t, 3, 5, 0);

    /* Unsorted input is sorted; everything available: all reserved. */
    stock_line_t ok[] = {{3, 2}, {1, 2}};
    size_t bad = 99;
    CHECK(stock_reserve_all(&t, ok, 2, &bad, NULL));
    CHECK(ok[0].product_id == 1 && ok[1].product_id == 3);
    CHECK(stock_find(&t, 1)->available == 3 && stock_find(&t, 3)->available == 3);

    /* Middle line short: earlier lines are rolled back, later ones never touched. */
    stock_line_t short_mid[] = {{1, 1}, {2, 2}, {3, 1}};
    CHECK(!stock_reserve_all(&t, short_mid, 3, &bad, NULL));
    CHECK(bad == 1 && short_mid[bad].product_id == 2);
    CHECK(stock_find(&t, 1)->available == 3 && stock_find(&t, 1)->reserved == 2);
    CHECK(stock_find(&t, 2)->available == 1 && stock_find(&t, 3)->available == 3);

    /* Unknown product fails the same way. */
    stock_line_t unknown[] = {{1, 1}, {42, 1}};
    CHECK(!stock_reserve_all(&t, unknown, 2, &bad, NULL) && bad == 1);
    CHECK(stock_find(&t, 1)->available == 3);

    stock_commit_all(&t, ok, 2);
    CHECK(stock_find(&t, 1)->reserved == 0 && stock_find(&t, 3)->reserved == 0);
    stock_close(&t, false);
}

/* ---- many threads, one hot product */

#define THREADS 16
#define ATTEMPTS 2000
#define HOT_STOCK 5000

static stock_table_t shared;
static _Atomic int sold;
static _Atomic uint64_t total_retries;

static void *buyer(void *arg)
{
    (void)arg;
    uint64_t retries = 0;
    stock_slot_t *s = stock_find(&shared, 1);
    for (int i = 0; i < ATTEMPTS; i++) {
        /* Mix single and multi-item carts so rollback runs under contention too. */
        if (i % 3 == 0) {
            stock_line_t lines[] = {{2, 1}, {1, 1}};
            if (stock_reserve_all(&shared, lines, 2, NULL, &retries)) {
                stock_commit_all(&shared, lines, 2);
                atomic_fetch_add(&sold, 1);
            }
        } else if (stock_reserve(s, 1, &retries)) {
            if (i % 7 == 0) {
                stock_release(s, 1); /* checkout failed: give it back */
            } else {
                stock_commit(s, 1);
                atomic_fetch_add(&sold, 1);
            }
        }
    }
    atomic_fetch_add(&total_retries, retries);
    return NULL;
}

static void test_threads(void)
{
    CHECK(stock_create_anon(&shared, 64) == 0);
    stock_upsert(&shared, 1, HOT_STOCK, 0);
    stock_upsert(&shared, 2, 1000000, 0); /* plenty: only product 1 can run out */

    pthread_t th[THREADS];
    for (int i = 0; i < THREADS; i++)
        pthread_create(&th[i], NULL, buyer, NULL);
    for (int i = 0; i < THREADS; i++)
        pthread_join(th[i], NULL);

    stock_slot_t *s = stock_find(&shared, 1);
    /* THREADS * ATTEMPTS attempts > HOT_STOCK, so the product must sell out exactly. */
    CHECK(atomic_load(&sold) == HOT_STOCK);
    CHECK(s->available == 0);
    CHECK(s->reserved == 0);
    CHECK(stock_find(&shared, 2)->reserved == 0);
    printf("  threads: %d sold of %d, %llu CAS retries\n", atomic_load(&sold), HOT_STOCK,
           (unsigned long long)atomic_load(&total_retries));
    stock_close(&shared, false);
}

/* ---- many processes through a named shm object */

static void test_processes(void)
{
    char name[64];
    snprintf(name, sizeof name, "/of_test_stock_%d", (int)getpid());
    stock_table_t t;
    CHECK(stock_create(&t, name, 64) == 0);
    stock_upsert(&t, 9, 1000, 0);

    /* Children inherit the MAP_SHARED mapping: all of them CAS the same physical page. */
    enum { PROCS = 8, EACH = 400 };
    int wins_pipe[2];
    CHECK(pipe(wins_pipe) == 0);
    for (int p = 0; p < PROCS; p++) {
        if (fork() == 0) {
            int wins = 0;
            stock_slot_t *s = stock_find(&t, 9);
            for (int i = 0; i < EACH; i++)
                if (stock_reserve(s, 1, NULL)) {
                    stock_commit(s, 1);
                    wins++;
                }
            if (write(wins_pipe[1], &wins, sizeof wins) != sizeof wins)
                _exit(3);
            _exit(0);
        }
    }
    close(wins_pipe[1]);
    int total = 0, w;
    while (read(wins_pipe[0], &w, sizeof w) == sizeof w)
        total += w;
    close(wins_pipe[0]);
    for (int p = 0; p < PROCS; p++) {
        int st;
        wait(&st);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
    CHECK(total == 1000); /* PROCS * EACH = 3200 attempts for 1000 units */
    CHECK(stock_find(&t, 9)->available == 0);

    /* A reader attaches read-only and sees the same numbers. */
    stock_table_t r;
    CHECK(stock_attach(&r, name) == 0);
    CHECK(stock_find(&r, 9) != NULL && stock_find(&r, 9)->available == 0);
    stock_close(&r, false);

    stock_close(&t, true);
    CHECK(stock_attach(&r, name) == -1); /* unlinked */
}

int main(void)
{
    test_layout();
    test_basic();
    test_probing_and_full();
    test_reserve_all();
    test_threads();
    test_processes();
    CHECK_DONE();
}
