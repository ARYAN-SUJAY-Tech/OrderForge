#!/usr/bin/env bash
# Oversell demonstration for the phase-1 naive checkout.
#
# Sets the hot SKU (product 1, HOT-001) to STOCK units, gives BUYERS customers one unit
# each in their cart, then fires every checkout at the same moment. With a correct
# stock path exactly STOCK orders succeed; the naive read-modify-write lets more
# through. Afterwards run `of-ctl invariants` to see the ledger disagree.
#
#   BASE=http://localhost:8080 STOCK=10 BUYERS=40 tests/integration/naive_oversell.sh
#
# This is a quick demo; the real experiment (1,000 buyers, three stock modes) is
# of-bench in phase 2. Reload the database afterwards: scripts/load-db.sh
set -uo pipefail

BASE=${BASE:-http://localhost:8080}
ADMIN_EMAIL=${ADMIN_EMAIL:-admin@orderforge.local}
ADMIN_PASSWORD=${ADMIN_PASSWORD:-admin12345}
STOCK=${STOCK:-10}
BUYERS=${BUYERS:-40}
PRODUCT=1

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
RUN_ID=$(date +%s)$$

jget() { python3 -c 'import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))' "$1"; }

# post JAR PATH BODY [CSRF] -> prints the response body
post() {
    curl -s -m 30 -b "$1" -c "$1" -H 'Content-Type: application/json' ${4:+-H "X-CSRF-Token: $4"} \
         --data-binary "$3" "$BASE/api$2"
}

echo "== setting $PRODUCT (HOT-001) to $STOCK units"
ADMIN_CSRF=$(post "$TMP/admin" /auth/login \
    "{\"email\":\"$ADMIN_EMAIL\",\"password\":\"$ADMIN_PASSWORD\"}" | jget 'd["csrf_token"]')
CURRENT=$(curl -s -m 30 "$BASE/api/products/$PRODUCT" | jget 'd["product"]["stock"]')
DELTA=$((STOCK - CURRENT))
if [[ $DELTA -ne 0 ]]; then
    post "$TMP/admin" "/admin/products/$PRODUCT/stock" "{\"delta\":$DELTA,\"reason\":\"adjust\"}" \
         "$ADMIN_CSRF" >/dev/null
fi

echo "== registering $BUYERS buyers, one unit each in the cart"
for i in $(seq "$BUYERS"); do
    (
        jar=$TMP/b$i
        csrf=$(post "$jar" /auth/register \
            "{\"email\":\"rush$RUN_ID-$i@example.com\",\"password\":\"rushpass1\",\"name\":\"Rush $i\"}" |
            jget 'd["csrf_token"]')
        echo "$csrf" > "$jar.csrf"
        post "$jar" /cart/items "{\"product_id\":$PRODUCT,\"qty\":1}" "$csrf" >/dev/null
    ) &
done
wait

echo "== $BUYERS simultaneous checkouts"
for i in $(seq "$BUYERS"); do
    curl -s -m 30 -o /dev/null -w '%{http_code}\n' -b "$TMP/b$i" -X POST \
         -H "X-CSRF-Token: $(cat "$TMP/b$i.csrf")" "$BASE/api/checkout" > "$TMP/status$i" &
done
wait

OK=$(cat "$TMP"/status* | grep -c '^201$')
SOLDOUT=$(cat "$TMP"/status* | grep -c '^409$')
LEFT=$(curl -s -m 30 "$BASE/api/products/$PRODUCT" | jget 'd["product"]["stock"]')

echo
echo "stock before:        $STOCK"
echo "successful orders:   $OK"
echo "rejected (409):      $SOLDOUT"
echo "stock_on_hand after: $LEFT"
if [[ $OK -gt $STOCK ]]; then
    echo "OVERSOLD by $((OK - STOCK)) units: the naive read-modify-write lost updates"
else
    echo "no oversell this run (try more BUYERS, or store.naive_delay_us = 2000)"
fi
echo "now run: ./build/debug/of-ctl invariants"
