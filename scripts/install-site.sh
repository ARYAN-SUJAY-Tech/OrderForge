#!/usr/bin/env bash
# Publish the web front end and enable the Apache virtual host on port 8080.
# Re-run after changing anything under web/. Run with sudo from the repo root.
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "run as root: sudo $0" >&2
    exit 1
fi
cd "$(dirname "$0")/.."

WEBROOT=/var/www/orderforge

# Copy (rather than point Apache at the repo) because home directories are usually not
# readable by the www-data user.
mkdir -p "$WEBROOT"
cp -r web/. "$WEBROOT/"
chown -R root:root "$WEBROOT"
chmod -R u=rwX,go=rX "$WEBROOT"

cp conf/apache-orderforge.conf /etc/apache2/sites-available/orderforge.conf
a2enmod -q proxy proxy_fcgi headers
a2ensite -q orderforge
# The phase-0 spike config would also claim /api/; disable it if it was enabled.
a2disconf -q orderforge-spike 2>/dev/null || true

apache2ctl configtest
systemctl reload apache2

echo "site enabled: http://localhost:8080/  (start of-api so /api/ answers)"
