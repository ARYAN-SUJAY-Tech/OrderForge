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

/*
 * Ledger check in MySQL, plus (when of-core runs in lockfree-shm mode) a comparison of
 * the live shared-memory counters with MySQL. The table is mapped read-only: of-ctl can
 * look but can never change stock.
 */
static int cmd_invariants(void)
{
    db_t *db = connect_db();
    if (!db)
        return 1;

    stock_table_t t;
    const stock_table_t *tp = NULL;
    const char *shm_name = of_config_get(&cfg, "core.shm_name", "/orderforge");
    if (stock_attach(&t, shm_name) == 0) {
        if (t.shm->stock_mode == STOCK_LOCKFREE && atomic_load(&t.shm->started_unix) != 0)
            tp = &t;
        printf("of-core shared memory: mode %s, %llu products%s\n",
               stock_mode_name((stock_mode_t)t.shm->stock_mode),
               (unsigned long long)atomic_load(&t.shm->count),
               tp ? " (compared with MySQL)" : " (not compared: only lockfree-shm keeps it exact)");
    } else {
        printf("of-core shared memory: not found (core not running), checking MySQL only\n");
    }

    buf_t out;
    buf_init(&out);
    int64_t oversold = 0;
    int bad = store_check_invariants(db, tp, &out, &oversold);
    if (out.data)
        fputs(out.data, stdout);
    buf_free(&out);
    if (tp || t.shm)
        stock_close(&t, false);
    if (bad < 0)
        fprintf(stderr, "of-ctl: %s\n", db_error(db));
    db_close(db);
    return bad == 0 ? 0 : 1;
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
