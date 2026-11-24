-- OrderForge schema (MySQL 8, InnoDB).
-- Loading this file DROPS and recreates every table: it is also the "reset" used
-- between benchmark runs. Money is always integer paise, never floating point.

SET NAMES utf8mb4;
SET FOREIGN_KEY_CHECKS = 0;
DROP TABLE IF EXISTS report_meta, product_sales_daily, daily_sales, payments, reservations,
                     audit_log, stock_movements, order_events, order_items, orders,
                     cart_items, carts, inventory, products, categories, sessions, users;
SET FOREIGN_KEY_CHECKS = 1;

CREATE TABLE users (
    id            BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    email         VARCHAR(254) NOT NULL,
    name          VARCHAR(100) NOT NULL,
    pw_hash       VARCHAR(255) NOT NULL,            -- Argon2id, libsodium encoded string
    role          ENUM('customer', 'admin') NOT NULL DEFAULT 'customer',
    failed_logins INT UNSIGNED NOT NULL DEFAULT 0,
    locked_until  DATETIME(3) NULL,
    created_at    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    UNIQUE KEY uq_users_email (email)
) ENGINE = InnoDB;

-- Only SHA-256(token) is stored, so a database leak does not yield usable sessions.
CREATE TABLE sessions (
    token_hash CHAR(64) NOT NULL PRIMARY KEY,
    user_id    BIGINT UNSIGNED NOT NULL,
    csrf_token CHAR(64) NOT NULL,
    ip         VARCHAR(45) NOT NULL DEFAULT '',
    created_at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    expires_at DATETIME(3) NOT NULL,
    KEY ix_sessions_user (user_id),
    KEY ix_sessions_expiry (expires_at),
    CONSTRAINT fk_sessions_user FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE CASCADE
) ENGINE = InnoDB;

CREATE TABLE categories (
    id   INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    name VARCHAR(64) NOT NULL,
    UNIQUE KEY uq_categories_name (name)
) ENGINE = InnoDB;

-- Delete is a soft delete (active = 0) so old orders stay valid.
CREATE TABLE products (
    id          BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    sku         VARCHAR(32) NOT NULL,
    name        VARCHAR(120) NOT NULL,
    description TEXT NOT NULL,
    price_paise BIGINT NOT NULL,
    category_id INT UNSIGNED NOT NULL,
    active      TINYINT(1) NOT NULL DEFAULT 1,
    created_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    UNIQUE KEY uq_products_sku (sku),
    KEY ix_products_category (category_id, active),
    CONSTRAINT ck_products_price CHECK (price_paise >= 0),
    CONSTRAINT fk_products_category FOREIGN KEY (category_id) REFERENCES categories (id)
) ENGINE = InnoDB;

-- The CHECK is the last line of defence against overselling (layer 3).
CREATE TABLE inventory (
    product_id    BIGINT UNSIGNED NOT NULL PRIMARY KEY,
    stock_on_hand INT NOT NULL DEFAULT 0,
    reorder_level INT NOT NULL DEFAULT 10,
    updated_at    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    CONSTRAINT ck_inventory_nonneg CHECK (stock_on_hand >= 0),
    CONSTRAINT ck_inventory_reorder CHECK (reorder_level >= 0),
    CONSTRAINT fk_inventory_product FOREIGN KEY (product_id) REFERENCES products (id)
) ENGINE = InnoDB;

CREATE TABLE carts (
    id         BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    user_id    BIGINT UNSIGNED NOT NULL,
    updated_at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    UNIQUE KEY uq_carts_user (user_id),
    CONSTRAINT fk_carts_user FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE CASCADE
) ENGINE = InnoDB;

CREATE TABLE cart_items (
    cart_id    BIGINT UNSIGNED NOT NULL,
    product_id BIGINT UNSIGNED NOT NULL,
    qty        INT NOT NULL,
    PRIMARY KEY (cart_id, product_id),
    CONSTRAINT ck_cart_items_qty CHECK (qty BETWEEN 1 AND 100),
    CONSTRAINT fk_cart_items_cart FOREIGN KEY (cart_id) REFERENCES carts (id) ON DELETE CASCADE,
    CONSTRAINT fk_cart_items_product FOREIGN KEY (product_id) REFERENCES products (id)
) ENGINE = InnoDB;

CREATE TABLE orders (
    id              BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    user_id         BIGINT UNSIGNED NOT NULL,
    status          VARCHAR(20) NOT NULL,
    total_paise     BIGINT NOT NULL,
    idempotency_key VARCHAR(100) NULL, -- "u<user id>:<client key>"
    wal_txid        BIGINT UNSIGNED NULL, -- the WAL RESERVE this order committed (lockfree-shm)
    created_at      DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at      DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    UNIQUE KEY uq_orders_idem (idempotency_key),
    UNIQUE KEY uq_orders_wal_txid (wal_txid),
    KEY ix_orders_user (user_id, created_at),
    KEY ix_orders_status (status, updated_at),
    CONSTRAINT ck_orders_status CHECK (status IN ('placed', 'payment_pending', 'paid', 'packed',
                                                  'shipped', 'delivered', 'cancelled', 'refunded')),
    CONSTRAINT ck_orders_total CHECK (total_paise >= 0),
    CONSTRAINT fk_orders_user FOREIGN KEY (user_id) REFERENCES users (id)
) ENGINE = InnoDB;

