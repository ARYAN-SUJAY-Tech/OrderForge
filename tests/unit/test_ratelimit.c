/* Token buckets: burst, refill, independence of keys, and exactness when many threads
 * and many processes take from the same bucket at once. */
#include "check.h"
#include "common/ratelimit.h"

#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>

static void test_burst_and_refill(void)
{
    rl_table_t t;
    CHECK(rl_create_anon(&t, 1024) == 0);
    rl_rule_t r = {.rate_milli_per_s = 10000, .burst = 5}; /* 10/s, bucket of 5 */
    int64_t now = t.shm->base_ms + 1000;

    int allowed = 0;
    for (int i = 0; i < 8; i++)
        allowed += rl_take(&t, "ip:1.2.3.4", &r, now) == 0;
    CHECK(allowed == 5);                               /* the burst, then no more */
    int64_t wait = rl_take(&t, "ip:1.2.3.4", &r, now);
    CHECK(wait == 100);                                /* one token per 100 ms */
    CHECK(rl_take(&t, "ip:1.2.3.4", &r, now + 99) > 0);
    CHECK(rl_take(&t, "ip:1.2.3.4", &r, now + 100) == 0); /* refilled exactly one */
    CHECK(rl_take(&t, "ip:1.2.3.4", &r, now + 100) > 0);

    /* After a long pause the bucket is full again, but never more than full. */
    allowed = 0;
    for (int i = 0; i < 10; i++)
        allowed += rl_take(&t, "ip:1.2.3.4", &r, now + 60000) == 0;
    CHECK(allowed == 5);

    /* Keys are independent. */
    CHECK(rl_take(&t, "ip:5.6.7.8", &r, now) == 0);

    /* A slow rule: 30 per minute = 500 thousandths per second -> 2 s per token. */
    rl_rule_t slow = {.rate_milli_per_s = 500, .burst = 1};
    CHECK(rl_take(&t, "auth:x", &slow, now) == 0);
    CHECK(rl_take(&t, "auth:x", &slow, now) == 2000);
    CHECK(rl_take(&t, "auth:x", &slow, now + 2000) == 0);

    CHECK(atomic_load(&t.shm->allowed) > 0 && atomic_load(&t.shm->limited) > 0);
    rl_close(&t, false);
}

static void test_table_full_fails_open(void)
{
    rl_table_t t;
    CHECK(rl_create_anon(&t, 64) == 0); /* 64 slots, 32-slot probe window */
    rl_rule_t r = {.rate_milli_per_s = 0, .burst = 1};
    char key[32];
    int64_t now = t.shm->base_ms;
    for (int i = 0; i < 500; i++) {
        snprintf(key, sizeof key, "ip:10.0.%d.%d", i / 256, i % 256);
        rl_take(&t, key, &r, now);
    }
    CHECK(atomic_load(&t.shm->table_full) > 0); /* overflow was allowed, and counted */
    rl_close(&t, false);
}

#define THREADS 8
static rl_table_t shared;
static _Atomic int thread_wins;

static void *taker(void *arg)
{
    (void)arg;
    rl_rule_t r = {.rate_milli_per_s = 0, .burst = 1000}; /* no refill: exactly 1000 */
    int64_t now = shared.shm->base_ms + 5;
    for (int i = 0; i < 1000; i++)
        if (rl_take(&shared, "s:hot", &r, now) == 0)
            atomic_fetch_add(&thread_wins, 1);
    return NULL;
}

static void test_threads_and_processes(void)
{
    CHECK(rl_create_anon(&shared, 1024) == 0);
    pthread_t th[THREADS];
    for (int i = 0; i < THREADS; i++)
        pthread_create(&th[i], NULL, taker, NULL);
    for (int i = 0; i < THREADS; i++)
        pthread_join(th[i], NULL);
    CHECK(atomic_load(&thread_wins) == 1000); /* 8000 attempts, exactly the bucket */

    /* The same across processes sharing the mapping, like of-api's workers. */
    rl_rule_t r = {.rate_milli_per_s = 0, .burst = 500};
    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    for (int p = 0; p < 6; p++) {
        if (fork() == 0) {
            int wins = 0;
            int64_t now = shared.shm->base_ms + 5;
            for (int i = 0; i < 300; i++)
                wins += rl_take(&shared, "ip:proc", &r, now) == 0;
            if (write(pipefd[1], &wins, sizeof wins) != sizeof wins)
                _exit(1);
            _exit(0);
        }
    }
    close(pipefd[1]);
    int total = 0, w;
    while (read(pipefd[0], &w, sizeof w) == sizeof w)
        total += w;
    close(pipefd[0]);
    for (int p = 0; p < 6; p++)
        wait(NULL);
    CHECK(total == 500); /* 1800 attempts from 6 processes, exactly 500 allowed */
    printf("  rate limit: 8 threads -> %d of 8000 allowed; 6 processes -> %d of 1800 allowed\n",
           atomic_load(&thread_wins), total);
    rl_close(&shared, false);
}

static void test_named(void)
{
    char name[64];
    snprintf(name, sizeof name, "/of_test_rl_%d", (int)getpid());
    rl_table_t t, r;
    CHECK(rl_create(&t, name, 128) == 0);
    rl_rule_t rule = {.rate_milli_per_s = 1000, .burst = 1};
    rl_take(&t, "k", &rule, rl_now_ms());
    CHECK(rl_attach(&r, name) == 0 && atomic_load(&r.shm->allowed) == 1);
    rl_close(&r, false);
    rl_close(&t, true);
    CHECK(rl_attach(&r, name) == -1);
}

int main(void)
{
    test_burst_and_refill();
    test_table_full_fails_open();
    test_threads_and_processes();
    test_named();
    CHECK_DONE();
}
