#!/usr/bin/env bash
# Headline experiment 1: BUYERS concurrent buyers for STOCK units of one product, in
# each stock mode. For every mode the database is reset, of-core is started in that
# mode, of-bench fires the rush, and of-core is stopped again.
#
#   scripts/oversell-experiment.sh            # 1000 buyers, 50 units, all three modes
#   BUYERS=500 STOCK=20 MODES="db-lock lockfree-shm" scripts/oversell-experiment.sh
#
# Needs Apache and of-api running (of-api reconnects to each new of-core by itself),
# and of-core NOT running (this script starts and stops it).
# WARNING: resets the database (scripts/load-db.sh) before every mode.
#
# Results are appended to bench-results/oversell-<timestamp>.csv.
set -uo pipefail
cd "$(dirname "$0")/.."

CONF=${CONF:-conf/orderforge.conf}
BIN=${BIN:-build/debug}
BUYERS=${BUYERS:-1000}
STOCK=${STOCK:-50}
MODES=${MODES:-"naive db-lock lockfree-shm"}
URL=${URL:-http://127.0.0.1:8080}

get() { sed -n "s/^$1 *= *\"\{0,1\}\([^\"]*\)\"\{0,1\} *$/\1/p" "$CONF" | tail -1; }
SOCK=$(get core.socket); SOCK=${SOCK:-run/of-core.sock}

if [[ -S $SOCK ]] && "$BIN/of-ctl" -c "$CONF" invariants 2>/dev/null | grep -q '^of-core shared memory: mode'; then
    echo "of-core seems to be running; stop it first (this script starts it in each mode)" >&2
    exit 1
fi

mkdir -p bench-results
CSV=bench-results/oversell-$(date +%Y%m%d-%H%M%S).csv
echo "oversell experiment: $BUYERS buyers, $STOCK units, modes: $MODES"
echo "results: $CSV"

for mode in $MODES; do
    echo
    echo "================================================================ $mode"
    ./scripts/load-db.sh --yes > /dev/null || { echo "database reset failed" >&2; exit 1; }

    "$BIN/of-core" -c "$CONF" -m "$mode" 2>/dev/null &
    CORE=$!
    for _ in $(seq 150); do
        [[ -S $SOCK ]] && break
        kill -0 "$CORE" 2>/dev/null || { echo "of-core failed to start" >&2; exit 1; }
        sleep 0.1
    done
    sleep 0.5 # let of-api's workers notice the new core

    "$BIN/of-bench" -c "$CONF" oversell --buyers "$BUYERS" --stock "$STOCK" --url "$URL" \
        --csv "$CSV"
    [[ $mode == naive ]] && echo "(naive mode is the unsafe baseline: FAIL is the expected result)"

    kill -TERM "$CORE"
    wait "$CORE" 2>/dev/null
done

echo
echo "================================================================ summary"
column -s, -t < "$CSV" | cut -c1-160
echo
echo "the database now holds the last run's orders; reset it with: scripts/load-db.sh"
