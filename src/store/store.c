/*
 * store.c - initialisation, routing, and the helpers every handler shares.
 *
 * The router is a static table: method + path pattern + access level + handler.
 * Access control is enforced here, before any handler runs:
 *   PUBLIC  anyone
 *   USER    valid session; state-changing methods also need a matching CSRF token
 *   ADMIN   as USER, and the account role must be admin
 */
#include "store/internal.h"

#include "common/log.h"
#include "common/util.h"

#include <sodium.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { PUBLIC, USER, ADMIN } access_t;

typedef struct {
    of_method_t method;
    const char *pattern; /* ":id" matches one positive integer segment */
    access_t    access;
    handler_fn  fn;
} route_t;

static const route_t routes[] = {
    {OF_POST,   "/api/auth/register",               PUBLIC, h_register},
    {OF_POST,   "/api/auth/login",                  PUBLIC, h_login},
    {OF_POST,   "/api/auth/logout",                 USER,   h_logout},
    {OF_GET,    "/api/me",                          USER,   h_me},

    {OF_GET,    "/api/categories",                  PUBLIC, h_categories},
    {OF_GET,    "/api/products",                    PUBLIC, h_products},
    {OF_GET,    "/api/products/:id",                PUBLIC, h_product},

    {OF_GET,    "/api/cart",                        USER,   h_cart_get},
    {OF_POST,   "/api/cart/items",                  USER,   h_cart_set_item},
    {OF_DELETE, "/api/cart/items/:id",              USER,   h_cart_delete_item},

    {OF_POST,   "/api/checkout",                    USER,   h_checkout},
    {OF_GET,    "/api/orders",                      USER,   h_orders_mine},
    {OF_GET,    "/api/orders/:id",                  USER,   h_order_get},

    {OF_GET,    "/api/admin/products",              ADMIN,  h_admin_products},
    {OF_POST,   "/api/admin/products",              ADMIN,  h_admin_product_create},
    {OF_PUT,    "/api/admin/products/:id",          ADMIN,  h_admin_product_update},
    {OF_DELETE, "/api/admin/products/:id",          ADMIN,  h_admin_product_delete},
    {OF_POST,   "/api/admin/products/:id/stock",    ADMIN,  h_admin_stock},
    {OF_GET,    "/api/admin/orders",                ADMIN,  h_admin_orders},
    {OF_GET,    "/api/admin/system",                ADMIN,  h_admin_system},
};

/* ------------------------------------------------------------------ init */

int store_hash_password(const char *password, char *out, size_t outlen)
{
    if (outlen < crypto_pwhash_STRBYTES)
        return -1;
    return crypto_pwhash_str(out, password, strlen(password),
                             crypto_pwhash_OPSLIMIT_INTERACTIVE,
                             crypto_pwhash_MEMLIMIT_INTERACTIVE);
}

static const char *const mode_names[] = {"naive", "db-lock", "lockfree-shm"};

const char *stock_mode_name(stock_mode_t m)
{
    return (unsigned)m < 3 ? mode_names[m] : "unknown";
}

int stock_mode_parse(const char *s)
{
    for (int i = 0; i < 3; i++)
        if (s && strcmp(s, mode_names[i]) == 0)
            return i;
    return -1;
}

