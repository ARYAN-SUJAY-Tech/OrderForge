#!/usr/bin/env bash
# The five headline experiments (design plan, section 8), run end to end:
#
#   1 oversell      1,000 buyers rush 50 units of one SKU, in each stock mode
#   2 throughput    mixed browse/add/checkout at 50..1,000 clients, in each stock mode
#   3 contention    checkout-only load on one hot SKU vs spread over 1,000 SKUs
#   4 recovery      kill -9 of-core under load, repeatedly (scripts/chaos.sh)
#   5 backpressure  a deliberately small core pushed far past capacity
#
#   scripts/bench-experiments.sh --yes                      # all five, about 25 minutes
#   EXPERIMENTS="oversell contention" scripts/bench-experiments.sh --yes
#   DURATION=10 CLIENTS_LIST="50 250" scripts/bench-experiments.sh --yes   # quicker
#
# WARNING: resets the database (scripts/load-db.sh) before each experiment and stock
# mode. It runs its OWN stack under of-supervisor from conf/bench.conf (a copy of CONF
# with benchmark overrides), so your stack must be stopped first (of-ctl stop); Apache
# must be running and forwarding /api/ to api.listen from CONF.
#
# Benchmark overrides, and why:
#   api.rate_limit = 0         every client comes from 127.0.0.1, so the per-IP limit
#                              would measure the limiter, not the engine
#   pay.*_pct = 0              every payment approves quickly: orders settle within the
#                              run instead of waiting out timeouts
#   store.reservation_ttl_s 30 bounds the settle wait
#   log.level = warn           per-request info logging is not what is being measured
#
# Output: bench-results/run-<timestamp>/ with one CSV per experiment, the raw text
# output, hardware.txt, and charts made by scripts/plot-bench.py.
#
# Env: CONF (default conf/orderforge.conf), BIN (default build/release),
#      URL (default http://127.0.0.1:8080), MODES, CLIENTS_LIST, DURATION, WARMUP,
#      CONTENTION_CLIENTS, SKUS, OVERSELL_REPEATS, CHAOS_KILLS, CHAOS_RUNS,
#      BACKPRESSURE_LIST, EXPERIMENTS, EXTRA_CONF ("key = value; key = value" overrides).
set -uo pipefail
cd "$(dirname "$0")/.."

if [[ ${1:-} != --yes ]]; then
    sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
    echo
    echo "This resets the database named in ${CONF:-conf/orderforge.conf}. Re-run with --yes."
    exit 1
fi

