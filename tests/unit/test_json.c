#include "check.h"
#include "common/json.h"

#include <stdint.h>

static json_obj_t obj;

static int parse(const char *s)
{
    char err[64];
    return json_parse_object(&obj, s, strlen(s), err, sizeof err);
}

static void test_valid(void)
{
    CHECK(parse("{}") == 0 && obj.count == 0);
    CHECK(parse("  { \"a\" : 1 , \"b\":\"x\", \"c\":true, \"d\":false, \"e\":null }  ") == 0);
    CHECK(obj.count == 5);

    int64_t v;
    bool b;
    CHECK(json_get_int(&obj, "a", &v) == 0 && v == 1);
    CHECK_STR(json_get_str(&obj, "b"), "x");
    CHECK(json_get_bool(&obj, "c", &b) == 0 && b);
    CHECK(json_get_bool(&obj, "d", &b) == 0 && !b);
    CHECK(json_get(&obj, "e")->type == JSON_NULL);
    CHECK(json_get_str(&obj, "a") == NULL);       /* wrong type */
    CHECK(json_get_int(&obj, "b", &v) == -1);     /* wrong type */
    CHECK(json_get(&obj, "missing") == NULL);

    CHECK(parse("{\"n\":-9223372036854775808,\"m\":9223372036854775807}") == 0);
    CHECK(json_get_int(&obj, "n", &v) == 0 && v == INT64_MIN);
    CHECK(json_get_int(&obj, "m", &v) == 0 && v == INT64_MAX);
}

static void test_escapes(void)
{
    CHECK(parse("{\"s\":\"q\\\" b\\\\ n\\n t\\t s\\/\"}") == 0);
    CHECK_STR(json_get_str(&obj, "s"), "q\" b\\ n\n t\t s/");
    CHECK(parse("{\"s\":\"\\u20b9 \\u00e9\"}") == 0);                 /* ₹ é */
    CHECK_STR(json_get_str(&obj, "s"), "\xe2\x82\xb9 \xc3\xa9");
    CHECK(parse("{\"s\":\"\\ud83d\\ude00\"}") == 0);                  /* surrogate pair */
    CHECK_STR(json_get_str(&obj, "s"), "\xf0\x9f\x98\x80");
}

static void test_invalid(void)
{
    const char *bad[] = {
        "", "[]", "{", "{\"a\"}", "{\"a\":}", "{\"a\":1,}", "{a:1}", "{\"a\":1} x",
        "{\"a\":1.5}", "{\"a\":1e3}", "{\"a\":01}", "{\"a\":-}", "{\"a\":{}}", "{\"a\":[1]}",
        "{\"a\":\"unterminated}", "{\"a\":\"bad \\x escape\"}", "{\"a\":\"\\ud800\"}",
        "{\"a\":\"\\u0000\"}", "{\"a\":\"tab\there\"}", "{\"a\":1,\"a\":2}",
        "{\"a\":9223372036854775808}", "{\"a\":tru}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int rc = parse(bad[i]);
        if (rc != -1)
            fprintf(stderr, "  accepted invalid input: %s\n", bad[i]);
        CHECK(rc == -1);
    }

    /* Field limit. */
    char big[2048] = "{";
    for (int i = 0; i < JSON_MAX_FIELDS + 1; i++) {
        char f[32];
        snprintf(f, sizeof f, "%s\"k%d\":%d", i ? "," : "", i, i);
        strcat(big, f);
    }
    strcat(big, "}");
    CHECK(parse(big) == -1);
}

int main(void)
{
    test_valid();
    test_escapes();
    test_invalid();
    CHECK_DONE();
}