-- Price is copied at purchase time so later price edits never change old orders.
CREATE TABLE order_items (
    order_id         BIGINT UNSIGNED NOT NULL,
    product_id       BIGINT UNSIGNED NOT NULL,
    qty              INT NOT NULL,
    unit_price_paise BIGINT NOT NULL,
    PRIMARY KEY (order_id, product_id),
    CONSTRAINT ck_order_items_qty CHECK (qty > 0),
    CONSTRAINT fk_order_items_order FOREIGN KEY (order_id) REFERENCES orders (id),
    CONSTRAINT fk_order_items_product FOREIGN KEY (product_id) REFERENCES products (id)
) ENGINE = InnoDB;

-- Append-only timeline: drives the tracking page and the audit.
CREATE TABLE order_events (
    id         BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    order_id   BIGINT UNSIGNED NOT NULL,
    from_state VARCHAR(20) NULL,
    to_state   VARCHAR(20) NOT NULL,
    actor      VARCHAR(64) NOT NULL,
    note       VARCHAR(255) NOT NULL DEFAULT '',
    at         DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    KEY ix_order_events_order (order_id),
    CONSTRAINT fk_order_events_order FOREIGN KEY (order_id) REFERENCES orders (id)
) ENGINE = InnoDB;

-- Ledger of every stock change. For each product:
--   SUM(delta) == inventory.stock_on_hand   (checked by the invariant checker)
CREATE TABLE stock_movements (
    id         BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    product_id BIGINT UNSIGNED NOT NULL,
    delta      INT NOT NULL,
    reason     ENUM('initial', 'restock', 'adjust', 'sale', 'release', 'refund') NOT NULL,
    order_id   BIGINT UNSIGNED NULL,
    actor      VARCHAR(64) NOT NULL,
    at         DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    KEY ix_stock_movements_product (product_id, at),
    CONSTRAINT fk_stock_movements_product FOREIGN KEY (product_id) REFERENCES products (id)
) ENGINE = InnoDB;

CREATE TABLE audit_log (
    id       BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    actor_id BIGINT UNSIGNED NULL,
    action   VARCHAR(64) NOT NULL,
    target   VARCHAR(128) NOT NULL DEFAULT '',
    detail   VARCHAR(255) NOT NULL DEFAULT '',
    ip       VARCHAR(45) NOT NULL DEFAULT '',
    at       DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    KEY ix_audit_at (at)
) ENGINE = InnoDB;

-- Stock held for an order. Checkout takes the units off stock_on_hand immediately and
-- records them here with a deadline:
--   active     waiting for payment; the expiry reaper releases it after expires_at
--   committed  the order was paid; the units are sold
--   released   cancelled, declined, timed out or refunded; the units went back
CREATE TABLE reservations (
    order_id   BIGINT UNSIGNED NOT NULL,
    product_id BIGINT UNSIGNED NOT NULL,
    qty        INT NOT NULL,
    state      ENUM('active', 'committed', 'released') NOT NULL DEFAULT 'active',
    expires_at DATETIME(3) NOT NULL,
    created_at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    PRIMARY KEY (order_id, product_id),
    KEY ix_reservations_expiry (state, expires_at),
    CONSTRAINT ck_reservations_qty CHECK (qty > 0),
    CONSTRAINT fk_reservations_order FOREIGN KEY (order_id) REFERENCES orders (id),
    CONSTRAINT fk_reservations_product FOREIGN KEY (product_id) REFERENCES products (id)
) ENGINE = InnoDB;

-- One row per payment attempt. The unique idempotency key makes a duplicated or
-- replayed gateway reply harmless: only one reply can move a row out of 'pending'.
CREATE TABLE payments (
    id            BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    order_id      BIGINT UNSIGNED NOT NULL,
    idem_key      VARCHAR(48) NOT NULL,
    method        VARCHAR(24) NOT NULL,
    status        ENUM('pending', 'succeeded', 'declined', 'timed_out', 'cancelled',
                       'refunded') NOT NULL DEFAULT 'pending',
    amount_paise  BIGINT NOT NULL,
    provider_ref  VARCHAR(32) NOT NULL DEFAULT '',
    note          VARCHAR(255) NOT NULL DEFAULT '',
    created_at    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    UNIQUE KEY uq_payments_idem (idem_key),
    KEY ix_payments_order (order_id),
    CONSTRAINT ck_payments_amount CHECK (amount_paise >= 0),
    CONSTRAINT fk_payments_order FOREIGN KEY (order_id) REFERENCES orders (id)
) ENGINE = InnoDB;

-- Written by of-report, read by the admin dashboards.
CREATE TABLE daily_sales (
    day           DATE NOT NULL PRIMARY KEY,
    orders        INT NOT NULL,
    units         INT NOT NULL,
    revenue_paise BIGINT NOT NULL
) ENGINE = InnoDB;

CREATE TABLE product_sales_daily (
    day           DATE NOT NULL,
    product_id    BIGINT UNSIGNED NOT NULL,
    units         INT NOT NULL,
    revenue_paise BIGINT NOT NULL,
    PRIMARY KEY (day, product_id),
    KEY ix_psd_product (product_id, day)
) ENGINE = InnoDB;

CREATE TABLE report_meta (
    name         VARCHAR(32) NOT NULL PRIMARY KEY,
    generated_at DATETIME(3) NOT NULL,
    duration_ms  INT NOT NULL,
    note         VARCHAR(255) NOT NULL DEFAULT ''
) ENGINE = InnoDB;
