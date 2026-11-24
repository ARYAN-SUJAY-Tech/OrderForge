# OrderForge design (draft, updated each phase)

This document grows from the project plan into a description of what was actually
built. Sections marked *planned* describe later phases.

## 1. Goal

OrderForge is an online retail engine in C on Linux. Its defining guarantee is that
stock is never oversold under concurrent checkout. The project is built around
operating-system concepts: processes, threads, IPC, shared memory, synchronisation,
signals and crash recovery.

## 2. Architecture (as built, phase 3)

```
Browser ─HTTP─▶ Apache httpd ─FastCGI (TCP 127.0.0.1:9000)─▶ of-api   (parent + N pre-forked workers)
                static web/, CSP                                │ one persistent connection per worker
                                                                ▼ Unix socket, binary frames, SO_PEERCRED
                    ┌────────────────────────── of-core ──────────────────────────┐
                    │ reactor thread: epoll + signalfd + eventfd                   │
                    │      │ bounded job queue (503 "busy" when full)              │
                    │      ▼                                                       │
                    │ worker threads × N ── own MySQL connection each              │
                    │   libofstore: auth, catalogue, cart, checkout, orders,       │      POSIX mqueue
                    │   admin, reports      │ payment outbox (bounded)             │   /orderforge-pay-req
                    │                       ▼                                      │ ───────────────▶ of-pay
                    │ pay-sender thread: HMAC-sign, mq_timedsend                   │   (mock gateway:
                    │ background thread: epoll over reply queue + timerfd reaper ◀─┼── epoll + timerfd
                    │                    (own MySQL connection)                    │   + reply heap)
                    └──────┬──────────────────────────────────┬────────────────────┘ ◀───────────────
                           ▼                                  ▼                        /orderforge-pay-rep
             POSIX shm "/orderforge"                    MySQL (InnoDB) ◀──── of-report (timerfd:
             stock table + metrics                      durable truth        aggregates + CSV via rename)
                   ▲ read-only
             of-ctl, of-bench, of-report
```

| Process | Role | Talks to |
|---|---|---|
| of-api | Turns each FastCGI request into one frame. Has no business logic and no database access, and does not link libmysqlclient or libsodium | Apache (FastCGI), of-core (Unix socket) |
| of-core | The engine: every request is authenticated, authorised and executed here. It also sends payments, applies gateway replies and expires unpaid reservations | of-api, MySQL, shared memory, both payment queues |
| of-pay | Mock payment gateway. Has no database; links only libsodium for the HMAC | Payment queues |
| of-report | Periodic sales aggregation and atomic CSV reports | MySQL; shared memory (read-only, for live low-stock figures) |
| of-ctl | Operator tool: `useradd`, `invariants` | MySQL; shared memory (read-only) |
| of-bench | Load generator and invariant checker | Apache (HTTP), MySQL; shared memory (read-only) |

*Planned:* the WAL and of-supervisor arrive in phase 4.

### Why these IPC mechanisms

| Link | Mechanism | Why |
|---|---|---|
| Apache → of-api | FastCGI over TCP | Workers stay alive between requests, so there is no fork per request |
| of-api → of-core | Unix domain stream socket with length-prefixed binary frames | Fast and not exposed to the network. The kernel reports the peer's uid (SO_PEERCRED), so the core can refuse other users |
| of-core ↔ tools | POSIX shared memory with C11 atomics | Live stock and metrics can be read without asking the core; the read-only mapping means tools cannot change stock |
| of-core ↔ of-pay | Two POSIX message queues, one per direction, fixed-size HMAC-signed messages | Message boundaries are kept (one send is one receive). Queues live in the kernel, so a request sent while of-pay restarts waits instead of being lost. A queue is a pollable descriptor, so it fits in an epoll loop |

## 3. of-api process model

| Concern | Mechanism |
|---|---|
| Parallelism | Pre-fork: N worker processes all `accept()` on one inherited socket, and the kernel gives each connection to exactly one of them |
| Talking to the core | One persistent Unix-socket connection per worker; N workers means up to N requests in flight at the core |
| Core restarted | Before reusing an idle connection, `recv(MSG_PEEK \| MSG_DONTWAIT)` detects EOF and the worker reconnects. A request is resent only if its send failed, never after it was sent, because the core may already have acted on it |
| Core down | 503 `core_unavailable` at once. A core that hangs gives 502 after `api.core_timeout_s`, using SO_RCVTIMEO |
| Signals in the parent | SIGCHLD, SIGTERM, SIGINT and SIGHUP are blocked before `fork()` and taken synchronously with `sigtimedwait()` |
| Crash recovery | Each dead worker is reaped and restarted after a backoff of 100 ms, doubling to 5 s |

