# OrderForge design

This document describes OrderForge as it was built. It grew from the project plan
phase by phase:

- Sections 1–10 describe the system.
- Section 11 gives the results measured at each gate.
- Section 12 covers testing.
- Sections 13–15 list where the build differs from the plan, the lessons learned and
  future work.

Companion documents:
- [installation guide](install.md);
- [user manual](user-manual.md);
- [API reference](api.md);
- [security review](security-review.md);
- [benchmark report](benchmark-report.md).

## 1. Goal

OrderForge is an online retail engine in C on Linux. Its defining guarantee is that
stock is never oversold under concurrent checkout. The project is built around
operating-system concepts: processes, threads, IPC, shared memory, synchronisation,
signals and crash recovery.

## 2. Architecture

```
                 of-supervisor ── fork/exec, own process group each, restart with backoff,
                       │          SIGHUP reload, ordered SIGTERM drain, flock pid file
                       ▼ (parent of every daemon below)
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
                    │ payment-reply workers × 4: mq_timedreceive, verify, apply ◀──┼── epoll + timerfd
                    │ reaper thread: timerfd, expires unpaid reservations          │   + reply heap)
                    │ WAL flusher thread: group commit (write + fdatasync)         │ ◀───────────────
                    └──────┬───────────────────┬──────────────────┬────────────────┘  /orderforge-pay-rep
                           ▼                   ▼                  ▼
             POSIX shm "/orderforge"     run/of-core.wal     MySQL (InnoDB) ◀──── of-report (timerfd:
             stock table + metrics       (CRC records)       durable truth        aggregates + CSV via rename)
             survives a crash
                   ▲ read-only
             of-ctl, of-bench, of-report, of-api (/api/metrics)
```

| Process | Role | Talks to |
|---|---|---|
| of-api | Edge checks first: rate limits (token buckets in shared memory, shared by all workers), `Origin`, JSON-only bodies, size caps. Then it turns each FastCGI request into one frame. Has no business logic and no database access, and does not link libmysqlclient or libsodium | Apache (FastCGI), of-core (Unix socket), the rate-limit table in shared memory |
| of-core | The engine: every request is authenticated, authorised and executed here. It also sends payments, applies gateway replies and expires unpaid reservations | of-api, MySQL, shared memory, both payment queues |
| of-pay | Mock payment gateway. Has no database; links only libsodium for the HMAC | Payment queues |
| of-report | Periodic sales aggregation and atomic CSV reports | MySQL; shared memory (read-only, for live low-stock figures) |
| of-supervisor | Parent of the four daemons: starts them in order, restarts crashes, forwards reloads, drains on shutdown | its children (signals, waitpid), the status file |
| of-ctl | Operator tool: `useradd`, `invariants`, `status`, `metrics`, `stuck`, `reload`, `stop` | MySQL; shared memory (read-only); supervisor pid and status files |
| of-bench | Load generator and invariant checker | Apache (HTTP), MySQL; shared memory (read-only) |

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
| lockfree-shm | CAS-reserve every line in shared memory first; a sold-out buyer is rejected without touching MySQL. Log the reservation in the WAL (one disk flush), then the guarded `UPDATE` and COMMIT. Commit the reservation on success, release it on any failure | Yes. Fastest when buyers lose (a sell-out). When every order succeeds, the extra WAL flush makes it slower than db-lock; see the [benchmark report](benchmark-report.md) |

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

**Lock late (phase 6).** In checkout, the inventory `UPDATE` is the last statement
before COMMIT. InnoDB holds the row lock from that statement until the commit, so
anything placed after it makes the next buyer of the same product wait longer. With one
hot product, that hold time sets the orders-per-second ceiling in every stock mode.
Moving the update from the middle of the transaction to the end raised hot-SKU
throughput by 1.5–1.9× (benchmark report, "Lock late"). It also matches the lock order
above.

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

## 8. Durability and supervision (phase 4)

### Why a write-ahead log, when MySQL is the source of truth

The shared-memory stock table is a cache of MySQL. It lives in `/dev/shm`, so it
**survives** a `kill -9` of of-core, together with every metric in it. A lockfree-shm
checkout changes it before MySQL commits:

```
CAS-reserve in shared memory ─▶ RESERVE(txid, lines) durable in the WAL ─▶ MySQL transaction
                                                                          (orders.wal_txid = txid)
                                                                      ─▶ COMMIT(txid, order) | RELEASE(txid)
```

