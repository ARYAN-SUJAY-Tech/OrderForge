# Installation guide

Tested on Ubuntu 24.04 with MySQL 8.0 and Apache 2.4. Run everything from the repository
root.

## 1. Prerequisites

```sh
sudo ./scripts/install-deps.sh
```

This installs the compiler toolchain, `pkg-config`, MySQL server and client library,
Apache, libfcgi and libsodium. It also enables `mod_proxy` and `mod_proxy_fcgi` and
starts MySQL and Apache.

## 2. Database and config file

```sh
sudo ./scripts/setup-db.sh
```

This creates the `orderforge` database and MySQL user with a random password, and
writes `conf/orderforge.conf` from `conf/orderforge.conf.example`. It also generates
`pay.hmac_key`, the secret that signs payment messages. The file has mode 0600 and is
git-ignored, because it holds both secrets.

Re-running the script is safe: it keeps the existing password, and adds the HMAC key if
it is missing.

## 3. Build

```sh
make              # debug build: build/debug/of-api, build/debug/of-ctl
make test         # unit tests
make deps         # shows which libraries pkg-config found
```

Other build modes: `make MODE=release`, `MODE=asan`, `MODE=ubsan` and `MODE=tsan`.

## 4. Load the schema and demo data

```sh
./scripts/load-db.sh
```

This drops and recreates every table, loads 5 categories and 24 products, and creates
these accounts:

| Account | Email | Password |
|---|---|---|
| Admin | admin@orderforge.local | admin12345 |
| Customer | alice@example.com | customer123 |
| Customer | bob@example.com | customer123 |

Override the passwords with `ADMIN_PASSWORD=... DEMO_PASSWORD=... ./scripts/load-db.sh`.
Re-run the script whenever you want a clean store.

## 5. Publish the web site

```sh
sudo ./scripts/install-site.sh
```

This copies `web/` to `/var/www/orderforge` and enables the virtual host on port 8080,
from `conf/apache-orderforge.conf`. Re-run it after changing files under `web/`.

## 6. Start the daemons

```sh
scripts/run.sh                    # of-supervisor and all daemons, in the foreground
MODE=naive scripts/run.sh         # override the stock mode: naive | db-lock | lockfree-shm
```

`run.sh` starts `of-supervisor`, which then:

1. starts `of-core` and waits until it serves, including any crash recovery;
2. starts `of-pay` (the mock payment gateway), `of-report` (sales aggregation) and
   `of-api`;
3. restarts any daemon that crashes.

Ctrl-C, or `of-ctl stop`, stops everything in order. Open <http://localhost:8080/>.

| What | Where |
|---|---|
| Logs | JSON lines in `run/log/<name>.log` |
| Reports | `run/reports/` |
| Pid files and the supervisor status | `run/` |
| Write-ahead log | `run/of-core.wal` |

Operating it, from another terminal:

```sh
build/debug/of-ctl status         # daemons, requests, latency, payments, WAL, recovery
build/debug/of-ctl metrics        # Prometheus text (also: curl localhost:8080/api/metrics)
build/debug/of-ctl invariants     # stock ledger, shared memory vs MySQL, order checks
build/debug/of-ctl stuck          # orders that should have moved on
build/debug/of-ctl reload         # SIGHUP: reopen logs, reload settings, retry failed daemons
build/debug/of-ctl stop           # ordered drain and shutdown
```

You can also start the daemons by hand, without the supervisor, `of-core` first:

```sh
./build/debug/of-core   -c conf/orderforge.conf [-m lockfree-shm]
./build/debug/of-pay    -c conf/orderforge.conf
./build/debug/of-report -c conf/orderforge.conf      # --once: one report run, then exit
./build/debug/of-api    -c conf/orderforge.conf
```

| Action | Command |
|---|---|
| Stop | Ctrl-C, or `kill -TERM <pid>`. of-core drains in-flight requests first |
| Reopen the log files | `kill -HUP <pid>` |
| Regenerate reports now | `pkill -HUP of-report` |

**Upgrading from phase 3:**

1. Stop the old stack: Ctrl-C in its terminal.
2. Run `./scripts/load-db.sh`. Phase 4 adds `orders.wal_txid`, and this resets the
   data.