int store_init(store_t *s, const of_config_t *cfg, stock_table_t *stock, int mode_override,
               char *err, size_t errlen)
{
    memset(s, 0, sizeof *s);
    s->cfg = cfg;
    s->stock = stock;
    if (sodium_init() < 0) {
        snprintf(err, errlen, "libsodium init failed");
        return -1;
    }

    int mode = mode_override >= 0
                   ? mode_override
                   : stock_mode_parse(of_config_get(cfg, "store.stock_mode", "lockfree-shm"));
    if (mode < 0) {
        snprintf(err, errlen, "store.stock_mode must be naive, db-lock or lockfree-shm");
        return -1;
    }
    if (mode == STOCK_LOCKFREE && !stock) {
        snprintf(err, errlen, "lockfree-shm needs the shared-memory stock table");
        return -1;
    }
    s->stock_mode = (stock_mode_t)mode;

    int ok1, ok2;
    s->naive_delay_us = of_config_get_long(cfg, "store.naive_delay_us", 0, 0, 1000000, &ok1);
    s->session_ttl_s = of_config_get_long(cfg, "auth.session_ttl", 86400, 60, 2592000, &ok2);
    if (!ok1 || !ok2) {
        snprintf(err, errlen, "store.naive_delay_us or auth.session_ttl out of range");
        return -1;
    }
    s->cookie_secure = strcmp(of_config_get(cfg, "auth.cookie_secure", "0"), "1") == 0;

    /* A hash to verify against when the email is unknown, so a login attempt takes the
     * same time whether or not the account exists. */
    unsigned char junk[16];
    char junk_hex[33];
    if (of_random_bytes(junk, sizeof junk) != 0) {
        snprintf(err, errlen, "getrandom failed");
        return -1;
    }
    of_hex(junk_hex, junk, sizeof junk);
    if (store_hash_password(junk_hex, s->dummy_hash, sizeof s->dummy_hash) != 0) {
        snprintf(err, errlen, "argon2id hashing failed (out of memory?)");
        return -1;
    }

    s->db = db_connect(cfg, err, errlen);
    return s->db ? 0 : -1;
}

void store_close(store_t *s)
{
    db_close(s->db);
    s->db = NULL;
}

int store_reconnect(store_t *s, char *err, size_t errlen)
{
    db_t *fresh = db_connect(s->cfg, err, errlen);
    if (!fresh)
        return -1;
    db_close(s->db);
    s->db = fresh;
    return 0;
}

int store_load_stock(db_t *db, stock_table_t *t, char *err, size_t errlen)
{
    db_res_t r;
    if (db_query(db, &r,
                 "SELECT p.id, i.stock_on_hand, p.price_paise FROM products p "
                 "JOIN inventory i ON i.product_id = p.id ORDER BY p.id",
                 "") != 0) {
        snprintf(err, errlen, "loading stock: %s", db_error(db));
        return -1;
    }
    int n = 0;
    while (db_fetch(&r) == 1) {
        if (!stock_upsert(t, (uint64_t)db_int(&r, 0), (int32_t)db_int(&r, 1), db_int(&r, 2))) {
            snprintf(err, errlen, "stock table full after %d products (raise core.stock_capacity)",
                     n);
            db_res_free(&r);
            return -1;
        }
        n++;
    }
    db_res_free(&r);
    return n;
}

