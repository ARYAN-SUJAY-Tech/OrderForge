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
writes `conf/orderforge.conf` from `conf/orderforge.conf.example`. The file has mode
0600 and is git-ignored, because it holds the database password.

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

## 6. Start the engine and the API

```sh
scripts/run.sh                    # of-core + of-api in the foreground; Ctrl-C stops both
MODE=naive scripts/run.sh         # override the stock mode: naive | db-lock | lockfree-shm
```

`run.sh` starts `of-core` first, waits for its socket, then starts `of-api`. If either
daemon exits, it stops the other.

Open <http://localhost:8080/>. Logs are written as JSON lines to `run/log/of-core.log`
and `run/log/of-api.log`.

You can also start the daemons by hand, in this order:

```sh
./build/debug/of-core -c conf/orderforge.conf [-m lockfree-shm]
./build/debug/of-api  -c conf/orderforge.conf
```

| Action | Command |
|---|---|
| Stop | Ctrl-C, or `kill -TERM <pid>`. of-core drains in-flight requests first |
| Reopen the log files | `kill -HUP <pid>` |

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

## 8. The oversell experiment (gate 2)

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

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `make` says libraries are missing | Run `sudo ./scripts/install-deps.sh`, then `make deps` |
| `/api/...` returns 503 from Apache (HTML page) | of-api is not running or is not listening on `api.listen` (default 127.0.0.1:9000) |
| `/api/...` returns 503 `core_unavailable` | of-core is not running; start it (or use `scripts/run.sh`) |
| of-core: "another of-core is already listening" | One is already running; stop it first (`pkill of-core`) |
| of-core: "cannot create shared memory" | A stale object owned by another user; check `ls -l /dev/shm/orderforge` |
| of-bench FAIL mentioning "oversold units from an earlier run" | A naive run left the ledger unbalanced; reset with `scripts/load-db.sh` |
| of-core logs `worker_init ... Access denied` | Wrong `db.*` settings in `conf/orderforge.conf`; re-run `sudo ./scripts/setup-db.sh` |
| `/api/...` requests hang under load | `enablereuse=on` was added to `ProxyPass`; remove it (see the comment in `conf/apache-orderforge.conf`) |
| Port 8080 already in use | Change `Listen` and `<VirtualHost>` in `conf/apache-orderforge.conf` and reinstall the site |
| `make MODE=tsan test` crashes with "unexpected memory mapping" | Run tsan binaries under `setarch -R` (`make test` already does) |
