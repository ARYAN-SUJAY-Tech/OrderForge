/*
 * store.h - OrderForge business logic, independent of transport.
 *
 * A request arrives as an of_req_t (method, path, query, body, session cookie, CSRF
 * header, client ip) and leaves as an of_resp_t (status, JSON body, optional
 * Set-Cookie). In phase 1 of-api calls store_handle() directly inside each FastCGI
 * worker; in phase 2 the same call runs inside of-core worker threads and of-api only
 * frames the request. Authentication, role checks and CSRF checks all live here, so
 * no transport can skip them.
 */
#ifndef OF_STORE_H
#define OF_STORE_H

#include "common/buf.h"
#include "common/config.h"
#include "store/db.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { OF_GET, OF_POST, OF_PUT, OF_DELETE, OF_METHOD_OTHER } of_method_t;

typedef struct {
    of_method_t method;
    const char *path;          /* "/api/products/3", no query string */
    const char *query;         /* raw query string, "" if none */
    const char *body;
    size_t      body_len;
    const char *session_token; /* value of the of_session cookie, "" if none */
    const char *csrf_token;    /* X-CSRF-Token header, "" if none */
    const char *ip;            /* client address as seen by Apache */
} of_req_t;

#define OF_COOKIE_MAX 256

typedef struct {
    int   status;
    buf_t body;                      /* JSON */
    char  set_cookie[OF_COOKIE_MAX]; /* "" when no cookie is set */
} of_resp_t;

typedef enum {
    STOCK_NAIVE = 0,  /* read-modify-write with no lock: oversells under load (baseline) */
} stock_mode_t;

typedef struct {
    db_t        *db;
    stock_mode_t stock_mode;
    int64_t      naive_delay_us; /* widen the naive race window for demos; 0 in normal use */
    int64_t      session_ttl_s;
    bool         cookie_secure;  /* add "Secure" to the cookie once Apache terminates TLS */
    char         dummy_hash[128];/* verified against for unknown emails: equal login timing */
} store_t;

/* Connect to MySQL and read store.* / auth.* settings. 0 or -1 with err. */
int  store_init(store_t *s, const of_config_t *cfg, char *err, size_t errlen);
void store_close(store_t *s);

void of_resp_init(of_resp_t *r);
void of_resp_free(of_resp_t *r);

/* Handle one request. Always fills resp (errors become JSON error bodies). */
void store_handle(store_t *s, const of_req_t *req, of_resp_t *resp);

/* Argon2id hashing (libsodium), shared with of-ctl. 0 or -1. */
int store_hash_password(const char *password, char *out, size_t outlen);

const char *of_status_text(int status);

#endif
