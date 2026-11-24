#!/usr/bin/env bash
# Start of-core and of-api together in the foreground. Ctrl-C stops both cleanly.
# (A development convenience; of-supervisor replaces it in phase 4.)
#
#   scripts/run.sh                     # stock mode from the config (default lockfree-shm)
#   MODE=naive scripts/run.sh          # override: naive | db-lock | lockfree-shm
#
# Env: CONF (default conf/orderforge.conf), BIN (default build/debug)
set -uo pipefail
cd "$(dirname "$0")/.."

CONF=${CONF:-conf/orderforge.conf}
BIN=${BIN:-build/debug}
for b in of-core of-api; do
    [[ -x $BIN/$b ]] || { echo "missing $BIN/$b: run make first" >&2; exit 1; }
done
[[ -f $CONF ]] || { echo "missing $CONF: run sudo ./scripts/setup-db.sh first" >&2; exit 1; }

get() { sed -n "s/^$1 *= *\"\{0,1\}\([^\"]*\)\"\{0,1\} *$/\1/p" "$CONF" | tail -1; }
SOCK=$(get core.socket); SOCK=${SOCK:-run/of-core.sock}
LOGDIR=$(get log.dir); LOGDIR=${LOGDIR:-run/log}

mode_args=()
[[ -n ${MODE:-} ]] && mode_args=(-m "$MODE")

"$BIN/of-core" -c "$CONF" "${mode_args[@]}" &
CORE=$!
# of-api can start first (it answers 503 until the core is up), but waiting for the
# socket makes the first page load succeed.
for _ in $(seq 100); do
    [[ -S $SOCK ]] && break
    kill -0 "$CORE" 2>/dev/null || { echo "of-core failed to start; see $LOGDIR/of-core.log" >&2; exit 1; }
    sleep 0.1
done

"$BIN/of-api" -c "$CONF" &
API=$!

stop() {
    trap - INT TERM
    kill -TERM "$API" "$CORE" 2>/dev/null
    wait "$API" "$CORE" 2>/dev/null
}
trap 'stop; exit 0' INT TERM

echo "running: of-core pid $CORE, of-api pid $API (logs in $LOGDIR). Ctrl-C to stop."
# If either daemon exits on its own, stop the other too.
wait -n "$CORE" "$API"
echo "a daemon exited; stopping the other" >&2
stop
exit 1