int store_check_invariants(db_t *db, const stock_table_t *t, buf_t *out, int64_t *oversold)
{
    db_res_t r;
    if (db_query(db, &r,
                 "SELECT p.id, p.sku, i.stock_on_hand, "
                 "  COALESCE((SELECT SUM(delta) FROM stock_movements m "
                 "            WHERE m.product_id = p.id), 0), "
                 "  COALESCE((SELECT SUM(oi.qty) FROM order_items oi "
                 "            JOIN orders o ON o.id = oi.order_id "
                 "            WHERE oi.product_id = p.id "
                 "              AND o.status NOT IN ('cancelled', 'refunded')), 0) "
                 "FROM products p JOIN inventory i ON i.product_id = p.id ORDER BY p.id",
                 "") != 0)
        return -1;

    int products = 0, bad = 0;
    int64_t over = 0;
    buf_printf(out, "%-10s %8s %8s %8s %8s %8s  %s\n", "sku", "on_hand", "ledger", "sold",
               "shm_av", "shm_res", "result");
    while (db_fetch(&r) == 1) {
        products++;
        uint64_t id = (uint64_t)db_int(&r, 0);
        int64_t on_hand = db_int(&r, 2), ledger = db_int(&r, 3), sold = db_int(&r, 4);
        char problems[128] = "";

        if (on_hand < 0)
            snprintf(problems + strlen(problems), sizeof problems - strlen(problems),
                     " negative-stock");
        if (ledger != on_hand)
            snprintf(problems + strlen(problems), sizeof problems - strlen(problems),
                     " ledger-mismatch%s", ledger < 0 ? "(oversold)" : "");
        if (ledger < 0)
            over += -ledger;

        long long shm_av = -1, shm_res = -1;
        if (t) {
            stock_slot_t *s = stock_find(t, id);
            if (!s) {
                snprintf(problems + strlen(problems), sizeof problems - strlen(problems),
                         " missing-from-shm");
            } else {
                shm_av = atomic_load(&s->available);
                shm_res = atomic_load(&s->reserved);
                if (shm_av != on_hand || shm_res != 0)
                    snprintf(problems + strlen(problems), sizeof problems - strlen(problems),
                             " shm-drift");
            }
        }
        if (problems[0]) {
            bad++;
            buf_printf(out, "%-10s %8lld %8lld %8lld %8lld %8lld %s\n", db_str(&r, 1),
                       (long long)on_hand, (long long)ledger, (long long)sold, shm_av, shm_res,
                       problems);
        }
    }
    db_res_free(&r);

    if (bad == 0)
        buf_printf(out, "all %d products balance: stock_on_hand == SUM(stock_movements), "
                        "none negative%s\n",
                   products, t ? ", shared memory matches MySQL" : "");
    else
        buf_printf(out, "%d of %d products FAIL; %lld units oversold\n", bad, products,
                   (long long)over);
    if (oversold)
        *oversold = over;
    return bad;
}

void of_resp_init(of_resp_t *r)
{
    r->status = 200;
    buf_init(&r->body);
    r->set_cookie[0] = '\0';
}

void of_resp_free(of_resp_t *r)
{
    buf_free(&r->body);
}

/* ------------------------------------------------------------------ helpers */

void resp_error(ctx_t *c, int status, const char *code, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    buf_t *b = &c->resp->body;
    buf_reset(b);
    c->resp->status = status;
    buf_puts(b, "{\"error\":{");
    buf_json_kv_str(b, "code", code);
    buf_puts(b, ",");
    buf_json_kv_str(b, "message", msg);
    buf_puts(b, "}}");
}

void resp_db_error(ctx_t *c)
{
    if (db_broken(c->db))
        resp_error(c, 503, "db_unavailable", "database unavailable, try again shortly");
    else
        resp_error(c, 500, "internal", "internal error");
}

void resp_ok_empty(ctx_t *c)
{
    c->resp->status = 200;
    buf_reset(&c->resp->body);
    buf_puts(&c->resp->body, "{\"ok\":true}");
}

int ctx_json(ctx_t *c)
{
    static _Thread_local json_obj_t obj; /* ~16 KiB: keep it off the stack */
    char err[64];
    if (c->req->body_len > JSON_MAX_TEXT) {
        resp_error(c, 413, "too_large", "request body too large");
        return -1;
    }
    const char *body = c->req->body ? c->req->body : "";
    if (json_parse_object(&obj, body, c->req->body_len, err, sizeof err) != 0) {
        resp_error(c, 400, "bad_json", "invalid JSON body: %s", err);
        return -1;
    }
    c->json = &obj;
    return 0;
}

void audit(ctx_t *c, const char *action, const char *target, const char *detail)
{
    int64_t actor = c->authed ? c->user.id : DB_NULL_INT;
    if (db_exec(c->db,
                "INSERT INTO audit_log (actor_id, action, target, detail, ip) "
                "VALUES (?, ?, ?, ?, ?)",
                "issss", actor, action, target ? target : "", detail ? detail : "",
                c->req->ip ? c->req->ip : "") < 0)
        LOG_WARN("audit_failed", "could not record %s", action);
}

int order_event(db_t *db, int64_t order_id, const char *from, const char *to,
                const char *actor, const char *note)
{
    return db_exec(db,
                   "INSERT INTO order_events (order_id, from_state, to_state, actor, note) "
                   "VALUES (?, ?, ?, ?, ?)",
                   "issss", order_id, from, to, actor, note ? note : "") == 1
               ? 0
               : -1;
}

