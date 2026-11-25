# API reference

All endpoints live under `/api/` on the Apache site (`http://localhost:8080` by
default). Request and response bodies are JSON. Every example below is a real response
from a running stack, shortened with `…` where it repeats.

## Rules for every request

- **Sessions:** login and registration set an `of_session` cookie (HttpOnly,
  SameSite=Strict) and return a `csrf_token` in the body.
- **CSRF:** every request that needs a session and is not a GET must send
  `X-CSRF-Token: <csrf_token>`.
- **Bodies must be `Content-Type: application/json`**, otherwise 415. This blocks
  cross-site HTML forms.
- **Cross-site requests:** a state-changing request whose `Origin` header names another
  site, or is `null`, gets 403 `cross_origin`. Browsers on the shop's own pages and
  tools that send no `Origin` (curl) are unaffected.
- **Size caps:** path 1 KiB, query string 2 KiB, body 16 KiB, `Idempotency-Key`
  64 characters.
- **Rate limits** (429 `rate_limited`, with a `Retry-After` header in seconds):

  | Limit | Rate | Burst |
  |---|---|---|
  | per client IP | 500/s | 2,000 |
  | per session | 20/s | 40 |
  | login and registration, per IP | 30/min | 30 |

  All of these are set by the `api.rate_*` keys in the config.
- **Money:** all amounts are integer paise (₹1 = 100 paise).
- **Times:** ISO 8601, UTC.

## Errors

Every error has the same shape:

```json
{"error": {"code": "out_of_stock", "message": "only 50 left of Limited Edition Mechanical Keyboard"}}
```

| Status | `code` | Meaning |
|---|---|---|
| 400 | `bad_request`, `invalid_<field>` (for example `invalid_price`), `weak_password`, `cart_empty`, `invalid_payment_method` | The request is malformed or a field is out of range |
| 401 | `unauthenticated`, `invalid_credentials` | Not logged in, or wrong email or password |
| 403 | `csrf` | `X-CSRF-Token` missing or wrong |
| 403 | `forbidden` | The endpoint needs an admin |
| 403 | `cross_origin` | `Origin` names another site |
| 404 | `not_found` | No such resource, or it belongs to someone else |
| 405 | `method_not_allowed` | Wrong method for this path |
| 409 | `out_of_stock`, `unavailable`, `insufficient_stock`, `invalid_transition`, `not_cancellable`, `email_taken`, `sku_taken`, `conflict` | The request conflicts with the current state (`conflict`: retry with the same key) |
| 413 | `too_large` | Body, path or query over its cap |
| 415 | `unsupported_media_type` | A body that is not `application/json` |
| 429 | `account_locked` | Too many failed logins for this account. The lock starts at 30 s and doubles per failure, up to 1 h |
| 429 | `rate_limited` | Over a rate limit; see `Retry-After` |
| 500 | `internal` | An unexpected server error (logged) |
| 502 | `core_error` | of-core did not answer, or sent a malformed reply |
| 503 | `busy`, `timeout` | of-core's job queue is full, or the job ran past `core.job_timeout_ms`; retry |
| 503 | `durability` | The write-ahead log cannot be written, so no order is accepted |
| 503 | `core_unavailable`, `shutting_down` | of-core is not running, or is restarting |
| 503 | `db_unavailable` | MySQL is down |

## Endpoint summary

| Method | Path | Access | Purpose |
|---|---|---|---|
| POST | `/api/auth/register` | public | Create a customer account and log in |
| POST | `/api/auth/login` | public | Log in |
| POST | `/api/auth/logout` | user | Log out |
| GET | `/api/me` | user | The current user and CSRF token |
| GET | `/api/categories` | public | Categories with product counts |
| GET | `/api/products` | public | Search and list products |
| GET | `/api/products/{id}` | public | One product |
| GET | `/api/cart` | user | The cart |
| POST | `/api/cart/items` | user | Set a line's quantity (0 removes it) |
| DELETE | `/api/cart/items/{product_id}` | user | Remove a line |
| POST | `/api/checkout` | user | Place an order from the cart |
| GET | `/api/orders` | user | Your orders |
| GET | `/api/orders/{id}` | user | One order with items, payment and timeline |
| POST | `/api/orders/{id}/cancel` | user | Cancel, or refund if paid and not yet packed |
| GET | `/api/admin/products` | admin | Every product, including deleted ones |
| POST | `/api/admin/products` | admin | Create a product |
| PUT | `/api/admin/products/{id}` | admin | Update a product |
| DELETE | `/api/admin/products/{id}` | admin | Soft-delete a product |
| POST | `/api/admin/products/{id}/stock` | admin | Restock or adjust stock |
| GET | `/api/admin/orders` | admin | Every order, filterable by status |
| POST | `/api/admin/orders/{id}/status` | admin | Move an order: packed, shipped, delivered, refunded, cancelled |
| GET | `/api/admin/system` | admin | Live engine figures |
| GET | `/api/admin/reports` | admin | Sales reports |
| GET | `/api/admin/reports/export` | admin | One of of-report's CSV files |
| GET | `/api/metrics` | this host only | Prometheus metrics |

