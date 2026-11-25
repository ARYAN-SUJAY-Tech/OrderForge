#!/usr/bin/env python3
"""Screenshots for the user manual, taken from a running OrderForge stack.

    scripts/take-screenshots.py --yes [--url http://127.0.0.1:8080] [--out docs/screenshots]

1. Through the API, it creates demo activity as the seeded accounts (alice, bob, admin):
   orders in every state (delivered, shipped, packed, paid, refunded, cancelled and one
   still waiting for the gateway) and a cart with two items.
2. It opens each page in headless Firefox, logged in as the right account, and saves a
   PNG.

It ADDS ORDERS to the database behind --url, so run it on a freshly loaded demo
database (scripts/load-db.sh), never on real data. It needs geckodriver running:

    geckodriver --port 4444 &          # or, with the snap: firefox.geckodriver --port 4444

Standard library only. The WebDriver protocol is plain HTTP and JSON.
"""
import argparse
import base64
import http.cookies
import json
import os
import secrets
import sys
import time
import urllib.error
import urllib.request

ADMIN = (os.environ.get("OF_ADMIN_EMAIL", "admin@orderforge.local"),
         os.environ.get("OF_ADMIN_PASSWORD", "admin12345"))
ALICE = ("alice@example.com", "customer123")
BOB = ("bob@example.com", "customer123")


class Api:
    """One logged-in session against /api."""

    def __init__(self, base, email, password):
        self.base, self.cookie, self.csrf = base, None, None
        r = self.call("POST", "/auth/login", {"email": email, "password": password})
        self.csrf = r["csrf_token"]

    def call(self, method, path, body=None, idem=None, ok=(200, 201)):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(self.base + "/api" + path, data=data, method=method)
        if data is not None:
            req.add_header("Content-Type", "application/json")
        if self.cookie:
            req.add_header("Cookie", "of_session=" + self.cookie)
        if self.csrf and method != "GET":
            req.add_header("X-CSRF-Token", self.csrf)
        if idem:
            req.add_header("Idempotency-Key", idem)
        try:
            resp = urllib.request.urlopen(req, timeout=30)
            status, raw, headers = resp.status, resp.read(), resp.headers
        except urllib.error.HTTPError as e:
            status, raw, headers = e.code, e.read(), e.headers
        for h in headers.get_all("Set-Cookie") or []:
            c = http.cookies.SimpleCookie(h)
            if "of_session" in c:
                self.cookie = c["of_session"].value
        if status not in ok:
            raise SystemExit("%s %s -> %d %s" % (method, path, status, raw[:200]))
        time.sleep(0.06)  # stay well inside the per-session rate limit
        return json.loads(raw) if raw else {}

    def order(self, items, method="tok_success"):
        for pid, qty in items:
            self.call("POST", "/cart/items", {"product_id": pid, "qty": qty})
        r = self.call("POST", "/checkout", {"payment_method": method},
                      idem=secrets.token_hex(12))
        return r["order"]["id"]

    def wait_status(self, oid, want, timeout=20):
        end = time.time() + timeout
        while time.time() < end:
            if self.call("GET", "/orders/%d" % oid)["order"]["status"] == want:
                return
            time.sleep(0.3)
        raise SystemExit("order %d never reached %s" % (oid, want))