**Lesson learned (phase 1):** FastCGI connection reuse deadlocked the API. With
`enablereuse=on`, libfcgi keeps a worker reading an idle Apache connection instead of
returning to `accept()`. Once every worker was pinned, nothing new was accepted.

The diagnosis came from `/proc/<pid>/wchan`, which showed every worker in
`wait_woken`. The fix is one FastCGI connection per request.

## 4. The binary protocol (`src/common/proto.h`)

```
header (24 bytes): magic "OFP1" | version u16 | type u16 | request_id u64 | length u32 | reserved u32
request payload:   method u8, then path, query, body, session, csrf, ip  (each: u32 length + bytes)
response payload:  status u16, then set_cookie, body
```

- **Caps:** each field has a hard cap: path 1 KiB, query 2 KiB, body 16 KiB, tokens
  128 B, IP 64 B. The whole request payload is limited to about 20 KiB, and a response
  to 4 MiB.
- **Header checks:** the header is validated (magic, version, type, length within the
  cap) before any payload byte is read. A malformed frame closes the connection.
- **Decoding:** every length is checked against the cap and against the bytes
  remaining before anything is copied. Trailing bytes are rejected.
- **Tests:** `tests/unit/test_proto.c` truncates a valid frame at every possible byte
  and checks that each one fails cleanly.
- **Byte order:** both ends run on one host, so integers use host byte order.

## 5. of-core internals (`src/core/main.c`)

```
of-api ─frame─▶ reactor ─queue─▶ worker ─completion list + eventfd─▶ reactor ─▶ of-api
```

- **Reactor (main thread).** One `epoll_wait` loop handles four kinds of descriptor:
  the listening socket, up to 1,024 of-api connections (non-blocking), an eventfd and
  a signalfd. It only moves bytes. It peels complete frames off each connection's read
  buffer, decodes them into jobs and pushes them onto the queue. It also writes
  responses, registering EPOLLOUT only when the kernel send buffer is full.
- **Bounded queue** (`src/common/queue.c`): a ring buffer with one mutex and one
  condition variable.
  - The reactor pushes with `queue_try_push`, which never blocks. When the queue is
    full it answers 503 "busy" at once: backpressure instead of unbounded memory.
  - Workers block in `queue_pop`. A `while` loop around `pthread_cond_wait` handles
    spurious wake-ups.
- **Workers** (`core.workers` threads, default one per CPU):
  - Each worker owns a `store_t` with its own MySQL connection, so there is no
    connection-pool lock.
  - A job that waited longer than `core.job_timeout_ms` is answered 503 without
    running.
  - Slow requests are logged with their trace id, which is the frame's request id.
  - A broken MySQL connection is re-established at most once a second.
- **Completions.** Workers never touch sockets. They append the encoded response to a
  mutex-protected list and write 1 to the eventfd, which wakes the reactor.
- **Connection lifetimes.** Each connection slot has a generation number that is bumped
  on close. A job remembers its slot and generation, so a response for a closed (and
  possibly reused) slot is dropped instead of reaching the wrong peer.
- **Signals (signalfd).**
  - SIGTERM or SIGINT: stop accepting, unlink the socket, finish queued and in-flight
    jobs (at most 10 s), join the workers, remove the shared memory, and exit 0.
  - SIGHUP: reopen the log.
- **Start-up.**
  1. Bind the socket first. If a live core answers on the path, refuse to start, so a
     second core never resets the first one's shared memory.
  2. Create the shared memory and load stock from MySQL.
  3. Start the workers and wait until every one has connected to MySQL.

## 6. Inventory engine and stock modes

### Shared-memory stock table (`src/common/stock.c`)

```c
typedef struct {                  // exactly one 64-byte cache line per product
    _Atomic int32_t  available;   // on hand minus reserved
    _Atomic int32_t  reserved;    // held by checkouts in flight
    _Atomic uint64_t product_id;  // 0 = empty; published last, with release order
    _Atomic int64_t  price_paise;
    _Atomic uint32_t version;     // bumped on every change
    uint8_t          pad[36];
} stock_slot_t;
```

- **Layout:** a 64-byte header holding the capacity, mode and metrics, followed by an
  open-addressing hash table of slots. The table uses Fibonacci hashing and linear
  probing, and its size is a power of two.
