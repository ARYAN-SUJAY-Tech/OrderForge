#!/usr/bin/env bash
# Failure injection: break things on purpose and check that OrderForge degrades cleanly
# and recovers with every invariant intact.
#
#   A  MySQL stops           requests fail fast with 503 (no hangs, no crashes); metrics
#                            and of-ctl keep working; when MySQL returns, the workers
#                            reconnect, sessions are still valid and orders flow again
#   B  gateway hangs         (SIGSTOP of-pay) more orders than the 10-message queue holds
#                            wait in the queue and the outbox, and are all paid on resume
#   C  gateway killed        while requests sit in the queue: the supervisor restarts it
#                            and the new of-pay answers them (the kernel queue survived)
#   D  gateway gone too long past the reservation TTL: orders expire, stock comes back,
#                            and the late approvals are refunded automatically
#   E  job queue full        tiny core (2 workers, 4-slot queue): load is shed with 503
#                            "busy", nothing is oversold, nothing crashes
#   F  other daemons die     an of-api worker and of-report are killed; service continues
#
# The script runs its own stack (conf/failure.conf = your config + short timings) under
# of-supervisor, so stop any running stack first. It needs to stop and start MySQL:
#   MYSQL_STOP / MYSQL_START   shell commands (default: sudo systemctl stop|start mysql)
#
#   tests/integration/failure_test.sh
#   CONF=conf/orderforge.conf URL=http://127.0.0.1:8080 tests/integration/failure_test.sh
# The test adds orders; reset afterwards with scripts/load-db.sh.
set -uo pipefail
cd "$(dirname "$0")/../.."

