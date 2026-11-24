/*
 * auth.c - registration, login, logout and session lookup.
 *
 *  - Passwords: Argon2id via libsodium's crypto_pwhash_str (salt and parameters are
 *    encoded in the stored string). Plain passwords are never stored or logged.
 *  - Sessions: 256 random bits from getrandom(2), sent to the browser as a 64-char hex
 *    cookie (HttpOnly, SameSite=Strict). Only SHA-256(token) is stored.
 *  - CSRF: a second random token per session, returned in the JSON body, which the
 *    browser echoes in X-CSRF-Token on every state-changing request.
 *  - Lockout: after 5 consecutive failures the account locks for 30 s, doubling on
 *    each further failure up to an hour. Every attempt is audited.
 */
#include "store/internal.h"

#include "common/log.h"
#include "common/util.h"

#include <ctype.h>
#include <sodium.h>
#include <stdio.h>
#include <string.h>

#define LOCK_THRESHOLD   5
#define LOCK_BASE_S      30
#define LOCK_MAX_S       3600

static void sha256_hex(const char *s, char out[65])
{
    unsigned char h[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(h, (const unsigned char *)s, strlen(s));
    of_hex(out, h, sizeof h);
}

static bool is_hex64(const char *s)
{
    if (!s || strlen(s) != 64)
        return false;
    for (int i = 0; i < 64; i++)
        if (!isxdigit((unsigned char)s[i]))
            return false;
    return true;
}

/* Lower-cases into out and checks the shape: local@domain.tld, no spaces or controls. */
static bool normalise_email(const char *in, char *out, size_t outlen)
{
    size_t n = in ? strlen(in) : 0;
    if (n < 3 || n > 254 || n >= outlen)
        return false;
    int at = -1;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)in[i];
        if (ch <= 0x20 || ch == 0x7F || ch == '"' || ch == '<' || ch == '>')
            return false;
        if (ch == '@') {
            if (at >= 0)
                return false;
            at = (int)i;
        }
        out[i] = (char)tolower(ch);
    }
    out[n] = '\0';
    if (at <= 0 || (size_t)at >= n - 1)
        return false;
    const char *dot = strrchr(out + at, '.');
    return dot && dot > out + at + 1 && dot[1] != '\0';
}

static bool valid_name(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n < 1 || n > 100)
        return false;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] < 0x20)
            return false;
    return true;
}

static void render_session(ctx_t *c)
{
    buf_t *b = &c->resp->body;
    buf_puts(b, "{\"user\":{");
    buf_json_kv_int(b, "id", c->user.id);
    buf_puts(b, ",");
    buf_json_kv_str(b, "email", c->user.email);
    buf_puts(b, ",");
    buf_json_kv_str(b, "name", c->user.name);
    buf_puts(b, ",");
    buf_json_kv_str(b, "role", c->user.is_admin ? "admin" : "customer");
    buf_puts(b, "},");
    buf_json_kv_str(b, "csrf_token", c->user.csrf);
    buf_puts(b, "}");
}

static void set_cookie(ctx_t *c, const char *token, int64_t max_age)
{
    snprintf(c->resp->set_cookie, sizeof c->resp->set_cookie,
             "of_session=%s; Path=/; Max-Age=%lld; HttpOnly; SameSite=Strict%s", token,
             (long long)max_age, c->s->cookie_secure ? "; Secure" : "");
}