- **No false sharing:** each slot is one cache line, so threads updating neighbouring
  products never fight over a line. `_Static_assert` enforces the size.
- **Lock-free lookup:** a lookup probes until it finds the matching `product_id`
  (acquire load) or an empty slot.
  - Inserting (a rare admin action) takes a process-local mutex. It initialises the
    counters first and stores `product_id` last, with release order. A reader that
    sees the id therefore also sees initialised counters.
  - Slots are never removed, because products are soft-deleted.
- **CAS reservation:**

  ```c
  cur = available
  loop: if cur < qty → fail
        if CAS(available, cur → cur - qty) → reserved += qty; done
        // CAS failed: another thread changed it; cur now holds the fresh value; retry
  ```

  - **No deadlock:** nothing is ever locked.
  - **No livelock:** a CAS fails only because another thread's CAS succeeded, so the
    system as a whole always makes progress.
  - **Multi-item carts:** `stock_reserve_all` sorts the lines by product id, reserves
    them in order, and gives back the earlier lines if a later one is short.
- **Counter rules:** reserve (`available -= q`, `reserved += q`); commit
  (`reserved -= q`, once MySQL has the sale); release (`available += q`,
  `reserved -= q`); adjust (CAS, never below zero).

### Three stock modes (`store.stock_mode`, or `of-core -m`)

| Mode | Checkout | Correct? |
|---|---|---|
| naive | Snapshot `SELECT stock`, check, then `UPDATE SET stock = <read> - qty`: a lost-update race | **No.** This is the baseline. Each write is non-negative, so the CHECK constraint never fires, but the ledger records every sale |
| db-lock | `SELECT ... FOR UPDATE` on each row in ascending product-id order (so no lock cycles), then `UPDATE ... SET stock = stock - n WHERE stock >= n`, checking that one row changed | Yes. Buyers of one product queue on its row lock |
| lockfree-shm | CAS-reserve every line in shared memory first; a sold-out buyer is rejected without touching MySQL. Then the guarded `UPDATE` and COMMIT. Commit the reservation on success, release it on any failure | Yes, and the fastest |

### Four layers against overselling

| Layer | Mechanism | Catches |
|---|---|---|
| 1. Reservation | CAS on the shared-memory counter | Concurrent buyers taking the same last units |
| 2. Guarded commit | `UPDATE ... WHERE stock_on_hand >= n`, then check the affected-row count | Any drift between shared memory and MySQL; logged as `stock_drift` |
| 3. Constraint | `CHECK (stock_on_hand >= 0)` | A bug in layers 1 and 2 |
| 4. Verification | `of-ctl invariants` and of-bench: ledger sum equals stock on hand; shared memory equals MySQL | Proof after the fact |

**Admin stock changes keep shared memory exact:**

- **Removals:** taken from shared memory first, by CAS. Units already reserved by
  in-flight checkouts therefore cannot be removed.
- **Additions:** published to shared memory only after the MySQL commit. Nobody can
  reserve stock that is not durable yet.
- **New products:** added to the table when they are created.

## 7. Orders, payments and reports (phase 3)

### The order state machine (`src/common/order_state.c`)

```
placed ──▶ payment_pending ──▶ paid ──▶ packed ──▶ shipped ──▶ delivered
  │              │               │        │
  └──────────────┴──▶ cancelled  └────────┴──▶ refunded
```

| Edge | Who may take it |
|---|---|
| placed → payment_pending | system (checkout) |
| placed / payment_pending → cancelled | system (decline, expiry), customer, admin |
| payment_pending → paid | **system only** (a verified gateway reply) |
| paid → packed → shipped → delivered | admin |
| paid → refunded | customer, admin |
| packed → refunded | admin |

delivered, cancelled and refunded are final. The table is pure C, unit-tested edge by
edge: all 64 state pairs, and every actor on each edge.

**Every transition is one locked, guarded transaction** (`src/store/flow.c`):

1. `SELECT ... FOR UPDATE` on the order row.
2. Check the edge and the actor's permission.
3. Update with a guard: `UPDATE orders SET status = <to> WHERE id = ? AND status = <from>`,
   and require exactly one changed row.
4. Append an `order_events` row and apply the side effects.
5. Commit. Only after the commit is stock returned to shared memory.

**Lock order everywhere:** order row, then payment row, then reservations, then
inventory rows in ascending product id. Every path takes locks in the same order, so
no two of them can deadlock.

### Stock across the order's life

