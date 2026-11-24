#include "common/json.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *p;
    const char *end;
    json_obj_t *obj;
    char       *err;
    size_t      errlen;
} parser_t;

static int fail(parser_t *ps, const char *msg)
{
    snprintf(ps->err, ps->errlen, "%s", msg);
    return -1;
}

static void skip_ws(parser_t *ps)
{
    while (ps->p < ps->end &&
           (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static int arena_put(parser_t *ps, char c)
{
    json_obj_t *o = ps->obj;
    if (o->arena_used >= sizeof o->arena)
        return fail(ps, "body too large");
    o->arena[o->arena_used++] = c;
    return 0;
}

static int put_utf8(parser_t *ps, uint32_t cp)
{
    if (cp < 0x80)
        return arena_put(ps, (char)cp);
    if (cp < 0x800)
        return arena_put(ps, (char)(0xC0 | (cp >> 6))) ||
               arena_put(ps, (char)(0x80 | (cp & 0x3F)));
    if (cp < 0x10000)
        return arena_put(ps, (char)(0xE0 | (cp >> 12))) ||
               arena_put(ps, (char)(0x80 | ((cp >> 6) & 0x3F))) ||
               arena_put(ps, (char)(0x80 | (cp & 0x3F)));
    return arena_put(ps, (char)(0xF0 | (cp >> 18))) ||
           arena_put(ps, (char)(0x80 | ((cp >> 12) & 0x3F))) ||
           arena_put(ps, (char)(0x80 | ((cp >> 6) & 0x3F))) ||
           arena_put(ps, (char)(0x80 | (cp & 0x3F)));
}

static int hex4(parser_t *ps, uint32_t *out)
{
    if (ps->end - ps->p < 4)
        return fail(ps, "bad \\u escape");
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = *ps->p++;
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (uint32_t)(c - 'A' + 10);
        else
            return fail(ps, "bad \\u escape");
    }
    *out = v;
    return 0;
}

/* On entry ps->p points at the opening quote. */
static int parse_string(parser_t *ps, const char **out)
{
    ps->p++;
    const char *start = ps->obj->arena + ps->obj->arena_used;
    while (ps->p < ps->end) {
        unsigned char c = (unsigned char)*ps->p++;
        if (c == '"') {
            if (arena_put(ps, '\0'))
                return -1;
            *out = start;
            return 0;
        }
        if (c < 0x20)
            return fail(ps, "control character in string");
        if (c != '\\') {
            if (arena_put(ps, (char)c))
                return -1;
            continue;
        }
        if (ps->p >= ps->end)
            break;
        char e = *ps->p++;
        char lit;
        switch (e) {
        case '"':  lit = '"'; break;
        case '\\': lit = '\\'; break;
        case '/':  lit = '/'; break;
        case 'b':  lit = '\b'; break;
        case 'f':  lit = '\f'; break;
        case 'n':  lit = '\n'; break;
        case 'r':  lit = '\r'; break;
        case 't':  lit = '\t'; break;
        case 'u': {
            uint32_t cp;
            if (hex4(ps, &cp))
                return -1;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                uint32_t lo;
                if (ps->end - ps->p < 2 || ps->p[0] != '\\' || ps->p[1] != 'u')
                    return fail(ps, "unpaired surrogate");
                ps->p += 2;
                if (hex4(ps, &lo))
                    return -1;
                if (lo < 0xDC00 || lo > 0xDFFF)
                    return fail(ps, "unpaired surrogate");
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                return fail(ps, "unpaired surrogate");
            }
            if (cp == 0)
                return fail(ps, "NUL in string");
            if (put_utf8(ps, cp))
                return -1;
            continue;
        }
        default:
            return fail(ps, "bad escape");
        }
        if (arena_put(ps, lit))
            return -1;
    }
    return fail(ps, "unterminated string");
}

static int parse_int(parser_t *ps, int64_t *out)
{
    bool neg = false;
    if (*ps->p == '-') {
        neg = true;
        ps->p++;
    }
    if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9')
        return fail(ps, "bad number");
    if (*ps->p == '0' && ps->p + 1 < ps->end && ps->p[1] >= '0' && ps->p[1] <= '9')
        return fail(ps, "leading zero");

    uint64_t v = 0;
    const uint64_t limit = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') {
        unsigned d = (unsigned)(*ps->p++ - '0');
        if (v > (limit - d) / 10)
            return fail(ps, "number out of range");
        v = v * 10 + d;
    }
    if (ps->p < ps->end && (*ps->p == '.' || *ps->p == 'e' || *ps->p == 'E'))
        return fail(ps, "only integers are accepted");
    *out = neg ? (int64_t)(0 - v) : (int64_t)v;
    return 0;
}

