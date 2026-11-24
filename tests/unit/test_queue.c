/* Bounded queue: FIFO order, backpressure when full, and many producers/consumers. */
#include "check.h"
#include "common/queue.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

static void test_basic(void)
{
    queue_t q;
    CHECK(queue_init(&q, 3) == 0);
    int a, b, c, d;
    CHECK(queue_try_push(&q, &a) && queue_try_push(&q, &b) && queue_try_push(&q, &c));
    CHECK(!queue_try_push(&q, &d)); /* full: backpressure, never blocks */
    CHECK(queue_depth(&q) == 3);
    CHECK(queue_pop(&q) == &a);
    CHECK(queue_try_push(&q, &d)); /* wraps around the ring */
    CHECK(queue_pop(&q) == &b && queue_pop(&q) == &c && queue_pop(&q) == &d);
    CHECK(queue_depth(&q) == 0);

    CHECK(queue_try_push(&q, &a));
    queue_close(&q);
    CHECK(!queue_try_push(&q, &b)); /* closed */
    CHECK(queue_pop(&q) == &a);     /* remaining items still drain */
    CHECK(queue_pop(&q) == NULL);   /* then NULL, without blocking */
    queue_destroy(&q);
}

#define PRODUCERS 4
#define CONSUMERS 4
#define PER_PRODUCER 20000

static queue_t shared;
static _Atomic uint64_t sum_popped;
static _Atomic uint64_t n_popped;
static _Atomic uint64_t n_rejected;

static void *producer(void *arg)
{
    uintptr_t base = (uintptr_t)arg * PER_PRODUCER;
    for (uintptr_t i = 1; i <= PER_PRODUCER; i++) {
        /* Retry on full: counts how often backpressure happened. */
        while (!queue_try_push(&shared, (void *)(base + i)))
            atomic_fetch_add(&n_rejected, 1);
    }
    return NULL;
}

static void *consumer(void *arg)
{
    (void)arg;
    void *item;
    while ((item = queue_pop(&shared)) != NULL) {
        atomic_fetch_add(&sum_popped, (uint64_t)(uintptr_t)item);
        atomic_fetch_add(&n_popped, 1);
    }
    return NULL;
}

static void test_threads(void)
{
    CHECK(queue_init(&shared, 64) == 0);
    pthread_t p[PRODUCERS], c[CONSUMERS];
    for (uintptr_t i = 0; i < CONSUMERS; i++)
        pthread_create(&c[i], NULL, consumer, NULL);
    for (uintptr_t i = 0; i < PRODUCERS; i++)
        pthread_create(&p[i], NULL, producer, (void *)i);
    for (int i = 0; i < PRODUCERS; i++)
        pthread_join(p[i], NULL);
    queue_close(&shared);
    for (int i = 0; i < CONSUMERS; i++)
        pthread_join(c[i], NULL);

    uint64_t n = (uint64_t)PRODUCERS * PER_PRODUCER;
    CHECK(atomic_load(&n_popped) == n);
    CHECK(atomic_load(&sum_popped) == n * (n + 1) / 2); /* every item exactly once */
    printf("  queue: %llu items through a 64-slot ring, %llu full-queue rejections\n",
           (unsigned long long)n, (unsigned long long)atomic_load(&n_rejected));
    queue_destroy(&shared);
}

int main(void)
{
    test_basic();
    test_threads();
    CHECK_DONE();
}