If of-core dies anywhere in that sequence, the surviving table holds units in
`reserved` that may or may not have been sold. The WAL tells recovery exactly which
reservations were in flight, and `orders.wal_txid` tells it what MySQL decided for
each one:

- **txid found in MySQL:** the order committed. Recovery commits the reservation, so
  the units stay sold.
- **Not found:** MySQL rolled the transaction back when the connection died. Recovery
  releases the reservation, so the units go back on sale.

A lost COMMIT or RELEASE record is therefore harmless, which is why only the RESERVE
record is waited on. MySQL is asked, not guessed. This is the participant-lookup step
of a two-phase commit, done at recovery time.

### The log (`src/common/wal.c`)

- **Record:**
  `magic "OFWL" | length | type | seq | payload | CRC32 over type, seq and payload`.
  - Types: RESERVE (txid and the cart lines), COMMIT (txid and order id), RELEASE
    (txid) and CHECKPOINT.
- **Group commit:**
  - Appenders encode into a shared buffer under a mutex and wait on a condition
    variable.
  - One flusher thread swaps the buffer out, then does one `write()` and one
    `fdatasync()` for the whole batch.
  - It lingers `core.wal_group_us` (2 ms) to gather more records, then wakes every
    waiter whose record is now on disk.
  - In the unit test, 8 threads made 8,000 records durable with 503 fsyncs.
- **Torn tail:** replay stops at the first bad magic, impossible length or CRC
  mismatch, then truncates the file there. The unit test cuts a record in half and
  corrupts every single byte of a record; each is rejected.
- **Compaction:** past `core.wal_max_bytes`, the log is rewritten as a CHECKPOINT plus
  a RESERVE for every reservation still in flight. The new file is written, fsynced,
  `rename()`d over the old one, and then the directory is fsynced, so a crash leaves
  either the old log or the new one.
- **Where it applies:** only lockfree-shm checkouts are logged. The sold-out fast path
  never waits on an fsync, because a CAS that fails writes nothing. Only the winners
  pay for durability.

### Start-up and recovery (`setup_stock_and_recover` in of-core)

1. **Single instance:** a pid file locked with `flock()` (the kernel drops the lock
   even on `kill -9`) and the socket probe guarantee one core.
2. **The table:** reopen a surviving table (`stock_reopen`), meaning the previous core
   crashed, or create a fresh one. A clean stop removes the table.
3. **Replay** the WAL, collecting the RESERVE records that have no outcome.
4. **Resolve** each one against MySQL, as described above.
5. **Reconcile** every product: shared memory must show `available == stock_on_hand`
   and `reserved == 0`. Any difference is corrected and counted.
   - Drift is legitimately possible only in tiny windows: between a CAS and its RESERVE
     record, or inside an admin change or a cancellation.
   - Every one of these windows can only make shared memory show **less** stock than
     MySQL, never more. A crash can make OrderForge briefly undersell, never oversell.
6. **New transaction ids** start above anything seen in the log or in
   `orders.wal_txid`.
7. **A fresh log** is started (the compaction path). The core then accepts traffic.

Every step is counted in shared memory: starts, recoveries, recovery time, resolved
committed and released, drift products and units, torn bytes. `of-ctl status` and
`/api/metrics` show them.

### of-supervisor (`src/supervisor/main.c`)

- **Start order:** of-core first, polling until its socket accepts connections.
  Recovery happens before that. Then of-pay, of-report and of-api.
- **Process groups:** each child runs `setpgid(0,0)` before `execv()`. A terminal
  Ctrl-C then reaches only the supervisor, and a stuck child can be killed together
  with its own children (`kill(-pgid)`; of-api has 16).
- **Events:** one epoll loop over a signalfd (SIGCHLD, SIGTERM, SIGINT, SIGHUP) and a
  timerfd armed for the next scheduled restart.
- **Crashes:** `waitpid(WNOHANG)` records the exit status or signal.
  - The child is restarted after a backoff of 100 ms, doubling to 5 s, which resets
    after 10 s of uptime.
  - More than `supervisor.max_restarts` crashes within `supervisor.window_s` marks it
    failed and the system **degraded**. Restarting stops, which prevents fork storms.
  - Tested: six quick `kill -9`s of of-pay tripped it, and `of-ctl reload` revived it.
- **SIGHUP:** forwarded to every child, and failed children get a fresh chance. What
  each daemon reloads:

| Daemon | Reloads on SIGHUP |
|---|---|
| of-core | log file, `log.level`, `core.slow_ms`, `core.job_timeout_ms` |
| of-pay | log file, latency and outcome rates |
| of-report | log file, interval and window (timer re-armed), then reports immediately |
| of-api | log files |

- **SIGTERM / SIGINT:** stops the daemons front to back (of-api, of-report, of-pay,
  then of-core, which drains in-flight requests). Any daemon still running after
  `supervisor.stop_timeout_s` is SIGKILLed with its whole group.
- **Status file:** state goes to `<run.dir>/supervisor.status`, written to a temporary
  file and renamed. `of-ctl status` reads it.

**Lessons learned building it:**

- **A thread that doesn't block SIGTERM kills the process.** of-core blocked
  SIGTERM/SIGINT/SIGHUP only after recovery, but recovery now started the WAL flusher
  thread with those signals unblocked. The kernel may deliver a process-directed signal
  to any thread that doesn't block it, and the default action of SIGTERM killed the
  whole core instead of letting the reactor drain it. The signals are now blocked as
  the first statement of `main`, before any thread exists.
- **One thread for two jobs starved the reaper.** The first chaos run left 344 orders
  unpaid past their deadline: one background thread applied every payment reply *and*
  ran the reaper. At ~230 checkouts/s the reply loop never emptied the queue, so the
  reaper's timer was never serviced. Replies now go to `core.payment_workers` threads,
  each blocking on its own descriptor for the reply queue (the kernel gives each message
  to exactly one of them), and the reaper has its own thread.
- **Per-thread setup slowed recovery.** Each of of-core's 17 store instances computed
  its own Argon2id dummy hash (64 MiB, about 0.1 s each), which slowed every restart.
  It is now computed once per process (`pthread_once`). Recovery to serving dropped
  from about 2.0 s to about 1.4 s.

### Observability

- **`/api/metrics`** (Prometheus text format, localhost only) and **`of-ctl metrics`**
  render the shared-memory metrics. They map the table read-only per scrape, so they
  work while of-core is busy, and they show `core_up 0` while it is down.
- **Contents:** counters for requests, orders, CAS retries, payments, busy rejections,
  expired reservations, the WAL and recovery; a request-latency histogram (time from
  queueing to response); per-product live stock (available and reserved).
- **`of-ctl status`:** daemons (pid, state, restarts, uptime, last exit), the engine,
  approximate latency percentiles, payments, WAL (average batch and fsync time) and
  recovery.
- **`of-ctl stuck` / `of-ctl invariants`:** the order-level invariant checks (section 9).

## 9. Data model

The schema is `sql/schema.sql`:

- users, sessions (SHA-256 of the token only), categories, products (soft delete)
- inventory (`CHECK stock_on_hand >= 0`), carts, cart_items
- orders (status CHECK, user-scoped unique idempotency key), order_items (unit price
  copied at purchase)
- order_events (append-only timeline), stock_movements (ledger), audit_log
- *phase 3:* reservations (`(state, expires_at)` index for the reaper); payments
  (unique idempotency key; pending, succeeded, declined, timed_out, cancelled or
  refunded); daily_sales, product_sales_daily, report_meta (written by of-report)
- *phase 4:* `orders.wal_txid` (unique). The WAL transaction whose reservation the
  order committed is what recovery looks up.

**Invariants, checked by `of-ctl invariants`, `of-bench` and the chaos test:**

- **Stock:** for every product, `SUM(stock_movements.delta) = stock_on_hand >= 0`.
- **Shared memory (lockfree-shm):** while no checkout is in flight,
  `available = stock_on_hand` and `reserved = 0`.
- **Orders:**
  - Every paid, packed, shipped or delivered order has exactly one successful payment.
  - Every order has a `placed` event, and its last event matches its status.
  - Reservation states agree with order states: active means unpaid; committed means
    paid or later; released means cancelled or refunded.
  - No refunded order still holds a successful payment.
  - No unpaid order is more than 30 s past its reservation deadline, and none has no
    active reservation at all.

## 10. Security

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

Phase 5 adds (full review: [security-review.md](security-review.md)):

- **Rate limits:** token buckets in shared memory, shared by every of-api worker
  process. Each bucket's state is one 64-bit word: 40 bits of refill time and 24 bits
  of thousandths of a token. Taking a token is one compare-and-swap, so no locks are
  needed across processes. Separate rules per IP, per session, and for login and
  registration.
