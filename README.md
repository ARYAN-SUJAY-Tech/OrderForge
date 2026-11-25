# OrderForge

A concurrent online retail engine written in C for Linux. It never oversells stock, even
when a thousand customers check out the same item at once.

It is built around operating-system concepts: processes, threads, IPC, shared memory,
synchronisation, signals, file I/O and crash recovery. It runs on Linux with MySQL and
Apache.

## Status

| Phase | Content | State |
|---|---|---|
| 0 | Setup and spikes: Makefile, sanitizer builds, config, logger, CRC32, three spikes | done |
| 1 | Baseline store: schema, auth, catalogue, cart, naive checkout, storefront and admin UI | done (gate 1) |
| 2 | Core engine: of-core, binary protocol, lock-free CAS reservation, three stock modes, of-bench | done (gate 2) |
| 3 | Order state machine, of-pay (mqueues, HMAC), idempotency, expiry reaper, fulfilment, of-report, dashboards | done (gate 3) |
| 4 | Write-ahead log, crash recovery, of-supervisor, metrics endpoint, of-ctl status, chaos test | done (gate 4) |
| 5 | Hardening: rate limits, CSRF/Origin, privilege drop, failure injection, sanitizers, Valgrind | done |
| 6 | Benchmarks: mixed workloads, hot vs spread SKUs, the five headline experiments, charts, report (gate 5) | done |
| 7 | Final design document, user manual with screenshots, API reference with examples, one-command install | **done** |

## Quick start

On Ubuntu 24.04, as your normal user:

```sh
./scripts/install.sh              # packages, database, build, tests, demo data, Apache site
scripts/run.sh                    # of-supervisor starts and watches every daemon; Ctrl-C stops
build/debug/of-ctl status         # (another terminal) daemons, engine, WAL, crash recovery
```

`install.sh` runs these steps, which can also be run one by one:

```sh
sudo ./scripts/install-deps.sh    # MySQL, Apache, libfcgi, libsodium, libmysqlclient
sudo ./scripts/setup-db.sh        # database, user, conf/orderforge.conf (0600)
make && make test
./scripts/load-db.sh              # schema, 24 products, admin and demo accounts
sudo ./scripts/install-site.sh    # web/ -> /var/www/orderforge, vhost on :8080
```

Then open <http://localhost:8080/>.

| Account | Email | Password |
|---|---|---|
| Admin | admin@orderforge.local | admin12345 |
| Customer | alice@example.com | customer123 |

Details and troubleshooting: [docs/install.md](docs/install.md).

## Architecture

```
                                 of-supervisor (restarts, reload, ordered drain)
Browser → Apache → of-api (pre-forked FastCGI) → Unix socket, binary frames → of-core → MySQL
                                                                     │          │   ▲
                                          POSIX shared-memory stock table (CAS) │   └── of-report
                                                     POSIX message queues (HMAC) ⇄ of-pay
```

- **of-core**, the engine:
  - An epoll reactor with signalfd and eventfd.
  - A bounded job queue that answers "busy" when full.
  - Worker threads, each with its own MySQL connection.
  - A lock-free stock table in POSIX shared memory, with one cache line per product.
  - Graceful SIGTERM drain.
- **of-api**, a thin pre-forked FastCGI front end:
  - Turns HTTP into protocol frames, and holds no business logic and no database
    access.
  - Reconnects by itself when of-core restarts.
  - The core checks its uid with `SO_PEERCRED`.
- **Orders:** a state machine (`placed → payment_pending → paid → packed → shipped →
  delivered`, with exits to `cancelled` and `refunded`). Every transition is one locked,
  guarded update, and every exit returns the stock.
- **of-pay**, the mock payment gateway:
  - Talks to of-core over two POSIX message queues, with HMAC-SHA256 signatures and
    replay protection.
  - One epoll loop with a timerfd-driven heap of delayed replies.
  - A test token chooses approve, decline, timeout, duplicate or slow.
- **Payment safety:**
  - A duplicated callback has exactly one effect.
  - An approval that arrives after expiry is refunded automatically.
  - Unpaid reservations expire through a timerfd reaper.
  - Checkout is idempotent via the `Idempotency-Key` header.
- **of-report**: periodic sales aggregation and CSV files, each written atomically with
  `fsync` and `rename()`; feeds the admin dashboard.
- **Crash recovery:**
  - The shared-memory stock table survives a `kill -9` of of-core.
  - A write-ahead log, with group commit and CRC-checked records, records every
    in-flight reservation before MySQL is touched.
  - On restart, the core resolves each in-flight reservation by asking MySQL whether
    that transaction committed, then reconciles every product.