3. Run `make`.
4. Start everything with `scripts/run.sh`.

**Upgrading from phase 2:**

1. **Reload the schema.** Phase 3 adds tables and widens `orders.idempotency_key`:
   `./scripts/load-db.sh`. This resets the data.
2. **Add the payment secret:**
   `echo "pay.hmac_key = $(openssl rand -hex 32)" >> conf/orderforge.conf`, or re-run
   `sudo ./scripts/setup-db.sh`.
3. **Optionally,** copy the new `pay.*`, `report.*` and `store.reservation_ttl_s`
   settings from `conf/orderforge.conf.example`. The defaults also work.

**Config from phase 1:** if your `conf/orderforge.conf` was created in phase 1, check
that it has `store.stock_mode = lockfree-shm`, not `naive`, and `api.workers = 16`.
Compare it with `conf/orderforge.conf.example`, which documents the new `core.*`
settings.

## 7. Check it works

```sh
tests/integration/api_test.sh            # 56 end-to-end checks through Apache
./build/debug/of-ctl invariants          # ledger check, plus shared memory vs MySQL
./scripts/load-db.sh --yes               # reset afterwards
```

The payment suite waits for reservations to expire, so run the daemons with short test
timings (`conf/test.conf`: your config plus a 4 s TTL):

```sh
scripts/make-test-conf.sh
CONF=conf/test.conf scripts/run.sh       # in one terminal
TTL=4 tests/integration/payment_test.sh  # in another: 56 checks, about a minute
```

## 8. The crash test (gate 4)

With the stack running under the supervisor (`scripts/run.sh`), in another terminal:

```sh
scripts/chaos.sh                        # 50 clients for 30 s, one kill -9 of of-core
KILLS=3 CLIENTS=80 DURATION=45 scripts/chaos.sh
```

It reports how long the API took to serve again after each kill (target under 5 s),
the load run's results, the recovery counters and every invariant. A copy is saved
under `bench-results/`. The test adds orders to the store; reset afterwards with
`scripts/load-db.sh`.

## 9. Security and failure tests (phase 5)

```sh
tests/integration/security_test.sh       # with the stack running; run it last (it uses up the login limit)
```

The failure suite starts its own stack, so stop yours first. It stops and starts MySQL,
so it asks for sudo:

```sh
build/debug/of-ctl stop
tests/integration/failure_test.sh
```

Everything at once, against one build mode (this resets the database):

```sh
tests/run-all.sh asan                 # also: debug, ubsan, tsan
make valgrind                         # unit tests under Valgrind
sudo tests/integration/privdrop_test.sh   # optional: proves the root privilege drop
```

## 10. The oversell experiment (gate 2)

Stop `run.sh`, then start `of-api` on its own. The experiment starts and stops `of-core`
itself, once per mode.

```sh
./build/debug/of-api -c conf/orderforge.conf &
scripts/oversell-experiment.sh           # 1000 buyers, 50 units, naive / db-lock / lockfree-shm
```

The script resets the database before every mode. It prints one report per mode and a
summary table, and writes a CSV to `bench-results/`.

To run one mode by hand while `run.sh` is running:

```sh
./build/debug/of-bench oversell --buyers 1000 --stock 50
```

## 11. The full benchmark suite (phase 6)

All five headline experiments, with charts, in one command (about 25 minutes). Use a
release build: the debug build is compiled without optimisation.

```sh
make MODE=release
./build/debug/of-ctl -c conf/orderforge.conf stop     # the suite runs its own stack
scripts/bench-experiments.sh --yes
```

- It **resets the database** before each experiment and stock mode, and once more at the
  end, so any orders you placed are gone afterwards.
- It needs Apache running. It starts and stops its own of-supervisor from
  `conf/bench.conf`: a copy of your config with rate limits off, every payment
  approved, a 30 s reservation TTL and quieter logs. That file is git-ignored.
- Results go to `bench-results/run-<timestamp>/`:
  - one CSV per experiment;
  - `output.txt` (everything printed);
  - `hardware.txt` (CPU, memory, kernel, MySQL and Apache versions, power profile);
  - `charts/*.svg`.
