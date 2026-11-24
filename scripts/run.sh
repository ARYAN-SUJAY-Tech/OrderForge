#!/usr/bin/env bash
# Start every OrderForge daemon in the foreground. Ctrl-C stops them all cleanly.
# (A development convenience; of-supervisor replaces it in phase 4.)
#
#   scripts/run.sh                     # stock mode from the config (default lockfree-shm)
#   MODE=naive scripts/run.sh          # override: naive | db-lock | lockfree-shm
#
# Order: of-core first (it creates the socket, shared memory and payment queues), then
# of-pay, of-report and finally of-api. If any daemon exits, the others are stopped.
#
# Env: CONF (default conf/orderforge.conf), BIN (default build/debug)
set -uo pipefail
cd "$(dirname "$0")/.."

CONF=${CONF:-conf/orderforge.conf}
BIN=${BIN:-build/debug}
for b in of-core of-pay of-report of-api; do
    [[ -x $BIN/$b ]] || { echo "missing $BIN/$b: run make first" >&2; exit 1; }
done
[[ -f $CONF ]] || { echo "missing $CONF: run sudo ./scripts/setup-db.sh first" >&2; exit 1; }

get() { sed -n "s/^$1 *= *\"\{0,1\}\([^\"]*\)\"\{0,1\} *$/\1/p" "$CONF" | tail -1; }
SOCK=$(get core.socket); SOCK=${SOCK:-run/of-core.sock}
LOGDIR=$(get log.dir); LOGDIR=${LOGDIR:-run/log}

if [[ -z $(get pay.hmac_key) ]]; then
    echo "pay.hmac_key is missing from $CONF. Add one with:" >&2
    echo "  echo \"pay.hmac_key = \$(openssl rand -hex 32)\" >> $CONF" >&2
    exit 1
fi

mode_args=()
[[ -n ${MODE:-} ]] && mode_args=(-m "$MODE")

# Refuse to start a second stack: it would fail half-way (socket and port taken), and
# tests would then silently talk to the stack that is already running.
running=$(for d in of-core of-pay of-report of-api; do pgrep -a -o -x "$d"; done 2>/dev/null)
if [[ -n $running ]]; then
    echo "OrderForge daemons are already running:" >&2
    echo "$running" | sed 's/^/  /' >&2
    echo "stop them first (Ctrl-C in their terminal, or: pkill -x of-api; pkill -x of-report; pkill -x of-pay; pkill -x of-core)" >&2
    exit 1
fi

PIDS=()
stop() {
    trap - INT TERM
    # Front to back: stop taking requests first, the engine last.
    for (( i=${#PIDS[@]}-1; i>=0; i-- )); do kill -TERM "${PIDS[i]}" 2>/dev/null; done
    wait "${PIDS[@]}" 2>/dev/null
}
trap 'stop; exit 0' INT TERM

"$BIN/of-core" -c "$CONF" "${mode_args[@]}" &
PIDS+=($!)
# Ready when the socket exists AND our of-core is still alive a moment later (a stale
# socket file from a crashed core must not count as "ready").
ready=0
for _ in $(seq 150); do
    kill -0 "${PIDS[0]}" 2>/dev/null || { echo "of-core failed to start; see $LOGDIR/of-core.log" >&2; exit 1; }
    if [[ -S $SOCK ]]; then
        sleep 0.3
        kill -0 "${PIDS[0]}" 2>/dev/null && { ready=1; break; }
    fi
    sleep 0.1
done
[[ $ready -eq 1 ]] || { echo "of-core did not become ready; see $LOGDIR/of-core.log" >&2; stop; exit 1; }

"$BIN/of-pay" -c "$CONF" &
PIDS+=($!)
"$BIN/of-report" -c "$CONF" &
PIDS+=($!)
"$BIN/of-api" -c "$CONF" &
PIDS+=($!)

echo "running: of-core ${PIDS[0]}, of-pay ${PIDS[1]}, of-report ${PIDS[2]}, of-api ${PIDS[3]}" \
     "(logs in $LOGDIR). Ctrl-C to stop."
wait -n "${PIDS[@]}"
echo "a daemon exited; stopping the others" >&2
stop
exit 1
