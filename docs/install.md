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

## 6. Start the API

```sh
./build/debug/of-api -c conf/orderforge.conf
```

Open <http://localhost:8080/>. Logs are written as JSON lines to `run/log/of-api.log`.

| Action | Command |
|---|---|
| Stop | Ctrl-C, or `kill -TERM <pid>` |
| Reopen the log file | `kill -HUP <pid>` |

## 7. Check it works

```sh
tests/integration/api_test.sh            # 56 end-to-end checks through Apache
tests/integration/naive_oversell.sh      # shows the phase-1 naive checkout overselling
./build/debug/of-ctl invariants          # stock ledger check
./scripts/load-db.sh --yes               # reset afterwards
```

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `make` says libraries are missing | Run `sudo ./scripts/install-deps.sh`, then `make deps` |
| `/api/...` returns 503 from Apache | of-api is not running or is not listening on `api.listen` (default 127.0.0.1:9000) |
| of-api logs `worker_init ... Access denied` | Wrong `db.*` settings in `conf/orderforge.conf`; re-run `sudo ./scripts/setup-db.sh` |
| `/api/...` requests hang under load | `enablereuse=on` was added to `ProxyPass`; remove it (see the comment in `conf/apache-orderforge.conf`) |
| Port 8080 already in use | Change `Listen` and `<VirtualHost>` in `conf/apache-orderforge.conf` and reinstall the site |
| `make MODE=tsan test` crashes with "unexpected memory mapping" | Run tsan binaries under `setarch -R` (`make test` already does) |