/* Create a session for c->user (id, email, name, is_admin already set). 0 or -1. */
static int create_session(ctx_t *c)
{
    unsigned char raw[32], csrf_raw[32];
    char token[65];
    if (of_random_bytes(raw, sizeof raw) != 0 || of_random_bytes(csrf_raw, sizeof csrf_raw) != 0)
        return -1;
    of_hex(token, raw, sizeof raw);
    of_hex(c->user.csrf, csrf_raw, sizeof csrf_raw);
    sha256_hex(token, c->user.token_hash);
    sodium_memzero(raw, sizeof raw);

    if (db_exec(c->db,
                "INSERT INTO sessions (token_hash, user_id, csrf_token, ip, expires_at) "
                "VALUES (?, ?, ?, ?, NOW(3) + INTERVAL ? SECOND)",
                "sissi", c->user.token_hash, c->user.id, c->user.csrf,
                c->req->ip ? c->req->ip : "", c->s->session_ttl_s) != 1)
        return -1;

    set_cookie(c, token, c->s->session_ttl_s);
    sodium_memzero(token, sizeof token);
    c->authed = true;
    return 0;
}

int auth_load_session(ctx_t *c)
{
    const char *token = c->req->session_token;
    if (!is_hex64(token))
        return 0;

    char hash[65];
    sha256_hex(token, hash);

    db_res_t r;
    if (db_query(c->db, &r,
                 "SELECT u.id, u.email, u.name, u.role, s.csrf_token "
                 "FROM sessions s JOIN users u ON u.id = s.user_id "
                 "WHERE s.token_hash = ? AND s.expires_at > NOW(3)",
                 "s", hash) != 0)
        return -1;

    int rc = db_fetch(&r);
    if (rc == 1) {
        c->user.id = db_int(&r, 0);
        snprintf(c->user.email, sizeof c->user.email, "%s", db_str(&r, 1));
        snprintf(c->user.name, sizeof c->user.name, "%s", db_str(&r, 2));
        c->user.is_admin = strcmp(db_str(&r, 3), "admin") == 0;
        snprintf(c->user.csrf, sizeof c->user.csrf, "%s", db_str(&r, 4));
        memcpy(c->user.token_hash, hash, sizeof hash);
        c->authed = true;
    }
    db_res_free(&r);
    return rc;
}

void h_register(ctx_t *c)
{
    if (ctx_json(c))
        return;
    const char *email_in = json_get_str(c->json, "email");
    const char *password = json_get_str(c->json, "password");
    const char *name = json_get_str(c->json, "name");

    char email[255];
    if (!normalise_email(email_in, email, sizeof email)) {
        resp_error(c, 400, "invalid_email", "enter a valid email address");
        return;
    }
    size_t plen = password ? strlen(password) : 0;
    if (plen < 8 || plen > 128) {
        resp_error(c, 400, "weak_password", "password must be 8 to 128 characters");
        return;
    }
    if (!valid_name(name)) {
        resp_error(c, 400, "invalid_name", "name must be 1 to 100 characters");
        return;
    }

    char hash[crypto_pwhash_STRBYTES];
    if (store_hash_password(password, hash, sizeof hash) != 0) {
        resp_error(c, 500, "internal", "could not hash password");
        return;
    }

    if (db_exec(c->db, "INSERT INTO users (email, name, pw_hash) VALUES (?, ?, ?)", "sss",
                email, name, hash) != 1) {
        if (db_errno(c->db) == DB_ER_DUP_ENTRY)
            resp_error(c, 409, "email_taken", "an account with this email already exists");
        else
            resp_db_error(c);
        return;
    }

    c->user.id = (int64_t)db_insert_id(c->db);
    snprintf(c->user.email, sizeof c->user.email, "%s", email);
    snprintf(c->user.name, sizeof c->user.name, "%s", name);
    c->user.is_admin = false;

    if (create_session(c) != 0) {
        resp_db_error(c);
        return;
    }
    audit(c, "register", email, "");
    c->resp->status = 201;
    render_session(c);
}

