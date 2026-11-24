/*
 * Spike 3: processes sharing a POSIX shared-memory counter.
 *
 * The parent creates a shared-memory object, forks N children, and each child
 * increments two counters M times:
 *   - "plain":  counter = counter + 1   (read-modify-write, races between processes)
 *   - "atomic": atomic_fetch_add        (one indivisible instruction)
 *
 * The plain counter loses updates; the atomic one is always exact. This is the same
 * effect as the naive vs lock-free stock modes in phase 2, in miniature.
 *
 *   usage: shm_counter [processes=4] [increments=1000000]
 */
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    volatile int64_t plain;                 /* deliberately racy */
    char             pad[56];               /* keep the counters on separate cache lines */
    _Atomic int64_t  atomic;
} counters_t;

static long arg_long(const char *s, long def)
{
    if (!s)
        return def;
    char *end;
    long v = strtol(s, &end, 10);
    if (*end != '\0' || v <= 0) {
        fprintf(stderr, "invalid number: %s\n", s);
        exit(2);
    }
    return v;
}

int main(int argc, char **argv)
{
    long nproc = arg_long(argc > 1 ? argv[1] : NULL, 4);
    long iters = arg_long(argc > 2 ? argv[2] : NULL, 1000000);

    char name[64];
    snprintf(name, sizeof name, "/of_spike_counter_%d", (int)getpid());

    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        perror("shm_open");
        return 1;
    }
    /* The name is only needed to open it; unlink now so it cannot leak if we crash. */
    shm_unlink(name);

    if (ftruncate(fd, sizeof(counters_t)) != 0) {
        perror("ftruncate");
        return 1;
    }
    counters_t *c = mmap(NULL, sizeof *c, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (c == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    close(fd);

    for (long p = 0; p < nproc; p++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        if (pid == 0) {
            for (long i = 0; i < iters; i++) {
                c->plain = c->plain + 1;
                atomic_fetch_add_explicit(&c->atomic, 1, memory_order_relaxed);
            }
            _exit(0);
        }
    }

    int failed = 0;
    for (long p = 0; p < nproc; p++) {
        int status;
        if (wait(&status) < 0) {
            perror("wait");
            return 1;
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            failed++;
    }

    int64_t expected = (int64_t)nproc * iters;
    int64_t atomic_v = atomic_load(&c->atomic);
    printf("processes=%ld increments=%ld expected=%lld\n", nproc, iters, (long long)expected);
    printf("  plain  counter = %lld  (lost %lld updates)\n", (long long)c->plain,
           (long long)(expected - c->plain));
    printf("  atomic counter = %lld  (%s)\n", (long long)atomic_v,
           atomic_v == expected ? "exact" : "WRONG");

    munmap(c, sizeof *c);
    return (failed == 0 && atomic_v == expected) ? 0 : 1;
}