- Run a subset, or make it shorter:

  ```sh
  EXPERIMENTS="oversell contention" scripts/bench-experiments.sh --yes
  DURATION=8 CLIENTS_LIST="50 250 1000" scripts/bench-experiments.sh --yes
  ```

- Redraw the charts from any run: `python3 scripts/plot-bench.py bench-results/run-…`
- Plug the laptop in and close other heavy programs first; both change the numbers.

The individual tools:

```sh
./build/release/of-bench seed --skus 1000 --stock 1000000      # of-core must be stopped
./build/release/of-bench mix --clients 250 --duration 15 --skus spread --csv out.csv
./build/release/of-bench mix --clients 200 --browse 0 --skus hot   # contention
```

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| 429 "too many requests" while testing | A rate limit; it refills within a minute. For heavy local testing raise `api.rate_*` in the config, or set `api.rate_limit = 0` |
| 415 "request bodies must be application/json" | A client sent a body without `Content-Type: application/json` |
| 403 `cross_origin` | The request's `Origin` header names another site; open the shop at the same host and port Apache serves |
| A daemon prints "config ... readable by every user" | `chmod 600 conf/orderforge.conf` |
| A daemon says it "refuses to run as root" | Start it as your normal user, or set `run.user` |
| `make` says libraries are missing | Run `sudo ./scripts/install-deps.sh`, then `make deps` |
| `/api/...` returns 503 from Apache (HTML page) | of-api is not running or is not listening on `api.listen` (default 127.0.0.1:9000) |
| `/api/...` returns 503 `core_unavailable` | of-core is not running; start it (or use `scripts/run.sh`) |
| of-core: "another of-core is already listening" or "already running (pid N holds run/of-core.pid)" | One is already running; stop it first (`of-ctl stop`) |
| `of-ctl status` shows DEGRADED | A daemon crashed more than 5 times in 60 s; read its log, fix the cause, then `of-ctl reload` |
| of-core logs `reconcile ... corrected` after a crash | Expected occasionally: a crash in the microseconds between a reservation and its log record; shared memory is corrected from MySQL |
| A MySQL error "Unknown column 'wal_txid'" | The schema is from phase 3; run `scripts/load-db.sh` |
| of-core: "cannot create shared memory" | A stale object owned by another user; check `ls -l /dev/shm/orderforge` |
| of-bench FAIL mentioning "oversold units from an earlier run" | A naive run left the ledger unbalanced; reset with `scripts/load-db.sh` |
| `of-bench: no BENCH-* products` | `mix` needs the benchmark catalogue: stop of-core, run `of-bench seed`, start it again (the suite does this for you) |
| `bench-experiments.sh`: "OrderForge (of-core) is running" | The suite runs its own stack; stop yours with `of-ctl -c conf/orderforge.conf stop` |
| of-core: "pay.hmac_key missing or invalid" | Add a key: `echo "pay.hmac_key = $(openssl rand -hex 32)" >> conf/orderforge.conf` |
| of-core: "queue ... exists with message size" | The queue was created by an older build; `rm /dev/mqueue/orderforge-pay-*` |
| of-core: "mq_open ... Invalid argument" | `pay.mq_maxmsg` is above `/proc/sys/fs/mqueue/msg_max` (10 for normal users); keep it at 10 |
| Orders stay "payment pending" and then cancel | of-pay is not running; start it (or use `scripts/run.sh`) |
| Reports page says "no sales report yet" | of-report is not running |
| A MySQL error "Unknown column ... idempotency_key" or "Table 'payments' doesn't exist" | The schema is from an older phase; run `scripts/load-db.sh` |
| of-core logs `worker_init ... Access denied` | Wrong `db.*` settings in `conf/orderforge.conf`; re-run `sudo ./scripts/setup-db.sh` |
| `/api/...` requests hang under load | `enablereuse=on` was added to `ProxyPass`; remove it (see the comment in `conf/apache-orderforge.conf`) |
| Port 8080 already in use | Change `Listen` and `<VirtualHost>` in `conf/apache-orderforge.conf` and reinstall the site |
| `make MODE=tsan test` crashes with "unexpected memory mapping" | Run tsan binaries under `setarch -R` (`make test` already does) |
