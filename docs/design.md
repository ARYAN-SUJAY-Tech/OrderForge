# OrderForge design (draft, updated each phase)

This document grows from the project plan into a description of what was actually
built. Sections marked *planned* describe later phases.

## 1. Goal

OrderForge is an online retail engine in C on Linux. Its defining guarantee is that
stock is never oversold under concurrent checkout. The project is built around
operating-system concepts: processes, threads, IPC, shared memory, synchronisation,
signals and crash recovery.

## 2. Architecture (as built, phase 2)

```
Browser ─HTTP─▶ Apache httpd ─FastCGI (TCP 127.0.0.1:9000)─▶ of-api   (parent + N pre-forked workers)
                static web/, CSP                                │ one persistent connection per worker
                                                                ▼ Unix socket, binary frames, SO_PEERCRED
                                     ┌──────────────────── of-core ────────────────────┐
                                     │ reactor thread: epoll + signalfd + eventfd       │
                                     │        │ bounded queue (503 "busy" when full)    │
                                     │        ▼                                         │
                                     │ worker threads × N ── own MySQL connection each  │
                                     │   libofstore: auth, catalogue, cart, checkout,   │
                                     │   admin; stock modes naive / db-lock / lockfree  │
                                     └──────┬──────────────────────────────┬────────────┘
                                            ▼                              ▼
                              POSIX shm "/orderforge"                MySQL (InnoDB)
                              stock table + metrics                  durable truth
                                    ▲ read-only
                              of-ctl invariants, of-bench
```

| Process | Role | Talks to |
|---|---|---|
| of-api | Turns each FastCGI request into one frame. Has no business logic and no database access, and does not link libmysqlclient or libsodium | Apache (FastCGI), of-core (Unix socket) |
| of-core | The engine: every request is authenticated, authorised and executed here | of-api, MySQL, shared memory |
| of-ctl | Operator tool: `useradd`, `invariants` | MySQL; shared memory (read-only) |
| of-bench | Load generator and invariant checker | Apache (HTTP), MySQL; shared memory (read-only) |

*Planned:* of-pay (POSIX message queues) and of-report join in phase 3. The WAL and
of-supervisor arrive in phase 4.

### Why these IPC mechanisms

| Link | Mechanism | Why |
|---|---|---|
| Apache → of-api | FastCGI over TCP | Workers stay alive between requests, so there is no fork per request |
| of-api → of-core | Unix domain stream socket with length-prefixed binary frames | Fast and not exposed to the network. The kernel reports the peer's uid (SO_PEERCRED), so the core can refuse other users |
| of-core ↔ tools | POSIX shared memory with C11 atomics | Live stock and metrics can be read without asking the core; the read-only mapping means tools cannot change stock |

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

## 7. Data model

The schema is `sql/schema.sql`, unchanged since phase 1:

- users, sessions (SHA-256 of the token only), categories, products (soft delete)
- inventory (`CHECK stock_on_hand >= 0`), carts, cart_items
- orders (status CHECK), order_items (unit price copied at purchase)
- order_events (append-only timeline), stock_movements (ledger), audit_log

**Invariant:** for every product, `SUM(stock_movements.delta) = stock_on_hand >= 0`.
In lockfree-shm mode, while no checkout is in flight, it must also hold that
shared-memory `available = stock_on_hand` and `reserved = 0`.

## 8. Security

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

## 9. Results so far

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

## 10. Testing

| Layer | What runs | Result |
|---|---|---|
| Unit | `make test`: 8 programs covering config, logger, CRC32, JSON, buffer and parsing, **protocol** (round trip, caps, every truncation), **queue** (FIFO, backpressure, 4×4 threads), and **stock** (layout, probing, reserve_all rollback, 16 threads on one hot SKU, 8 processes through named shm) | Passes in debug, asan, ubsan, tsan and release |
| Integration | `tests/integration/api_test.sh`: 56 checks through Apache → of-api → of-core | 56/56 |
| Concurrency | `of-bench oversell` / `scripts/oversell-experiment.sh` | Gate 2 table above |
| Lifecycle | Restart of-core under a running of-api; SIGTERM drain; core down gives 503 | of-api reconnects by itself; clean drain; shared memory and socket removed |
