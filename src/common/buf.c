#include "common/buf.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUF_DEFAULT_LIMIT (16u * 1024 * 1024)

void buf_init(buf_t *b)
{
    memset(b, 0, sizeof *b);
}

void buf_free(buf_t *b)
{
    free(b->data);
    memset(b, 0, sizeof *b);
}

void buf_reset(buf_t *b)
{
    b->len = 0;
    b->failed = false;
    if (b->data)
        b->data[0] = '\0';
}

bool buf_failed(const buf_t *b)
{
    return b->failed;
}

static bool reserve(buf_t *b, size_t extra)
{
    if (b->failed)
        return false;
    size_t limit = b->limit ? b->limit : BUF_DEFAULT_LIMIT;
    if (extra > limit || b->len > limit - extra) {
        b->failed = true;
        return false;
    }
    size_t need = b->len + extra + 1;
    if (need <= b->cap)
        return true;

    size_t cap = b->cap ? b->cap : 256;
    while (cap < need)
        cap *= 2;
    char *p = realloc(b->data, cap);
    if (!p) {
        b->failed = true;
        return false;
    }
    b->data = p;
    b->cap = cap;
    return true;
}

void buf_append(buf_t *b, const void *data, size_t len)
{
    if (!reserve(b, len))
        return;
    memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = '\0';
}

void buf_puts(buf_t *b, const char *s)
{
    buf_append(b, s, strlen(s));
}

void buf_printf(buf_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        b->failed = true;
        return;
    }
    if (!reserve(b, (size_t)n))
        return;
    va_start(ap, fmt);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

void buf_json_str(buf_t *b, const char *s)
{
    if (!s) {
        buf_puts(b, "null");
        return;
    }
    buf_append(b, "\"", 1);
    const char *run = s;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        const char *rep = NULL;
        char tmp[8];
        switch (*p) {
        case '"':  rep = "\\\""; break;
        case '\\': rep = "\\\\"; break;
        case '\n': rep = "\\n"; break;
        case '\r': rep = "\\r"; break;
        case '\t': rep = "\\t"; break;
        case '<':  rep = "\\u003c"; break; /* keeps "</script>" out of any HTML context */
        default:
            if (*p < 0x20) {
                snprintf(tmp, sizeof tmp, "\\u%04x", *p);
                rep = tmp;
            }
        }
        if (rep) {
            buf_append(b, run, (size_t)((const char *)p - run));
            buf_puts(b, rep);
            run = (const char *)p + 1;
        }
    }
    buf_puts(b, run);
    buf_append(b, "\"", 1);
}

void buf_json_int(buf_t *b, int64_t v)
{
    buf_printf(b, "%" PRId64, v);
}

void buf_json_kv_str(buf_t *b, const char *key, const char *val)
{
    buf_json_str(b, key);
    buf_append(b, ":", 1);
    buf_json_str(b, val);
}

void buf_json_kv_int(buf_t *b, const char *key, int64_t val)
{
    buf_json_str(b, key);
    buf_append(b, ":", 1);
    buf_json_int(b, val);
}

void buf_json_kv_bool(buf_t *b, const char *key, bool val)
{
    buf_json_str(b, key);
    buf_append(b, ":", 1);
    buf_puts(b, val ? "true" : "false");
}
