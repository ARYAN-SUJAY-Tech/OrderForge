# OrderForge design (draft, updated each phase)

This document grows from the project plan into a description of what was actually
built. Sections marked *planned* describe later phases.

## 1. Goal

OrderForge is an online retail engine in C on Linux. Its defining guarantee is that
stock is never oversold under concurrent checkout. The project is built around
operating-system concepts: processes, IPC, shared memory, synchronisation, signals and
crash recovery.

## 2. Architecture

### Phase 1, as built

```
Browser ──HTTP──▶ Apache httpd ──FastCGI (TCP 127.0.0.1:9000)──▶ of-api parent
                  static web/                                     │ fork() × N
                  security headers                                ▼
                                                     of-api worker 1..N ──▶ MySQL
                                                     (libofstore: auth, catalogue,
                                                      cart, checkout, admin)
```

- **Apache** serves the static storefront and admin console. It forwards `/api/*` to
  of-api over FastCGI with `mod_proxy_fcgi`, and adds the CSP and other security
  headers.
- **The of-api parent** opens the listening socket once, forks N workers, and then
  only supervises them (section 3).
- **Each worker** loops on `FCGX_Accept_r` → `store_handle()` → response, using its
  own MySQL connection.
- **libofstore** (`src/store/`) holds all the business logic, behind one call:
  `store_handle(store, of_req_t*, of_resp_t*)`. It knows nothing about FastCGI. In
  phase 2 the same library moves behind of-core, and of-api becomes a thin forwarder.

### Target (phases 2–4, *planned*)

Apache → of-api → Unix socket with binary frames → of-core, which has an epoll reactor,
a worker thread pool, a lock-free stock table in POSIX shared memory, and a WAL.
Beside it run of-pay (POSIX message queues) and of-report. of-supervisor restarts any
of them.

## 3. Process model of of-api (phase 1)

| Concern | Mechanism |
|---|---|
| Parallelism | Pre-fork: N worker processes all `accept()` on one inherited socket, and the kernel gives each connection to exactly one of them |
| Isolation | Workers share nothing but the socket and the O_APPEND log; a crash loses one request |
| Signals in the parent | `SIGCHLD`, `SIGTERM`, `SIGINT` and `SIGHUP` are blocked before `fork()` and taken synchronously with `sigtimedwait()`, so there is no handler code and no async-signal-safety risk |
| Crash recovery | `waitpid(WNOHANG)` reaps each dead worker and records exit status or signal; a replacement starts after a backoff (100 ms doubling to 5 s, reset once a worker has run 10 s) |
| DB failure | A worker whose MySQL connection breaks finishes the request and exits with status 3; the parent restarts it with a fresh connection |
| Shutdown | SIGTERM is forwarded to the workers; stragglers get SIGKILL after 5 s |
| Log rotation | SIGHUP makes every process `dup3()` a freshly opened log file over the old descriptor |

**Lesson learned (FastCGI connection reuse).** With `ProxyPass ... enablereuse=on`,
Apache keeps FastCGI connections open between requests. libfcgi then waits for the
next request *on the same connection* instead of returning to `accept()`. Once Apache
held one idle connection per worker, every worker was blocked in `read()`, and new
connections were never accepted. Under concurrent load the API hung; a sequential test
did not show it.

The fix is one connection per request. Diagnosis: `/proc/<pid>/wchan` showed every
worker in `wait_woken`, which is a socket read, rather than in accept.

## 4. Data model

The schema is `sql/schema.sql`, on MySQL 8 with InnoDB.

| Table | Key points |
|---|---|
| users | Argon2id `pw_hash`, `role` customer or admin, `failed_logins` and `locked_until` for lockout |
| sessions | Primary key is `SHA-256(token)`; the raw token exists only in the browser cookie. Each session has its own CSRF token and expiry |
| categories, products | Unique `sku`; price in integer paise with `CHECK >= 0`; soft delete with `active` |
| inventory | `stock_on_hand` with `CHECK (stock_on_hand >= 0)`, defence layer 3 |
| carts, cart_items | Server-side; quantity limited to 1..100; holds no prices |
| orders, order_items | Status `CHECK`ed against the state list; unit price copied at purchase |
| order_events | Append-only timeline that drives the tracking page |
| stock_movements | Ledger of every stock change (initial, restock, adjust, sale, ...) |
| audit_log | Logins, failures, registrations and every admin change |

