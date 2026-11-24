/*
 * of-ctl - operator command line tool.
 *
 *   of-ctl [-c config] useradd [--admin] EMAIL NAME
 *        Create or update an account. The password is read from the first line of
 *        stdin (so it never appears in `ps` or shell history), e.g.
 *            echo 'secret123' | of-ctl useradd --admin admin@orderforge.local Admin
 *
 *   of-ctl [-c config] invariants
 *        Check the stock ledger: for every product, the sum of stock_movements must
 *        equal inventory.stock_on_hand, and stock must not be negative. A naive-mode
 *        oversell shows up here as products whose ledger went below zero.
 *
 * Later phases add: status, drain, reload, metrics (read from shared memory).
 */
#include "common/config.h"
#include "common/log.h"
#include "store/db.h"
#include "store/store.h"

#include <ctype.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static of_config_t cfg;

static void usage(void)
{
    fprintf(stderr,
            "usage: of-ctl [-c config] useradd [--admin] EMAIL NAME   (password on stdin)\n"
            "       of-ctl [-c config] invariants\n");
    exit(2);
}

static db_t *connect_db(void)
{
    char err[256];
    db_t *db = db_connect(&cfg, err, sizeof err);
    if (!db)
        fprintf(stderr, "of-ctl: database: %s\n", err);
    return db;
}

static int cmd_useradd(int argc, char **argv)
{
    bool admin = false;
    int i = 0;
    if (i < argc && strcmp(argv[i], "--admin") == 0) {
        admin = true;
        i++;
    }
    if (argc - i != 2)
        usage();

    char email[255];
    snprintf(email, sizeof email, "%s", argv[i]);
    for (char *p = email; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    const char *name = argv[i + 1];
    if (!strchr(email, '@') || strlen(name) == 0 || strlen(name) > 100) {
        fprintf(stderr, "of-ctl: invalid email or name\n");
        return 1;
    }

    char password[160];
    if (isatty(STDIN_FILENO))
        fprintf(stderr, "password: ");
    if (!fgets(password, sizeof password, stdin)) {
        fprintf(stderr, "of-ctl: no password on stdin\n");
        return 1;
    }
    password[strcspn(password, "\r\n")] = '\0';
    if (strlen(password) < 8 || strlen(password) > 128) {
        fprintf(stderr, "of-ctl: password must be 8 to 128 characters\n");
        return 1;
    }

    if (sodium_init() < 0) {
        fprintf(stderr, "of-ctl: libsodium init failed\n");
        return 1;
    }
    char hash[crypto_pwhash_STRBYTES];
    int rc = store_hash_password(password, hash, sizeof hash);
    sodium_memzero(password, sizeof password);
    if (rc != 0) {
        fprintf(stderr, "of-ctl: hashing failed\n");
        return 1;
    }

    db_t *db = connect_db();
    if (!db)
        return 1;
    int64_t n = db_exec(db,
                        "INSERT INTO users (email, name, pw_hash, role) VALUES (?, ?, ?, ?) AS new "
                        "ON DUPLICATE KEY UPDATE name = new.name, pw_hash = new.pw_hash, "
                        "  role = new.role, failed_logins = 0, locked_until = NULL",
                        "ssss", email, name, hash, admin ? "admin" : "customer");
    if (n < 0) {
        fprintf(stderr, "of-ctl: %s\n", db_error(db));
        db_close(db);
        return 1;
    }
    printf("%s %s (%s)\n", n == 1 ? "created" : "updated", email, admin ? "admin" : "customer");
    db_close(db);
    return 0;
}

static int cmd_invariants(void)
{
    db_t *db = connect_db();
    if (!db)
        return 1;

    db_res_t r;
    if (db_query(db, &r,
                 "SELECT p.id, p.sku, i.stock_on_hand, "
                 "  COALESCE((SELECT SUM(delta) FROM stock_movements m "
                 "            WHERE m.product_id = p.id), 0) AS ledger, "
                 "  COALESCE((SELECT SUM(oi.qty) FROM order_items oi "
                 "            JOIN orders o ON o.id = oi.order_id "
                 "            WHERE oi.product_id = p.id "
                 "              AND o.status NOT IN ('cancelled', 'refunded')), 0) AS sold "
                 "FROM products p JOIN inventory i ON i.product_id = p.id ORDER BY p.id",
                 "") != 0) {
        fprintf(stderr, "of-ctl: %s\n", db_error(db));
        db_close(db);
        return 1;
    }

    int products = 0, bad = 0;
    int64_t oversold_total = 0;
    printf("%-10s %8s %8s %8s  %s\n", "sku", "on_hand", "ledger", "sold", "result");
    while (db_fetch(&r) == 1) {
        products++;
        int64_t on_hand = db_int(&r, 2), ledger = db_int(&r, 3), sold = db_int(&r, 4);
        bool ok = on_hand >= 0 && ledger == on_hand;
        if (!ok) {
            bad++;
            /* The ledger is the truth about how many units left the shelf. A negative
             * ledger means more units were sold than ever existed. */
            if (ledger < 0)
                oversold_total += -ledger;
            printf("%-10s %8lld %8lld %8lld  MISMATCH%s\n", db_str(&r, 1), (long long)on_hand,
                   (long long)ledger, (long long)sold,
                   ledger < 0 ? " (oversold)" : "");
        }
    }
    db_res_free(&r);
    db_close(db);

    if (bad == 0) {
        printf("all %d products balance: stock_on_hand == SUM(stock_movements), none negative\n",
               products);
        return 0;
    }
    printf("%d of %d products FAIL the invariant; %lld units oversold\n", bad, products,
           (long long)oversold_total);
    return 1;
}

int main(int argc, char **argv)
{
    const char *conf_path = "conf/orderforge.conf";
    int opt;
    while ((opt = getopt(argc, argv, "+c:h")) != -1) {
        if (opt == 'c')
            conf_path = optarg;
        else
            usage();
    }
    if (optind >= argc)
        usage();

    char err[256];
    if (of_config_load(&cfg, conf_path, err, sizeof err) != 0) {
        fprintf(stderr, "of-ctl: %s\n", err);
        return 1;
    }
    of_log_open(NULL, "of-ctl", OF_LOG_ERROR);

    const char *cmd = argv[optind];
    if (strcmp(cmd, "useradd") == 0)
        return cmd_useradd(argc - optind - 1, argv + optind + 1);
    if (strcmp(cmd, "invariants") == 0)
        return cmd_invariants();
    usage();
    return 2;
}