The examples use these shell variables:

```sh
API=http://localhost:8080/api
J='Content-Type: application/json'
```

## Auth

### POST /api/auth/register

Body: `{email, password, name}`.
- `email`: at most 254 characters; it is lower-cased.
- `password`: 8–128 characters.
- `name`: 1–100 characters, no control characters.

```sh
curl -si -c jar -H "$J" -d '{"email":"carol@example.com","password":"carol-pass-1","name":"Carol Dsouza"}' $API/auth/register
```

```
HTTP/1.1 201 Created
Set-Cookie: of_session=9c41…; Path=/; HttpOnly; SameSite=Strict; Max-Age=86400
```

```json
{"user":{"id":4,"email":"carol@example.com","name":"Carol Dsouza","role":"customer"},
 "csrf_token":"0b8bfbc2d70dfffc6de0d3d8a68e89bc14dc1fadbe16e797b065de82130374d0"}
```

An email that is already registered gets 409 `email_taken`. A password under 8 characters gets 400 `weak_password`.

### POST /api/auth/login

```sh
curl -s -c jar -H "$J" -d '{"email":"carol@example.com","password":"carol-pass-1"}' $API/auth/login
```

```json
{"user":{"id":4,"email":"carol@example.com","name":"Carol Dsouza","role":"customer"},
 "csrf_token":"855af330c220feabc69b84b7c92501746831d44d8687acd06f0c09dc516981c8"}
```

Save the token for the next requests: `CSRF=855af330…`.

- **Wrong password:** 401 `invalid_credentials`.
- **Lockout:** after 5 failures in a row, every attempt gets 429 `account_locked`
  until the lock ends. An unknown email costs the same time as a known one, so timing
  does not reveal which emails are registered.

### POST /api/auth/logout · GET /api/me

```sh
curl -s -b jar $API/me
```

```json
{"user":{"id":4,"email":"carol@example.com","name":"Carol Dsouza","role":"customer"},"csrf_token":"855af330…"}
```

```sh
curl -s -b jar -H "X-CSRF-Token: $CSRF" -X POST $API/auth/logout
```

```json
{"ok":true}
```

## Catalogue

### GET /api/categories

```json
{"categories":[{"id":2,"name":"Books","products":5},{"id":3,"name":"Clothing","products":4},
 {"id":1,"name":"Electronics","products":6},{"id":4,"name":"Home & Kitchen","products":4},
 {"id":5,"name":"Sports","products":5}]}
```

### GET /api/products

Query parameters:
- `q`: matches the name or SKU, at most 64 characters;
- `category`: a category id;
- `page`: from 1;
- `per_page`: default 12, at most 50.

Deleted products are never listed.

```sh
curl -s "$API/products?q=hub&per_page=2"
```

```json
{"products":[{"id":3,"sku":"ELE-003","name":"USB-C Hub 7-in-1",
  "description":"HDMI 4K, 3x USB-A, SD and microSD readers, 100 W pass-through charging.",
  "price_paise":249900,"category_id":1,"category":"Electronics","active":true,
  "stock":199,"reorder_level":25}],
 "page":1,"per_page":2,"total":1}
```

`stock` is the number that can still be bought. Units held by unpaid orders are
already taken off.

### GET /api/products/{id}

Returns `{"product": {…}}`, with the same fields as above, or 404.

## Cart and checkout

### POST /api/cart/items · GET /api/cart · DELETE /api/cart/items/{product_id}

`POST /api/cart/items` sets a line's quantity: `qty` is 0–100, and 0 removes the line.
A cart holds at most 50 lines. Prices are always read from the database; the client
never sends one.

```sh
curl -s -b jar -H "$J" -H "X-CSRF-Token: $CSRF" -d '{"product_id":3,"qty":2}' $API/cart/items
```

```json
{"items":[{"product_id":3,"sku":"ELE-003","name":"USB-C Hub 7-in-1","price_paise":249900,
  "qty":2,"line_total_paise":499800,"stock":199,"active":true}],
 "units":2,"total_paise":499800}
```

`GET /api/cart` and `DELETE /api/cart/items/{product_id}` return the same shape.

### POST /api/checkout

Body (optional): `{"payment_method": "<token>"}`. Header (recommended):
`Idempotency-Key`.