void render_product(buf_t *b, const db_res_t *r)
{
    buf_puts(b, "{");
    buf_json_kv_int(b, "id", db_int(r, 0));
    buf_puts(b, ",");
    buf_json_kv_str(b, "sku", db_str(r, 1));
    buf_puts(b, ",");
    buf_json_kv_str(b, "name", db_str(r, 2));
    buf_puts(b, ",");
    buf_json_kv_str(b, "description", db_str(r, 3));
    buf_puts(b, ",");
    buf_json_kv_int(b, "price_paise", db_int(r, 4));
    buf_puts(b, ",");
    buf_json_kv_int(b, "category_id", db_int(r, 5));
    buf_puts(b, ",");
    buf_json_kv_str(b, "category", db_str(r, 6));
    buf_puts(b, ",");
    buf_json_kv_bool(b, "active", db_int(r, 7) != 0);
    buf_puts(b, ",");
    buf_json_kv_int(b, "stock", db_int(r, 8));
    buf_puts(b, ",");
    buf_json_kv_int(b, "reorder_level", db_int(r, 9));
    buf_puts(b, "}");
}

/* ------------------------------------------------------------------ routing */

/* Match path against pattern; on success store a captured ":id" in *id. */
static bool match(const char *pattern, const char *path, int64_t *id)
{
    const char *p = pattern, *q = path;
    while (*p && *q) {
        if (*p == ':') {
            while (*p && *p != '/')
                p++;
            const char *seg = q;
            while (*q && *q != '/')
                q++;
            char num[24];
            size_t n = (size_t)(q - seg);
            if (n == 0 || n >= sizeof num)
                return false;
            memcpy(num, seg, n);
            num[n] = '\0';
            for (size_t i = 0; i < n; i++)
                if (num[i] < '0' || num[i] > '9')
                    return false;
            if (of_parse_i64(num, 1, INT64_MAX, id) != 0)
                return false;
            continue;
        }
        if (*p != *q)
            return false;
        p++;
        q++;
    }
    /* Tolerate one trailing slash on the request path. */
    if (*q == '/' && q[1] == '\0')
        q++;
    return *p == '\0' && *q == '\0';
}

void store_handle(store_t *s, const of_req_t *req, of_resp_t *resp)
{
    ctx_t c;
    memset(&c, 0, sizeof c);
    c.s = s;
    c.db = s->db;
    c.req = req;
    c.resp = resp;
    resp->status = 200;
    buf_reset(&resp->body);

    const route_t *found = NULL;
    bool path_matched = false;
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) {
        int64_t id = 0;
        if (!match(routes[i].pattern, req->path, &id))
            continue;
        path_matched = true;
        if (routes[i].method == req->method) {
            found = &routes[i];
            c.id = id;
            break;
        }
    }
    if (!found) {
        if (path_matched)
            resp_error(&c, 405, "method_not_allowed", "method not allowed");
        else
            resp_error(&c, 404, "not_found", "no such endpoint");
        return;
    }

    if (found->access != PUBLIC) {
        int rc = auth_load_session(&c);
        if (rc < 0) {
            resp_db_error(&c);
            return;
        }
        if (rc == 0) {
            resp_error(&c, 401, "unauthenticated", "please log in");
            return;
        }
        if (req->method != OF_GET) {
            const char *t = req->csrf_token ? req->csrf_token : "";
            if (strlen(t) != 64 || sodium_memcmp(t, c.user.csrf, 64) != 0) {
                resp_error(&c, 403, "csrf", "missing or invalid CSRF token");
                return;
            }
        }
        if (found->access == ADMIN && !c.user.is_admin) {
            resp_error(&c, 403, "forbidden", "admin access required");
            return;
        }
    }

    found->fn(&c);

    if (buf_failed(&resp->body))
        resp_error(&c, 500, "internal", "response too large");
}
