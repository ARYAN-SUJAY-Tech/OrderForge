#!/usr/bin/env bash
# Run every test suite against the whole stack built in one mode, and (for sanitizer
# builds) fail if any daemon or tool wrote a sanitizer report.
#
#   tests/run-all.sh                 # debug build
#   tests/run-all.sh asan            # AddressSanitizer + LeakSanitizer
#   tests/run-all.sh ubsan           # UndefinedBehaviorSanitizer
#   tests/run-all.sh tsan            # ThreadSanitizer (runs everything under setarch -R)
#
# Order: unit tests; then, on a stack started by of-supervisor with short test timings:
# API, payments, the 1,000-buyer oversell rush, a kill -9 chaos run, the security suite;
# then the failure-injection suite (which starts and stops its own stacks).
# WARNING: resets the database first. Needs Apache; the failure suite also needs to stop
# and start MySQL (MYSQL_STOP / MYSQL_START, default sudo systemctl ...).
#
# Env: CONF (default conf/orderforge.conf), URL (default http://127.0.0.1:8080)
set -uo pipefail
cd "$(dirname "$0")/.."

MODE=${1:-debug}
CONF=${CONF:-conf/orderforge.conf}
URL=${URL:-http://127.0.0.1:8080}
BIN=build/$MODE
LOGS=bench-results/sanitizer-$MODE-$(date +%Y%m%d-%H%M%S)
TCONF=conf/run-all.conf
mkdir -p "$LOGS"

# TSan cannot map its shadow memory with full ASLR; setarch -R is inherited by every
# process the run starts (supervisor, daemons, tools).
if [[ $MODE == tsan && ${RUN_ALL_NOASLR:-} != 1 ]]; then
    exec env RUN_ALL_NOASLR=1 setarch -R "$0" "$@"
fi

export ASAN_OPTIONS="log_path=$PWD/$LOGS/asan:detect_leaks=1"
export UBSAN_OPTIONS="log_path=$PWD/$LOGS/ubsan:print_stacktrace=1"
export TSAN_OPTIONS="log_path=$PWD/$LOGS/tsan"
TARGET_MS=5000
[[ $MODE == asan || $MODE == ubsan ]] && TARGET_MS=10000
[[ $MODE == tsan ]] && TARGET_MS=30000

RESULTS=()
run() { # run NAME COMMAND...
    local name=$1; shift
    echo
    echo "================================================================ $name"
    "$@"
    local rc=$?
    RESULTS+=("$(printf '%-12s %s' "$name" "$([[ $rc == 0 ]] && echo PASS || echo "FAIL ($rc)")")")
}

echo "run-all: mode $MODE, binaries $BIN, sanitizer logs in $LOGS"
make -s MODE="$MODE" || exit 1
run unit make -s MODE="$MODE" test

CONF="$CONF" BIN="$BIN" ./scripts/load-db.sh --yes > /dev/null || exit 1
umask 077
{ cat "$CONF"; echo
  echo "store.reservation_ttl_s = 4"
  echo "pay.slow_ms = 7000"
  echo "core.reaper_interval_ms = 500"
  echo "report.interval_s = 2"
} > "$TCONF"

"$BIN/of-supervisor" -c "$TCONF" 2> "$LOGS/supervisor.err" &
SUP=$!
for _ in $(seq 300); do curl -sf -m 2 -o /dev/null "$URL/api/categories" && break; sleep 0.2; done

run api env BASE="$URL" tests/integration/api_test.sh
run payments env BASE="$URL" TTL=4 tests/integration/payment_test.sh
run oversell "$BIN/of-bench" -c "$TCONF" oversell --buyers 1000 --stock 50 --url "$URL"
run chaos env CONF="$TCONF" BIN="$BIN" URL="$URL" KILLS=1 CLIENTS=30 DURATION=20 \
    TARGET_MS="$TARGET_MS" scripts/chaos.sh
run security env BASE="$URL" CONF="$TCONF" BIN="$BIN" tests/integration/security_test.sh

kill -TERM "$SUP"
wait "$SUP"

run failure env CONF="$CONF" BIN="$BIN" URL="$URL" tests/integration/failure_test.sh

echo
echo "================================================================ summary ($MODE)"
printf '%s\n' "${RESULTS[@]}"
reports=$(ls "$LOGS" | grep -E '^(asan|ubsan|tsan)\.' || true)
if [[ -n $reports ]]; then
    echo "sanitizer reports written: $(echo "$reports" | wc -l) (see $LOGS)"
    head -40 "$LOGS"/$(echo "$reports" | head -1)
    exit 1
fi
[[ $MODE == debug ]] || echo "sanitizer reports written: none"
printf '%s\n' "${RESULTS[@]}" | grep -q FAIL && exit 1
echo "ALL PASS"