```sh
curl -s -b jar -H "$J" -H "X-CSRF-Token: $CSRF" -H 'Idempotency-Key: checkout-7f3a9c21' \
     -d '{"payment_method":"tok_success"}' $API/checkout
```

```
HTTP/1.1 201 Created
```

```json
{"order":{"id":8,"user_id":4,"email":"carol@example.com","status":"payment_pending",
  "total_paise":499800,"created_at":"2026-11-25T06:29:31Z","updated_at":"2026-11-25T06:29:31Z",
  "items":[{"product_id":3,"sku":"ELE-003","name":"USB-C Hub 7-in-1","qty":2,"unit_price_paise":249900}],
  "events":[{"from":null,"to":"placed","actor":"customer:4","note":"order placed","at":"2026-11-25T06:29:31Z"},
            {"from":"placed","to":"payment_pending","actor":"of-core","note":"sent to the payment gateway","at":"2026-11-25T06:29:31Z"}],
  "payment":{"status":"pending","method":"tok_success","provider_ref":"","note":"","amount_paise":499800},
  "reservation_expires_at":"2026-11-25T06:31:31Z","reservation_seconds_left":120}}
```

**Sending the same request again with the same key** returns `200 OK` with the same
order (id 8). A retry never creates a second order. The key:
- is 8–64 characters from `A-Z a-z 0-9 - _`;
- is scoped to the user.

**If an item has run out:**

```json
{"error":{"code":"out_of_stock","message":"only 50 left of Limited Edition Mechanical Keyboard"}}
```

The other errors are:
- 400 `cart_empty`;
- 409 `unavailable` (a product was deleted);
- 503 `busy`. Retry with the **same** key.

**Payment methods** are mock tokens; no card data is accepted anywhere.

| Token | What the mock gateway does |
|---|---|
| `tok_success` (default) | Approves |
| `tok_decline` | Declines |
| `tok_timeout` | Never answers; the order expires after `store.reservation_ttl_s` |
| `tok_duplicate` | Approves, then sends the same approval again (ignored) |
| `tok_slow` | Approves after `pay.slow_ms` |
| `tok_random` | Random outcome, using the `pay.*_pct` rates |

After checkout the order stays `payment_pending` until the gateway answers:
- **Approved:** it becomes `paid`.
- **Declined, or no answer before the reservation expires:** it becomes `cancelled`,
  and the stock goes back on sale.
- **An approval that arrives after expiry:** refunded automatically.

## Orders

### GET /api/orders

Query: `page`.

```json
{"orders":[{"id":8,"status":"paid","total_paise":499800,"created_at":"2026-11-25T06:29:31Z","units":2}],"page":1}
```

### GET /api/orders/{id}

Returns the full order:
- the same shape as the checkout response;
- every status change in `events`;
- the payment's `provider_ref` once approved;
- `reservation_seconds_left` while unpaid.

Customers see only their own orders; any other id answers 404 (not 403, so it is not
revealed that the order exists). Admins see every order.

```json
{"order":{"id":8,"status":"paid", …,
  "events":[…,{"from":"payment_pending","to":"paid","actor":"of-pay",
               "note":"payment approved (ref mockpay_6c5ac91f2cb3)","at":"2026-11-25T06:29:31Z"}],
  "payment":{"status":"succeeded","method":"tok_success","provider_ref":"mockpay_6c5ac91f2cb3", …}}}
```

### POST /api/orders/{id}/cancel

| Order state | What happens |
|---|---|
| Unpaid | Cancelled; the stock is returned |
| Paid, not yet packed | Refunded; the stock is returned |
| Packed or later | 409 `not_cancellable` |

```json
{"order":{"id":8,"status":"refunded", …}}
```

## Admin

Every admin change is recorded in `audit_log`, and every stock change in
`stock_movements`.

### Products

| Method | Path | Body or query |
|---|---|---|
| GET | `/api/admin/products` | `low_stock=1`: only products at or below their reorder level |
| POST | `/api/admin/products` | `{sku, name, description, price_paise, category_id, stock, reorder_level}` |
| PUT | `/api/admin/products/{id}` | any of `{name, description, price_paise, category_id, reorder_level, active}` |
| DELETE | `/api/admin/products/{id}` | none. A soft delete: the product is hidden from the shop and removed from carts; past orders keep it |
| POST | `/api/admin/products/{id}/stock` | `{delta, reason}`: `reason` is `restock` or `adjust`; `delta` is non-zero and at most ±1,000,000 |

Field limits:

| Field | Limit |
|---|---|
| `sku` | `[A-Z0-9-]`, 1–32 characters |
| `name` | 1–120 characters |
| `description` | at most 2,000 characters |
| `price_paise` | 0 to 10¹⁰ |
| `reorder_level` | 0 to 10⁶ |

