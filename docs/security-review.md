# Security review (phase 5)

This review went through every endpoint and every process boundary of OrderForge. It
lists the controls in place, what this phase found and fixed, and the risks that
remain. Each control is backed by a test, named in the right-hand column.

## 1. Controls that apply to every request

Each request meets these controls in order.

| # | Where | Control | Test |
|---|---|---|---|
| 1 | Apache | CSP `default-src 'self'` (no inline script or style), `X-Frame-Options: DENY`, `nosniff`, `Referrer-Policy`, bodies over 64 KiB refused | security_test (headers) |
| 2 | of-api | **Rate limits.** Token buckets in shared memory, shared by all of-api workers. Per client IP (500/s, burst 2000); per session (20/s, burst 40); login and registration per IP (30/min, burst 30). Over a limit: 429 with `Retry-After` | unit (exactness across threads and processes), security_test |
| 3 | of-api | **Cross-site requests.** A non-GET request whose `Origin` names another site, or is `null`, gets 403 | security_test |
| 4 | of-api | **Bodies must be `application/json`**, otherwise 415. This is what defeats login CSRF through `text/plain` forms (finding F1) | security_test |
| 5 | of-api | **Hard size caps.** Path 1 KiB, query 2 KiB, body 16 KiB, CSRF token 128 B, Idempotency-Key 64 B | security_test, unit (proto) |
| 6 | protocol | Every frame length is checked against its cap and the bytes remaining; trailing bytes are rejected | unit: every truncation of a valid frame |
| 7 | of-core | The Unix socket is mode 0660, and the peer's uid is checked with `SO_PEERCRED` | design §5 |
| 8 | of-core | **Authentication.** A 64-hex session cookie, looked up by its SHA-256. Sessions expire. | api_test |
| 9 | of-core | **CSRF token** on every state-changing call that needs a session, compared in constant time | api_test, security_test |
| 10 | of-core | **Authorisation.** The route table gives each endpoint public, user or admin access, checked before any handler | api_test, security_test |
| 11 | of-core | **Strict JSON.** Flat objects only, integers only; duplicate keys and nesting are rejected | unit (json), security_test |
| 12 | of-core | **Prepared statements only.** `LIKE` wildcards are escaped; ids must be numeric path segments | security_test (SQL injection) |
| 13 | of-core | **Output.** JSON escapes `<` as `<`; the UI inserts all text with `textContent` | security_test |

## 2. Endpoint by endpoint

Notation:

- **Access:** P = public, U = logged-in customer, A = admin.
- **CSRF:** whether the CSRF token is required (it always is for a state-changing
  request that needs a session).
- **Rate limit:** every endpoint also has the IP rule. U and A endpoints also have the
  session rule. Login and registration also have the auth rule.

| Endpoint | Access | CSRF | Input validation | Other protections |
|---|---|---|---|---|
| `POST /api/auth/register` | P | – | Email shape, at most 254 characters, lower-cased; password 8–128; name 1–100, no control characters | Argon2id hash; auth rate limit; audited |
| `POST /api/auth/login` | P | – | The same; password at most 128 | Lockout after 5 failures (30 s doubling to 1 h); an unknown email still costs a full Argon2 verify, so timing reveals nothing; the auth rate limit throttles guessing across accounts; every attempt audited |
| `POST /api/auth/logout` | U | yes | – | Session row deleted, cookie cleared |
| `GET /api/me` | U | – | – | Returns the CSRF token only to its own session |
| `GET /api/categories`, `/api/products`, `/api/products/{id}` | P | – | Search at most 64 characters with LIKE escaping; page and per-page clamped (at most 50) | Inactive products hidden |
| `GET /api/cart` | U | – | – | The user's own cart only |
| `POST /api/cart/items` | U | yes | `product_id` > 0; `qty` 0–100; at most 50 lines | Prices are never taken from the client |
| `DELETE /api/cart/items/{id}` | U | yes | Numeric id | Own cart only |
| `POST /api/checkout` | U | yes | `payment_method` from a fixed list of 6; Idempotency-Key 8–64 characters of `[A-Za-z0-9_-]` | Key scoped per user; totals computed on the server; the four layers against overselling |
| `GET /api/orders`, `/api/orders/{id}` | U | – | Numeric id | Someone else's order answers 404, not 403, so its existence isn't revealed |
| `POST /api/orders/{id}/cancel` | U | yes | – | Own orders only; the state machine allows it only before packing |
| `GET/POST /api/admin/products`, `PUT/DELETE /api/admin/products/{id}` | A | yes for writes | SKU `[A-Z0-9-]` 1–32; name 1–120; description at most 2000; price 0 – 10¹⁰ paise; reorder level 0 – 10⁶; updates accept only a whitelist of fields | Soft delete; audited |
| `POST /api/admin/products/{id}/stock` | A | yes | `delta` non-zero, at most ±10⁶; `reason` restock or adjust | Can never go below zero; shared memory stays exact; audited |
| `GET /api/admin/orders`, `POST /api/admin/orders/{id}/status` | A | yes for writes | Status from the enum; note at most 200 | State machine: only the gateway can mark an order paid; audited |
| `GET /api/admin/system`, `/api/admin/reports` | A | – | `days` 1–366 | Read-only |
| `GET /api/admin/reports/export` | A | – | `name` must be one of 4 fixed names | `O_NOFOLLOW`; 1 MiB cap; no part of the request reaches `open()` |
| `GET /api/metrics` | 127.0.0.1 or ::1 only | – | – | Read-only shared-memory mapping; never touches the core |

