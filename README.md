# OrderForge

A concurrent online retail engine written in C for Linux. It never oversells stock, even
when hundreds of customers check out the same item at once.

It is built around operating-system concepts: processes, threads, IPC, shared memory,
synchronisation, signals, file I/O and crash recovery. It runs on Linux with MySQL and
Apache.

## Status

| Phase | Content | State |
|---|---|---|
| 0 | Setup and spikes: Makefile, sanitizer builds, config, logger, CRC32, three spikes | **done** |
| 1 | Baseline store: schema, auth, catalogue, cart, naive checkout, basic UI | next |
| 2 | Core engine: of-core, binary protocol, CAS reservation, stock modes | |
| 3 | Orders, payments (of-pay) and reports | |
| 4 | WAL, crash recovery, supervisor, metrics | |
| 5 | Hardening | |
| 6 | Benchmarks and chaos tests | |
| 7 | Documentation, video, slides | |

## Quick start

```sh
sudo ./scripts/install-deps.sh   # MySQL, Apache, libfcgi, libsodium, libmysqlclient
sudo ./scripts/setup-db.sh       # database, user, conf/orderforge.conf (0600)
make && make test
make deps                        # shows which optional libraries were found
```

## Phase-0 spikes

```sh
# 3. Shared memory: four processes increment a shared counter; the plain one loses updates
./build/debug/spikes/shm_counter 4 1000000

# 2. MySQL prepared statement
./build/debug/spikes/mysql_stmt conf/orderforge.conf

# 1. FastCGI through Apache
sudo cp conf/apache-spike.conf /etc/apache2/conf-available/orderforge-spike.conf
sudo a2enconf orderforge-spike && sudo systemctl reload apache2
./build/debug/spikes/fcgi_hello &
curl http://localhost/api/hello
```

## Build modes

`make MODE=debug|release|asan|tsan|ubsan`. Each mode builds into `build/<mode>/`.
