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
#include "common/proto.h" /* of_req_t, of_method_t */
#include "common/stock.h"
#include "common/wal.h"
#include "store/db.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OF_COOKIE_MAX 256

typedef struct {
    int   status;
    buf_t body;                      /* JSON */
    char  set_cookie[OF_COOKIE_MAX]; /* "" when no cookie is set */
} of_resp_t;

/*
 * How checkout protects stock (store.stock_mode):
 *   naive         read stock, check, write stock - qty back. No lock: lost updates
 *                 oversell under concurrency. The benchmark baseline.
 *   db-lock       SELECT ... FOR UPDATE on each inventory row (ascending product id, so
 *                 no deadlock), then a guarded relative UPDATE. Correct; serialises
 *                 buyers of the same product inside MySQL.
 *   lockfree-shm  reserve every line with a CAS loop on the shared-memory stock table
 *                 first (sold-out buyers are rejected without touching MySQL), then
 *                 commit with the guarded relative UPDATE. Correct and the default.
 */
typedef enum {
    STOCK_NAIVE = 0,
    STOCK_DBLOCK = 1,
    STOCK_LOCKFREE = 2,
} stock_mode_t;

const char *stock_mode_name(stock_mode_t m);
int         stock_mode_parse(const char *s); /* -1 if unknown */

/*
 * Payment hand-off. After a checkout commits, the store builds an (unsigned) payment
 * request and passes it to this callback, which of-core points at its outbox; a sender
 * thread signs it and puts it on the POSIX message queue to of-pay. Returns 0 when
 * accepted, -1 when not (the order then simply waits and the expiry reaper cancels it).
 */
typedef int (*pay_submit_fn)(void *ctx, const void *pay_request);

typedef struct {
    db_t          *db;
    pay_submit_fn  pay_submit;     /* NULL: payments are never sent (orders expire) */
    void          *pay_ctx;
    int64_t        reservation_ttl_s; /* unpaid orders are cancelled after this */
    stock_table_t *stock;        /* shared-memory table (of-core); NULL disables lockfree */
    wal_t         *wal;          /* write-ahead log for shm reservations (lockfree mode) */
    _Atomic uint64_t *next_txid; /* shared WAL transaction counter (of-core) */
    stock_mode_t   stock_mode;
    int64_t        naive_delay_us; /* widen the naive race window for demos; 0 in normal use */
    int64_t        session_ttl_s;
    bool           cookie_secure;  /* add "Secure" to the cookie once Apache terminates TLS */
    char           dummy_hash[128];/* verified against for unknown emails: equal login timing */
    const of_config_t *cfg;        /* kept for reconnecting */
} store_t;

/* Connect to MySQL and read store.* / auth.* settings. stock may be NULL unless the mode
 * is lockfree-shm. mode_override >= 0 replaces store.stock_mode. 0 or -1 with err. */
int  store_init(store_t *s, const of_config_t *cfg, stock_table_t *stock, int mode_override,
                char *err, size_t errlen);
void store_close(store_t *s);

/* Replace a broken MySQL connection. 0 or -1. */
int  store_reconnect(store_t *s, char *err, size_t errlen);

/* Fill the shared-memory table from MySQL (stock_on_hand, price) for every product.
 * Returns the number of products loaded, or -1. */
int  store_load_stock(db_t *db, stock_table_t *t, char *err, size_t errlen);

/*
 * Stock invariants, used by of-ctl and of-bench. Writes a human-readable report to out.
 * When t is non-NULL it also checks that the shared-memory counters match MySQL
 * (available == stock_on_hand, reserved == 0 while no checkout is in flight).
 * Returns the number of failing products (0 = all good), or -1 on database error.
 * *oversold receives the total units sold beyond what existed.
 */
int  store_check_invariants(db_t *db, const stock_table_t *t, buf_t *out, int64_t *oversold);

/*
 * Order-level invariants (the plan's list beyond the stock ledger):
 *   - no unpaid order is past its reservation deadline by more than grace_s (the reaper
 *     must have cancelled it), and none is left with no active reservation at all;
 *   - every paid / packed / shipped / delivered order has exactly one successful payment;
 *   - every order has a 'placed' event and its last event matches its status;
 *   - reservation states agree with order states (active <-> unpaid, committed <-> paid
 *     or later, released <-> cancelled/refunded);
 *   - no refunded order still has a successful (un-refunded) payment.
 * Writes one line per check to out. Returns the number of failing checks, or -1.
 */
int  store_check_orders(db_t *db, buf_t *out, int64_t grace_s);

/*
 * Apply a verified reply from of-pay (runs on of-core's background thread).
 *   success + order still waiting   -> payment succeeded, order paid, reservation committed
 *   decline + order still waiting   -> payment declined, order cancelled, stock returned
 *   success after the order expired -> refunded automatically, audited (late success)
 *   anything else (duplicate reply) -> ignored: only one reply can leave 'pending'
 * Returns 1 when it changed something, 0 when ignored, -1 on database error.
 */
int  store_payment_reply(store_t *s, const void *pay_reply);

/* The expiry reaper: cancel up to `limit` orders whose reservation deadline passed while
 * unpaid, returning their stock. Returns how many were cancelled, or -1. */
int  store_expire_reservations(store_t *s, int limit);

void of_resp_init(of_resp_t *r);
void of_resp_free(of_resp_t *r);

/* Handle one request. Always fills resp (errors become JSON error bodies). */
void store_handle(store_t *s, const of_req_t *req, of_resp_t *resp);

/* Argon2id hashing (libsodium), shared with of-ctl. 0 or -1. */
int store_hash_password(const char *password, char *out, size_t outlen);

#endif
