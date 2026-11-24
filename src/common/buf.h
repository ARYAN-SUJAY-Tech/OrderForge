/*
 * buf.h - growable byte buffer, used to build JSON responses.
 *
 * Allocation failure is sticky: after one failed grow every append is a no-op and
 * buf_failed() returns true, so callers check once at the end instead of after
 * every append.
 */
#ifndef OF_BUF_H
#define OF_BUF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char  *data;   /* always NUL-terminated when non-NULL */
    size_t len;
    size_t cap;
    size_t limit;  /* hard cap on len; 0 means 16 MiB */
    bool   failed;
} buf_t;

void buf_init(buf_t *b);
void buf_free(buf_t *b);
void buf_reset(buf_t *b);
bool buf_failed(const buf_t *b);

void buf_append(buf_t *b, const void *data, size_t len);
void buf_puts(buf_t *b, const char *s);
void buf_printf(buf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* JSON helpers: write a quoted, escaped string ("null" when s is NULL) or an integer. */
void buf_json_str(buf_t *b, const char *s);
void buf_json_int(buf_t *b, int64_t v);

/* Convenience for objects: writes "key": then the value. The caller adds commas. */
void buf_json_kv_str(buf_t *b, const char *key, const char *val);
void buf_json_kv_int(buf_t *b, const char *key, int64_t val);
void buf_json_kv_bool(buf_t *b, const char *key, bool val);

#endif