**Invariant:** for each product, `SUM(stock_movements.delta) = inventory.stock_on_hand`
and stock is never negative. `of-ctl invariants` checks this.

## 5. Checkout and the naive baseline (phase 1)

The phase-1 checkout is deliberately unsafe. It is the "before" in the benchmark.

```
BEGIN
  read cart lines (ascending product id)       ← snapshot taken here (REPEATABLE READ)
  INSERT orders
  for each line:
     stock = SELECT stock_on_hand              ← reads the snapshot, possibly stale
     if stock < qty: ROLLBACK, 409
     UPDATE inventory SET stock_on_hand = stock - qty   ← absolute write: lost update
     INSERT order_items, stock_movements(-qty)
  INSERT order_events, DELETE cart_items
COMMIT
```

- **Why it oversells:** two transactions read the same stock and both pass the check.
  Each writes `stock - qty`, so the second write erases the first.
- **Why the CHECK doesn't catch it:** each written value is non-negative.
- **How it is detected:** the ledger still records every sale, so `SUM(delta)` falls
  below `stock_on_hand`.

**Measured on the development machine** (4 workers, `naive_delay_us = 0`): 40
simultaneous buyers for 10 units produced 38 successful orders, so 28 units were
oversold. `of-ctl invariants` reported `HOT-001 on_hand 0, ledger -28`.

Phase 2 adds the `db-lock` mode (`SELECT ... FOR UPDATE` with a guarded relative
update) and the `lockfree-shm` mode (a CAS loop in shared memory). Both must produce
exactly 10 orders.

## 6. Security (phase 1)

- **Passwords:** Argon2id through libsodium `crypto_pwhash_str`, with the
  interactive limits. Plain passwords are never stored or logged.
  `of-ctl useradd` reads the password from stdin, so it never appears in `ps` output.
- **Session tokens:** 256 bits from `getrandom(2)`. The cookie is HttpOnly and
  SameSite=Strict, and the database stores only SHA-256 of the token.
- **CSRF:** a per-session token is required in `X-CSRF-Token` on every
  state-changing request that needs a session. It is compared in constant time with
  `sodium_memcmp`.
- **Lockout:** 5 failures lock the account for 30 s, doubling up to one hour.
  A login for an unknown email still runs an Argon2 verify against a dummy hash, so
  response time does not reveal which accounts exist.
- **Authorisation:** checked in `store_handle()` before any handler runs, from a
  route table that gives each endpoint public, user or admin access. A customer asking
  for someone else's order gets 404.
- **Input:**
  - Every SQL statement is a prepared statement.
  - The JSON parser is strict: flat objects only, integers only, duplicate keys and
    bodies over 16 KiB rejected.
  - Every field is length- and range-checked.
  - `LIKE` wildcards in search terms are escaped.
  - Prices always come from the database.
- **Browser:**
  - The CSP allows only same-origin scripts and styles, with no inline code.
  - All page text is inserted with `textContent`.
  - Every JSON response escapes `<` as `<`.
- **Build hardening:**
  - Compiled with `-fstack-protector-strong`, PIE, full RELRO and `_FORTIFY_SOURCE=2`
    in release builds.
  - Tested under ASan and UBSan.

## 7. Testing (phase 1)

| Layer | What runs | Result |
|---|---|---|
| Unit | `make test`: config, logger (threads, rotation), CRC32, JSON parser, buffer, query and cookie parsing | 123 checks pass in debug, asan, ubsan and tsan |
| Integration | `tests/integration/api_test.sh` through Apache: catalogue, auth, cart, checkout, orders, admin, CSRF, roles, ownership, lockout | 56/56, also with an ASan-built of-api |
| Concurrency | `tests/integration/naive_oversell.sh` + `of-ctl invariants` | Shows the naive mode overselling (gate 1 baseline) |
| Supervision | `kill -9` of a worker under load | Worker restarted after 100 ms; requests kept succeeding |