class Browser:
    """A minimal WebDriver client for geckodriver."""

    def __init__(self, driver, width, height):
        self.driver = driver
        caps = {"capabilities": {"alwaysMatch": {"browserName": "firefox", "moz:firefoxOptions": {
            "args": ["-headless", "-width", str(width), "-height", str(height)],
            "prefs": {"ui.systemUsesDarkTheme": 0}}}}}
        self.sid = self.call("POST", "/session", caps)["sessionId"]

    def call(self, method, path, body=None):
        req = urllib.request.Request(self.driver + path, method=method,
                                     data=json.dumps(body).encode() if body is not None else None,
                                     headers={"Content-Type": "application/json"})
        return json.loads(urllib.request.urlopen(req, timeout=60).read())["value"]

    def s(self, method, path, body=None):
        return self.call(method, "/session/%s%s" % (self.sid, path), body)

    def go(self, url):
        self.s("POST", "/url", {"url": url})

    def login_as(self, base, api):
        self.go(base + "/")
        self.s("DELETE", "/cookie")
        if api:
            self.s("POST", "/cookie", {"cookie": {"name": "of_session", "value": api.cookie,
                                                  "path": "/", "httpOnly": True}})

    def settle(self, ready_js, timeout=10):
        """Wait until ready_js (an expression) is true, then let rendering finish."""
        end = time.time() + timeout
        while time.time() < end:
            if self.s("POST", "/execute/sync", {"script": "return !!(%s);" % ready_js, "args": []}):
                break
            time.sleep(0.2)
        time.sleep(0.8)

    def js(self, script):
        return self.s("POST", "/execute/sync", {"script": script, "args": []})

    def shot(self, path, full=True):
        png = self.s("GET", "/moz/screenshot/full" if full else "/screenshot")
        with open(path, "wb") as f:
            f.write(base64.b64decode(png))
        print("  saved", path)

    def close(self):
        self.call("DELETE", "/session/" + self.sid)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", default="http://127.0.0.1:8080")
    ap.add_argument("--out", default="docs/screenshots")
    ap.add_argument("--driver", default="http://127.0.0.1:4444")
    ap.add_argument("--yes", action="store_true", help="confirm: this adds demo orders")
    a = ap.parse_args()
    if not a.yes:
        ap.print_help()
        sys.exit("\nrefusing to run without --yes: this adds demo orders to the database")
    base = a.url.rstrip("/")
    os.makedirs(a.out, exist_ok=True)

    print("demo activity")
    alice, bob, admin = Api(base, *ALICE), Api(base, *BOB), Api(base, *ADMIN)
    prods = {p["sku"]: p["id"] for p in
             alice.call("GET", "/products?per_page=50")["products"]}
    pid = lambda sku: prods[sku]
    skus = sorted(prods)
    pick = [pid(s) for s in skus if not s.startswith("HOT")]

    def adm(oid, *moves):
        for m in moves:
            admin.call("POST", "/admin/orders/%d/status" % oid, {"status": m})

    o_delivered = alice.order([(pick[0], 1), (pick[4], 2)])
    o_packed = alice.order([(pick[6], 1)])
    o_declined = alice.order([(pick[3], 1)], "tok_decline")
    o_refunded = alice.order([(pick[8], 1)])
    o_bob_shipped = bob.order([(pick[1], 1), (pick[9], 1)])
    o_bob_paid = bob.order([(pick[5], 3)])
    for api, oid in ((alice, o_delivered), (alice, o_packed), (alice, o_refunded),
                     (bob, o_bob_shipped), (bob, o_bob_paid)):
        api.wait_status(oid, "paid")
    alice.wait_status(o_declined, "cancelled")
    adm(o_delivered, "packed", "shipped", "delivered")
    adm(o_packed, "packed")
    adm(o_bob_shipped, "packed", "shipped")
    alice.call("POST", "/orders/%d/cancel" % o_refunded)
    o_pending = alice.order([(pick[10], 1)], "tok_slow")  # stays waiting for the gateway
    alice.call("POST", "/cart/items", {"product_id": pick[2], "qty": 1})
    alice.call("POST", "/cart/items", {"product_id": pick[11], "qty": 2})
    print("  orders: delivered %d, packed %d, declined %d, refunded %d, pending %d; bob %d, %d"
          % (o_delivered, o_packed, o_declined, o_refunded, o_pending, o_bob_shipped, o_bob_paid))
    time.sleep(11)  # one of-report cycle, so the reports page includes today's sales

    print("screenshots ->", a.out)
    b = Browser(a.driver, 1280, 860)
    out = lambda n: os.path.join(a.out, n)
    try:
        b.login_as(base, None)
        b.go(base + "/login.html")
        b.settle("document.getElementById('login-form')")
        b.shot(out("01-login.png"))

        b.login_as(base, alice)
        b.go(base + "/")
        b.settle("document.querySelectorAll('#products .card').length > 0")
        b.shot(out("02-shop.png"), full=False)

        b.go(base + "/product.html?id=%d" % pick[0])
        b.settle("document.querySelector('#product h1, #product h2')")
        b.shot(out("03-product.png"), full=False)

        b.go(base + "/cart.html")
        b.settle("document.querySelector('#cart table, #cart .empty')")
        b.shot(out("04-cart.png"))

        b.go(base + "/orders.html?id=%d" % o_pending)
        b.settle("document.querySelector('.timeline, ol, table')")
        b.shot(out("05-order-waiting.png"))

        b.go(base + "/orders.html?id=%d" % o_delivered)
        b.settle("document.querySelector('.timeline, ol, table')")
        b.shot(out("06-order-delivered.png"))

        b.go(base + "/orders.html")
        b.settle("document.querySelector('#orders table')")
        b.shot(out("07-my-orders.png"))

        b.login_as(base, admin)
        b.go(base + "/admin/")
        b.settle("document.querySelectorAll('#rows tr').length > 0")
        b.shot(out("08-admin-add-product.png"), full=False)
        b.js("document.getElementById('rows').closest('table').scrollIntoView(); window.scrollBy(0, -90);")
        time.sleep(0.5)
        b.shot(out("09-admin-catalogue.png"), full=False)

        b.go(base + "/admin/orders.html")
        b.settle("document.querySelectorAll('#rows tr').length > 0")
        b.shot(out("10-admin-orders.png"))

        b.go(base + "/orders.html?id=%d" % o_bob_paid)
        b.settle("document.querySelector('.timeline, ol, table')")
        b.shot(out("11-admin-order.png"))

        b.go(base + "/admin/reports.html")
        b.settle("document.querySelector('#kpis *') && document.querySelector('#health *')")
        b.shot(out("12-admin-reports.png"))
    finally:
        b.close()


if __name__ == "__main__":
    main()
