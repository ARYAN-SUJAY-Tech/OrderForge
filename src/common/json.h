/*
 * json.h - strict parser for flat JSON request bodies.
 *
 * Accepts exactly one object whose values are strings, integers, true, false or null:
 *   {"email":"a@b.c","qty":3,"active":true}
 * Nested objects, arrays, fractions and exponents are rejected: no OrderForge request
 * needs them, and a smaller grammar is a smaller attack surface. Strings are decoded
 * (including \uXXXX and surrogate pairs) into an arena inside the struct, so the
 * parsed object owns everything and needs no free.
 */
#ifndef OF_JSON_H
#define OF_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JSON_MAX_FIELDS 32
#define JSON_MAX_TEXT   16384  /* also the largest accepted request body */

typedef enum { JSON_STRING, JSON_INT, JSON_BOOL, JSON_NULL } json_type_t;

typedef struct {
    const char *key;
    json_type_t type;
    const char *str;   /* JSON_STRING */
    int64_t     num;   /* JSON_INT; JSON_BOOL as 0/1 */
} json_field_t;

typedef struct {
    size_t       count;
    json_field_t fields[JSON_MAX_FIELDS];
    size_t       arena_used;
    char         arena[JSON_MAX_TEXT + JSON_MAX_FIELDS * 2];
} json_obj_t;

/* Returns 0 on success, -1 with a short reason in err. */
int json_parse_object(json_obj_t *obj, const char *text, size_t len, char *err, size_t errlen);

const json_field_t *json_get(const json_obj_t *obj, const char *key);

/* NULL when absent or not a string. */
const char *json_get_str(const json_obj_t *obj, const char *key);

/* 0 and *out set when present as an integer, -1 otherwise. */
int json_get_int(const json_obj_t *obj, const char *key, int64_t *out);

/* 0 and *out set when present as a boolean, -1 otherwise. */
int json_get_bool(const json_obj_t *obj, const char *key, bool *out);

#endif