- **Cross-site requests:** bodies must be `application/json`, which closes login CSRF
  through `text/plain` forms. A state-changing request with a foreign `Origin` is
  refused.
- **Process hardening:**
  - core dumps off (`RLIMIT_CORE = 0`, `PR_SET_DUMPABLE = 0`);
  - `umask 027`;
  - a warning when the config file is readable by others;
  - started as root, every daemon drops to `run.user`, or refuses to run.
- **Payment replies survive a MySQL outage:** they are retried until applied, and a
  shared "database epoch" makes every thread reconnect after any one of them sees a
  dead connection.

Phase 4 adds:

- **Single instance:** every daemon holds a flock pid file, so a second copy refuses to
  start.
- **Metrics:** `/api/metrics` is served only to 127.0.0.1 or ::1.
- **Read-only tools:** `of-ctl` reads metrics and status through a read-only mapping
  and the status file, and changes nothing except by sending signals.

## 11. Results at each gate

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

### Gate 4 (phase 4): kill -9 under load

Run with `scripts/chaos.sh`. `of-bench load` keeps 50 customers shopping continuously
(add to cart, check out, repeat) through Apache. Partway through, `kill -9` hits of-core
with no warning. The supervisor restarts it, and the new core replays the WAL and
reconciles before serving. Clients whose checkout failed mid-crash retry with the same
Idempotency-Key.

| Run | Load | Kills | Serving again after | In-flight resolved (committed / released) | Drift fixed | Retries answered "already placed" | Invariants |
|---|---|---|---|---|---|---|---|
| 1 | 205 checkouts/s, 30 s | 2 | 2487, 1893 ms | 6 / 5 | 0 | 10 | all pass |
| 2 (one Argon2 hash per process) | 230 checkouts/s, 40 s | 3 | 1368, 1438, 1399 ms | 6 / 8 | 3 products, 5 units | 7 | all pass |

- **Target met:** serving again in under 5 s, with every invariant intact. These are
  the stock ledger, shared memory equals MySQL, and all six order checks.
- **No lost checkouts:** no client gave up, and no checkout was placed twice. The
  "already placed" retries are checkouts that committed in MySQL just before the crash;
  the client retried with the same key and was handed the existing order.
- **Where the outage shows:** in the per-second timeline it is one second with about
  50 failed attempts and one with no traffic, while clients back off.
- **Under ThreadSanitizer**, with every daemon under TSan via `setarch -R` on the
  supervisor, which children inherit:
  - Two kills during load produced no data-race reports, including at shutdown, and
    every invariant passed.
  - Recovery took about 7 s there, because TSan slows everything.
  - Reconciliation corrected 7 units: the CAS-to-WAL window, which is wider when
    everything runs slowly.
- **Under AddressSanitizer:** both 56-check suites passed, chaos passed, and there
  were no memory errors and no leaks at shutdown.

### Phase 5: hardening

`tests/run-all.sh MODE` runs every suite against the whole stack built in one mode:
unit, API, payments, the 1,000-buyer rush, a `kill -9` chaos run, security and failure
injection. It fails if any daemon or tool writes a sanitizer report.

| Mode | unit | api | payments | oversell | chaos | security | failure | Sanitizer reports |
|---|---|---|---|---|---|---|---|---|
| asan (+ LeakSanitizer) | pass | 56/56 | 56/56 | 50 of 50 | pass | 29/29 | 24/24 | none |
| ubsan | pass | 56/56 | 56/56 | 50 of 50 | pass | 29/29 | 24/24 | none |
| tsan (whole stack, `setarch -R`) | pass | 56/56 | 56/56 | 50 of 50 | pass | 29/29 | 24/24 | none |

**Failure injection** (`tests/integration/failure_test.sh`, 24 checks):

| Failure | Result |
|---|---|
| MySQL stopped | Requests answered 503 in about 25 ms (no hang); metrics and `of-ctl status` kept working; no daemon crashed. When MySQL returned, service resumed about 2 s after its start command, sessions were still valid, and orders flowed again. |
| Gateway frozen (SIGSTOP) | 12 checkouts accepted. 10 waited in the kernel queue and 2 in the outbox; all 12 were paid on resume. |
| Gateway killed with requests queued | The supervisor restarted it, and the new of-pay answered the queued requests (all paid). |
| Gateway gone longer than the TTL | Orders cancelled and their stock returned; the late approvals were refunded automatically. |
| Job queue full (2 workers, 4 slots) | 394 requests shed with 503 "busy"; nothing oversold; of-core did not crash. |
| An of-api worker and of-report killed | The API served 20 out of 20 requests meanwhile; of-report was restarted. |

