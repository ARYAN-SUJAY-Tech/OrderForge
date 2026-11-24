#!/usr/bin/env bash
# Write conf/test.conf: your conf/orderforge.conf plus short timings, so the payment
# tests can watch reservations expire and late payments get refunded in seconds.
# (A later duplicate key overrides an earlier one in OrderForge config files.)
#
#   scripts/make-test-conf.sh
#   CONF=conf/test.conf scripts/run.sh
#   TTL=4 tests/integration/payment_test.sh
set -euo pipefail
cd "$(dirname "$0")/.."

SRC=${SRC:-conf/orderforge.conf}
OUT=conf/test.conf
[[ -f $SRC ]] || { echo "missing $SRC: run sudo ./scripts/setup-db.sh first" >&2; exit 1; }

umask 077 # it contains the same secrets as the original
{
    cat "$SRC"
    echo
    echo "# ---- test overrides (scripts/make-test-conf.sh)"
    echo "store.reservation_ttl_s = 4"
    echo "pay.slow_ms = 7000"
    echo "core.reaper_interval_ms = 500"
    echo "report.interval_s = 2"
} > "$OUT"
echo "wrote $OUT (reservation TTL 4 s, slow payments 7 s, reports every 2 s)"
