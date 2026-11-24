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
| 503 | Database unavailable |

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
| POST | `/api/checkout` | – | 201 `{order}`, or 409 `out_of_stock` / `unavailable` |

- **Setting a quantity:** `POST /api/cart/items` sets the quantity, from 0 to 100.
  A quantity of 0 removes the line.
- **Prices:** the server always reads prices from the database. Any price sent by the
  client is ignored.

## Orders (user)

| Method | Path | Returns |
|---|---|---|
| GET | `/api/orders?page=` | `{orders: [{id, status, total_paise, created_at, units}]}` |
| GET | `/api/orders/{id}` | `{order: {..., items: [...], events: [...]}}` |

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

Every admin change is recorded in `audit_log`. Every stock change is recorded in
`stock_movements`.