CONF=${CONF:-conf/orderforge.conf}
BIN=${BIN:-build/release}
URL=${URL:-http://127.0.0.1:8080}
MODES=${MODES:-"naive db-lock lockfree-shm"}
CLIENTS_LIST=${CLIENTS_LIST:-"50 100 250 500 1000"}
DURATION=${DURATION:-15}
WARMUP=${WARMUP:-3}
CONTENTION_CLIENTS=${CONTENTION_CLIENTS:-200}
SKUS=${SKUS:-1000}
OVERSELL_REPEATS=${OVERSELL_REPEATS:-3}
CHAOS_KILLS=${CHAOS_KILLS:-5}
CHAOS_RUNS=${CHAOS_RUNS:-2}
BACKPRESSURE_LIST=${BACKPRESSURE_LIST:-"25 100 250 500 1000 2000"}
EXPERIMENTS=${EXPERIMENTS:-"oversell throughput contention recovery backpressure"}
BCONF=conf/bench.conf

[[ -x $BIN/of-bench && -x $BIN/of-supervisor ]] || { echo "build first: make MODE=${BIN#build/}" >&2; exit 1; }
[[ -f $CONF ]] || { echo "no config $CONF" >&2; exit 1; }

STAMP=$(date +%Y%m%d-%H%M%S)
OUT=bench-results/run-$STAMP
mkdir -p "$OUT"
LOG=$OUT/output.txt
say() { echo "$*" | tee -a "$LOG"; }

get() { sed -n "s/^$1 *= *\"\{0,1\}\([^\"]*\)\"\{0,1\} *$/\1/p" "$BCONF" | tail -1; }

make_conf() { # extra "key = value" overrides as arguments
    umask 077
    { cat "$CONF"; echo
      echo "# ---- bench-experiments.sh overrides"
      echo "api.rate_limit = 0"
      echo "pay.decline_pct = 0"
      echo "pay.timeout_pct = 0"
      echo "pay.duplicate_pct = 0"
      echo "store.reservation_ttl_s = 30"
      echo "log.level = warn"
      for o in "$@"; do echo "$o"; done
      [[ -n ${EXTRA_CONF:-} ]] && echo "$EXTRA_CONF" | tr ';' '\n'
    } > "$BCONF"
    umask 022
}

SUP=""
stop_stack() {
    [[ -z $SUP ]] && return
    kill -TERM "$SUP" 2>/dev/null
    wait "$SUP" 2>/dev/null
    SUP=""
}
trap 'stop_stack; [[ -n ${SAMPLER:-} ]] && kill "$SAMPLER" 2>/dev/null' EXIT

start_stack() { # MODE [overrides...]
    local mode=$1; shift
    make_conf "$@"
    "$BIN/of-supervisor" -c "$BCONF" -m "$mode" 2>>"$OUT/supervisor.err" &
    SUP=$!
    for _ in $(seq 300); do
        curl -sf -m 2 -o /dev/null "$URL/api/categories" && { sleep 0.5; return 0; }
        kill -0 "$SUP" 2>/dev/null || break
        sleep 0.2
    done
    say "the stack did not start in mode $mode; see $OUT/supervisor.err"
    exit 1
}

reset_db() { # [seed SKUs]
    CONF="$BCONF" BIN="$BIN" ./scripts/load-db.sh --yes > /dev/null 2>>"$OUT/load-db.err" ||
        { say "database reset failed (see $OUT/load-db.err)"; exit 1; }
    if [[ ${1:-} == seed ]]; then
        "$BIN/of-bench" -c "$BCONF" seed --skus "$SKUS" --stock 1000000 >> "$LOG" ||
            { say "seeding failed"; exit 1; }
    fi
}

bench() { "$BIN/of-bench" -c "$BCONF" "$@" --url "$URL" 2>&1 | tee -a "$LOG"; return "${PIPESTATUS[0]}"; }

# ---------------------------------------------------------------- preflight
make_conf
RUN_DIR=$(get run.dir); RUN_DIR=${RUN_DIR:-run}
for d in of-supervisor of-core of-api; do
    if [[ -f $RUN_DIR/$d.pid ]] && ! flock -n -s "$RUN_DIR/$d.pid" true 2>/dev/null; then
        echo "OrderForge ($d) is running from $RUN_DIR; stop it first (of-ctl -c $CONF stop)" >&2
        exit 1
    fi
done
code=$(curl -s -m 3 -o /dev/null -w '%{http_code}' "$URL/" || true)
[[ $code == 200 ]] || { echo "Apache is not answering at $URL (got '$code')" >&2; exit 1; }

# ---------------------------------------------------------------- hardware and versions
{
    echo "date:        $(date -Is)"
    echo "host cpu:    $(lscpu | sed -n 's/^Model name: *//p')"
    echo "cpus:        $(nproc) logical ($(lscpu | sed -n 's/^Core(s) per socket: *//p') cores per socket, $(lscpu | sed -n 's/^Thread(s) per core: *//p') threads per core)"
    echo "max clock:   $(lscpu | sed -n 's/^CPU max MHz: *//p') MHz"
    echo "memory:      $(free -h | awk '/^Mem:/ {print $2}')"
    echo "kernel:      $(uname -sr)"
    echo "os:          $(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-?}")"
    echo "mysql:       server $(MYSQL_PWD=$(get db.password) mysql -N -h "$(get db.host)" -P "$(get db.port)" -u "$(get db.user)" --protocol=TCP -e 'SELECT CONCAT(VERSION(), ", innodb_buffer_pool_size ", @@innodb_buffer_pool_size DIV 1048576, " MiB, flush_log_at_trx_commit ", @@innodb_flush_log_at_trx_commit)' 2>/dev/null || echo '?')"
    echo "apache:      $(/usr/sbin/apache2 -v 2>/dev/null | head -1)"
    echo "compiler:    $(cc --version | head -1)"
    echo "build:       $BIN"
    echo "commit:      $(git rev-parse --short HEAD 2>/dev/null || echo '?')$(git diff --quiet 2>/dev/null || echo ' (with local changes)')"
    echo "config:      core.workers ${CW:-$(get core.workers)}, core.queue_size $(get core.queue_size), api.workers $(get api.workers), core.payment_workers $(get core.payment_workers), core.wal_group_us $(get core.wal_group_us) (blank = built-in default)"
    echo "power:       on mains: $(cat /sys/class/power_supply/{AC,ADP}*/online 2>/dev/null | head -1), governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null), profile: $(cat /sys/firmware/acpi/platform_profile 2>/dev/null)"
} > "$OUT/hardware.txt"
say "benchmarks -> $OUT  (experiments: $EXPERIMENTS)"
cat "$OUT/hardware.txt" | tee -a "$LOG"

