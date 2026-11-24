/* Protocol tests: round trips, caps, and every way a hostile frame can be malformed. */
#include "check.h"
#include "common/proto.h"

#include <stdlib.h>

static char arena[OF_MAX_REQ_PAYLOAD + 64];
static char arena2[OF_MAX_RESP_PAYLOAD + 64];

static const of_frame_hdr_t *hdr(const buf_t *b)
{
    return (const of_frame_hdr_t *)b->data;
}

static void test_request_roundtrip(void)
{
    buf_t b;
    buf_init(&b);
    of_req_t in = {OF_POST, "/api/cart/items", "a=1&b=2", "{\"qty\":2}", 9,
                   "abc", "def", "127.0.0.1", "retry-key-1"};
    CHECK(proto_encode_request(&b, 77, &in) == 0);
    CHECK(proto_check_header(hdr(&b), OF_MSG_REQUEST) == 0);
    CHECK(hdr(&b)->request_id == 77);
    CHECK(hdr(&b)->length == b.len - OF_PROTO_HDR_SIZE);
    CHECK(proto_check_header(hdr(&b), OF_MSG_RESPONSE) == -1); /* wrong type */

    of_req_t out;
    CHECK(proto_decode_request(b.data + OF_PROTO_HDR_SIZE, hdr(&b)->length, &out, arena,
                               sizeof arena) == 0);
    CHECK(out.method == OF_POST);
    CHECK_STR(out.path, "/api/cart/items");
    CHECK_STR(out.query, "a=1&b=2");
    CHECK_STR(out.body, "{\"qty\":2}");
    CHECK(out.body_len == 9);
    CHECK_STR(out.session_token, "abc");
    CHECK_STR(out.csrf_token, "def");
    CHECK_STR(out.ip, "127.0.0.1");
    CHECK_STR(out.idem_key, "retry-key-1");

    /* NULL fields encode as empty strings. */
    buf_reset(&b);
    of_req_t empty = {OF_GET, "/api/me", NULL, NULL, 0, NULL, NULL, NULL, NULL};
    CHECK(proto_encode_request(&b, 1, &empty) == 0);
    CHECK(proto_decode_request(b.data + OF_PROTO_HDR_SIZE, hdr(&b)->length, &out, arena,
                               sizeof arena) == 0);
    CHECK_STR(out.query, "");
    CHECK_STR(out.idem_key, "");
    CHECK(out.body_len == 0 && out.body[0] == '\0');
    buf_free(&b);
}

static void test_response_roundtrip(void)
{
    buf_t b;
    buf_init(&b);
    CHECK(proto_encode_response(&b, 5, 201, "of_session=x", "{\"ok\":true}", 11) == 0);
    CHECK(proto_check_header(hdr(&b), OF_MSG_RESPONSE) == 0);
    int st;
    const char *cookie, *body;
    size_t blen;
    CHECK(proto_decode_response(b.data + OF_PROTO_HDR_SIZE, hdr(&b)->length, &st, &cookie,
                                &body, &blen, arena2, sizeof arena2) == 0);
    CHECK(st == 201 && blen == 11);
    CHECK_STR(cookie, "of_session=x");
    CHECK_STR(body, "{\"ok\":true}");
    buf_free(&b);
}

static void test_caps(void)
{
    buf_t b;
    buf_init(&b);
    char *big = malloc(OF_MAX_PATH + 2);
    memset(big, 'a', OF_MAX_PATH + 1);
    big[OF_MAX_PATH + 1] = '\0';
    of_req_t in = {OF_GET, big, "", "", 0, "", "", "", ""};
    CHECK(proto_encode_request(&b, 1, &in) == -1);
    CHECK(b.len == 0); /* partial frame dropped */
    big[OF_MAX_PATH] = '\0';
    CHECK(proto_encode_request(&b, 1, &in) == 0); /* exactly at the cap is fine */
    free(big);

    /* A header claiming more than the cap is rejected before any read. */
    of_frame_hdr_t h = {OF_PROTO_MAGIC, OF_PROTO_VERSION, OF_MSG_REQUEST, 1,
                        OF_MAX_REQ_PAYLOAD + 1, 0};
    CHECK(proto_check_header(&h, OF_MSG_REQUEST) == -1);
    h.length = 10;
    h.magic ^= 1;
    CHECK(proto_check_header(&h, OF_MSG_REQUEST) == -1);
    h.magic = OF_PROTO_MAGIC;
    h.version = OF_PROTO_VERSION + 1;
    CHECK(proto_check_header(&h, OF_MSG_REQUEST) == -1);
    buf_free(&b);
}

static void test_malformed(void)
{
    buf_t b;
    buf_init(&b);
    of_req_t in = {OF_DELETE, "/api/x", "q", "body", 4, "s", "c", "ip", "k"};
    CHECK(proto_encode_request(&b, 1, &in) == 0);
    const char *p = b.data + OF_PROTO_HDR_SIZE;
    size_t len = hdr(&b)->length;
    of_req_t out;

    /* Every truncation must fail cleanly. */
    int all_fail = 1;
    for (size_t cut = 0; cut < len; cut++)
        if (proto_decode_request(p, cut, &out, arena, sizeof arena) == 0)
            all_fail = 0;
    CHECK(all_fail);

    /* Trailing garbage is rejected. */
    char *copy = malloc(len + 1);
    memcpy(copy, p, len);
    copy[len] = 'x';
    CHECK(proto_decode_request(copy, len + 1, &out, arena, sizeof arena) == -1);

    /* A field length pointing past the end. */
    uint32_t huge = 0xFFFFFFF0u;
    memcpy(copy + 1, &huge, sizeof huge);
    CHECK(proto_decode_request(copy, len, &out, arena, sizeof arena) == -1);

    /* Unknown method byte. */
    memcpy(copy, p, len);
    copy[0] = 9;
    CHECK(proto_decode_request(copy, len, &out, arena, sizeof arena) == -1);

    /* Arena too small. */
    CHECK(proto_decode_request(p, len, &out, arena, 8) == -1);
    free(copy);

    /* Response with an impossible status. */
    buf_reset(&b);
    CHECK(proto_encode_response(&b, 1, 42, "", "", 0) == 0);
    int st;
    const char *c, *body;
    size_t blen;
    CHECK(proto_decode_response(b.data + OF_PROTO_HDR_SIZE, hdr(&b)->length, &st, &c, &body,
                                &blen, arena2, sizeof arena2) == -1);
    buf_free(&b);
}

int main(void)
{
    test_request_roundtrip();
    test_response_roundtrip();
    test_caps();
    test_malformed();
    CHECK_DONE();
}
