/*
 * cart.c - server-side cart, one per customer. Prices are never taken from the client:
 * the cart stores only product ids and quantities, and prices are read at checkout.
 */
#include "store/internal.h"

#include <stdio.h>

#define CART_MAX_LINES 50
#define CART_MAX_QTY   100

int64_t cart_id(ctx_t *c)
{
    int64_t id;
    int rc = db_query_int(c->db, &id, "SELECT id FROM carts WHERE user_id = ?", "i", c->user.id);
    if (rc == 1)
        return id;
    if (rc < 0)
        return -1;
    /* INSERT IGNORE + re-select is safe if two requests race to create the cart. */
    if (db_exec(c->db, "INSERT IGNORE INTO carts (user_id) VALUES (?)", "i", c->user.id) < 0)
        return -1;
    rc = db_query_int(c->db, &id, "SELECT id FROM carts WHERE user_id = ?", "i", c->user.id);
    return rc == 1 ? id : -1;
}

static void render_cart(ctx_t *c, int64_t cid)
{
    db_res_t r;
    if (db_query(c->db, &r,
                 "SELECT p.id, p.sku, p.name, p.price_paise, ci.qty, i.stock_on_hand, p.active "
                 "FROM cart_items ci JOIN products p ON p.id = ci.product_id "
                 "JOIN inventory i ON i.product_id = p.id "
                 "WHERE ci.cart_id = ? ORDER BY p.id",
                 "i", cid) != 0) {
        resp_db_error(c);
        return;
    }

    buf_t *b = &c->resp->body;
    buf_reset(b);
    int64_t total = 0, units = 0;
    buf_puts(b, "{\"items\":[");
    for (int n = 0; db_fetch(&r) == 1; n++) {
        int64_t price = db_int(&r, 3), qty = db_int(&r, 4);
        total += price * qty;
        units += qty;
        buf_puts(b, n ? ",{" : "{");
        buf_json_kv_int(b, "product_id", db_int(&r, 0));
        buf_puts(b, ",");
        buf_json_kv_str(b, "sku", db_str(&r, 1));
        buf_puts(b, ",");
        buf_json_kv_str(b, "name", db_str(&r, 2));
        buf_puts(b, ",");
        buf_json_kv_int(b, "price_paise", price);
        buf_puts(b, ",");
        buf_json_kv_int(b, "qty", qty);
        buf_puts(b, ",");
        buf_json_kv_int(b, "line_total_paise", price * qty);
        buf_puts(b, ",");
        buf_json_kv_int(b, "stock", db_int(&r, 5));
        buf_puts(b, ",");
        buf_json_kv_bool(b, "active", db_int(&r, 6) != 0);
        buf_puts(b, "}");
    }
    buf_puts(b, "],");
    buf_json_kv_int(b, "units", units);
    buf_puts(b, ",");
    buf_json_kv_int(b, "total_paise", total);
    buf_puts(b, "}");
    db_res_free(&r);
}

void h_cart_get(ctx_t *c)
{
    int64_t cid = cart_id(c);
    if (cid < 0) {
        resp_db_error(c);
        return;
    }
    render_cart(c, cid);
}

/* POST /api/cart/items {"product_id": 3, "qty": 2}: sets the quantity; 0 removes. */
void h_cart_set_item(ctx_t *c)
{
    if (ctx_json(c))
        return;
    int64_t pid, qty;
    if (json_get_int(c->json, "product_id", &pid) != 0 || pid <= 0) {
        resp_error(c, 400, "bad_request", "product_id is required");
        return;
    }
    if (json_get_int(c->json, "qty", &qty) != 0 || qty < 0 || qty > CART_MAX_QTY) {
        resp_error(c, 400, "bad_request", "qty must be 0 to %d", CART_MAX_QTY);
        return;
    }

    int64_t cid = cart_id(c);
    if (cid < 0) {
        resp_db_error(c);
        return;
    }

    if (qty == 0) {
        if (db_exec(c->db, "DELETE FROM cart_items WHERE cart_id = ? AND product_id = ?", "ii",
                    cid, pid) < 0) {
            resp_db_error(c);
            return;
        }
        render_cart(c, cid);
        return;
    }

    int64_t active;
    int rc = db_query_int(c->db, &active, "SELECT active FROM products WHERE id = ?", "i", pid);
    if (rc < 0) {
        resp_db_error(c);
        return;
    }
    if (rc == 0 || !active) {
        resp_error(c, 404, "not_found", "product not found");
        return;
    }

    int64_t lines;
    if (db_query_int(c->db, &lines,
                     "SELECT COUNT(*) FROM cart_items WHERE cart_id = ? AND product_id <> ?", "ii",
                     cid, pid) < 0) {
        resp_db_error(c);
        return;
    }
    if (lines >= CART_MAX_LINES) {
        resp_error(c, 400, "cart_full", "a cart holds at most %d different products",
                   CART_MAX_LINES);
        return;
    }

    if (db_exec(c->db,
                "INSERT INTO cart_items (cart_id, product_id, qty) VALUES (?, ?, ?) AS new "
                "ON DUPLICATE KEY UPDATE qty = new.qty",
                "iii", cid, pid, qty) < 0) {
        resp_db_error(c);
        return;
    }
    render_cart(c, cid);
}

void h_cart_delete_item(ctx_t *c)
{
    int64_t cid = cart_id(c);
    if (cid < 0) {
        resp_db_error(c);
        return;
    }
    if (db_exec(c->db, "DELETE FROM cart_items WHERE cart_id = ? AND product_id = ?", "ii", cid,
                c->id) < 0) {
        resp_db_error(c);
        return;
    }
    render_cart(c, cid);
}
