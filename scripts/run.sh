#!/usr/bin/env bash
# Start every OrderForge daemon under of-supervisor, in the foreground.
# Ctrl-C (or `of-ctl stop`) drains and stops them in order.
#
#   scripts/run.sh                     # stock mode from the config (default lockfree-shm)
#   MODE=naive scripts/run.sh          # override: naive | db-lock | lockfree-shm
#
# The supervisor starts of-core first (crash recovery included), then of-pay, of-report
# and of-api; it restarts anything that crashes. See `of-ctl status`.
#
# Env: CONF (default conf/orderforge.conf), BIN (default build/debug)
set -uo pipefail
cd "$(dirname "$0")/.."

CONF=${CONF:-conf/orderforge.conf}
BIN=${BIN:-build/debug}
for b in of-supervisor of-core of-pay of-report of-api; do
    [[ -x $BIN/$b ]] || { echo "missing $BIN/$b: run make first" >&2; exit 1; }
done
[[ -f $CONF ]] || { echo "missing $CONF: run sudo ./scripts/setup-db.sh first" >&2; exit 1; }

get() { sed -n "s/^$1 *= *\"\{0,1\}\([^\"]*\)\"\{0,1\} *$/\1/p" "$CONF" | tail -1; }
if [[ -z $(get pay.hmac_key) ]]; then
    echo "pay.hmac_key is missing from $CONF. Add one with:" >&2
    echo "  echo \"pay.hmac_key = \$(openssl rand -hex 32)\" >> $CONF" >&2
    exit 1
fi

# A second stack would fail half-way (socket, port and pid files taken), and tests would
# then silently talk to the stack that is already running.
running=$(for d in of-supervisor of-core of-pay of-report of-api; do pgrep -a -o -x "$d"; done 2>/dev/null)
if [[ -n $running ]]; then
    echo "OrderForge daemons are already running:" >&2
    echo "$running" | sed 's/^/  /' >&2
    echo "stop them first: $BIN/of-ctl -c <their config> stop   (or Ctrl-C in their terminal)" >&2
    exit 1
fi

mode_args=()
[[ -n ${MODE:-} ]] && mode_args=(-m "$MODE")
exec "$BIN/of-supervisor" -c "$CONF" "${mode_args[@]}"
