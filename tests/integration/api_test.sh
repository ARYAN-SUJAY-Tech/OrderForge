#!/usr/bin/env bash
# End-to-end API test through Apache: catalogue, auth, cart, checkout, orders, admin,
# and the security checks (CSRF, roles, ownership, lockout).
#
#   BASE=http://localhost:8080 ADMIN_EMAIL=... ADMIN_PASSWORD=... tests/integration/api_test.sh
#
# Needs: curl, python3, a running of-api + Apache, a freshly loaded database
# (scripts/load-db.sh) and the admin account it creates.
set -uo pipefail

BASE=${BASE:-http://localhost:8080}
ADMIN_EMAIL=${ADMIN_EMAIL:-admin@orderforge.local}
ADMIN_PASSWORD=${ADMIN_PASSWORD:-admin12345}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
PASS=0
FAIL=0
RUN_ID=$(date +%s)$$

# ---------------------------------------------------------------- helpers

# req NAME JAR METHOD PATH [BODY]  -> sets STATUS and BODY; NAME's CSRF token is sent
req() {
    local jar=$TMP/$1.jar csrf_file=$TMP/$1.csrf method=$2 path=$3 data=${4:-}
    local args=(-s -m 30 -o "$TMP/body" -w '%{http_code}' -b "$jar" -c "$jar" -X "$method"
                -D "$TMP/headers")
    [[ -f $csrf_file ]] && args+=(-H "X-CSRF-Token: $(cat "$csrf_file")")
    [[ -n $data ]] && args+=(-H 'Content-Type: application/json' --data-binary "$data")
    STATUS=$(curl "${args[@]}" "$BASE/api$path")
    BODY=$(cat "$TMP/body")
}

# Like req, but without the CSRF header.
req_nocsrf() {
    local jar=$TMP/$1.jar
    STATUS=$(curl -s -m 30 -o "$TMP/body" -w '%{http_code}' -b "$jar" -c "$jar" -X "$2" \
                  -H 'Content-Type: application/json' ${4:+--data-binary "$4"} "$BASE/api$3")
    BODY=$(cat "$TMP/body")
}

# json EXPR : evaluate a Python expression against the last body, bound to `d`
json() {
    python3 -c 'import json,sys; d=json.loads(sys.argv[1]); print(eval(sys.argv[2]))' "$BODY" "$1"
}

save_csrf() { json 'd["csrf_token"]' > "$TMP/$1.csrf"; }

check() { # check DESCRIPTION CONDITION...
    local desc=$1; shift
    if "$@"; then
        PASS=$((PASS + 1)); printf '  ok    %s\n' "$desc"
    else
        FAIL=$((FAIL + 1)); printf '  FAIL  %s  (status %s, body %.200s)\n' "$desc" "$STATUS" "$BODY"
    fi
}
is() { [[ "$1" == "$2" ]]; }

# ---------------------------------------------------------------- catalogue
echo "catalogue"
req anon GET /categories
check "categories listed" is "$STATUS $(json 'len(d["categories"]) > 0')" "200 True"
req anon GET '/products?per_page=5'
check "first page has 5 products" is "$STATUS $(json 'len(d["products"])')" "200 5"
TOTAL=$(json 'd["total"]')
req anon GET '/products?q=keyboard'
check "search finds the keyboard" is "$(json 'd["products"][0]["sku"]')" "HOT-001"
req anon GET '/products?category=2&per_page=50'
check "category filter" is "$(json 'all(p["category"] == "Books" for p in d["products"])')" "True"
req anon GET '/products?q=%25'
check "LIKE wildcards are escaped" is "$(json 'd["total"]')" "0"
req anon GET /products/1
check "product page" is "$STATUS $(json 'd["product"]["id"]')" "200 1"
req anon GET /products/999999
check "unknown product is 404" is "$STATUS" "404"
req anon GET /nope
check "unknown endpoint is 404" is "$STATUS" "404"
req anon DELETE /products
check "wrong method is 405" is "$STATUS" "405"

# ---------------------------------------------------------------- registration and login
echo "auth"
EMAIL="buyer$RUN_ID@example.com"
req alice POST /auth/register "{\"email\":\"$EMAIL\",\"password\":\"buyerpass1\",\"name\":\"Buyer\"}"
check "register" is "$STATUS $(json 'd["user"]["role"]')" "201 customer"
save_csrf alice
check "session cookie is HttpOnly + SameSite=Strict" \
    grep -qi 'set-cookie: of_session=[0-9a-f]\{64\};.*HttpOnly; SameSite=Strict' "$TMP/headers"
req bob POST /auth/register "{\"email\":\"$EMAIL\",\"password\":\"buyerpass1\",\"name\":\"Dup\"}"
check "duplicate email is 409" is "$STATUS" "409"
req bob POST /auth/register '{"email":"x@example.com","password":"short","name":"X"}'
check "short password is 400" is "$STATUS" "400"
req bob POST /auth/register '{"email":"not-an-email","password":"longenough","name":"X"}'
check "bad email is 400" is "$STATUS" "400"
req bob POST /auth/register '{"email":'
check "malformed JSON is 400" is "$STATUS" "400"
req alice GET /me
check "me with session" is "$STATUS $(json 'd["user"]["email"]')" "200 $EMAIL"
req anon GET /me
check "me without session is 401" is "$STATUS" "401"

# ---------------------------------------------------------------- cart and checkout
echo "cart and checkout"
req_nocsrf alice POST /cart/items '{"product_id":2,"qty":1}'
check "cart change without CSRF token is 403" is "$STATUS" "403"
req alice POST /cart/items '{"product_id":2,"qty":2}'
check "add to cart" is "$STATUS $(json 'd["units"]')" "200 2"
req alice POST /cart/items '{"product_id":3,"qty":1}'
req alice POST /cart/items '{"product_id":2,"qty":3}'
check "set quantity" is "$(json '[(i["product_id"], i["qty"]) for i in d["items"]]')" "[(2, 3), (3, 1)]"
req alice DELETE /cart/items/3
check "remove line" is "$(json 'd["units"]')" "3"
req alice POST /cart/items '{"product_id":2,"qty":101}'
check "qty above 100 rejected" is "$STATUS" "400"
req alice POST /cart/items '{"product_id":2,"qty":1,"price_paise":1}'
check "client price is ignored" is "$(json 'd["items"][0]["price_paise"]')" "349900"
req alice POST /cart/items '{"product_id":2,"qty":3}'

req anon GET /products/2
STOCK_BEFORE=$(json 'd["product"]["stock"]')
req alice POST /checkout
check "checkout" is "$STATUS $(json 'd["order"]["status"]')" "201 placed"
ORDER=$(json 'd["order"]["id"]')
check "order total is server-computed" is "$(json 'd["order"]["total_paise"]')" "1049700"
check "order has a timeline event" is "$(json 'd["order"]["events"][0]["to"]')" "placed"
req anon GET /products/2
check "stock went down by 3" is "$(json 'd["product"]["stock"]')" "$((STOCK_BEFORE - 3))"
req alice GET /cart
check "cart emptied after checkout" is "$(json 'd["units"]')" "0"
req alice POST /checkout
check "empty cart cannot check out" is "$STATUS" "400"
req alice GET /orders
check "order history lists it" is "$(json 'd["orders"][0]["id"]')" "$ORDER"
req alice GET "/orders/$ORDER"
check "order detail" is "$STATUS $(json 'len(d["order"]["items"])')" "200 1"

# A second customer cannot see the first one's order.
req carol POST /auth/register "{\"email\":\"carol$RUN_ID@example.com\",\"password\":\"carolpass1\",\"name\":\"Carol\"}"
save_csrf carol
req carol GET "/orders/$ORDER"
check "another customer's order is 404" is "$STATUS" "404"
req carol GET /admin/products
check "customer cannot use admin API" is "$STATUS" "403"

# ---------------------------------------------------------------- admin
echo "admin"
req admin POST /auth/login "{\"email\":\"$ADMIN_EMAIL\",\"password\":\"$ADMIN_PASSWORD\"}"
check "admin login" is "$STATUS $(json 'd["user"]["role"]')" "200 admin"
save_csrf admin
SKU="TST-$RUN_ID"
SKU=${SKU:0:32}
req admin POST /admin/products "{\"sku\":\"$SKU\",\"name\":\"Test Widget\",\"description\":\"made by api_test\",\"price_paise\":12345,\"category_id\":1,\"stock\":2,\"reorder_level\":1}"
check "create product" is "$STATUS $(json 'd["product"]["stock"]')" "201 2"
PID=$(json 'd["product"]["id"]')
req admin POST /admin/products "{\"sku\":\"$SKU\",\"name\":\"Again\",\"price_paise\":1,\"category_id\":1}"
check "duplicate sku is 409" is "$STATUS" "409"
req admin POST /admin/products '{"sku":"bad sku","name":"x","price_paise":1,"category_id":1}'
check "invalid sku is 400" is "$STATUS" "400"
req admin PUT "/admin/products/$PID" '{"price_paise":20000,"name":"Test Widget v2"}'
check "update product" is "$(json 'd["product"]["price_paise"]') $(json 'd["product"]["name"]')" "20000 Test Widget v2"
req admin POST "/admin/products/$PID/stock" '{"delta":5,"reason":"restock"}'
check "restock" is "$(json 'd["product"]["stock"]')" "7"
req admin POST "/admin/products/$PID/stock" '{"delta":-1000,"reason":"adjust"}'
check "stock cannot go negative" is "$STATUS" "409"
req admin POST "/admin/products/$PID/stock" '{"delta":-6,"reason":"adjust"}'
check "adjust down" is "$(json 'd["product"]["stock"]')" "1"

# Out of stock: carol wants 2, only 1 left.
req carol POST /cart/items "{\"product_id\":$PID,\"qty\":2}"
req carol POST /checkout
check "checkout beyond stock is 409 out_of_stock" is "$STATUS $(json 'd["error"]["code"]')" "409 out_of_stock"
req anon GET "/products/$PID"
check "failed checkout left stock untouched" is "$(json 'd["product"]["stock"]')" "1"

req admin DELETE "/admin/products/$PID"
check "soft delete" is "$(json 'd["product"]["active"]')" "False"
req anon GET "/products/$PID"
check "deleted product hidden from shop" is "$STATUS" "404"
req carol GET /cart
check "deleted product removed from carts" is "$(json 'd["units"]')" "0"
req anon GET '/products?per_page=50'
check "catalogue count unchanged" is "$(json 'd["total"]')" "$TOTAL"
req admin PUT "/admin/products/$PID" '{"active":true}'
check "restore product" is "$(json 'd["product"]["active"]')" "True"
req admin GET "/orders/$ORDER"
check "admin can view any order" is "$STATUS" "200"
req admin GET '/admin/orders?status=placed'
check "order board" is "$STATUS $(json 'all(o["status"] == "placed" for o in d["orders"])')" "200 True"
req admin GET '/admin/products?low_stock=1'
check "low-stock filter" is "$(json "any(p['id'] == $PID for p in d['products'])")" "True"
req admin DELETE "/admin/products/$PID"

# ---------------------------------------------------------------- lockout and logout
echo "lockout and logout"
for _ in 1 2 3 4 5; do
    req mallory POST /auth/login "{\"email\":\"carol$RUN_ID@example.com\",\"password\":\"wrong-password\"}"
done
check "5th wrong password still 401" is "$STATUS" "401"
req mallory POST /auth/login "{\"email\":\"carol$RUN_ID@example.com\",\"password\":\"carolpass1\"}"
check "account locked after 5 failures (even with the right password)" is "$STATUS" "429"
req mallory POST /auth/login '{"email":"nobody@example.com","password":"whatever1"}'
check "unknown email gives the same 401" is "$STATUS $(json 'd["error"]["code"]')" "401 invalid_credentials"

req alice POST /auth/logout
check "logout" is "$STATUS" "200"
req alice GET /me
check "session gone after logout" is "$STATUS" "401"

echo
echo "integration: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