The first run of this suite found finding F3: a payment reply lost after a MySQL outage.
Running it under ThreadSanitizer found F7: of-core starts slowly under TSan, so the MySQL
stop landed while it was starting. The payment-reply workers and the reaper could not
connect, logged the error and exited for good, leaving a core that served requests but
never applied a payment or expired an order. These threads now retry the connection
every second until they succeed or the core shuts down.

**Security suite** (`tests/integration/security_test.sh`, 29 checks): headers; login
CSRF through `text/plain` and form-encoded bodies (415); foreign and `null` Origin
(403); missing CSRF token; size limits; SQL injection in search and in ids; duplicate
and nested JSON; admin access; forged, stale and replayed payment messages injected
straight into the reply queue (`of-bench inject`), where only the genuine signed one
took effect; and both rate limits.

**Valgrind:** `make valgrind` runs every unit test under memcheck, following forked
children, and fails on any memory error or definite leak. Result: all 12 programs clean.
It found one problem in a test: the queue test's producers spun on a full queue, which
nearly livelocks under Valgrind (it runs one thread at a time); they now yield.

### Phase 6: benchmarks (gate 5)

The five headline experiments are in the [benchmark report](benchmark-report.md), with
raw CSVs and charts in [benchmarks/](benchmarks/). The results that change this design:

- **Lock-free admission wins when buyers lose.** In the 1,000-buyer sell-out, p50
  is 350–450 ms against 630–765 ms for db-lock. When every order succeeds, its extra
  WAL flush makes it about 15% slower than db-lock.
- **A hot product is limited by its InnoDB row lock** in every mode, because the
  durable stock write still serialises on one row. Taking that lock last ("lock late",
  §7) gave 1.5–1.9× on one hot SKU.
- **Recovery after `kill -9` takes about 0.9 s** end to end, of which WAL replay is
  under 80 ms.
- **Overload is shed with 503 "busy"** at up to 15,000 requests/s, with of-core's
  memory flat at 20 MiB.

## 12. Testing

| Layer | What runs | Result |
|---|---|---|
| Unit | `make test`: 12 programs (phase 5 adds the **rate limiter**: burst, refill, fail-open, exactness across 8 threads and 6 processes) covering config, logger, CRC32, JSON, buffer and parsing, **protocol** (round trip, caps, every truncation), **queue** (FIFO, backpressure, 4×4 threads), **stock** (layout, probing, reserve_all rollback, 16 threads on one hot SKU, 8 processes through named shm), **order state machine** (all 64 edges, actors, final states), **payment messages** (every-byte tampering, forgery, wrong key, replay window and nonce ring), and **WAL** (encoding, every-byte corruption, torn tail truncated, group commit with 8 threads, compaction keeping exactly the in-flight reservations) | Passes in debug, asan, ubsan, tsan and release |
| Integration | `tests/integration/api_test.sh`: 56 checks through Apache → of-api → of-core | 56/56 |
| Payments and orders | `tests/integration/payment_test.sh` with `conf/test.conf` (4 s TTL): 56 checks | 56/56 |
| Concurrency | `of-bench oversell` / `scripts/oversell-experiment.sh` | Gate 2 table above |
| Lifecycle | Restart of-core under a running of-api; SIGTERM drain; core down gives 503 | of-api reconnects by itself; clean drain; shared memory and socket removed |
| Chaos | `scripts/chaos.sh`: `kill -9` of-core under `of-bench load`, then every invariant | Gate 4 table above |
| Supervision | Crash-loop of-pay six times; `of-ctl reload`; `of-ctl stop` | Degraded after the sixth crash, revived by reload, ordered drain on stop |
| Security | `tests/integration/security_test.sh` | 29/29 |
| Failure injection | `tests/integration/failure_test.sh` | 24/24 |
| Privilege drop | `sudo tests/integration/privdrop_test.sh` | Needs root; run it yourself |
| Everything per build mode | `tests/run-all.sh asan\|ubsan\|tsan` | Phase 5 table above |
| Memory (Valgrind) | `make valgrind` | Unit tests under memcheck |
| Benchmarks | `scripts/bench-experiments.sh --yes`: the five headline experiments, CSVs and charts | [benchmark report](benchmark-report.md) |

## 13. Plan versus what was built

