/*
 * proto.h - the binary protocol between of-api and of-core (Unix domain socket).
 *
 * Every message is one frame: a fixed 24-byte header followed by `length` payload bytes.
 *
 *   offset size  field
 *   0      4     magic       "OFP1"
 *   4      2     version     2 (bumped when the payload layout changes)
 *   6      2     type        1 = request, 2 = response
 *   8      8     request_id  chosen by the sender of the request, echoed in the response
 *   16     4     length      payload bytes (hard cap per type, checked before reading)
 *   20     4     reserved    0
 *
 * Request payload:  u8 method, then seven fields: path, query, body, session, csrf, ip,
 *                   idempotency key
 * Response payload: u16 status, then two fields: set_cookie, body
 * A field is u32 length + that many bytes (no terminator). Each field has its own cap.
 *
 * Both ends run on the same host, so integers are in host byte order. Every decoder
 * checks each length against both its cap and the bytes remaining before copying, and
 * rejects trailing bytes, so a malformed frame can never cause an out-of-bounds read.
 */
#ifndef OF_PROTO_H
#define OF_PROTO_H

#include "common/buf.h"

#include <stddef.h>
#include <stdint.h>

#define OF_PROTO_MAGIC   0x3150464Fu /* "OFP1" read as little-endian */
#define OF_PROTO_VERSION 2
#define OF_MSG_REQUEST   1
#define OF_MSG_RESPONSE  2

#define OF_PROTO_HDR_SIZE 24

/* Field caps. */
#define OF_MAX_PATH     1024
#define OF_MAX_QUERY    2048
#define OF_MAX_BODY     16384
#define OF_MAX_TOKEN    128
#define OF_MAX_IP       64
#define OF_MAX_IDEM     64
#define OF_MAX_COOKIE   256
#define OF_MAX_RESP_BODY (4u * 1024 * 1024)

#define OF_MAX_REQ_PAYLOAD  (1 + 7 * 4 + OF_MAX_PATH + OF_MAX_QUERY + OF_MAX_BODY + \
                             2 * OF_MAX_TOKEN + OF_MAX_IP + OF_MAX_IDEM)
#define OF_MAX_RESP_PAYLOAD (2 + 2 * 4 + OF_MAX_COOKIE + OF_MAX_RESP_BODY)

typedef enum { OF_GET, OF_POST, OF_PUT, OF_DELETE, OF_METHOD_OTHER } of_method_t;

/* A request as the store sees it. Strings are NUL-terminated. */
typedef struct {
    of_method_t method;
    const char *path;          /* "/api/products/3", no query string */
    const char *query;         /* raw query string, "" if none */
    const char *body;
    size_t      body_len;
    const char *session_token; /* value of the of_session cookie, "" if none */
    const char *csrf_token;    /* X-CSRF-Token header, "" if none */
    const char *ip;            /* client address as seen by Apache */
    const char *idem_key;      /* Idempotency-Key header, "" if none */
} of_req_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint64_t request_id;
    uint32_t length;
    uint32_t reserved;
} of_frame_hdr_t;

_Static_assert(sizeof(of_frame_hdr_t) == OF_PROTO_HDR_SIZE, "frame header must be 24 bytes");

/* HTTP reason phrase for a status code. */
const char *of_status_text(int status);

/* Append a complete frame (header + payload) to out. 0, or -1 if a field is over its cap. */
int proto_encode_request(buf_t *out, uint64_t request_id, const of_req_t *req);
int proto_encode_response(buf_t *out, uint64_t request_id, int status, const char *set_cookie,
                          const char *body, size_t body_len);

/* Validate a header. 0 when it is a well-formed frame of the expected type. */
int proto_check_header(const of_frame_hdr_t *h, uint16_t expected_type);

/*
 * Decode a request payload. Strings are copied, NUL-terminated, into arena, which must
 * hold at least len + 8 bytes; req's pointers point into it. 0 or -1.
 */
int proto_decode_request(const void *payload, size_t len, of_req_t *req, char *arena,
                         size_t arena_len);

/* Decode a response payload the same way. body is NUL-terminated too. 0 or -1. */
int proto_decode_response(const void *payload, size_t len, int *status, const char **set_cookie,
                          const char **body, size_t *body_len, char *arena, size_t arena_len);

#endif
