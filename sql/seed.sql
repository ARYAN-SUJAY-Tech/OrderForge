-- OrderForge seed data: categories, products and opening stock.
-- Users are created by `of-ctl useradd` (see scripts/load-db.sh), because their
-- passwords must be hashed with Argon2id.

SET NAMES utf8mb4;

INSERT INTO categories (id, name) VALUES
    (1, 'Electronics'),
    (2, 'Books'),
    (3, 'Clothing'),
    (4, 'Home & Kitchen'),
    (5, 'Sports');

INSERT INTO products (id, sku, name, description, price_paise, category_id) VALUES
    (1,  'HOT-001', 'Limited Edition Mechanical Keyboard',
         'Hot-swappable 75% keyboard with tactile switches. Only 50 made: this is the SKU the oversell test hammers.',
         899900, 1),
    (2,  'ELE-002', 'Wireless Earbuds', 'Bluetooth 5.3 earbuds with active noise cancellation and 30 h battery.', 349900, 1),
    (3,  'ELE-003', 'USB-C Hub 7-in-1', 'HDMI 4K, 3x USB-A, SD and microSD readers, 100 W pass-through charging.', 249900, 1),
    (4,  'ELE-004', '27-inch QHD Monitor', '2560x1440 IPS panel, 144 Hz, height-adjustable stand.', 2299900, 1),
    (5,  'ELE-005', 'Portable SSD 1 TB', 'USB 3.2 Gen 2 external SSD, up to 1050 MB/s.', 699900, 1),
    (6,  'ELE-006', 'Smart Watch', 'AMOLED display, heart-rate and SpO2 sensors, 7-day battery.', 499900, 1),
    (7,  'BOO-001', 'Operating System Concepts', 'The dinosaur book: processes, threads, synchronisation, memory and storage.', 89900, 2),
    (8,  'BOO-002', 'The Linux Programming Interface', 'The definitive guide to Linux and UNIX system programming.', 649900, 2),
    (9,  'BOO-003', 'The C Programming Language', 'Kernighan and Ritchie, second edition.', 49900, 2),
    (10, 'BOO-004', 'Designing Data-Intensive Applications', 'Reliable, scalable and maintainable systems.', 179900, 2),
    (11, 'BOO-005', 'Computer Networking: A Top-Down Approach', 'Application layer to link layer, with Wireshark labs.', 99900, 2),
    (12, 'CLO-001', 'Cotton Crew T-Shirt', '100% combed cotton, regular fit.', 59900, 3),
    (13, 'CLO-002', 'Denim Jacket', 'Classic trucker jacket in mid-wash denim.', 249900, 3),
    (14, 'CLO-003', 'Running Shoes', 'Lightweight mesh upper with cushioned sole.', 399900, 3),
    (15, 'CLO-004', 'Hooded Sweatshirt', 'Brushed fleece hoodie with kangaroo pocket.', 149900, 3),
    (16, 'HOM-001', 'Stainless Steel Pressure Cooker 5 L', 'Induction-compatible, with safety valve.', 279900, 4),
    (17, 'HOM-002', 'Electric Kettle 1.5 L', 'Auto shut-off, boil-dry protection.', 129900, 4),
    (18, 'HOM-003', 'Non-stick Frying Pan 28 cm', 'Granite-coated, PFOA free.', 99900, 4),
    (19, 'HOM-004', 'Ceramic Dinner Set (18 pcs)', 'Microwave and dishwasher safe.', 349900, 4),
    (20, 'SPO-001', 'Yoga Mat 6 mm', 'Non-slip TPE mat with carrying strap.', 79900, 5),
    (21, 'SPO-002', 'Adjustable Dumbbells 20 kg', 'Pair of spin-lock dumbbells with plates.', 299900, 5),
    (22, 'SPO-003', 'Cricket Bat (Kashmir Willow)', 'Full size, pre-knocked.', 189900, 5),
    (23, 'SPO-004', 'Football Size 5', 'Machine-stitched, all-weather.', 69900, 5),
    (24, 'SPO-005', 'Badminton Racket Pair', 'Aluminium frame, with shuttle tube.', 89900, 5);

INSERT INTO inventory (product_id, stock_on_hand, reorder_level) VALUES
    (1, 50, 5), (2, 120, 20), (3, 200, 25), (4, 30, 5), (5, 80, 10), (6, 60, 10),
    (7, 150, 20), (8, 40, 10), (9, 300, 30), (10, 75, 10), (11, 90, 10),
    (12, 500, 50), (13, 70, 10), (14, 110, 15), (15, 160, 20),
    (16, 45, 5), (17, 140, 20), (18, 220, 25), (19, 25, 5),
    (20, 180, 20), (21, 35, 5), (22, 55, 10), (23, 130, 15), (24, 95, 10);

-- Opening balance in the ledger, so SUM(delta) == stock_on_hand from the start.
INSERT INTO stock_movements (product_id, delta, reason, actor)
    SELECT product_id, stock_on_hand, 'initial', 'seed' FROM inventory;