void h_login(ctx_t *c)
{
    if (ctx_json(c))
        return;
    const char *email_in = json_get_str(c->json, "email");
    const char *password = json_get_str(c->json, "password");
    char email[255];
    if (!normalise_email(email_in, email, sizeof email) || !password ||
        strlen(password) > 128) {
        resp_error(c, 401, "invalid_credentials", "wrong email or password");
        return;
    }

    db_res_t r;
    if (db_query(c->db, &r,
                 "SELECT id, name, role, pw_hash, failed_logins, "
                 "       GREATEST(0, TIMESTAMPDIFF(SECOND, NOW(3), "
                 "                 COALESCE(locked_until, NOW(3)))) "
                 "FROM users WHERE email = ?",
                 "s", email) != 0) {
        resp_db_error(c);
        return;
    }

    int found = db_fetch(&r);
    char pw_hash[crypto_pwhash_STRBYTES] = "";
    int64_t failed = 0, locked_for = 0;
    if (found == 1) {
        c->user.id = db_int(&r, 0);
        snprintf(c->user.email, sizeof c->user.email, "%s", email);
        snprintf(c->user.name, sizeof c->user.name, "%s", db_str(&r, 1));
        c->user.is_admin = strcmp(db_str(&r, 2), "admin") == 0;
        snprintf(pw_hash, sizeof pw_hash, "%s", db_str(&r, 3));
        failed = db_int(&r, 4);
        locked_for = db_int(&r, 5);
    }
    db_res_free(&r);
    if (found < 0) {
        resp_db_error(c);
        return;
    }

    if (found == 0) {
        /* Same work as a real check so response time does not reveal unknown emails. */
        if (crypto_pwhash_str_verify(c->s->dummy_hash, password, strlen(password)) == 0)
            LOG_WARN("login", "dummy hash matched (impossible in practice)");
        audit(c, "login_failed", email, "unknown email");
        resp_error(c, 401, "invalid_credentials", "wrong email or password");
        return;
    }

    if (locked_for > 0) {
        audit(c, "login_locked", email, "");
        resp_error(c, 429, "account_locked",
                   "too many failed attempts, try again in %lld seconds", (long long)locked_for);
        return;
    }

    if (crypto_pwhash_str_verify(pw_hash, password, strlen(password)) != 0) {
        failed++;
        int64_t lock_s = 0;
        if (failed >= LOCK_THRESHOLD) {
            int64_t shift = failed - LOCK_THRESHOLD;
            lock_s = shift >= 7 ? LOCK_MAX_S : LOCK_BASE_S << shift;
            if (lock_s > LOCK_MAX_S)
                lock_s = LOCK_MAX_S;
        }
        if (db_exec(c->db,
                    "UPDATE users SET failed_logins = failed_logins + 1, "
                    "  locked_until = IF(? > 0, NOW(3) + INTERVAL ? SECOND, locked_until) "
                    "WHERE id = ?",
                    "iii", lock_s, lock_s, c->user.id) < 0)
            LOG_WARN("login", "could not record failed login for user %lld",
                     (long long)c->user.id);
        char detail[64];
        snprintf(detail, sizeof detail, "failures=%lld lock_s=%lld", (long long)failed,
                 (long long)lock_s);
        audit(c, "login_failed", email, detail);
        resp_error(c, 401, "invalid_credentials", "wrong email or password");
        return;
    }

    if (failed > 0 &&
        db_exec(c->db, "UPDATE users SET failed_logins = 0, locked_until = NULL WHERE id = ?",
                "i", c->user.id) < 0) {
        resp_db_error(c);
        return;
    }
    /* Opportunistic cleanup of this user's expired sessions. */
    (void)db_exec(c->db, "DELETE FROM sessions WHERE user_id = ? AND expires_at <= NOW(3)", "i",
                  c->user.id);

    if (create_session(c) != 0) {
        resp_db_error(c);
        return;
    }
    audit(c, "login", email, "");
    render_session(c);
}

void h_logout(ctx_t *c)
{
    if (db_exec(c->db, "DELETE FROM sessions WHERE token_hash = ?", "s", c->user.token_hash) < 0) {
        resp_db_error(c);
        return;
    }
    set_cookie(c, "", 0);
    audit(c, "logout", c->user.email, "");
    resp_ok_empty(c);
}

void h_me(ctx_t *c)
{
    render_session(c);
}