static int match_word(parser_t *ps, const char *w)
{
    size_t n = strlen(w);
    if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, w, n) != 0)
        return -1;
    ps->p += n;
    return 0;
}

int json_parse_object(json_obj_t *obj, const char *text, size_t len, char *err, size_t errlen)
{
    obj->count = 0;
    obj->arena_used = 0;
    if (len > JSON_MAX_TEXT) {
        snprintf(err, errlen, "body too large");
        return -1;
    }

    parser_t ps = {text, text + len, obj, err, errlen};
    skip_ws(&ps);
    if (ps.p >= ps.end || *ps.p != '{')
        return fail(&ps, "expected a JSON object");
    ps.p++;
    skip_ws(&ps);
    if (ps.p < ps.end && *ps.p == '}') {
        ps.p++;
        goto done;
    }

    for (;;) {
        skip_ws(&ps);
        if (ps.p >= ps.end || *ps.p != '"')
            return fail(&ps, "expected a key");
        if (obj->count == JSON_MAX_FIELDS)
            return fail(&ps, "too many fields");
        json_field_t *f = &obj->fields[obj->count];
        if (parse_string(&ps, &f->key))
            return -1;
        if (json_get(obj, f->key))
            return fail(&ps, "duplicate key");

        skip_ws(&ps);
        if (ps.p >= ps.end || *ps.p != ':')
            return fail(&ps, "expected ':'");
        ps.p++;
        skip_ws(&ps);
        if (ps.p >= ps.end)
            return fail(&ps, "expected a value");

        char c = *ps.p;
        f->str = NULL;
        f->num = 0;
        if (c == '"') {
            f->type = JSON_STRING;
            if (parse_string(&ps, &f->str))
                return -1;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            f->type = JSON_INT;
            if (parse_int(&ps, &f->num))
                return -1;
        } else if (match_word(&ps, "true") == 0) {
            f->type = JSON_BOOL;
            f->num = 1;
        } else if (match_word(&ps, "false") == 0) {
            f->type = JSON_BOOL;
        } else if (match_word(&ps, "null") == 0) {
            f->type = JSON_NULL;
        } else if (c == '{' || c == '[') {
            return fail(&ps, "nested values are not accepted");
        } else {
            return fail(&ps, "bad value");
        }
        obj->count++;

        skip_ws(&ps);
        if (ps.p < ps.end && *ps.p == ',') {
            ps.p++;
            continue;
        }
        if (ps.p < ps.end && *ps.p == '}') {
            ps.p++;
            break;
        }
        return fail(&ps, "expected ',' or '}'");
    }

done:
    skip_ws(&ps);
    if (ps.p != ps.end)
        return fail(&ps, "trailing data after object");
    return 0;
}

const json_field_t *json_get(const json_obj_t *obj, const char *key)
{
    for (size_t i = 0; i < obj->count; i++)
        if (strcmp(obj->fields[i].key, key) == 0)
            return &obj->fields[i];
    return NULL;
}

const char *json_get_str(const json_obj_t *obj, const char *key)
{
    const json_field_t *f = json_get(obj, key);
    return f && f->type == JSON_STRING ? f->str : NULL;
}

int json_get_int(const json_obj_t *obj, const char *key, int64_t *out)
{
    const json_field_t *f = json_get(obj, key);
    if (!f || f->type != JSON_INT)
        return -1;
    *out = f->num;
    return 0;
}

int json_get_bool(const json_obj_t *obj, const char *key, bool *out)
{
    const json_field_t *f = json_get(obj, key);
    if (!f || f->type != JSON_BOOL)
        return -1;
    *out = f->num != 0;
    return 0;
}
