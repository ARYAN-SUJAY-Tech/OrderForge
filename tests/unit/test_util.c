#include "check.h"
#include "common/buf.h"
#include "common/util.h"

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

static void test_query(void)
{
    char out[32];
    const char *q = "q=red+shoe%21&category=3&empty=&flag&page=2";
    CHECK(of_query_get(q, "q", out, sizeof out) == 0);
    CHECK_STR(out, "red shoe!");
    CHECK(of_query_get(q, "empty", out, sizeof out) == 0 && out[0] == '\0');
    CHECK(of_query_get(q, "flag", out, sizeof out) == 0 && out[0] == '\0');
    CHECK(of_query_get(q, "cat", out, sizeof out) == -1);       /* prefix is not a match */
    CHECK(of_query_get(q, "missing", out, sizeof out) == -1);
    CHECK(of_query_get("q=%2", "q", out, sizeof out) == -1);      /* truncated escape */
    CHECK(of_query_get("q=%zz", "q", out, sizeof out) == -1);     /* bad hex */
    CHECK(of_query_get("q=%00", "q", out, sizeof out) == -1);     /* NUL byte */
    CHECK(of_query_get("q=0123456789", "q", out, 5) == -1);       /* too long */
    CHECK(of_query_get(NULL, "q", out, sizeof out) == -1);

    CHECK(of_query_get_int(q, "page", 1, 1, 100) == 2);
    CHECK(of_query_get_int(q, "category", 0, 0, 2) == 0);         /* out of range: default */
    CHECK(of_query_get_int(q, "q", 7, 0, 100) == 7);              /* not a number: default */
    CHECK(of_query_get_int(q, "nope", 9, 0, 100) == 9);
}

static void test_parse_i64(void)
{
    int64_t v;
    CHECK(of_parse_i64("42", 0, 100, &v) == 0 && v == 42);
    CHECK(of_parse_i64("-1", 0, 100, &v) == -1);
    CHECK(of_parse_i64("12abc", 0, 100, &v) == -1);
    CHECK(of_parse_i64("", 0, 100, &v) == -1);
    CHECK(of_parse_i64("99999999999999999999", 0, INT64_MAX, &v) == -1);
}

static void test_cookie(void)
{
    char out[80];
    const char *h = "theme=dark; of_session=abc123; other=1";
    CHECK(of_cookie_get(h, "of_session", out, sizeof out) == 0);
    CHECK_STR(out, "abc123");
    CHECK(of_cookie_get(h, "theme", out, sizeof out) == 0);
    CHECK_STR(out, "dark");
    CHECK(of_cookie_get(h, "of_sess", out, sizeof out) == -1);
    CHECK(of_cookie_get("of_session=toolong", "of_session", out, 4) == -1);
    CHECK(of_cookie_get(NULL, "of_session", out, sizeof out) == -1);
}

static void test_random_hex(void)
{
    unsigned char a[32], b[32];
    CHECK(of_random_bytes(a, sizeof a) == 0);
    CHECK(of_random_bytes(b, sizeof b) == 0);
    CHECK(memcmp(a, b, sizeof a) != 0);

    char hex[9];
    of_hex(hex, "\x00\xab\xff\x10", 4);
    CHECK_STR(hex, "00abff10");
}

static void test_buf(void)
{
    buf_t b;
    buf_init(&b);
    buf_puts(&b, "{");
    buf_json_kv_str(&b, "s", "a\"b<\n");
    buf_puts(&b, ",");
    buf_json_kv_int(&b, "n", -5);
    buf_puts(&b, ",");
    buf_json_kv_bool(&b, "t", true);
    buf_puts(&b, ",\"z\":");
    buf_json_str(&b, NULL);
    buf_puts(&b, "}");
    CHECK(!buf_failed(&b));
    CHECK_STR(b.data, "{\"s\":\"a\\\"b\\u003c\\n\",\"n\":-5,\"t\":true,\"z\":null}");

    /* Growth across many appends. */
    buf_reset(&b);
    for (int i = 0; i < 10000; i++)
        buf_printf(&b, "%d,", i);
    CHECK(!buf_failed(&b) && b.len > 40000);

    /* The size limit makes failure sticky. */
    buf_reset(&b);
    b.limit = 8;
    buf_puts(&b, "12345678");
    CHECK(!buf_failed(&b));
    buf_puts(&b, "9");
    CHECK(buf_failed(&b));
    buf_puts(&b, "more");
    CHECK(b.len == 8);
    buf_free(&b);
}

static void test_mkdir_p(void)
{
    char base[] = "/tmp/of_test_mkdir_XXXXXX";
    CHECK(mkdtemp(base) != NULL);
    char deep[128], file[128];
    snprintf(deep, sizeof deep, "%s/run/log/nested", base);
    CHECK(of_mkdir_p(deep, 0750) == 0);
    CHECK(access(deep, W_OK) == 0);
    CHECK(of_mkdir_p(deep, 0750) == 0); /* already exists: still fine */

    snprintf(file, sizeof file, "%s/run/afile", base);
    FILE *f = fopen(file, "w");
    CHECK(f != NULL);
    if (f)
        fclose(f);
    char under_file[160];
    snprintf(under_file, sizeof under_file, "%s/sub", file);
    CHECK(of_mkdir_p(under_file, 0750) == -1); /* parent is a file */
    CHECK(of_mkdir_p(file, 0750) == -1);       /* path is a file */

    unlink(file);
    rmdir(deep);
    snprintf(deep, sizeof deep, "%s/run/log", base);
    rmdir(deep);
    snprintf(deep, sizeof deep, "%s/run", base);
    rmdir(deep);
    rmdir(base);
}

int main(void)
{
    test_mkdir_p();
    test_query();
    test_parse_i64();
    test_cookie();
    test_random_hex();
    test_buf();
    CHECK_DONE();
}
