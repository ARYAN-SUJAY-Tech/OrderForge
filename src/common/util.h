/*
 * util.h - small helpers: kernel randomness, hex encoding, URL query parsing.
 */
#ifndef OF_UTIL_H
#define OF_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/* Monotonic time in microseconds. */
int64_t of_now_us(void);

#endif