Checkout takes the units off `stock_on_hand` immediately, and records them in
`reservations` with a deadline of `store.reservation_ttl_s`:

| Event | Reservations | Stock |
|---|---|---|
| Checkout | `active` | `stock_on_hand -= qty`; ledger `sale` |
| Paid | `committed` | (sold) |
| Cancelled: decline, expiry, customer or admin | `released` | `stock_on_hand += qty`; ledger `release`; shared memory `+= qty` after the commit |
| Refunded | `released` | `stock_on_hand += qty`; ledger `refund`; shared memory `+= qty` after the commit |

*Deviation from the plan:* the plan reduced `stock_on_hand` only at payment time. Taking
it at checkout instead means:

- MySQL always holds the true number of sellable units.
- The ledger invariant (`SUM(delta) = stock_on_hand`) and the shared-memory check
  (`available = stock_on_hand`) stay simple, and hold at every moment.
- Every exit path gives the stock back explicitly.

### Payments: of-core ⇄ of-pay

```
checkout tx commits ──▶ outbox (bounded) ──▶ pay-sender: pay_sign() + mq_timedsend ──▶ of-pay
of-pay: verify MAC + nonce, decide by token, schedule reply on a min-heap, timerfd fires,
        pay_sign() + mq_send ──▶ background thread: verify MAC + nonce ──▶ store_payment_reply()
```

- **Message** (`src/common/paymsg.h`): a fixed 184-byte struct containing the order,
  amount, idempotency key, method token, provider reference, a random 64-bit nonce and
  a timestamp. HMAC-SHA256 covers every byte before the MAC, using the shared secret
  `pay.hmac_key`.
- **Checking a message:** both ends verify the MAC with libsodium's constant-time
  compare. They also reject a timestamp more than 5 minutes off and any nonce seen
  among the last 4,096 messages.
  - Unit tests flip every byte of a signed message, forge "declined → succeeded", use
    the wrong key and unterminated strings, and replay messages. All are rejected.
- **The kernel limit:** an unprivileged process can hold at most
  `/proc/sys/fs/mqueue/msg_max` messages (10) in a queue.
  - Workers never call `mq_send`. They push to a bounded in-memory outbox, and the
    sender thread blocks while the queue is full.
  - The sender waits with a 1 s timeout, so shutdown never hangs. Once stopping, it
    tries each message only once; measured shutdown with of-pay absent was 0.7 s.
  - If the outbox is full, the order is not lost: it waits, and the reaper cancels it.
- **of-pay is single-threaded.** One epoll loop watches the request queue (pollable on
  Linux), a timerfd always armed for the earliest due reply, and a signalfd.
  - Replies wait in a min-heap ordered by due time, so hundreds of payments can be "in
    progress" with no extra threads.
  - The method token decides the outcome: `tok_success`, `tok_decline`, `tok_timeout`
    (never answers), `tok_duplicate` (the same approval twice), `tok_slow` (answers
    after the TTL), or `tok_random` (configured mix).
  - A repeated idempotency key gets the same outcome and reference.
- **Applying a reply** (`src/store/payments.c`, on the background thread with its own
  MySQL connection). It locks the order, then the payment row, then:

| Payment row is | Order is | Reply | Effect |
|---|---|---|---|
| pending | payment_pending | approved | payment `succeeded`, order `paid`, reservations committed |
| pending | payment_pending | declined | payment `declined`, order `cancelled`, stock returned |
| timed_out / cancelled | cancelled | approved | **late success:** payment `refunded` automatically, timeline note, `audit_log` row, `late_refunds` metric |
| anything else | – | any | duplicate or replay: ignored, `payments_ignored` metric |

- **The expiry reaper:** a timerfd in the same epoll loop, every
  `core.reaper_interval_ms`.
  - It finds orders that have `active` reservations past `expires_at`, using the index
    on `(state, expires_at)`.
  - Each one is cancelled through the same locked, guarded path, with the payment
    marked `timed_out`.
  - When a late reply and the reaper race, the guarded update lets exactly one win;
    the loser takes the "already decided" branch.

### Idempotent checkout

- **The key:** the browser sends `Idempotency-Key: <32 hex>`. It carries the same key
  on retries after a network failure, and uses a new key after any answer from the
  server.
- **Protocol:** the key travels in the binary protocol (version 2 added the field).
- **Storage:** the key is scoped to the user (`u<id>:<key>`) and stored in
  `orders.idempotency_key`, which has a UNIQUE index.
