#!/usr/bin/env bash
# Security checks through the whole stack (Apache -> of-api -> of-core):
# headers, cross-site request defences, input limits, injection attempts, the payment
# channel against forged / stale / replayed messages, and the rate limits.
#
#   BASE=http://localhost:8080 CONF=conf/orderforge.conf tests/integration/security_test.sh
#
# Needs the stack running (scripts/run.sh). Run it LAST in a test session: the final
# section deliberately exhausts the login rate limit for this IP (it refills within a
# minute).
set -uo pipefail
cd "$(dirname "$0")/../.."

BASE=${BASE:-http://localhost:8080}
CONF=${CONF:-conf/orderforge.conf}
BIN=${BIN:-build/debug}
ADMIN_EMAIL=${ADMIN_EMAIL:-admin@orderforge.local}
ADMIN_PASSWORD=${ADMIN_PASSWORD:-admin12345}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
PASS=0
FAIL=0
RUN_ID=$(date +%s)$$

req() { # req WHO METHOD PATH [BODY] -> STATUS, BODY, headers in $TMP/headers
    local jar=$TMP/$1.jar csrf=$TMP/$1.csrf
    local args=(-s -m 30 -o "$TMP/body" -D "$TMP/headers" -w '%{http_code}' -b "$jar" -c "$jar" -X "$2")
    [[ -f $csrf ]] && args+=(-H "X-CSRF-Token: $(cat "$csrf")")
    [[ -n ${4:-} ]] && args+=(-H 'Content-Type: application/json' --data-binary "$4")
    STATUS=$(curl "${args[@]}" "${EXTRA[@]}" "$BASE/api$3")
    BODY=$(cat "$TMP/body")
}
EXTRA=()
json() { python3 -c 'import json,sys; d=json.loads(sys.argv[1]); print(eval(sys.argv[2]))' "$BODY" "$1"; }
check() {
    local desc=$1; shift
    if "$@"; then PASS=$((PASS + 1)); printf '  ok    %s\n' "$desc"
    else FAIL=$((FAIL + 1)); printf '  FAIL  %s  (status %s, body %.200s)\n' "$desc" "$STATUS" "$BODY"; fi
}
is() { [[ "$1" == "$2" ]]; }
has_header() { grep -qi "^$1" "$TMP/headers"; }

req admin POST /auth/login "{\"email\":\"$ADMIN_EMAIL\",\"password\":\"$ADMIN_PASSWORD\"}"
json 'd["csrf_token"]' > "$TMP/admin.csrf"
req eve POST /auth/register "{\"email\":\"eve$RUN_ID@example.com\",\"password\":\"evetest123\",\"name\":\"Eve <script>alert(1)</script>\"}"
json 'd["csrf_token"]' > "$TMP/eve.csrf"

echo "headers"
curl -s -m 10 -D "$TMP/headers" -o /dev/null "$BASE/"
check "page: Content-Security-Policy" has_header "Content-Security-Policy: default-src 'self'"
check "page: X-Frame-Options DENY" has_header "X-Frame-Options: DENY"
check "page: X-Content-Type-Options nosniff" has_header "X-Content-Type-Options: nosniff"
req eve GET /me
check "API: Cache-Control no-store" has_header "Cache-Control: no-store"
check "user-supplied markup is escaped in JSON (\\u003c)" grep -q 'Eve \\u003cscript' "$TMP/body"

echo "cross-site requests"
STATUS=$(curl -s -m 10 -o "$TMP/body" -w '%{http_code}' -X POST -H 'Content-Type: text/plain' \
    --data-binary '{"email":"eve@example.com","password":"x","pad":"="}' "$BASE/api/auth/login")
BODY=$(cat "$TMP/body")
check "login CSRF via a text/plain form is refused (415)" is "$STATUS" "415"
STATUS=$(curl -s -m 10 -o "$TMP/body" -w '%{http_code}' -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' --data 'email=a&password=b' "$BASE/api/auth/login")
check "form-encoded bodies are refused (415)" is "$STATUS" "415"
EXTRA=(-H "Origin: http://evil.example")
req eve POST /cart/items '{"product_id":2,"qty":1}'
check "POST from another Origin is refused (403)" is "$STATUS $(json 'd["error"]["code"]')" "403 cross_origin"
EXTRA=(-H "Origin: null")
req eve POST /cart/items '{"product_id":2,"qty":1}'
check "Origin: null is refused (403)" is "$STATUS" "403"
HOSTPORT=${BASE#*://}
EXTRA=(-H "Origin: http://$HOSTPORT")
req eve POST /cart/items '{"product_id":2,"qty":1}'
check "same-origin POST is accepted" is "$STATUS" "200"
EXTRA=(-H "Origin: http://evil.example")
req eve GET /cart
check "GET with a foreign Origin is still allowed (no side effects)" is "$STATUS" "200"
EXTRA=()
rm -f "$TMP/eve.csrf.bak"; cp "$TMP/eve.csrf" "$TMP/eve.csrf.bak"; rm "$TMP/eve.csrf"
req eve POST /cart/items '{"product_id":2,"qty":1}'
check "state change without the CSRF token is refused (403)" is "$STATUS $(json 'd["error"]["code"]')" "403 csrf"
mv "$TMP/eve.csrf.bak" "$TMP/eve.csrf"

echo "input limits and injection"
BIG=$(python3 -c 'print("{\"q\":\"" + "x"*20000 + "\"}")')
req eve POST /cart/items "$BIG"
check "body over 16 KiB is refused (413)" is "$STATUS" "413"
LONGQ=$(python3 -c 'print("a"*3000)')
req anon GET "/products?q=$LONGQ"
check "query string over 2 KiB is refused (400)" is "$STATUS" "400"
req anon GET "/products?q=%27%20OR%201%3D1%20--%20"
check "SQL injection in search is just text (0 results)" is "$STATUS $(json 'd["total"]')" "200 0"
req anon GET "/products/1%20OR%201=1"
check "SQL injection in an id is not a route (404)" is "$STATUS" "404"
req eve POST /cart/items '{"product_id":2,"qty":1,"qty":50}'
check "duplicate JSON keys are refused (400)" is "$STATUS" "400"
req eve POST /cart/items '{"product_id":2,"qty":{"$gt":0}}'
check "nested JSON values are refused (400)" is "$STATUS" "400"
req eve POST /admin/products '{"sku":"X-1","name":"x","price_paise":1,"category_id":1}'
check "customer cannot reach admin endpoints (403)" is "$STATUS" "403"

echo "payment channel (forged, stale and replayed gateway replies)"
req eve POST /cart/items '{"product_id":3,"qty":1}'
req eve POST /checkout '{"payment_method":"tok_timeout"}'
ORDER=$(json 'd["order"]["id"]')
req admin GET /admin/system
REJ0=$(json 'd["metrics"]["payments_rejected"]')
"$BIN/of-bench" -c "$CONF" inject "$ORDER" > "$TMP/inject.txt" 2>&1
INJECT_RC=$?
sleep 1
req eve GET "/orders/$ORDER"
check "injection tool ran" is "$INJECT_RC" "0"
check "only the genuine signed reply took effect (order paid)" is "$(json 'd["order"]["status"]')" "paid"
check "it was the genuine one, not the forged one" is "$(json 'd["order"]["payment"]["provider_ref"]')" "inject_valid"
check "exactly one 'paid' event" is "$(json 'sum(e["to"] == "paid" for e in d["order"]["events"])')" "1"
req admin GET /admin/system
check "forged, stale and replayed messages all rejected (+3)" is "$(( $(json 'd["metrics"]["payments_rejected"]') - REJ0 ))" "3"

echo "metrics endpoint"
STATUS=$(curl -s -m 10 -o "$TMP/body" -D "$TMP/headers" -w '%{http_code}' "$BASE/api/metrics")
check "served to localhost as Prometheus text" has_header "Content-Type: text/plain; version=0.0.4"
check "includes rate-limit counters" grep -q '^orderforge_ratelimit_limited_total' "$TMP/body"

echo "rate limits"
for i in $(seq 120); do
    curl -s -m 10 -o /dev/null -D "$TMP/h$i" -w '%{http_code}\n' -b "$TMP/eve.jar" "$BASE/api/cart" &
done > "$TMP/codes"
wait
LIMITED=$(grep -c '^429$' "$TMP/codes")
check "one session hammering the API gets 429s ($LIMITED of 120)" test "$LIMITED" -gt 0
check "429 carries Retry-After" grep -qil '^Retry-After:' "$TMP"/h*
for i in $(seq 45); do
    curl -s -m 10 -o /dev/null -w '%{http_code}\n' -X POST -H 'Content-Type: application/json' \
        --data-binary "{\"email\":\"guess$i@example.com\",\"password\":\"wrong-password\"}" \
        "$BASE/api/auth/login" &
done > "$TMP/auth_codes"
wait
AUTH_LIMITED=$(grep -c '^429$' "$TMP/auth_codes")
check "password guessing from one IP is throttled ($AUTH_LIMITED of 45 got 429)" test "$AUTH_LIMITED" -gt 0

echo
echo "security: $PASS passed, $FAIL failed"
echo "(the login limit for this IP refills within a minute)"
[[ $FAIL -eq 0 ]]
