#!/usr/bin/env bash
# Create the orderforge database and user, then write conf/orderforge.conf with a
# freshly generated password (mode 0600, git-ignored). Run with sudo from the repo root.
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "run as root: sudo $0" >&2
    exit 1
fi

cd "$(dirname "$0")/.."
CONF=conf/orderforge.conf
DB=orderforge
DBUSER=orderforge

if [[ -f $CONF ]] && grep -q '^db.password' "$CONF"; then
    PASS=$(sed -n 's/^db.password *= *"\{0,1\}\([^"]*\)"\{0,1\}$/\1/p' "$CONF")
    echo "reusing password from $CONF"
else
    PASS=$(head -c 24 /dev/urandom | base64 | tr -d '/+=' | head -c 24)
    sed "s|^db.password *=.*|db.password = $PASS|" conf/orderforge.conf.example > "$CONF"
    echo "wrote $CONF"
fi

mysql <<SQL
CREATE DATABASE IF NOT EXISTS \`$DB\` CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci;
CREATE USER IF NOT EXISTS '$DBUSER'@'localhost' IDENTIFIED BY '$PASS';
ALTER USER '$DBUSER'@'localhost' IDENTIFIED BY '$PASS';
GRANT ALL PRIVILEGES ON \`$DB\`.* TO '$DBUSER'@'localhost';
FLUSH PRIVILEGES;
SQL

# The config holds secrets: readable only by the invoking user.
OWNER=${SUDO_USER:-root}
chown "$OWNER" "$CONF"
chmod 0600 "$CONF"

echo "database '$DB' and user '$DBUSER' ready"
