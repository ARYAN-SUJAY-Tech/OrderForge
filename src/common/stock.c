#include "common/stock.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static uint32_t round_pow2(uint32_t n)
{
    uint32_t p = 64;
    while (p < n && p < (1u << 24))
        p <<= 1;
    return p;
}

static size_t table_bytes(uint32_t capacity)
{
    return sizeof(stock_shm_t) + (size_t)capacity * sizeof(stock_slot_t);
}

/* Fibonacci hashing spreads sequential product ids across the table. */
static uint64_t slot_index(const stock_table_t *t, uint64_t product_id)
{
    return (product_id * 0x9E3779B97F4A7C15ull) >> 17 & t->mask;
}

static void init_header(stock_table_t *t, uint32_t capacity)
{
    stock_shm_t *h = t->shm;
    memset(h, 0, t->map_size);
    h->capacity = capacity;
    h->abi_version = OF_STOCK_ABI;
    t->mask = capacity - 1;
    /* magic last, so a reader that sees it sees an initialised header */
    atomic_thread_fence(memory_order_release);
    h->magic = OF_STOCK_MAGIC;
}

static int map_fd(stock_table_t *t, int fd, size_t size, int prot)
{
    void *p = mmap(NULL, size, prot, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED)
        return -1;
    t->shm = p;
    t->map_size = size;
    return 0;
}

