#include "common/proto.h"

#include <string.h>

const char *of_status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default:  return "Unknown";
    }
}

/* ------------------------------------------------------------------ encoding */

static int put_field(buf_t *b, const char *s, size_t len, size_t cap)
{
    if (len > cap)
        return -1;
    uint32_t n = (uint32_t)len;
    buf_append(b, &n, sizeof n);
    if (len)
        buf_append(b, s, len);
    return 0;
}

static size_t begin_frame(buf_t *b, uint16_t type, uint64_t request_id)
{
    of_frame_hdr_t h = {OF_PROTO_MAGIC, OF_PROTO_VERSION, type, request_id, 0, 0};
    size_t at = b->len;
    buf_append(b, &h, sizeof h);
    return at;
}

/* Patch the payload length into the header written at offset `at`. */
static int end_frame(buf_t *b, size_t at)
{
    if (buf_failed(b))
        return -1;
    uint32_t len = (uint32_t)(b->len - at - sizeof(of_frame_hdr_t));
    memcpy(b->data + at + offsetof(of_frame_hdr_t, length), &len, sizeof len);
    return 0;
}

static const char *nz(const char *s)
{
    return s ? s : "";
}

int proto_encode_request(buf_t *out, uint64_t request_id, const of_req_t *req)
{
    size_t start = out->len;
    size_t at = begin_frame(out, OF_MSG_REQUEST, request_id);
    uint8_t method = (uint8_t)req->method;
    buf_append(out, &method, 1);

    const char *path = nz(req->path), *query = nz(req->query), *sess = nz(req->session_token),
               *csrf = nz(req->csrf_token), *ip = nz(req->ip), *idem = nz(req->idem_key);
    if (put_field(out, path, strlen(path), OF_MAX_PATH) ||
        put_field(out, query, strlen(query), OF_MAX_QUERY) ||
        put_field(out, nz(req->body), req->body ? req->body_len : 0, OF_MAX_BODY) ||
        put_field(out, sess, strlen(sess), OF_MAX_TOKEN) ||
        put_field(out, csrf, strlen(csrf), OF_MAX_TOKEN) ||
        put_field(out, ip, strlen(ip), OF_MAX_IP) ||
        put_field(out, idem, strlen(idem), OF_MAX_IDEM) || end_frame(out, at)) {
        out->len = start; /* drop the partial frame */
        return -1;
    }
    return 0;
}

int proto_encode_response(buf_t *out, uint64_t request_id, int status, const char *set_cookie,
                          const char *body, size_t body_len)
{
    size_t start = out->len;
    size_t at = begin_frame(out, OF_MSG_RESPONSE, request_id);
    uint16_t st = (uint16_t)status;
    buf_append(out, &st, sizeof st);
    const char *cookie = nz(set_cookie);
    if (put_field(out, cookie, strlen(cookie), OF_MAX_COOKIE) ||
        put_field(out, nz(body), body ? body_len : 0, OF_MAX_RESP_BODY) || end_frame(out, at)) {
        out->len = start;
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ decoding */

int proto_check_header(const of_frame_hdr_t *h, uint16_t expected_type)
{
    if (h->magic != OF_PROTO_MAGIC || h->version != OF_PROTO_VERSION ||
        h->type != expected_type || h->reserved != 0)
        return -1;
    uint32_t cap = expected_type == OF_MSG_REQUEST ? OF_MAX_REQ_PAYLOAD : OF_MAX_RESP_PAYLOAD;
    return h->length <= cap ? 0 : -1;
}

typedef struct {
    const uint8_t *p;
    size_t         left;
    char          *arena;
    size_t         arena_left;
} reader_t;

static int take(reader_t *r, void *dst, size_t n)
{
    if (r->left < n)
        return -1;
    memcpy(dst, r->p, n);
    r->p += n;
    r->left -= n;
    return 0;
}

/* Copy one length-prefixed field into the arena, NUL-terminated. */
static int get_field(reader_t *r, size_t cap, const char **out, size_t *out_len)
{
    uint32_t n;
    if (take(r, &n, sizeof n) != 0 || n > cap || n > r->left || (size_t)n + 1 > r->arena_left)
        return -1;
    memcpy(r->arena, r->p, n);
    r->arena[n] = '\0';
    *out = r->arena;
    if (out_len)
        *out_len = n;
    r->p += n;
    r->left -= n;
    r->arena += n + 1;
    r->arena_left -= n + 1;
    return 0;
}

int proto_decode_request(const void *payload, size_t len, of_req_t *req, char *arena,
                         size_t arena_len)
{
    reader_t r = {payload, len, arena, arena_len};
    uint8_t method;
    memset(req, 0, sizeof *req);
    if (take(&r, &method, 1) != 0 || method > OF_METHOD_OTHER)
        return -1;
    req->method = (of_method_t)method;
    if (get_field(&r, OF_MAX_PATH, &req->path, NULL) ||
        get_field(&r, OF_MAX_QUERY, &req->query, NULL) ||
        get_field(&r, OF_MAX_BODY, &req->body, &req->body_len) ||
        get_field(&r, OF_MAX_TOKEN, &req->session_token, NULL) ||
        get_field(&r, OF_MAX_TOKEN, &req->csrf_token, NULL) ||
        get_field(&r, OF_MAX_IP, &req->ip, NULL) ||
        get_field(&r, OF_MAX_IDEM, &req->idem_key, NULL))
        return -1;
    return r.left == 0 ? 0 : -1;
}

int proto_decode_response(const void *payload, size_t len, int *status, const char **set_cookie,
                          const char **body, size_t *body_len, char *arena, size_t arena_len)
{
    reader_t r = {payload, len, arena, arena_len};
    uint16_t st;
    if (take(&r, &st, sizeof st) != 0 || st < 100 || st > 599)
        return -1;
    *status = st;
    if (get_field(&r, OF_MAX_COOKIE, set_cookie, NULL) ||
        get_field(&r, OF_MAX_RESP_BODY, body, body_len))
        return -1;
    return r.left == 0 ? 0 : -1;
}
