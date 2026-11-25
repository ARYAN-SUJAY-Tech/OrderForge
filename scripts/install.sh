#!/usr/bin/env bash
# One-command install on Ubuntu 24.04 (or Debian): system packages, database and config,
# build, unit tests, schema and demo data, and the Apache site.
#
#   ./scripts/install.sh          # run as your normal user; it asks for sudo when needed
#
# Then start everything with ./scripts/run.sh and open http://localhost:8080/
#
# Safe to re-run: packages are already there, setup-db keeps the existing password, and
# demo data is loaded only into an empty database (reset later with scripts/load-db.sh).
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ $EUID -eq 0 ]]; then
    echo "run this as your normal user, not with sudo: it calls sudo for the system steps," >&2
    echo "and the build and config file must belong to you" >&2
    exit 1
fi

step() { printf '\n==> %s\n' "$*"; }

step "1/6 system packages (sudo)"
sudo ./scripts/install-deps.sh

step "2/6 database, MySQL user and conf/orderforge.conf (sudo)"
sudo ./scripts/setup-db.sh

step "3/6 build"
make

step "4/6 unit tests"
make test

step "5/6 schema and demo data"
CONF=conf/orderforge.conf
get() { sed -n "s/^$1 *= *\"\{0,1\}\([^\"]*\)\"\{0,1\} *$/\1/p" "$CONF" | tail -1; }
OPTS=$(mktemp)
trap 'rm -f "$OPTS"' EXIT
chmod 600 "$OPTS"
printf '[client]\nhost=%s\nport=%s\nuser=%s\npassword=%s\n' \
    "$(get db.host)" "$(get db.port)" "$(get db.user)" "$(get db.password)" > "$OPTS"
tables=$(mysql --defaults-extra-file="$OPTS" -N -e \
    "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = '$(get db.name)'")
if [[ $tables -eq 0 ]]; then
    ./scripts/load-db.sh --yes
else
    echo "the database already has $tables tables: left as it is"
    echo "(to reset it to the demo data: ./scripts/load-db.sh)"
fi

step "6/6 web site on port 8080 (sudo)"
sudo ./scripts/install-site.sh

cat <<'EOF'

OrderForge is installed.

  start:   ./scripts/run.sh              (foreground; Ctrl-C stops everything)
  open:    http://localhost:8080/
  log in:  admin@orderforge.local / admin12345   or   alice@example.com / customer123
  check:   ./build/debug/of-ctl status   and   ./build/debug/of-ctl invariants

Change the demo passwords before anyone else uses the site (docs/user-manual.md).
EOF