## 3. Between processes

| Boundary | Threat | Control | Test |
|---|---|---|---|
| of-api → of-core (Unix socket) | Another local user connects | Mode 0660 plus the `SO_PEERCRED` uid check | – |
| | Malformed frames | A checked header; the payload is decoded with bounds checks | unit (proto) |
| of-core ⇄ of-pay (POSIX message queues) | A forged "payment approved" | HMAC-SHA256 with a shared secret, compared in constant time | security_test: forged message rejected; unit: every byte flipped |
| | A replayed or stale message | A random nonce, checked against the last 4,096 seen, and a 5-minute timestamp window | security_test: replay and 10-minute-old message both rejected; unit |
| | A duplicated callback | The payment row must be `pending` to change; the guarded order transition | payment_test |
| Shared memory (stock, metrics, rate limits) | Tampering by tools | Created 0600; every tool maps it **read-only** | – |
| Files | Secrets readable by others | Config 0600, warned about at start-up if looser; WAL 0600; logs 0640; `umask 027` | security warning shown in testing |
| Process memory | Secrets in core dumps; ptrace by same-user processes | `RLIMIT_CORE = 0` and `PR_SET_DUMPABLE = 0` (override with `run.core_dumps = 1`) | privdrop_test |
| Root | A daemon started with sudo keeps root | Drops to `run.user` (initgroups, setgid, setuid) before opening any file, then verifies `setuid(0)` fails. Without `run.user` it refuses to run as root | privdrop_test (needs sudo) |

## 4. Findings fixed in this phase

| # | Finding | Impact | Fix |
|---|---|---|---|
| F1 | **Login CSRF through `text/plain` forms.** A cross-site HTML form with `enctype="text/plain"` can produce a body that is valid JSON (`{"email":"…","password":"…","x":"=…"}`). The JSON parser accepted it, and login needs no CSRF token. | An attacker could log a victim into the attacker's account and watch what they do | of-api requires `application/json` for every body (415 otherwise), and refuses cross-site `Origin` on state-changing requests (403). Cross-site pages cannot send `application/json` without a CORS preflight, which is never granted. |
| F2 | **No rate limiting.** | Password guessing across many accounts (the lockout is per account); Argon2 cost as a denial-of-service lever; one client could flood the core | Shared-memory token buckets per IP, per session and for authentication (section 1). |
| F3 | **A payment reply could be lost after a MySQL outage.** A worker took a reply off the queue, then found its connection dead. | The order stayed unpaid until it expired, and the customer was refunded although they had paid | The reply is retried, with a reconnect, until applied; a shared "database epoch" makes every thread reconnect before its next job. Found by failure_test. |
| F4 | **Secrets reachable through core dumps and ptrace.** | The MySQL password and the HMAC key could be read from a crash dump, or by any process of the same user | `RLIMIT_CORE = 0` and `PR_SET_DUMPABLE = 0`. |
| F5 | **Started as root, the daemons kept root.** | Any bug would have been a root compromise | Privilege drop, or refusal (section 3). |
| F6 | **A world-readable config went unnoticed.** | Database password and payment key exposed to every local user | Every daemon warns at start-up, naming the fix. |
| F7 | **Background threads gave up if MySQL was down when of-core started.** The payment-reply workers and the reaper exited after one failed connection. | Availability: payments were never applied and orders never expired until of-core was restarted by hand | They retry every second until connected or shutting down. Found by failure_test under ThreadSanitizer, whose slow start-up overlapped the MySQL outage. |

## 5. Remaining risks (accepted, documented)

- **No TLS in the development setup.**
  - Cookies go over plain HTTP on localhost.
  - With TLS: set `auth.cookie_secure = 1`, which adds `Secure`, and add HSTS in Apache.
- **Registration reveals whether an email is registered** (409).
  - This is the usual trade-off for a shop sign-up.
  - The auth rate limit makes harvesting slow.
- **The rate limiter fails open when its 32-slot probe window is full.**
  - That needs tens of thousands of distinct keys at once, i.e. a large distributed
    attack, which belongs at the network edge anyway.
  - It is counted in `orderforge_ratelimit_table_full_total`, so it is visible.
- **The session limit applies per session,** so one person with several sessions gets
  several buckets. The IP limit still bounds them.
- **Prices in old orders.** Price changes are audited and never alter past orders,
  because unit prices are copied at purchase. Admin accounts have no second factor.
- **The HMAC key is a shared secret.** A local process running as the same user can
  read the 0600 config and forge payment messages. The boundary is the Unix account,
  which is standard for local IPC.