int stock_create(stock_table_t *t, const char *name, uint32_t capacity)
{
    memset(t, 0, sizeof *t);
    capacity = round_pow2(capacity);
    snprintf(t->name, sizeof t->name, "%s", name);

    int fd = shm_open(name, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    size_t size = table_bytes(capacity);
    if (ftruncate(fd, (off_t)size) != 0 || map_fd(t, fd, size, PROT_READ | PROT_WRITE) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    close(fd);
    pthread_mutex_init(&t->insert_lock, NULL);
    t->owner = true;
    init_header(t, capacity);
    return 0;
}

int stock_create_anon(stock_table_t *t, uint32_t capacity)
{
    memset(t, 0, sizeof *t);
    capacity = round_pow2(capacity);
    size_t size = table_bytes(capacity);
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return -1;
    t->shm = p;
    t->map_size = size;
    pthread_mutex_init(&t->insert_lock, NULL);
    t->owner = true;
    init_header(t, capacity);
    return 0;
}

int stock_attach(stock_table_t *t, const char *name)
{
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", name);
    int fd = shm_open(name, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    struct stat st;
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof(stock_shm_t)) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    int rc = map_fd(t, fd, (size_t)st.st_size, PROT_READ);
    close(fd);
    if (rc != 0)
        return -1;

    stock_shm_t *h = t->shm;
    if (h->magic != OF_STOCK_MAGIC || h->abi_version != OF_STOCK_ABI ||
        table_bytes(h->capacity) > t->map_size) {
        munmap(t->shm, t->map_size);
        t->shm = NULL;
        errno = EPROTO;
        return -1;
    }
    atomic_thread_fence(memory_order_acquire);
    t->mask = h->capacity - 1;
    return 0;
}

void stock_close(stock_table_t *t, bool unlink_name)
{
    if (t->shm)
        munmap(t->shm, t->map_size);
    t->shm = NULL;
    if (t->owner) {
        pthread_mutex_destroy(&t->insert_lock);
        if (unlink_name && t->name[0])
            shm_unlink(t->name);
    }
}

stock_slot_t *stock_find(const stock_table_t *t, uint64_t product_id)
{
    if (product_id == 0 || !t->shm)
        return NULL;
    uint64_t i = slot_index(t, product_id);
    for (uint64_t probes = 0; probes <= t->mask; probes++, i = (i + 1) & t->mask) {
        stock_slot_t *s = &t->shm->slots[i];
        uint64_t id = atomic_load_explicit(&s->product_id, memory_order_acquire);
        if (id == product_id)
            return s;
        if (id == 0)
            return NULL;
    }
    return NULL;
}

stock_slot_t *stock_upsert(stock_table_t *t, uint64_t product_id, int32_t on_hand,
                           int64_t price_paise)
{
    if (product_id == 0) {
        errno = EINVAL;
        return NULL;
    }
    pthread_mutex_lock(&t->insert_lock);
    stock_slot_t *found = NULL;
    uint64_t i = slot_index(t, product_id);
    for (uint64_t probes = 0; probes <= t->mask; probes++, i = (i + 1) & t->mask) {
        stock_slot_t *s = &t->shm->slots[i];
        uint64_t id = atomic_load_explicit(&s->product_id, memory_order_relaxed);
        if (id == product_id) {
            found = s;
            break;
        }
        if (id == 0) {
            /* Initialise first, publish the id last (release). */
            atomic_store_explicit(&s->available, on_hand, memory_order_relaxed);
            atomic_store_explicit(&s->reserved, 0, memory_order_relaxed);
            atomic_store_explicit(&s->price_paise, price_paise, memory_order_relaxed);
            atomic_store_explicit(&s->version, 1, memory_order_relaxed);
            atomic_store_explicit(&s->product_id, product_id, memory_order_release);
            atomic_fetch_add(&t->shm->count, 1);
            pthread_mutex_unlock(&t->insert_lock);
            return s;
        }
    }
    if (found) {
        atomic_store(&found->available, on_hand);
        atomic_store(&found->reserved, 0);
        atomic_store(&found->price_paise, price_paise);
        atomic_fetch_add(&found->version, 1);
    } else {
        errno = ENOSPC;
    }
    pthread_mutex_unlock(&t->insert_lock);
    return found;
}

bool stock_reserve(stock_slot_t *s, int32_t qty, uint64_t *retries)
{
    if (qty <= 0)
        return false;
    int32_t cur = atomic_load_explicit(&s->available, memory_order_acquire);
    for (;;) {
        if (cur < qty)
            return false;
        /* On failure the CAS writes the current value into cur; loop and re-check. */
        if (atomic_compare_exchange_weak_explicit(&s->available, &cur, cur - qty,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            atomic_fetch_add_explicit(&s->reserved, qty, memory_order_relaxed);
            atomic_fetch_add_explicit(&s->version, 1, memory_order_relaxed);
            return true;
        }
        if (retries)
            (*retries)++;
    }
}

void stock_commit(stock_slot_t *s, int32_t qty)
{
    atomic_fetch_sub_explicit(&s->reserved, qty, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->version, 1, memory_order_relaxed);
}

void stock_release(stock_slot_t *s, int32_t qty)
{
    atomic_fetch_sub_explicit(&s->reserved, qty, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->available, qty, memory_order_release);
    atomic_fetch_add_explicit(&s->version, 1, memory_order_relaxed);
}

bool stock_adjust(stock_slot_t *s, int32_t delta)
{
    int32_t cur = atomic_load_explicit(&s->available, memory_order_acquire);
    for (;;) {
        if (delta < 0 && cur + delta < 0)
            return false;
        if (atomic_compare_exchange_weak_explicit(&s->available, &cur, cur + delta,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            atomic_fetch_add_explicit(&s->version, 1, memory_order_relaxed);
            return true;
        }
    }
}

static int cmp_line(const void *a, const void *b)
{
    uint64_t x = ((const stock_line_t *)a)->product_id;
    uint64_t y = ((const stock_line_t *)b)->product_id;
    return (x > y) - (x < y);
}

bool stock_reserve_all(stock_table_t *t, stock_line_t *lines, size_t n, size_t *failed_index,
                       uint64_t *retries)
{
    qsort(lines, n, sizeof *lines, cmp_line);
    for (size_t i = 0; i < n; i++) {
        stock_slot_t *s = stock_find(t, lines[i].product_id);
        if (!s || !stock_reserve(s, lines[i].qty, retries)) {
            /* Give back what this call already took. */
            for (size_t j = 0; j < i; j++)
                stock_release(stock_find(t, lines[j].product_id), lines[j].qty);
            if (failed_index)
                *failed_index = i;
            return false;
        }
    }
    return true;
}

void stock_commit_all(stock_table_t *t, const stock_line_t *lines, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        stock_slot_t *s = stock_find(t, lines[i].product_id);
        if (s)
            stock_commit(s, lines[i].qty);
    }
}

void stock_release_all(stock_table_t *t, const stock_line_t *lines, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        stock_slot_t *s = stock_find(t, lines[i].product_id);
        if (s)
            stock_release(s, lines[i].qty);
    }
}