- **A retry** finds the existing order and answers 200 with it, so nothing is created
  twice. Two simultaneous retries both pass the first lookup, but the second INSERT
  violates the UNIQUE index. That request rolls back, returns its shared-memory
  reservation, and answers with the winner's order.
- **Payments** are idempotent the same way: `payments.idem_key` is UNIQUE, and of-pay
  remembers outcomes by key.

### Reports (of-report)

- **Schedule:** a timerfd every `report.interval_s` seconds, and SIGHUP for "now".
- **Aggregates:** in one transaction, of-report deletes and re-inserts the last
  `report.days` days of `daily_sales` and `product_sales_daily`. These cover paid,
  packed, shipped and delivered orders. The run is recorded in `report_meta`.
- **CSV files:** daily sales, top products, low stock and order status. The low-stock
  report includes the **live** available count from shared memory.
- **Atomic replace:** each file is written to a temporary file, `fsync()`ed, renamed
  over the old one with `rename()`, and then the directory is `fsync()`ed. The export
  API therefore never reads a half-written report, and a new report survives a power
  cut.
- **Export safety:** `/api/admin/reports/export` accepts only the four fixed names, so
  no part of the request ever reaches `open()` as a path. It also opens with
  `O_NOFOLLOW` and caps the size at 1 MiB.
- **The dashboard** (`web/admin/reports.html`) shows:
  - revenue, orders, units and average order value;
  - a daily-revenue bar chart with a hover tooltip and a table view;
  - top products, low stock, and live order and payment counts;
  - a system-health panel polling the shared-memory metrics every 2 s.

### Failure handling, as built and tested

| Failure | What happens | Test |
|---|---|---|
| Payment declined | Order cancelled, payment declined, stock returned | payment_test |
| Gateway never answers | Reaper cancels at the TTL, payment `timed_out`, stock returned | payment_test |
| Duplicate or replayed callback | Exactly one effect; the second reply is counted and ignored | payment_test, unit |
| Forged reply (bad MAC) | Dropped before it touches the database; `payments_rejected` metric | unit (paymsg) |
| Approval after the order expired | Refunded automatically and audited; stock not returned twice | payment_test |
| Customer retries checkout | The same Idempotency-Key returns the same order (200) | payment_test |
| of-pay down | Requests wait in the kernel queue (or the outbox); unpaid orders expire; core shutdown stays fast | manual |
| Illegal transition | 409 `invalid_transition`; nothing changes | payment_test, unit |

## 8. Data model

The schema is `sql/schema.sql`:

- users, sessions (SHA-256 of the token only), categories, products (soft delete)
- inventory (`CHECK stock_on_hand >= 0`), carts, cart_items
- orders (status CHECK, user-scoped unique idempotency key), order_items (unit price
  copied at purchase)
- order_events (append-only timeline), stock_movements (ledger), audit_log
- *phase 3:* reservations (`(state, expires_at)` index for the reaper); payments
  (unique idempotency key; pending, succeeded, declined, timed_out, cancelled or
  refunded); daily_sales, product_sales_daily, report_meta (written by of-report)

**Invariant:** for every product, `SUM(stock_movements.delta) = stock_on_hand >= 0`.
In lockfree-shm mode, while no checkout is in flight, it must also hold that
shared-memory `available = stock_on_hand` and `reserved = 0`.

## 9. Security

Security is enforced in of-core, so of-api cannot bypass it.

| Area | Protection |
|---|---|
| Passwords | Argon2id (libsodium) |
| Session tokens | 256 bits from `getrandom(2)`; the database stores only SHA-256 of the token |
| Cookies | HttpOnly and SameSite=Strict |
| CSRF | Per-session token, compared in constant time |
| Login | Lockout with a growing delay; unknown emails take the same time as wrong passwords |
| Authorisation | Route table with public, user and admin access, checked before any handler |
| Order ownership | A customer asking for someone else's order gets 404 |
| SQL | Prepared statements only |
| Request bodies | Strict JSON parser |
| Prices | Always read from the server |
| Browser | Strict CSP, and all text inserted with `textContent` |

Phase 2 adds:

- **Socket permissions:** the core socket is created mode 0660.
- **Peer check:** every connection's uid is checked with SO_PEERCRED; only our own uid
  or `core.allow_uid` is accepted.
- **Smaller front end:** of-api links no database or crypto library.
- **Read-only tools:** tools map shared memory read-only.

Phase 3 adds:

