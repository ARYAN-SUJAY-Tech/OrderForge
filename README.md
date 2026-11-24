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
| 2 | Core engine: of-core, binary protocol, lock-free CAS reservation, three stock modes, of-bench | **done** (gate 2) |
| 3 | Orders, payments (of-pay) and reports | next |
| 4 | WAL, crash recovery, supervisor, metrics | |
| 5 | Hardening | |
| 6 | Benchmarks and chaos tests | |
| 7 | Documentation, video, slides | |

## Quick start

```sh
sudo ./scripts/install-deps.sh    # MySQL, Apache, libfcgi, libsodium, libmysqlclient
sudo ./scripts/setup-db.sh        # database, user, conf/orderforge.conf (0600)
make && make test
./scripts/load-db.sh              # schema, 24 products, admin and demo accounts
sudo ./scripts/install-site.sh    # web/ -> /var/www/orderforge, vhost on :8080
scripts/run.sh                    # of-core + of-api; Ctrl-C stops both
```

Then open <http://localhost:8080/>.

| Account | Email | Password |
|---|---|---|
| Admin | admin@orderforge.local | admin12345 |
| Customer | alice@example.com | customer123 |

Details and troubleshooting: [docs/install.md](docs/install.md).

## Architecture

```
Browser → Apache → of-api (pre-forked FastCGI) → Unix socket, binary frames → of-core → MySQL
                                                                               │
                                                     POSIX shared-memory stock table (CAS)
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
- **of-ctl**: `useradd` and `invariants`. The invariant check covers the stock ledger
  and, read-only, compares shared memory with MySQL.
- **of-bench**: an epoll load generator with a barrier-synchronised rush, latency
  percentiles, invariant checks and CSV output.

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
[docs/design.md](docs/design.md#9-results-so-far).

## Documentation

- [docs/install.md](docs/install.md): installation guide
- [docs/design.md](docs/design.md): design document (updated every phase)
- [docs/api.md](docs/api.md): API reference
- [docs/user-manual.md](docs/user-manual.md): customer and admin guide

## Build modes

`make MODE=debug|release|asan|tsan|ubsan`. Each mode builds into `build/<mode>/`.
`make test` runs the unit tests.