# ---------------------------------------------------------------- 1 oversell
if [[ " $EXPERIMENTS " == *" oversell "* ]]; then
    say ""; say "================ 1. oversell: 1000 buyers, 50 units, $OVERSELL_REPEATS runs per mode"
    for mode in $MODES; do
        for rep in $(seq "$OVERSELL_REPEATS"); do
            reset_db
            start_stack "$mode"
            say "-- $mode, run $rep"
            bench oversell --buyers 1000 --stock 50 --csv "$OUT/oversell.csv"
            stop_stack
        done
    done
fi

# ---------------------------------------------------------------- 2 throughput
if [[ " $EXPERIMENTS " == *" throughput "* ]]; then
    say ""; say "================ 2. throughput and latency: clients $CLIENTS_LIST, $DURATION s each"
    for mode in $MODES; do
        reset_db seed
        start_stack "$mode"
        for c in $CLIENTS_LIST; do
            say "-- $mode, $c clients"
            bench mix --clients "$c" --duration "$DURATION" --warmup "$WARMUP" --browse 4 \
                --skus spread --label throughput --csv "$OUT/throughput.csv"
        done
        stop_stack
    done
fi

# ---------------------------------------------------------------- 3 contention
if [[ " $EXPERIMENTS " == *" contention "* ]]; then
    say ""; say "================ 3. contention: $CONTENTION_CLIENTS clients, checkout only, hot vs spread"
    for mode in $MODES; do
        reset_db seed
        start_stack "$mode"
        for skus in spread hot; do
            say "-- $mode, $skus"
            bench mix --clients "$CONTENTION_CLIENTS" --duration "$DURATION" --warmup "$WARMUP" \
                --browse 0 --skus "$skus" --label contention --csv "$OUT/contention.csv"
        done
        stop_stack
    done
fi

