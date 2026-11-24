/*
 * util.h - small helpers: kernel randomness, hex encoding, URL query parsing.
 */
#ifndef OF_UTIL_H
#define OF_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Fill buf from getrandom(2), retrying on EINTR and short reads. 0 or -1. */
int of_random_bytes(void *buf, size_t len);

/* Lower-case hex; out must hold 2 * len + 1 bytes. */
void of_hex(char *out, const void *data, size_t len);

/*
 * Find key in an application/x-www-form-urlencoded query string and URL-decode its
 * value into out. Returns 0 when found (out may be ""), -1 when absent, too long or
 * malformed.
 */
int of_query_get(const char *query, const char *key, char *out, size_t outlen);

/* Like of_query_get but parses a base-10 integer in [min, max]; def when absent. */
int64_t of_query_get_int(const char *query, const char *key, int64_t def,
                         int64_t min, int64_t max);

/* Strict base-10 parse of the whole string into [min, max]. 0 or -1. */
int of_parse_i64(const char *s, int64_t min, int64_t max, int64_t *out);

/* Value of cookie name from an HTTP Cookie header into out. 0 or -1. */
int of_cookie_get(const char *header, const char *name, char *out, size_t outlen);

/* Create a directory and any missing parents, like `mkdir -p`. 0 or -1 with errno. */
int of_mkdir_p(const char *path, unsigned mode);

/*
 * PID files with flock(2). of_pidfile_acquire takes an exclusive, non-blocking lock on
 * the file and writes our pid into it; keep the returned descriptor open for the life of
 * the process. The kernel releases the lock when the process dies, even on kill -9, so
 * a stale file never prevents a restart, and two copies can never run at once.
 * Returns the descriptor, or -1: errno EWOULDBLOCK (and *holder set) when another live
 * process holds the lock.
 */
int of_pidfile_acquire(const char *path, pid_t *holder);

/* The pid of the live process holding path's lock. 0 and *pid set, or -1 when nobody
 * holds it (no file, or a stale one). */
int of_pidfile_read(const char *path, pid_t *pid);

/* <run.dir>/<name>.pid, run.dir defaulting to "run". cfg is an of_config_t. */
void of_pidfile_path(const void *cfg, const char *name, char *out, size_t outlen);

/* Monotonic time in microseconds. */
int64_t of_now_us(void);

#endif