- **Signed payment messages:** HMAC-SHA256 in both directions, compared in constant
  time, with nonce and timestamp replay protection. The secret is in the 0600 config
  file and is generated by `setup-db.sh`.
- **Only the gateway can mark an order paid:** the state machine gives that edge to
  the system actor only. Admins and customers get 409.
- **No card data:** checkout sends a test token.
- **Smaller gateway:** of-pay has no database access and links only libsodium.
- **Report export:** a whitelist of names and `O_NOFOLLOW`.
- **Audit:** every cancellation, refund, status change and automatic refund is written
  to `audit_log`.

## 10. Results so far

### Gate 1 (phase 1)

In naive mode, 40 buyers rushing 10 units produced 38 orders.

### Gate 2: oversell experiment

Run with `scripts/oversell-experiment.sh`: 1,000 buyers, each wanting 1 unit of
HOT-001, with 50 units in stock. Every checkout is fired at the same instant through
Apache, from 8 client threads behind a pthread barrier.

**Setup:** development machine with 12 CPUs, 12 core workers, 16 of-api workers, and
MySQL 8.0 on the same host.

| Mode | Accepted | Sold out (409) | Units sold | Oversold | p50 ms | p95 ms | p99 ms | Invariants |
|---|---|---|---|---|---|---|---|---|
| naive | 600 | 400 | 600 | **550** | 2709 | 3452 | 3474 | ledger -550: FAIL |
| db-lock | 50 | 950 | 50 | 0 | 665 | 968 | 993 | pass |
| lockfree-shm | 50 | 950 | 50 | 0 | 417 | 561 | 572 | pass (MySQL and shared memory) |

Observations:

- **naive** oversold the product twelve times over, and was also the slowest. Nothing
  rejected buyers early, so all 1,000 went through MySQL and queued on the row's
  write lock.
- **db-lock** is correct, but every checkout waits its turn on the `FOR UPDATE` lock.
- **lockfree-shm** is correct and the fastest. The 950 losing buyers are rejected by a
  CAS in shared memory without a single MySQL round trip. Only the 50 winners reach
  the database.
- **Reproducibility:** the numbers above repeated across runs within a few percent.

### Backpressure

`core.workers = 2` and `core.queue_size = 8`, with 1,000 buyers, under AddressSanitizer:

- 968 requests were answered 503 "busy" immediately, so memory did not grow.
- 32 orders went through correctly.
- Nothing was oversold, and shared memory matched MySQL.

### Sanitizers

Both runs used the 1,000-buyer rush plus the 56-check API suite:

- **ThreadSanitizer** build of of-core: no reports.
- **AddressSanitizer** build: no errors, and no leaks at shutdown.

### Gate 3 (phase 3): every Tier 1 feature end to end

- **Payments:** `tests/integration/payment_test.sh` passes 56/56. It covers all five
  gateway behaviours, idempotent checkout, customer cancel and refund, the full admin
  fulfilment flow, illegal transitions, stock returns and the reports API.
- **Rush with payments:** gate 2 still holds with payments flowing. 1,000 buyers for
  50 units gave 50 orders and 0 oversold. All 50 went through the 10-message queues
  via the outbox and were paid. Invariants and shared memory still balance.
- **Sanitizers:** a ThreadSanitizer of-core and AddressSanitizer of-pay and of-report
  ran the payment suite, the API suite and the 1,000-buyer rush. They reported no data
  race, memory error or leak, including at SIGTERM shutdown.

## 11. Testing

| Layer | What runs | Result |
|---|---|---|
| Unit | `make test`: 10 programs covering config, logger, CRC32, JSON, buffer and parsing, **protocol** (round trip, caps, every truncation), **queue** (FIFO, backpressure, 4×4 threads), **stock** (layout, probing, reserve_all rollback, 16 threads on one hot SKU, 8 processes through named shm), **order state machine** (all 64 edges, actors, final states), and **payment messages** (every-byte tampering, forgery, wrong key, replay window and nonce ring) | Passes in debug, asan, ubsan, tsan and release |
| Integration | `tests/integration/api_test.sh`: 56 checks through Apache → of-api → of-core | 56/56 |
| Payments and orders | `tests/integration/payment_test.sh` with `conf/test.conf` (4 s TTL): 56 checks | 56/56 |
| Concurrency | `of-bench oversell` / `scripts/oversell-experiment.sh` | Gate 2 table above |
| Lifecycle | Restart of-core under a running of-api; SIGTERM drain; core down gives 503 | of-api reconnects by itself; clean drain; shared memory and socket removed |
