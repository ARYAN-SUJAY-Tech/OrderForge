#include "common/ratelimit.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TOKEN      1000u                 /* one token, in thousandths */
#define TOK_MASK   ((1ull << 24) - 1)

int64_t rl_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint32_t pow2(uint32_t n)
{
    uint32_t p = 64;
    while (p < n && p < (1u << 24))
        p <<= 1;
    return p;
}

static void init(rl_table_t *t, uint32_t capacity)
{
    memset(t->shm, 0, t->size);
    t->shm->capacity = capacity;
    t->shm->abi = RL_ABI;
    t->shm->base_ms = rl_now_ms();
    t->mask = capacity - 1;
    atomic_thread_fence(memory_order_release);
    t->shm->magic = RL_MAGIC;
}

int rl_create(rl_table_t *t, const char *name, uint32_t capacity)
{
    memset(t, 0, sizeof *t);
    capacity = pow2(capacity);
    snprintf(t->name, sizeof t->name, "%s", name);
    t->size = sizeof(rl_shm_t) + (size_t)capacity * sizeof(rl_slot_t);
    int fd = shm_open(name, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, (off_t)t->size) != 0) {
        close(fd);
        return -1;
    }
    void *p = mmap(NULL, t->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED)
        return -1;
    t->shm = p;
    t->owner = true;
    init(t, capacity);
    return 0;
}

int rl_create_anon(rl_table_t *t, uint32_t capacity)
{
    memset(t, 0, sizeof *t);
    capacity = pow2(capacity);
    t->size = sizeof(rl_shm_t) + (size_t)capacity * sizeof(rl_slot_t);
    void *p = mmap(NULL, t->size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return -1;
    t->shm = p;
    t->owner = true;
    init(t, capacity);
    return 0;
}

int rl_attach(rl_table_t *t, const char *name)
{
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", name);
    int fd = shm_open(name, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof(rl_shm_t)) {
        close(fd);
        errno = EPROTO;
        return -1;
    }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED)
        return -1;
    t->shm = p;
    t->size = (size_t)st.st_size;
    if (t->shm->magic != RL_MAGIC || t->shm->abi != RL_ABI) {
        munmap(p, t->size);
        t->shm = NULL;
        errno = EPROTO;
        return -1;
    }
    t->mask = t->shm->capacity - 1;
    return 0;
}

void rl_close(rl_table_t *t, bool unlink_name)
{
    if (t->shm)
        munmap(t->shm, t->size);
    t->shm = NULL;
    if (t->owner && unlink_name && t->name[0])
        shm_unlink(t->name);
}

static uint64_t hash_key(const char *s)
{
    uint64_t h = 1469598103934665603ull; /* FNV-1a */
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 1099511628211ull;
    return h | 1; /* never 0: 0 marks an empty slot */
}

static rl_slot_t *find_or_claim(rl_table_t *t, uint64_t h)
{
    uint64_t i = h & t->mask;
    for (int probe = 0; probe < RL_PROBES; probe++, i = (i + 1) & t->mask) {
        rl_slot_t *s = &t->shm->slots[i];
        uint64_t k = atomic_load_explicit(&s->key, memory_order_acquire);
        if (k == h)
            return s;
        if (k == 0) {
            uint64_t expected = 0;
            if (atomic_compare_exchange_strong_explicit(&s->key, &expected, h,
                                                        memory_order_acq_rel,
                                                        memory_order_acquire) ||
                expected == h)
                return s; /* claimed it, or another process just claimed it for us */
        }
    }
    return NULL;
}

int64_t rl_take(rl_table_t *t, const char *key, const rl_rule_t *rule, int64_t now_ms)
{
    rl_slot_t *s = find_or_claim(t, hash_key(key));
    if (!s) {
        atomic_fetch_add_explicit(&t->shm->table_full, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&t->shm->allowed, 1, memory_order_relaxed);
        return 0; /* fail open: never lock real users out because of a key flood */
    }

    uint64_t full = (uint64_t)(rule->burst > RL_MAX_BURST ? RL_MAX_BURST : rule->burst) * TOKEN;
    uint64_t now = (uint64_t)(now_ms - t->shm->base_ms) & ((1ull << 40) - 1);
    uint64_t st = atomic_load_explicit(&s->state, memory_order_acquire);
    for (;;) {
        uint64_t last = st >> 24, tok = st & TOK_MASK;
        if (st == 0) { /* a brand-new bucket starts full */
            last = now;
            tok = full;
        }
        uint64_t elapsed = now > last ? now - last : 0;
        tok += elapsed * rule->rate_milli_per_s / 1000;
        if (tok > full)
            tok = full;
        if (tok < TOKEN) {
            atomic_fetch_add_explicit(&t->shm->limited, 1, memory_order_relaxed);
            if (rule->rate_milli_per_s == 0)
                return 60000;
            uint64_t wait = ((TOKEN - tok) * 1000 + rule->rate_milli_per_s - 1) / rule->rate_milli_per_s;
            return wait ? (int64_t)wait : 1;
        }
        uint64_t next = (now << 24) | (tok - TOKEN);
        if (next == 0)
            next = 1u << 24; /* keep "0 = new" unambiguous */
        if (atomic_compare_exchange_weak_explicit(&s->state, &st, next, memory_order_acq_rel,
                                                  memory_order_acquire)) {
            atomic_fetch_add_explicit(&t->shm->allowed, 1, memory_order_relaxed);
            return 0;
        }
        /* Lost the race to another worker: st now holds its value; recompute. */
    }
}