Create:

```sh
curl -s -b admin.jar -H "$J" -H "X-CSRF-Token: $ADMIN_CSRF" $API/admin/products -d \
  '{"sku":"ELE-010","name":"Mechanical Pencil Set","description":"Five 0.5 mm pencils.",
    "price_paise":34900,"category_id":1,"stock":40,"reorder_level":5}'
```

```json
{"product":{"id":25,"sku":"ELE-010","name":"Mechanical Pencil Set","description":"Five 0.5 mm pencils.",
  "price_paise":34900,"category_id":1,"category":"Electronics","active":true,"stock":40,"reorder_level":5}}
```

Stock adjustments:

| Body | Result |
|---|---|
| `{"delta":-5,"reason":"adjust"}` | `{"product":{…,"stock":35,…}}` |
| `{"delta":-100,"reason":"adjust"}` | 409 `{"error":{"code":"insufficient_stock","message":"stock cannot go below zero (some units may be reserved by checkouts)"}}` |

### Orders

`GET /api/admin/orders` takes `status`, `page` and `per_page`:

```json
{"orders":[{"id":6,"email":"bob@example.com","status":"paid","total_paise":179700,
  "created_at":"2026-11-25T06:28:11Z","updated_at":"2026-11-25T06:28:11Z","units":3}],"page":1,"total":1}
```

`POST /api/admin/orders/{id}/status` takes `{status, note?}`. `status` is one of
`packed`, `shipped`, `delivered`, `refunded`, `cancelled`. The order state machine
decides which moves are legal. Only the payment gateway can mark an order `paid`.

```sh
curl -s -b admin.jar -H "$J" -H "X-CSRF-Token: $ADMIN_CSRF" -d '{"status":"packed","note":"boxed"}' $API/admin/orders/6/status
```

```json
{"order":{"id":6,"status":"packed", …}}
```

The same order sent straight to `delivered`:

```json
{"error":{"code":"invalid_transition","message":"an order cannot go from packed to delivered"}}
```

### GET /api/admin/system

```json
{"stock_mode":"lockfree-shm","reservation_ttl_s":120,"products_in_shm":25,"started_unix":1795588088,
 "metrics":{"requests_total":107,"busy_rejects":0,"queue_depth":0,"orders_total":8,"orders_failed":1,
  "cas_retries":0,"active_workers":1,"payments_sent":8,"payments_succeeded":6,"payments_declined":1,
  "payments_ignored":0,"payments_rejected":0,"late_refunds":0,"reservations_expired":0}}
```

### GET /api/admin/reports

Query: `days`, 1–366 (default 30). The figures are of-report's latest aggregation:
`generated_at`, and `age_seconds` since then.

```json
{"days":7,"generated_at":"2026-11-25T06:29:27Z","generation_ms":30,"age_seconds":5,
 "daily":[{"day":"2026-11-25","orders":4,"units":9,"revenue_paise":1719100}],
 "totals":{"orders":4,"units":9,"revenue_paise":1719100},
 "top_products":[{"product_id":8,"sku":"BOO-002","name":"The Linux Programming Interface","units":1,"revenue_paise":649900}, …],
 "order_status":[…],"payments":[…],"low_stock":[…]}
```

### GET /api/admin/reports/export

Query: `name`, which must be `daily_sales`, `top_products`, `low_stock` or
`order_status`. Nothing else from the request reaches the file system.

```json
{"name":"order_status","filename":"orderforge-order_status.csv",
 "csv":"status,orders,value_rupees\ncancelled,1,1799.00\ndelivered,1,2897.00\npacked,1,2499.00\n…"}
```

## Metrics (operators)

`GET /api/metrics` answers only clients on the same host (127.0.0.1 or ::1), and 403
otherwise. It returns Prometheus text format.

```sh
curl -s http://localhost:8080/api/metrics | grep -E '^orderforge_(core_up|orders_total|busy)'
```

```
orderforge_core_up 1
orderforge_orders_total 8
orderforge_busy_rejects_total 0
```

of-api renders this straight from of-core's shared memory, through a read-only
mapping, and never sends a request to the core. It therefore keeps answering while the
core is busy, and reports `orderforge_core_up 0` while the core is down. It covers:

- **Counters:** requests, orders, CAS retries, payments by outcome, busy rejections,
  expired reservations, WAL appends, fsyncs and bytes, crash recoveries, and rate-limit
  decisions (`orderforge_ratelimit_{allowed,limited,table_full}_total`).
- **Histogram:** `orderforge_request_duration_seconds`.
- **Per-product gauges:** `orderforge_stock_available` and `orderforge_stock_reserved`.

`of-ctl metrics` prints the same text without going through Apache.
