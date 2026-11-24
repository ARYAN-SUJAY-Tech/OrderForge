#!/usr/bin/env bash
# Payments, the order state machine and reports, end to end through Apache.
#
# Covers every gateway outcome (approve, decline, timeout, duplicate reply, late reply
# after expiry), idempotent checkout, customer cancel and refund, the admin fulfilment
# flow, illegal transitions, stock returns, and the reports API.
#
#   BASE=http://localhost:8080 TTL=4 tests/integration/payment_test.sh
#
# Needs all daemons running (scripts/run.sh) and, because it waits for reservations to
# expire, a short reservation TTL in the config the core runs with:
#     store.reservation_ttl_s = 4      (TTL below must match)
#     pay.slow_ms = 7000               (longer than the TTL: drives the late-success case)
#     core.reaper_interval_ms = 500
#     report.interval_s = 2
set -uo pipefail

BASE=${BASE:-http://localhost:8080}
TTL=${TTL:-4}
ADMIN_EMAIL=${ADMIN_EMAIL:-admin@orderforge.local}
ADMIN_PASSWORD=${ADMIN_PASSWORD:-admin12345}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
PASS=0
FAIL=0
RUN_ID=$(date +%s)$$

req() { # req WHO METHOD PATH [BODY] [IDEMPOTENCY-KEY]
    local jar=$TMP/$1.jar csrf=$TMP/$1.csrf
    local args=(-s -m 30 -o "$TMP/body" -w '%{http_code}' -b "$jar" -c "$jar" -X "$2")
    [[ -f $csrf ]] && args+=(-H "X-CSRF-Token: $(cat "$csrf")")
    [[ -n ${4:-} ]] && args+=(-H 'Content-Type: application/json' --data-binary "$4")
    [[ -n ${5:-} ]] && args+=(-H "Idempotency-Key: $5")
    STATUS=$(curl "${args[@]}" "$BASE/api$3")
    BODY=$(cat "$TMP/body")
}
json() { python3 -c 'import json,sys; d=json.loads(sys.argv[1]); print(eval(sys.argv[2]))' "$BODY" "$1"; }
check() {
    local desc=$1; shift
    if "$@"; then PASS=$((PASS + 1)); printf '  ok    %s\n' "$desc"
    else FAIL=$((FAIL + 1)); printf '  FAIL  %s  (status %s, body %.240s)\n' "$desc" "$STATUS" "$BODY"; fi
}
is() { [[ "$1" == "$2" ]]; }

stock_of() { req anon GET "/products/$1"; json 'd["product"]["stock"]'; }

# buy WHO PRODUCT QTY METHOD [KEY] -> sets ORDER (and STATUS/BODY)
buy() {
    req "$1" POST /cart/items "{\"product_id\":$2,\"qty\":$3}"
    req "$1" POST /checkout "{\"payment_method\":\"$4\"}" "${5:-}"
    ORDER=$(json 'd["order"]["id"]' 2>/dev/null)
}

# wait_status WHO ORDER STATE SECONDS -> 0 when the order reaches STATE in time
wait_status() {
    local deadline=$((SECONDS + $4))
    while (( SECONDS <= deadline )); do
        req "$1" GET "/orders/$2"
        [[ $(json 'd["order"]["status"]') == "$3" ]] && return 0
        sleep 0.3
    done
    return 1
}

metric() { req admin GET /admin/system; json "d['metrics']['$1']"; }

# ---------------------------------------------------------------- setup
# Check first (before creating anything): the expiry checks only make sense against a
# core running with the short test TTL.
req admin POST /auth/login "{\"email\":\"$ADMIN_EMAIL\",\"password\":\"$ADMIN_PASSWORD\"}"
json 'd["csrf_token"]' > "$TMP/admin.csrf"
req admin GET /admin/system
CORE_TTL=$(json 'd.get("reservation_ttl_s", "?")' 2>/dev/null)
if [[ $CORE_TTL != "$TTL" ]]; then
    echo "the running of-core holds reservations for ${CORE_TTL:-?} s, but this test expects $TTL s." >&2
    echo "start the stack with the test timings:  scripts/make-test-conf.sh && CONF=conf/test.conf scripts/run.sh" >&2
    echo "(stop any stack that is already running first)" >&2
    exit 2
fi

req alice POST /auth/register "{\"email\":\"pay$RUN_ID@example.com\",\"password\":\"paytest123\",\"name\":\"Payer\"}"
json 'd["csrf_token"]' > "$TMP/alice.csrf"
req bob POST /auth/register "{\"email\":\"other$RUN_ID@example.com\",\"password\":\"paytest123\",\"name\":\"Other\"}"
json 'd["csrf_token"]' > "$TMP/bob.csrf"
P=3 # USB-C hub: plenty of stock


echo "gateway outcomes"
before=$(stock_of $P)
buy alice $P 2 tok_success
check "checkout returns payment_pending" is "$STATUS $(json 'd["order"]["status"]')" "201 payment_pending"
check "order shows a reservation countdown" is "$(json '"reservation_seconds_left" in d["order"]')" "True"
check "approved payment: order becomes paid" wait_status alice "$ORDER" paid 5
check "payment row succeeded with a provider reference" \
    is "$(json 'd["order"]["payment"]["status"]') $(json 'd["order"]["payment"]["provider_ref"].startswith("mockpay_")')" "succeeded True"
check "paid order holds its stock" is "$(stock_of $P)" "$((before - 2))"
PAID=$ORDER

before=$(stock_of $P)
buy alice $P 1 tok_decline
check "declined payment: order cancelled" wait_status alice "$ORDER" cancelled 5
check "payment row declined" is "$(json 'd["order"]["payment"]["status"]')" "declined"
check "declined order's stock returned" is "$(stock_of $P)" "$before"

before=$(stock_of $P)
buy alice $P 1 tok_timeout
TIMEOUT_ORDER=$ORDER
req alice GET "/orders/$ORDER"
check "silent gateway: still waiting before the TTL" is "$(json 'd["order"]["status"]')" "payment_pending"
check "silent gateway: reaper cancels after the TTL" wait_status alice "$ORDER" cancelled $((TTL + 4))
check "payment row timed_out" is "$(json 'd["order"]["payment"]["status"]')" "timed_out"
check "expired order's stock returned" is "$(stock_of $P)" "$before"

ignored_before=$(metric payments_ignored)
buy alice $P 1 tok_duplicate
check "duplicate callback: order paid" wait_status alice "$ORDER" paid 5
sleep 0.5
req alice GET "/orders/$ORDER"
check "duplicate callback: exactly one 'paid' event" is "$(json 'sum(e["to"] == "paid" for e in d["order"]["events"])')" "1"
check "duplicate callback: second reply counted as ignored" \
    is "$(( $(metric payments_ignored) > ignored_before ))" "1"

before=$(stock_of $P)
late_before=$(metric late_refunds)
buy alice $P 1 tok_slow
SLOW=$ORDER
check "slow gateway: order expires first" wait_status alice "$SLOW" cancelled $((TTL + 4))
echo "        (waiting for the late approval to arrive...)"
deadline=$((SECONDS + 10))
while (( SECONDS < deadline )); do
    req alice GET "/orders/$SLOW"
    [[ $(json 'd["order"]["payment"]["status"]') == refunded ]] && break
    sleep 0.5
done
check "late success after expiry: refunded automatically" is "$(json 'd["order"]["payment"]["status"]')" "refunded"
check "late success: order stays cancelled" is "$(json 'd["order"]["status"]')" "cancelled"
check "late success: noted on the timeline" is "$(json 'any("refunded automatically" in e["note"] for e in d["order"]["events"])')" "True"
check "late success: stock not returned twice" is "$(stock_of $P)" "$before"
check "late refund counted" is "$(( $(metric late_refunds) > late_before ))" "1"

echo "idempotent checkout"
KEY="retry-$RUN_ID"
buy alice $P 1 tok_success "$KEY"
FIRST=$ORDER
check "first attempt creates the order" is "$STATUS" "201"
req alice POST /checkout '{"payment_method":"tok_success"}' "$KEY"
check "retry with the same key returns the same order" is "$STATUS $(json 'd["order"]["id"]')" "200 $FIRST"
req bob POST /cart/items "{\"product_id\":$P,\"qty\":1}"
req bob POST /checkout '{}' "$KEY"
check "same key from another user is a different order" is "$STATUS" "201"
req alice POST /checkout '{}' "bad key!"
check "malformed Idempotency-Key is 400" is "$STATUS" "400"
req alice POST /checkout '{"payment_method":"tok_steal"}'
check "unknown payment method is 400" is "$STATUS" "400"
wait_status alice "$FIRST" paid 5

echo "customer cancel and refund"
before=$(stock_of $P)
buy alice $P 1 tok_timeout
req alice POST "/orders/$ORDER/cancel"
check "customer cancels an unpaid order" is "$STATUS $(json 'd["order"]["status"]')" "200 cancelled"
check "its payment is marked cancelled" is "$(json 'd["order"]["payment"]["status"]')" "cancelled"
check "its stock is back" is "$(stock_of $P)" "$before"
req alice POST "/orders/$ORDER/cancel"
check "cancelling twice is 409" is "$STATUS" "409"
req bob POST "/orders/$PAID/cancel"
check "cannot cancel someone else's order (404)" is "$STATUS" "404"

before=$(stock_of $P)
req alice POST "/orders/$FIRST/cancel"
check "customer cancelling a paid order gets a refund" is "$STATUS $(json 'd["order"]["status"]')" "200 refunded"
check "payment refunded" is "$(json 'd["order"]["payment"]["status"]')" "refunded"
check "refunded stock is back" is "$(stock_of $P)" "$((before + 1))"

echo "admin fulfilment"
req alice POST "/admin/orders/$PAID/status" '{"status":"packed"}'
check "customer cannot use the admin endpoint" is "$STATUS" "403"
req admin POST "/admin/orders/$PAID/status" '{"status":"delivered"}'
check "paid -> delivered skips steps: 409" is "$STATUS $(json 'd["error"]["code"]')" "409 invalid_transition"
req admin POST "/admin/orders/$PAID/status" '{"status":"paid"}'
check "nobody but the gateway marks an order paid" is "$STATUS" "409"
req admin POST "/admin/orders/$PAID/status" '{"status":"packed","note":"boxed at bench 3"}'
check "pack" is "$STATUS $(json 'd["order"]["status"]')" "200 packed"
req alice POST "/orders/$PAID/cancel"
check "customer can no longer cancel a packed order" is "$STATUS" "409"
req admin POST "/admin/orders/$PAID/status" '{"status":"shipped"}'
check "ship" is "$(json 'd["order"]["status"]')" "shipped"
req admin POST "/admin/orders/$PAID/status" '{"status":"refunded"}'
check "shipped orders cannot be refunded" is "$STATUS" "409"
req admin POST "/admin/orders/$PAID/status" '{"status":"delivered"}'
check "deliver" is "$(json 'd["order"]["status"]')" "delivered"
check "timeline has every step" is "$(json '[e["to"] for e in d["order"]["events"]]')" \
    "['placed', 'payment_pending', 'paid', 'packed', 'shipped', 'delivered']"
check "admin note is on the timeline" is "$(json 'any(e["note"] == "boxed at bench 3" for e in d["order"]["events"])')" "True"
req admin POST "/admin/orders/$TIMEOUT_ORDER/status" '{"status":"packed"}'
check "cancelled orders are final" is "$STATUS" "409"

before=$(stock_of $P)
buy alice $P 2 tok_success
wait_status alice "$ORDER" paid 5
req admin POST "/admin/orders/$ORDER/status" '{"status":"packed"}'
req admin POST "/admin/orders/$ORDER/status" '{"status":"refunded","note":"damaged in packing"}'
check "admin refunds a packed order" is "$STATUS $(json 'd["order"]["status"]')" "200 refunded"
check "admin refund returns the stock" is "$(stock_of $P)" "$before"

echo "reports"
sleep 2.5 # one of-report cycle
req admin GET '/admin/reports?days=7'
check "reports respond" is "$STATUS" "200"
check "of-report has run" is "$(json '"generated_at" in d')" "True"
check "today's revenue is counted" is "$(json 'd["totals"]["revenue_paise"] > 0')" "True"
check "top products listed" is "$(json 'len(d["top_products"]) > 0')" "True"
check "order status counts include delivered" is "$(json 'd["order_status"].get("delivered", 0) >= 1')" "True"
check "payment outcomes counted" is "$(json 'all(k in d["payments"] for k in ("succeeded","declined","timed_out","refunded"))')" "True"
req admin GET '/admin/reports/export?name=daily_sales'
check "CSV export" is "$STATUS $(json 'd["csv"].splitlines()[0]')" "200 day,orders,units,revenue_rupees"
req admin GET '/admin/reports/export?name=../../etc/passwd'
check "export refuses anything but the four reports" is "$STATUS" "400"
req alice GET '/admin/reports'
check "customers cannot read reports" is "$STATUS" "403"

echo
echo "payments: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
