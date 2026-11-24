# API reference

All endpoints live under `/api/`. Request and response bodies are JSON.

- **Authentication:** login and registration set an `of_session` cookie, which is
  HttpOnly and SameSite=Strict. They also return a `csrf_token` in the response body.
- **CSRF:** every request that needs a session and is not a GET must send
  `X-CSRF-Token: <csrf_token>`.
- **Money:** all amounts are integer paise.

Errors use one shape:

```json
{ "error": { "code": "out_of_stock", "message": "only 2 left of Wireless Earbuds" } }
```

| Status | Meaning |
|---|---|
| 400 | Invalid input |
| 401 | Not logged in, or wrong credentials |
| 403 | CSRF token missing, or not an admin |
| 404 | Not found, or not yours |
| 405 | Wrong method |
| 409 | Conflict: duplicate, out of stock, or busy |
| 413 | Body too large |
| 429 | Account locked |
| 502 | of-core did not answer in time, or sent a malformed reply |
| 503 | Database or engine unavailable (`db_unavailable`, `core_unavailable`), overloaded (`busy`, `timeout`), or restarting (`shutting_down`) |

## Auth

| Method | Path | Access | Body | Returns |
|---|---|---|---|---|
| POST | `/api/auth/register` | public | `{email, password (8-128), name}` | 201 `{user, csrf_token}` + cookie |
| POST | `/api/auth/login` | public | `{email, password}` | 200 `{user, csrf_token}` + cookie |
| POST | `/api/auth/logout` | user | – | 200 `{ok}`, cookie cleared |
| GET | `/api/me` | user | – | `{user, csrf_token}` |

After 5 consecutive wrong passwords the account locks for 30 s. Each further failure
doubles the lock, up to one hour, and the endpoint answers 429 while it lasts.

## Catalogue

| Method | Path | Query | Returns |
|---|---|---|---|
| GET | `/api/categories` | – | `{categories: [{id, name, products}]}` |
| GET | `/api/products` | `q`, `category`, `page`, `per_page` (≤ 50) | `{products, page, per_page, total}` |
| GET | `/api/products/{id}` | – | `{product}` |

The `q` parameter matches the name or SKU. A product object looks like this:

```json
{"id":1,"sku":"HOT-001","name":"...","description":"...","price_paise":899900,
 "category_id":1,"category":"Electronics","active":true,"stock":50,"reorder_level":5}
```

## Cart and checkout (user)

| Method | Path | Body | Returns |
|---|---|---|---|
| GET | `/api/cart` | – | `{items, units, total_paise}` |
| POST | `/api/cart/items` | `{product_id, qty}` | the cart |
| DELETE | `/api/cart/items/{product_id}` | – | the cart |
| POST | `/api/checkout` | `{payment_method}` (optional) + header `Idempotency-Key` (optional) | 201 `{order}` in `payment_pending`; 200 with the existing order when the key was used before; 409 `out_of_stock` / `unavailable` |

- **Setting a quantity:** `POST /api/cart/items` sets the quantity, from 0 to 100.
  A quantity of 0 removes the line.
- **Prices:** the server always reads prices from the database. Any price sent by the
  client is ignored.
- **Payment methods:** mock tokens only. No card data is accepted anywhere.

| Token | What the mock gateway does |
|---|---|
| `tok_success` (default) | Approves |
| `tok_decline` | Declines |
| `tok_timeout` | Never answers; the order expires after `store.reservation_ttl_s` |
| `tok_duplicate` | Approves, then sends the same approval again |
| `tok_slow` | Approves after `pay.slow_ms` |
| `tok_random` | Random outcome, using the `pay.*_pct` rates |

- **Idempotency-Key:** 8–64 characters from `A-Z a-z 0-9 - _`, scoped to the user.
  Retrying a checkout with the same key returns the same order and never creates a
  second one.
- **After checkout:** the order is `payment_pending` until the gateway answers.
  - Approved: it becomes `paid`.
  - Declined, or no answer before the reservation expires: it becomes `cancelled`,
    and the stock is returned.

## Orders (user)

| Method | Path | Returns |
|---|---|---|
| GET | `/api/orders?page=` | `{orders: [{id, status, total_paise, created_at, units}]}` |
| GET | `/api/orders/{id}` | `{order: {..., items, events, payment: {status, method, provider_ref, note, amount_paise}, reservation_expires_at?, reservation_seconds_left?}}` |
| POST | `/api/orders/{id}/cancel` | Cancels an unpaid order, or refunds a paid one that is not yet packed. 409 `not_cancellable` otherwise |

Customers can only see their own orders; anyone else's order returns 404. Admins can
see every order.

## Admin

| Method | Path | Body / query | Returns |
|---|---|---|---|
| GET | `/api/admin/products` | `low_stock=1` | every product, including deleted ones |
| POST | `/api/admin/products` | `{sku, name, description, price_paise, category_id, stock, reorder_level}` | 201 `{product}` |
| PUT | `/api/admin/products/{id}` | any of `{name, description, price_paise, category_id, reorder_level, active}` | `{product}` |
| DELETE | `/api/admin/products/{id}` | – | soft delete: `active=false`, removed from carts |
| POST | `/api/admin/products/{id}/stock` | `{delta, reason: "restock" or "adjust"}` | `{product}`; 409 if stock would go below 0 |
| GET | `/api/admin/orders` | `status`, `page`, `per_page` | `{orders, page, total}` |
| POST | `/api/admin/orders/{id}/status` | `{status: "packed" \| "shipped" \| "delivered" \| "refunded" \| "cancelled", note?}` | `{order}`; 409 `invalid_transition` for a move the state machine forbids |
| GET | `/api/admin/system` | – | `{stock_mode, reservation_ttl_s, products_in_shm, started_unix, metrics: {requests_total, busy_rejects, queue_depth, orders_total, orders_failed, cas_retries, active_workers, payments_sent, payments_succeeded, payments_declined, payments_ignored, payments_rejected, late_refunds, reservations_expired}}` |
| GET | `/api/admin/reports` | `days` (1–366, default 30) | `{days, generated_at, generation_ms, age_seconds, daily: [{day, orders, units, revenue_paise}], totals, top_products, order_status, payments, low_stock}` |
| GET | `/api/admin/reports/export` | `name`: `daily_sales`, `top_products`, `low_stock` or `order_status` | `{name, filename, csv}`: the CSV that of-report last wrote |

Every admin change is recorded in `audit_log`. Every stock change is recorded in
`stock_movements`.
