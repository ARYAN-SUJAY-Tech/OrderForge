#!/usr/bin/env bash
# Install everything OrderForge needs on Ubuntu 24.04 / Debian. Run with sudo.
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "run as root: sudo $0" >&2
    exit 1
fi

apt-get update
apt-get install -y \
    build-essential pkg-config \
    mysql-server libmysqlclient-dev \
    apache2 \
    libfcgi-dev libfcgi-bin \
    libsodium-dev \
    valgrind curl

# FastCGI reverse proxy for of-api
a2enmod proxy proxy_fcgi headers >/dev/null

systemctl enable --now mysql apache2

echo
echo "dependencies installed. next: sudo ./scripts/setup-db.sh"