CONF=${CONF:-conf/orderforge.conf}
BIN=${BIN:-build/debug}
URL=${URL:-${BASE:-http://127.0.0.1:8080}} # BASE accepted too, like the other suites
MYSQL_STOP=${MYSQL_STOP:-sudo systemctl stop mysql}
MYSQL_START=${MYSQL_START:-sudo systemctl start mysql}
ADMIN_EMAIL=${ADMIN_EMAIL:-admin@orderforge.local}
ADMIN_PASSWORD=${ADMIN_PASSWORD:-admin12345}
FCONF=conf/failure.conf
TTL=4

TMP=$(mktemp -d)
PASS=0
FAIL=0
SUP=""
cleanup() {
    [[ -n $SUP ]] && kill -TERM "$SUP" 2>/dev/null && wait "$SUP" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

check() {
    local desc=$1; shift
    if "$@"; then PASS=$((PASS + 1)); printf '  ok    %s\n' "$desc"
    else FAIL=$((FAIL + 1)); printf '  FAIL  %s  (status %s, body %.200s)\n' "$desc" "${STATUS:-}" "${BODY:-}"; fi
}
is() { [[ "$1" == "$2" ]]; }
req() { # req WHO METHOD PATH [BODY]
    local jar=$TMP/$1.jar csrf=$TMP/$1.csrf
    local args=(-s -m 20 -o "$TMP/body" -w '%{http_code}' -b "$jar" -c "$jar" -X "$2")
    [[ -f $csrf ]] && args+=(-H "X-CSRF-Token: $(cat "$csrf")")
    [[ -n ${4:-} ]] && args+=(-H 'Content-Type: application/json' --data-binary "$4")
    STATUS=$(curl "${args[@]}" "$URL/api$3")
    BODY=$(cat "$TMP/body")
}
json() { python3 -c 'import json,sys; d=json.loads(sys.argv[1]); print(eval(sys.argv[2]))' "$BODY" "$1" 2>/dev/null; }
now_ms() { echo $(( $(date +%s%N) / 1000000 )); }

get() { sed -n "s/^$1 *= *\"\{0,1\}\([^\"]*\)\"\{0,1\} *$/\1/p" "$FCONF" | tail -1; }
pid_of() { tr -d ' \n' < "$RUN_DIR/$1.pid" 2>/dev/null; }

# A config with short timings (+ extra overrides for a stage).
make_conf() {
    umask 077
    { cat "$CONF"; echo
      echo "# ---- failure_test.sh overrides"
      echo "store.reservation_ttl_s = $TTL"
      echo "pay.slow_ms = 7000"
      echo "core.reaper_interval_ms = 500"
      echo "report.interval_s = 2"
      for o in "$@"; do echo "$o"; done
    } > "$FCONF"
}

start_stack() {
    "$BIN/of-supervisor" -c "$FCONF" 2>"$TMP/sup.err" &
    SUP=$!
    for _ in $(seq 150); do
        curl -sf -m 2 -o /dev/null "$URL/api/categories" && return 0
        kill -0 "$SUP" 2>/dev/null || break
        sleep 0.2
    done
    echo "the stack did not start:" >&2
    cat "$TMP/sup.err" >&2
    exit 1
}

stop_stack() {
    kill -TERM "$SUP" 2>/dev/null
    wait "$SUP" 2>/dev/null
    SUP=""
}

login() {
    rm -f "$TMP/admin.jar" "$TMP/admin.csrf" "$TMP/c.jar" "$TMP/c.csrf"
    req admin POST /auth/login "{\"email\":\"$ADMIN_EMAIL\",\"password\":\"$ADMIN_PASSWORD\"}"
    json 'd["csrf_token"]' > "$TMP/admin.csrf"
    req c POST /auth/register "{\"email\":\"fail$(date +%s%N)@example.com\",\"password\":\"failtest123\",\"name\":\"Failure Tester\"}"
    json 'd["csrf_token"]' > "$TMP/c.csrf"
}

# buy METHOD -> ORDER id (STATUS of the checkout)
buy() {
    req c POST /cart/items '{"product_id":3,"qty":1}'
    req c POST /checkout "{\"payment_method\":\"$1\"}"
    ORDER=$(json 'd["order"]["id"]')
}
status_of() { req c GET "/orders/$1"; json 'd["order"]["status"]'; }
wait_status() { # ORDER STATE SECONDS
    local deadline=$((SECONDS + $3))
    while (( SECONDS <= deadline )); do
        [[ $(status_of "$1") == "$2" ]] && return 0
        sleep 0.3
    done
    return 1
}
metric() { req admin GET /admin/system; json "d['metrics']['$1']"; }
restarts() { grep -o "child=$1 pid=[0-9]* state=[a-z]* restarts=[0-9]*" "$RUN_DIR/supervisor.status" | sed 's/.*restarts=//'; }

# ---------------------------------------------------------------- preflight
make_conf
RUN_DIR=$(get run.dir); RUN_DIR=${RUN_DIR:-run}
# A stack using this run directory is running if its pid files are flock-ed.
for d in of-supervisor of-core of-api; do
    if [[ -f $RUN_DIR/$d.pid ]] && ! flock -n -s "$RUN_DIR/$d.pid" true 2>/dev/null; then
        echo "OrderForge ($d) is already running from $RUN_DIR; stop it first (of-ctl stop)" >&2
        exit 1
    fi
done
start_stack
login
echo "stack up (config $FCONF, reservation TTL ${TTL}s, requests to $URL)"

# ---------------------------------------------------------------- A: MySQL down
echo "A. MySQL stops"
bash -c "$MYSQL_STOP" >/dev/null 2>&1
for _ in $(seq 60); do
    req anon GET /products/1
    [[ $STATUS == 503 ]] && break
    sleep 0.25
done
t0=$(now_ms)
req anon GET /products/1
t1=$(now_ms)
check "requests fail fast with 503 db_unavailable ($((t1 - t0)) ms)" \
    is "$STATUS $(json 'd["error"]["code"]') $(( t1 - t0 < 3000 ))" "503 db_unavailable 1"
req c POST /checkout '{"payment_method":"tok_success"}'
check "checkout answers 503 instead of hanging" is "$STATUS" "503"
STATUS=$(curl -s -m 5 -o "$TMP/body" -w '%{http_code}' "$URL/api/metrics"); BODY=""
check "metrics still served from shared memory" is "$STATUS" "200"
check "of-ctl status still works" "$BIN/of-ctl" -c "$FCONF" status >/dev/null
check "no daemon crashed" is "$(restarts of-core)$(restarts of-api)" "00"
bash -c "$MYSQL_START" >/dev/null 2>&1
t0=$(now_ms)
back=0
for _ in $(seq 240); do
    req anon GET /products/1
    [[ $STATUS == 200 ]] && { back=1; break; }
    sleep 0.25
done
t1=$(now_ms)
check "service resumes when MySQL returns ($((t1 - t0)) ms after the start command)" is "$back" "1"
sleep 1.5 # let every worker reconnect (each retries once a second)
req c GET /me
check "sessions survived the outage" is "$STATUS" "200"
buy tok_success
check "orders flow again" wait_status "$ORDER" paid 6

# ---------------------------------------------------------------- B: gateway hangs
echo "B. payment gateway hangs (SIGSTOP), more orders than the queue holds"
PAY=$(pid_of of-pay)
kill -STOP "$PAY"
ORDERS=()
for _ in $(seq 12); do
    buy tok_success
    ORDERS+=("$ORDER")
done
check "all 12 checkouts accepted while the gateway is frozen" is "${#ORDERS[@]}" "12"
check "they wait in payment_pending" is "$(status_of "${ORDERS[0]}")" "payment_pending"
kill -CONT "$PAY"
paid=0
for o in "${ORDERS[@]}"; do wait_status "$o" paid 8 && paid=$((paid + 1)); done
check "after resuming, all 12 are paid (10 waited in the kernel queue, the rest in the outbox)" is "$paid" "12"

# ---------------------------------------------------------------- C: gateway killed
echo "C. payment gateway killed with requests waiting in its queue"
PAY=$(pid_of of-pay)
kill -STOP "$PAY"
ORDERS=()
for _ in $(seq 3); do buy tok_success; ORDERS+=("$ORDER"); done
kill -9 "$PAY"
paid=0
for o in "${ORDERS[@]}"; do wait_status "$o" paid 8 && paid=$((paid + 1)); done
check "the restarted gateway answered the queued requests (3 paid)" is "$paid" "3"
check "the supervisor restarted of-pay" test "$(restarts of-pay)" -ge 1

# ---------------------------------------------------------------- D: gateway gone too long
echo "D. payment gateway gone longer than the reservation TTL"
req anon GET /products/3
STOCK0=$(json 'd["product"]["stock"]')
LATE0=$(metric late_refunds)
PAY=$(pid_of of-pay)
kill -STOP "$PAY"
buy tok_success; O1=$ORDER
buy tok_success; O2=$ORDER
check "unpaid orders are cancelled when the TTL runs out" wait_status "$O2" cancelled $((TTL + 5))
req anon GET /products/3
check "their stock is back" is "$(json 'd["product"]["stock"]')" "$STOCK0"
kill -CONT "$PAY"
for _ in $(seq 30); do
    req c GET "/orders/$O1"
    [[ $(json 'd["order"]["payment"]["status"]') == refunded ]] && break
    sleep 0.3
done
check "the late approvals are refunded automatically" is "$(json 'd["order"]["payment"]["status"]')" "refunded"
check "late refunds counted (+2)" is "$(( $(metric late_refunds) - LATE0 ))" "2"

# ---------------------------------------------------------------- F: other daemons die
echo "F. an of-api worker and of-report are killed"
API=$(pid_of of-api)
WORKER=$(pgrep -P "$API" | head -1)
kill -9 "$WORKER"
REP=$(pid_of of-report)
kill -9 "$REP"
ok=0
for _ in $(seq 20); do req anon GET /products/1; [[ $STATUS == 200 ]] && ok=$((ok + 1)); done
check "the API kept serving (20/20)" is "$ok" "20"
sleep 1
check "the supervisor restarted of-report" test "$(restarts of-report)" -ge 1

echo "   invariants after stages A-D, F"
"$BIN/of-ctl" -c "$FCONF" invariants > "$TMP/inv.txt" 2>&1
check "every invariant holds" is "$?" "0"
grep -E "FAIL" "$TMP/inv.txt" || true
stop_stack

# ---------------------------------------------------------------- E: job queue full
echo "E. job queue full (2 workers, 4-slot queue, 32 API workers)"
make_conf "core.workers = 2" "core.queue_size = 4" "api.workers = 32"
start_stack
"$BIN/of-bench" -c "$FCONF" oversell --buyers 400 --stock 50 --url "$URL" > "$TMP/bench.txt" 2>&1
BENCH_RC=$?
BUSY=$(sed -n 's/^ *busy (503) *//p' "$TMP/bench.txt")
OVER=$(sed -n 's/^ *OVERSOLD *//p' "$TMP/bench.txt")
check "overload is shed with 503 busy ($BUSY requests)" test "${BUSY:-0}" -gt 0
check "nothing oversold under overload" is "$OVER" "0"
check "the run passes its invariants" is "$BENCH_RC" "0"
check "of-core did not crash" is "$(restarts of-core)" "0"
stop_stack

echo
echo "failure injection: $PASS passed, $FAIL failed"
echo "(orders were added; reset with scripts/load-db.sh)"
[[ $FAIL -eq 0 ]]
