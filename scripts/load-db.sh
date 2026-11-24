#!/usr/bin/env bash
# Load (or reset) the database: schema, seed catalogue, admin and demo accounts.
# WARNING: drops every OrderForge table first. Also used to reset between benchmarks.
#
#   scripts/load-db.sh [--yes]
#
# Env: CONF (default conf/orderforge.conf), BIN (default build/debug),
#      ADMIN_PASSWORD (default admin12345), DEMO_PASSWORD (default customer123)
set -euo pipefail
cd "$(dirname "$0")/.."

CONF=${CONF:-conf/orderforge.conf}
BIN=${BIN:-build/debug}
ADMIN_PASSWORD=${ADMIN_PASSWORD:-admin12345}
DEMO_PASSWORD=${DEMO_PASSWORD:-customer123}

[[ -f $CONF ]] || { echo "missing $CONF: run sudo ./scripts/setup-db.sh first" >&2; exit 1; }
[[ -x $BIN/of-ctl ]] || { echo "missing $BIN/of-ctl: run make first" >&2; exit 1; }

if [[ ${1:-} != --yes && -t 0 ]]; then
    read -r -p "This DROPS all OrderForge tables and reloads demo data. Continue? [y/N] " ans
    [[ $ans == [yY]* ]] || exit 1
fi

# Read "key = value" (optionally quoted) from the config file.
get() { sed -n "s/^$1 *= *\"\{0,1\}\([^\"]*\)\"\{0,1\} *$/\1/p" "$CONF" | tail -1; }

# Pass credentials to the mysql client through a private option file, never argv.
OPTS=$(mktemp)
trap 'rm -f "$OPTS"' EXIT
chmod 600 "$OPTS"
{
    echo "[client]"
    echo "user=$(get db.user)"
    echo "password=$(get db.password)"
    echo "host=$(get db.host)"
    port=$(get db.port); [[ -n $port ]] && echo "port=$port"
    sock=$(get db.socket); [[ -n $sock ]] && echo "socket=$sock"
} > "$OPTS"
DB=$(get db.name)

echo "loading schema into '$DB'"
mysql --defaults-extra-file="$OPTS" "$DB" < sql/schema.sql
echo "loading seed catalogue"
mysql --defaults-extra-file="$OPTS" "$DB" < sql/seed.sql

echo "creating accounts"
echo "$ADMIN_PASSWORD" | "$BIN/of-ctl" -c "$CONF" useradd --admin admin@orderforge.local "Store Admin"
echo "$DEMO_PASSWORD"  | "$BIN/of-ctl" -c "$CONF" useradd alice@example.com "Alice Sharma"
echo "$DEMO_PASSWORD"  | "$BIN/of-ctl" -c "$CONF" useradd bob@example.com "Bob Fernandes"

"$BIN/of-ctl" -c "$CONF" invariants | tail -1
echo
echo "admin login:     admin@orderforge.local / $ADMIN_PASSWORD"
echo "customer logins: alice@example.com, bob@example.com / $DEMO_PASSWORD"
