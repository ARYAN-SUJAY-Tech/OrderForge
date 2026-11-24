# OrderForge

A concurrent online retail engine written in C for Linux. It never oversells stock, even
when hundreds of customers check out the same item at once.

It is built around operating-system concepts: processes, threads, IPC, shared memory,
synchronisation, signals, file I/O and crash recovery. It runs on Linux with MySQL and
Apache.

## Status

| Phase | Content | State |
|---|---|---|
| 0 | Setup and spikes: Makefile, sanitizer builds, config, logger, CRC32, three spikes | done |
| 1 | Baseline store: schema, auth, catalogue, cart, naive checkout, storefront and admin UI | **done** (gate 1) |
| 2 | Core engine: of-core, binary protocol, CAS reservation, stock modes | next |
| 3 | Orders, payments (of-pay) and reports | |
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
./build/debug/of-api -c conf/orderforge.conf
```

Then open <http://localhost:8080/>.

| Account | Email | Password |
|---|---|---|
| Admin | admin@orderforge.local | admin12345 |
| Customer | alice@example.com | customer123 |

Details and troubleshooting: [docs/install.md](docs/install.md).

## What phase 1 contains

- **of-api**: a pre-forked FastCGI server behind Apache.
  - The parent supervises its workers with `sigtimedwait`, restarts dead ones with
    backoff, forwards SIGHUP for log reopen, and drains on SIGTERM.
  - Each worker has its own MySQL connection.
- **libofstore**: all business logic, independent of transport.
  - Registration and login with Argon2id.
  - Hashed session tokens, CSRF tokens, and account lockout.
  - Catalogue with search, filter and pagination.
  - Server-side cart, and a checkout that is naive on purpose.
  - Admin product management with soft delete, stock adjustment and the order board.
  - Audit log and stock ledger.
- **of-ctl**: `useradd` (password read from stdin) and `invariants` (stock ledger check).
- **Web UI**: storefront, cart, order tracking and admin console in plain HTML, CSS
  and JavaScript, under a strict CSP.
- **Tests**: 123 unit checks, 56 end-to-end API checks through Apache, and an oversell
  demonstration.

## The baseline oversells (on purpose)

Phase 1 checkout reads the stock, checks it, then writes `stock - qty` back. This is the
classic lost update.

```sh
tests/integration/naive_oversell.sh      # 40 buyers rush 10 units
./build/debug/of-ctl invariants          # the stock ledger disagrees
```

On the development machine, 38 orders succeeded for 10 units, so 28 units were
oversold. Phase 2 adds the `db-lock` and lock-free shared-memory modes that fix it.

## Documentation

- [docs/install.md](docs/install.md): installation guide
- [docs/design.md](docs/design.md): design document (updated every phase)
- [docs/api.md](docs/api.md): API reference
- [docs/user-manual.md](docs/user-manual.md): customer and admin guide

## Build modes

`make MODE=debug|release|asan|tsan|ubsan`. Each mode builds into `build/<mode>/`.