Every Tier 1 (core) and Tier 2 (systems depth) feature in the plan was built. No Tier 3
(stretch) feature was; they are listed under future work. Where the build departs from
the plan, it is for a reason found while building:

| Plan | As built | Why |
|---|---|---|
| Stock on hand is reduced when the payment succeeds | Reduced at checkout; the reservation row carries the deadline, and every cancel, expiry or refund gives the stock back (§7) | MySQL always holds the true sellable count, so the ledger and shared-memory invariants hold at every moment, not only at rest |
| Recovery re-applies a RESERVE with no COMMIT if its TTL has not expired | Each unresolved RESERVE is looked up in MySQL by its transaction id (`orders.wal_txid`): kept if the order committed, released if not (§8) | MySQL is the source of truth. Asking it removes any guess about whether the crash came before or after the commit |
| WAL checkpoints | Compaction: after recovery, and when the log grows past `core.wal_max_bytes`, the open reservations are rewritten to a fresh file, which is renamed into place | Same effect, and the log never needs scanning from a checkpoint marker |
| The WAL applies to the stock path | Only lockfree-shm mode writes the WAL | The other two modes keep no state outside MySQL, so there is nothing to recover |
| of-api checks the session, CSRF token and input | of-api does the edge checks (rate limits, Origin, content type, size caps). Sessions, CSRF and authorisation are in of-core | of-api has no database access by design; the session table lives in MySQL behind the core |
| Apache reuses FastCGI connections | One FastCGI connection per request (§3) | Reuse pinned every libfcgi worker on an idle connection and deadlocked the API |
| One background thread for payment replies and the reaper | `core.payment_workers` reply threads plus a reaper thread of its own (§8) | One thread could not keep up at ~230 checkouts/s, and the reaper starved |
| Rate limiting only | Also a JSON-only body rule and an Origin check (§10) | The security review found login CSRF through `text/plain` forms (finding F1) |
| Inventory updated within the checkout transaction | Updated as the last statement before COMMIT ("lock late", §7) | The row lock is held until COMMIT, so on a hot product every statement after it added to every other buyer's wait (1.5–1.9× faster) |

## 14. Lessons learned

1. **Measure before believing a design.** The lock-free path was expected to help most
   on a hot product. The benchmark showed that the hot product's limit is the InnoDB
   row lock that every successful order still takes. Lock-free admission helps when
   buyers *lose*: a sell-out halves the median wait. It costs about 15% when every
   order succeeds, because of the extra WAL flush.
2. **Kernel details decide correctness.**
   - A process-directed signal can go to any thread that has not blocked it, so
     signals must be blocked before the first thread exists (§8).
   - The kernel drops a `flock()` even on `kill -9`, which is why a pid-file lock is
     reliable and a pid number is not.
3. **Every queue needs a consumer that cannot starve.** One thread serving payment
   replies and the reaper left 344 orders past their deadline under load. Separate
   threads fixed it.
4. **Failure tests find what unit tests cannot.**
   - Injecting a MySQL outage found a payment reply lost on a dead connection (F3).
   - Running that test under ThreadSanitizer, whose slow start overlapped the outage,
     found background threads that gave up for good (F7).
5. **Cheap observability pays for itself.** Shared-memory counters read by `of-ctl`
   and `/api/metrics` explained the deadlock, the starvation and the recovery timings
   without a debugger. `/proc/<pid>/wchan` found the FastCGI deadlock.
6. **Defence in depth is worth its cost.** Four layers guard against overselling. In
   every crash test, the layers behind shared memory (the guarded UPDATE, the CHECK
   constraint and the invariant checker) agreed with it.

## 15. Future work

- **The plan's Tier 3:**
  - signed order receipts (Dilithium through liboqs);
  - a seccomp-bpf sandbox for of-pay;
  - systemd units with cgroup v2 limits;
  - server-sent events instead of polling;
  - TLS in Apache;
  - fuzzing the frame parser with AFL++ or libFuzzer. The parser already rejects
    every truncation in the unit tests; fuzzing would cover random inputs as well.
- **Hot products:** batch the durable stock writes for one product (for example, a
  per-product sales counter flushed periodically, with the WAL as the durable record)
  so its buyers do not commit one at a time.
- **HTTP keep-alive and FastCGI connection pooling** once the libfcgi accept-loop issue
  is worked around, to cut per-request connection cost.
- **Multiple machines:** the shared-memory table limits of-core to one host. Scaling
  out would need the stock partitioned by product across cores.

