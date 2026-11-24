#!/usr/bin/env bash
# Headline experiment 4: kill -9 of-core in the middle of sustained load, then verify
# that it came back quickly and that every invariant still holds.
#
#   scripts/chaos.sh                          # 50 clients, 30 s, one kill
#   KILLS=3 CLIENTS=80 DURATION=45 scripts/chaos.sh
#
# Needs the stack running under the supervisor (scripts/run.sh), which restarts of-core;
# the restarted core replays its write-ahead log and reconciles shared memory with MySQL
# before it accepts traffic. Target: serving again in under 5 s, invariants intact.
#
# Env: CONF (default conf/orderforge.conf), BIN (default build/debug),
#      URL (default http://127.0.0.1:8080). Output also saved under bench-results/.
set -uo pipefail
cd "$(dirname "$0")/.."

CONF=${CONF:-conf/orderforge.conf}
BIN=${BIN:-build/debug}
URL=${URL:-http://127.0.0.1:8080}
CLIENTS=${CLIENTS:-50}
DURATION=${DURATION:-30}
KILLS=${KILLS:-1}
TARGET_MS=5000

get() { sed -n "s/^$1 *= *\"\{0,1\}\([^\"]*\)\"\{0,1\} *$/\1/p" "$CONF" | tail -1; }
RUN_DIR=$(get run.dir); RUN_DIR=${RUN_DIR:-run}
CORE_PID_FILE=$RUN_DIR/of-core.pid

if ! "$BIN/of-ctl" -c "$CONF" status 2>/dev/null | grep -q '^supervisor pid'; then
    echo "the supervisor is not running: start the stack with scripts/run.sh first" >&2
    exit 1
fi

mkdir -p bench-results
STAMP=$(date +%Y%m%d-%H%M%S)
OUT=bench-results/chaos-$STAMP
echo "chaos: $CLIENTS clients for $DURATION s, $KILLS x kill -9 of-core; output in $OUT.*"

"$BIN/of-bench" -c "$CONF" load --clients "$CLIENTS" --duration "$DURATION" --url "$URL" \
    > "$OUT.bench.txt" 2>&1 &
BENCH=$!

now_ms() { echo $(( $(date +%s%N) / 1000000 )); }
RECOVERIES=()
gap=$(( DURATION / (KILLS + 1) ))
(( gap < 2 )) && gap=2
for k in $(seq "$KILLS"); do
    sleep "$gap"
    pid=$(tr -d ' \n' < "$CORE_PID_FILE" 2>/dev/null)
    if [[ -z $pid ]] || ! kill -0 "$pid" 2>/dev/null; then
        echo "  kill $k: of-core is not running?" >&2
        continue
    fi
    t0=$(now_ms)
    kill -9 "$pid"
    echo "  kill $k: SIGKILL to of-core pid $pid"
    # Serving again = a request that needs of-core succeeds through the whole stack.
    ok=0
    for _ in $(seq 600); do
        if curl -sf -m 2 -o /dev/null "$URL/api/categories"; then ok=1; break; fi
        sleep 0.05
    done
    t1=$(now_ms)
    newpid=$(tr -d ' \n' < "$CORE_PID_FILE" 2>/dev/null)
    if (( ok )); then
        echo "  kill $k: serving again after $((t1 - t0)) ms (new of-core pid $newpid)"
        RECOVERIES+=($((t1 - t0)))
    else
        echo "  kill $k: NOT serving after 30 s" >&2
        RECOVERIES+=(999999)
    fi
done

echo "  waiting for the load run to finish and settle..."
wait "$BENCH"
BENCH_RC=$?
echo
cat "$OUT.bench.txt"

echo
echo "== of-core recovery, from shared-memory metrics"
"$BIN/of-ctl" -c "$CONF" status | sed -n '/== crash recovery/,$p' | tail -n +2

echo
echo "== invariants"
"$BIN/of-ctl" -c "$CONF" invariants > "$OUT.invariants.txt" 2>&1
INV_RC=$?
grep -E "balance|FAIL|^ok|^orders" "$OUT.invariants.txt"

echo
worst=0
for ms in "${RECOVERIES[@]}"; do (( ms > worst )) && worst=$ms; done
pass=1
(( ${#RECOVERIES[@]} == KILLS )) || pass=0
(( worst <= TARGET_MS )) || pass=0
(( BENCH_RC == 0 && INV_RC == 0 )) || pass=0
{
    echo "recovery times (ms): ${RECOVERIES[*]}  (target <= $TARGET_MS)"
    echo "load run: $([[ $BENCH_RC == 0 ]] && echo PASS || echo FAIL), invariants: $([[ $INV_RC == 0 ]] && echo PASS || echo FAIL)"
    echo "CHAOS $([[ $pass == 1 ]] && echo PASS || echo FAIL)"
} | tee "$OUT.summary.txt"
(( pass ))