# ---------------------------------------------------------------- 4 crash recovery
if [[ " $EXPERIMENTS " == *" recovery "* ]]; then
    say ""; say "================ 4. crash recovery: $CHAOS_RUNS runs x $CHAOS_KILLS kills under load"
    echo "run,kill,recovery_ms,load_pass,invariants_pass" > "$OUT/recovery.csv"
    for run in $(seq "$CHAOS_RUNS"); do
        reset_db
        start_stack lockfree-shm
        CONF="$BCONF" BIN="$BIN" URL="$URL" KILLS="$CHAOS_KILLS" CLIENTS=100 \
            DURATION=$(( CHAOS_KILLS * 12 + 12 )) ./scripts/chaos.sh > "$OUT/chaos-$run.txt" 2>&1
        cat "$OUT/chaos-$run.txt" >> "$LOG"
        tail -3 "$OUT/chaos-$run.txt" | tee -a /dev/stderr > /dev/null
        times=$(sed -n 's/^recovery times (ms): \([0-9 ]*\).*/\1/p' "$OUT/chaos-$run.txt")
        lp=$(grep -q '^load run: PASS' "$OUT/chaos-$run.txt" && echo 1 || echo 0)
        ip=$(grep -q 'invariants: PASS' "$OUT/chaos-$run.txt" && echo 1 || echo 0)
        k=0
        for ms in $times; do k=$((k + 1)); echo "$run,$k,$ms,$lp,$ip" >> "$OUT/recovery.csv"; done
        say "-- run $run: recovery ms: $times  load $lp invariants $ip"
        stop_stack
    done
fi

# ---------------------------------------------------------------- 5 backpressure
if [[ " $EXPERIMENTS " == *" backpressure "* ]]; then
    say ""; say "================ 5. backpressure: 2 core workers, 16-slot queue, 64 API workers"
    reset_db seed
    start_stack lockfree-shm "core.workers = 2" "core.queue_size = 16" "api.workers = 64"
    CORE_PID0=$(tr -d ' \n' < "$RUN_DIR/of-core.pid")
    echo "t_ms,core_rss_kb,queue_depth,active_workers,busy_rejects_total,requests_total" > "$OUT/backpressure-samples.csv"
    (
        t0=$(date +%s%N)
        v() { awk -v k="$1" '$1 == k {print $2}' <<< "$m"; }
        while sleep 0.5; do
            pid=$(tr -d ' \n' < "$RUN_DIR/of-core.pid" 2>/dev/null)
            rss=$(awk '/^VmRSS:/ {print $2}' "/proc/$pid/status" 2>/dev/null)
            m=$("$BIN/of-ctl" -c "$BCONF" metrics 2>/dev/null)
            echo "$(( ($(date +%s%N) - t0) / 1000000 )),${rss:-0},$(v orderforge_queue_depth),$(v orderforge_active_workers),$(v orderforge_busy_rejects_total),$(v orderforge_requests_total)"
        done
    ) >> "$OUT/backpressure-samples.csv" &
    SAMPLER=$!
    for c in $BACKPRESSURE_LIST; do
        say "-- $c clients"
        bench mix --clients "$c" --duration "$DURATION" --warmup "$WARMUP" --browse 4 \
            --skus spread --label backpressure --csv "$OUT/backpressure.csv"
    done
    kill "$SAMPLER" 2>/dev/null; wait "$SAMPLER" 2>/dev/null; SAMPLER=""
    CORE_PID1=$(tr -d ' \n' < "$RUN_DIR/of-core.pid")
    restarts=$(grep -o 'child=of-core pid=[0-9]* state=[a-z]* restarts=[0-9]*' "$RUN_DIR/supervisor.status" | sed 's/.*restarts=//')
    say "-- of-core pid before $CORE_PID0, after $CORE_PID1, supervisor restarts: ${restarts:-?}"
    echo "core_pid_before,core_pid_after,core_restarts" > "$OUT/backpressure-core.csv"
    echo "$CORE_PID0,$CORE_PID1,${restarts:-}" >> "$OUT/backpressure-core.csv"
    stop_stack
fi

# ---------------------------------------------------------------- charts
say ""
if python3 scripts/plot-bench.py "$OUT" >> "$LOG" 2>&1; then
    say "charts: $OUT/charts/"
else
    say "plotting failed (see $LOG)"
fi
reset_db >/dev/null 2>&1 && say "database reset to the seed data"
say "done: $OUT"