- **of-supervisor:** starts every daemon in its own process group, restarts crashes
  with backoff, and gives up on a crash loop ("degraded"). SIGHUP reloads settings;
  SIGTERM stops everything in order.
- **of-ctl**: `status`, `metrics`, `invariants`, `stuck`, `reload`, `stop`, `useradd`.
  It reads shared memory read-only and never calls the core.
- **Metrics:** `/api/metrics` in Prometheus format, localhost only, with a latency
  histogram and per-product live stock.
- **of-bench**: a load generator. `oversell` is a barrier-synchronised rush; `mix` is
  closed-loop shoppers (browse, add, checkout) over one hot SKU or 1,000. Exact latency
  percentiles per operation, invariant checks after every run, CSV output.

## The oversell experiment (gate 2)

One thousand buyers check out the last 50 units of one product at the same instant:

```sh
scripts/oversell-experiment.sh     # resets the DB and runs all three stock modes
```

| Stock mode | Orders accepted | Oversold | p50 latency |
|---|---|---|---|
| naive (read-modify-write) | 600 | **550** | 2709 ms |
| db-lock (`SELECT … FOR UPDATE`) | 50 | 0 | 665 ms |
| lockfree-shm (CAS in shared memory) | **50** | **0** | **417 ms** |

The lock-free mode rejects the 950 losing buyers with one compare-and-swap each, without
touching MySQL. Method, hardware and analysis are in
[docs/design.md](docs/design.md#11-results-at-each-gate).

## Payments and order tracking (gate 3)

```sh
scripts/make-test-conf.sh && CONF=conf/test.conf scripts/run.sh   # 4-second reservations
TTL=4 tests/integration/payment_test.sh                            # 56 checks
```

The test covers every gateway outcome: approve, decline, silence, duplicate reply, and
late reply after expiry. It also covers idempotent retries, customer cancel and refund,
admin pack, ship and deliver, illegal transitions, stock returns and the reports API.

## Crash test (gate 4)

```sh
scripts/run.sh                       # terminal 1
scripts/chaos.sh                     # terminal 2: kill -9 of-core under 50 shoppers
KILLS=3 DURATION=40 scripts/chaos.sh
```

| Load | Kills | Back to serving | In-flight reservations resolved | Invariants |
|---|---|---|---|---|
| 230 checkouts/s | 3 × `kill -9 of-core` | 1.37 s, 1.44 s, 1.40 s | 6 committed, 8 released (MySQL decides) | all pass |

No checkout was lost or duplicated. A checkout that committed just before the crash is
answered "already placed" when the client retries it with the same key.

## Hardening (phase 5)

```sh
tests/run-all.sh asan        # every suite against the whole stack under AddressSanitizer
tests/integration/security_test.sh   # 29 checks: CSRF, Origin, limits, injection, forged payments
tests/integration/failure_test.sh    # 24 checks: MySQL down, gateway hung/killed, queue full
make valgrind                # unit tests under Valgrind memcheck
```

The security review in [docs/security-review.md](docs/security-review.md) covers every
endpoint and process boundary, and lists six findings fixed in this phase. One example:
login could be forged from another site with a `text/plain` HTML form.

## Benchmarks (phase 6)

```sh
make MODE=release
scripts/bench-experiments.sh --yes     # all five experiments + charts, ~30 min, resets the DB
```

| Experiment | Result (one laptop, on battery; details in the report) |
|---|---|
| Oversell: 1,000 buyers, 50 units | db-lock and lockfree-shm sold exactly 50 in every run; naive sold ~590 |
| Throughput, 50 → 1,000 clients | ~2,200 requests/s, ~450 orders/s; the CPUs (mostly MySQL) are the limit |
| One hot SKU vs 1,000 SKUs | a hot row halves throughput in every mode; moving the row lock to just before COMMIT gave 1.5–1.9× |
| `kill -9` of-core × 10 under load | back in 0.89–1.0 s every time, all invariants intact |
| 2-worker core under 2,000 clients | up to 15,000 req/s refused with 503 busy; no crash; memory flat at 20 MiB |

Method, hardware, charts and limitations: [docs/benchmark-report.md](docs/benchmark-report.md).

## Documentation

- [docs/install.md](docs/install.md): installation guide
- [docs/design.md](docs/design.md): design document (updated every phase)
- [docs/api.md](docs/api.md): API reference
- [docs/security-review.md](docs/security-review.md): security review, endpoint by endpoint
- [docs/benchmark-report.md](docs/benchmark-report.md): benchmark report, with raw results in [docs/benchmarks/](docs/benchmarks/)
- [docs/user-manual.md](docs/user-manual.md): customer and admin guide, with screenshots

## Build modes

`make MODE=debug|release|asan|tsan|ubsan`. Each mode builds into `build/<mode>/`.
`make test` runs the unit tests.
