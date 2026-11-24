#include "store/db.h"

#include "common/log.h"

#include <errmsg.h>
#include <mysql.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DB_MAX_PARAMS 24
#define DB_STMT_CACHE 96

typedef struct {
    const char *sql;
    MYSQL_STMT *stmt;
} cached_stmt_t;

struct db {
    MYSQL        *conn;
    cached_stmt_t cache[DB_STMT_CACHE];
    size_t        ncached;
    uint64_t      insert_id;
    unsigned      err_no;
    char          err_msg[256];
    bool          broken;
    bool          in_result; /* a db_res_t is open: catches misuse in debug builds */
};

static bool is_connection_error(unsigned e)
{
    return e == CR_SERVER_GONE_ERROR || e == CR_SERVER_LOST || e == CR_CONNECTION_ERROR ||
           e == CR_CONN_HOST_ERROR || e == CR_COMMANDS_OUT_OF_SYNC;
}

static void record_stmt_error(db_t *db, MYSQL_STMT *st, const char *sql)
{
    db->err_no = mysql_stmt_errno(st);
    snprintf(db->err_msg, sizeof db->err_msg, "%s", mysql_stmt_error(st));
    if (is_connection_error(db->err_no))
        db->broken = true;
    /* Constraint violations are expected outcomes (e.g. duplicate email); the caller
     * decides how loud to be. Anything else is worth a log line. */
    if (db->err_no != DB_ER_DUP_ENTRY && db->err_no != DB_ER_CHECK_VIOLATED)
        LOG_WARN("db_error", "%u %s | %.80s", db->err_no, db->err_msg, sql);
}

static void record_conn_error(db_t *db)
{
    db->err_no = mysql_errno(db->conn);
    snprintf(db->err_msg, sizeof db->err_msg, "%s", mysql_error(db->conn));
    if (is_connection_error(db->err_no))
        db->broken = true;
    LOG_WARN("db_error", "%u %s", db->err_no, db->err_msg);
}

db_t *db_connect(const of_config_t *cfg, char *err, size_t errlen)
{
    db_t *db = calloc(1, sizeof *db);
    if (!db) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    db->conn = mysql_init(NULL);
    if (!db->conn) {
        snprintf(err, errlen, "mysql_init failed");
        free(db);
        return NULL;
    }

    unsigned timeout = 5;
    mysql_options(db->conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    mysql_options(db->conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    int ok;
    unsigned port = (unsigned)of_config_get_long(cfg, "db.port", 3306, 1, 65535, &ok);
    const char *sock = of_config_get(cfg, "db.socket", NULL);
    if (!ok ||
        !mysql_real_connect(db->conn, of_config_get(cfg, "db.host", "localhost"),
                            of_config_get(cfg, "db.user", "orderforge"),
                            of_config_get(cfg, "db.password", ""),
                            of_config_get(cfg, "db.name", "orderforge"), port,
                            (sock && *sock) ? sock : NULL, 0)) {
        snprintf(err, errlen, "%s", ok ? mysql_error(db->conn) : "db.port out of range");
        mysql_close(db->conn);
        free(db);
        return NULL;
    }

    /* Session settings: UTC timestamps, strict mode so bad data is an error, not a
     * silent truncation. */
    const char *init = "SET time_zone = '+00:00', "
                       "sql_mode = 'STRICT_ALL_TABLES,NO_ENGINE_SUBSTITUTION,"
                       "ERROR_FOR_DIVISION_BY_ZERO,NO_ZERO_DATE,NO_ZERO_IN_DATE'";
    if (mysql_query(db->conn, init) != 0) {
        snprintf(err, errlen, "%s", mysql_error(db->conn));
        mysql_close(db->conn);
        free(db);
        return NULL;
    }
    return db;
}

void db_close(db_t *db)
{
    if (!db)
        return;
    for (size_t i = 0; i < db->ncached; i++)
        mysql_stmt_close(db->cache[i].stmt);
    mysql_close(db->conn);
    free(db);
}

static MYSQL_STMT *get_stmt(db_t *db, const char *sql)
{
    for (size_t i = 0; i < db->ncached; i++)
        if (db->cache[i].sql == sql || strcmp(db->cache[i].sql, sql) == 0)
            return db->cache[i].stmt;

    MYSQL_STMT *st = mysql_stmt_init(db->conn);
    if (!st) {
        record_conn_error(db);
        return NULL;
    }
    if (mysql_stmt_prepare(st, sql, strlen(sql)) != 0) {
        record_stmt_error(db, st, sql);
        mysql_stmt_close(st);
        return NULL;
    }
    bool update_max = true; /* lets store_result report column widths for sizing buffers */
    mysql_stmt_attr_set(st, STMT_ATTR_UPDATE_MAX_LENGTH, &update_max);

    if (db->ncached < DB_STMT_CACHE) {
        db->cache[db->ncached].sql = sql;
        db->cache[db->ncached].stmt = st;
        db->ncached++;
    } else {
        LOG_WARN("db_cache_full", "statement cache full, raise DB_STMT_CACHE");
    }
    return st;
}

/* Bind and execute. Parameter storage lives on the caller's stack frame (params). */
typedef struct {
    MYSQL_BIND    binds[DB_MAX_PARAMS];
    long long     ints[DB_MAX_PARAMS];
    unsigned long lens[DB_MAX_PARAMS];
    bool          nulls[DB_MAX_PARAMS];
} params_t;

static MYSQL_STMT *prepare_and_execute(db_t *db, params_t *p, const char *sql,
                                       const char *types, va_list ap)
{
    db->err_no = 0;
    db->err_msg[0] = '\0';
    if (db->broken)
        return NULL;

    MYSQL_STMT *st = get_stmt(db, sql);
    if (!st)
        return NULL;

    size_t n = strlen(types);
    if (n > DB_MAX_PARAMS || n != mysql_stmt_param_count(st)) {
        LOG_ERROR("db_params", "placeholder/type mismatch (%zu vs %lu): %.80s", n,
                  mysql_stmt_param_count(st), sql);
        snprintf(db->err_msg, sizeof db->err_msg, "parameter count mismatch");
        db->err_no = 1;
        return NULL;
    }

    memset(p->binds, 0, sizeof(MYSQL_BIND) * n);
    for (size_t i = 0; i < n; i++) {
        MYSQL_BIND *b = &p->binds[i];
        if (types[i] == 'i') {
            int64_t v = va_arg(ap, int64_t);
            b->buffer_type = MYSQL_TYPE_LONGLONG;
            if (v == INT64_MIN) { /* DB_NULL_INT sentinel: bind SQL NULL */
                p->nulls[i] = true;
                b->is_null = &p->nulls[i];
            } else {
                p->ints[i] = (long long)v;
                b->buffer = &p->ints[i];
            }
        } else if (types[i] == 's') {
            const char *s = va_arg(ap, const char *);
            b->buffer_type = MYSQL_TYPE_STRING;
            if (s) {
                p->lens[i] = strlen(s);
                b->buffer = (void *)s;
                b->buffer_length = p->lens[i];
                b->length = &p->lens[i];
            } else {
                p->nulls[i] = true;
                b->is_null = &p->nulls[i];
            }
        } else {
            LOG_ERROR("db_params", "bad type char '%c'", types[i]);
            db->err_no = 1;
            return NULL;
        }
    }

    if ((n > 0 && mysql_stmt_bind_param(st, p->binds) != 0) || mysql_stmt_execute(st) != 0) {
        record_stmt_error(db, st, sql);
        return NULL;
    }
    return st;
}

int db_query(db_t *db, db_res_t *res, const char *sql, const char *types, ...)
{
    memset(res, 0, sizeof *res);
    params_t p;
    memset(&p, 0, sizeof p);

    va_list ap;
    va_start(ap, types);
    MYSQL_STMT *st = prepare_and_execute(db, &p, sql, types, ap);
    va_end(ap);
    if (!st)
        return -1;

    if (mysql_stmt_store_result(st) != 0) {
        record_stmt_error(db, st, sql);
        return -1;
    }

    MYSQL_RES *meta = mysql_stmt_result_metadata(st);
    if (!meta) {
        mysql_stmt_free_result(st);
        return -1;
    }
    unsigned ncols = mysql_num_fields(meta);
    MYSQL_FIELD *fields = mysql_fetch_fields(meta);

    res->stmt = st;
    res->db = db;
    res->ncols = ncols;
    res->binds = calloc(ncols, sizeof(MYSQL_BIND));
    res->bufs = calloc(ncols, sizeof(char *));
    res->lens = calloc(ncols, sizeof(unsigned long));
    res->nulls = calloc(ncols, sizeof(bool));
    if (!res->binds || !res->bufs || !res->lens || !res->nulls) {
        mysql_free_result(meta);
        db_res_free(res);
        return -1;
    }

    MYSQL_BIND *b = res->binds;
    for (unsigned i = 0; i < ncols; i++) {
        /* Every column is fetched as text; max_length is exact thanks to
         * STMT_ATTR_UPDATE_MAX_LENGTH, and numeric columns need at most 21 chars. */
        size_t cap = fields[i].max_length + 1;
        if (cap < 32)
            cap = 32;
        res->bufs[i] = malloc(cap);
        if (!res->bufs[i]) {
            mysql_free_result(meta);
            db_res_free(res);
            return -1;
        }
        b[i].buffer_type = MYSQL_TYPE_STRING;
        b[i].buffer = res->bufs[i];
        b[i].buffer_length = cap - 1;
        b[i].length = &res->lens[i];
        b[i].is_null = &res->nulls[i];
    }
    mysql_free_result(meta);

    if (mysql_stmt_bind_result(st, b) != 0) {
        record_stmt_error(db, st, sql);
        db_res_free(res);
        return -1;
    }
    db->in_result = true;
    return 0;
}

int db_fetch(db_res_t *res)
{
    if (!res->stmt)
        return -1;
    int rc = mysql_stmt_fetch(res->stmt);
    if (rc == MYSQL_NO_DATA)
        return 0;
    if (rc != 0 && rc != MYSQL_DATA_TRUNCATED)
        return -1;
    for (unsigned i = 0; i < res->ncols; i++) {
        unsigned long cap = ((MYSQL_BIND *)res->binds)[i].buffer_length;
        unsigned long len = res->lens[i] < cap ? res->lens[i] : cap;
        res->bufs[i][len] = '\0';
    }
    return 1;
}

const char *db_str(const db_res_t *res, unsigned col)
{
    if (col >= res->ncols || res->nulls[col])
        return NULL;
    return res->bufs[col];
}

int64_t db_int(const db_res_t *res, unsigned col)
{
    const char *s = db_str(res, col);
    return s ? strtoll(s, NULL, 10) : 0;
}

bool db_null(const db_res_t *res, unsigned col)
{
    return col >= res->ncols || res->nulls[col];
}

void db_res_free(db_res_t *res)
{
    if (res->stmt)
        mysql_stmt_free_result(res->stmt);
    if (res->bufs)
        for (unsigned i = 0; i < res->ncols; i++)
            free(res->bufs[i]);
    free(res->bufs);
    free(res->binds);
    free(res->lens);
    free(res->nulls);
    if (res->db)
        res->db->in_result = false;
    memset(res, 0, sizeof *res);
}

int64_t db_exec(db_t *db, const char *sql, const char *types, ...)
{
    params_t p;
    memset(&p, 0, sizeof p);

    va_list ap;
    va_start(ap, types);
    MYSQL_STMT *st = prepare_and_execute(db, &p, sql, types, ap);
    va_end(ap);
    if (!st)
        return -1;

    db->insert_id = mysql_stmt_insert_id(st);
    my_ulonglong n = mysql_stmt_affected_rows(st);
    return n == (my_ulonglong)-1 ? -1 : (int64_t)n;
}

uint64_t db_insert_id(const db_t *db)
{
    return db->insert_id;
}

int db_query_int(db_t *db, int64_t *out, const char *sql, const char *types, ...)
{
    params_t p;
    memset(&p, 0, sizeof p);

    va_list ap;
    va_start(ap, types);
    MYSQL_STMT *st = prepare_and_execute(db, &p, sql, types, ap);
    va_end(ap);
    if (!st)
        return -1;

    long long v = 0;
    bool is_null = false;
    MYSQL_BIND b;
    memset(&b, 0, sizeof b);
    b.buffer_type = MYSQL_TYPE_LONGLONG;
    b.buffer = &v;
    b.is_null = &is_null;

    int found = -1;
    if (mysql_stmt_bind_result(st, &b) == 0) {
        int rc = mysql_stmt_fetch(st);
        if (rc == 0)
            found = 1;
        else if (rc == MYSQL_NO_DATA)
            found = 0;
        else
            record_stmt_error(db, st, sql);
    }
    mysql_stmt_free_result(st);
    if (found == 1)
        *out = is_null ? 0 : v;
    return found;
}

static int simple(db_t *db, const char *sql)
{
    if (db->broken)
        return -1;
    if (mysql_query(db->conn, sql) != 0) {
        record_conn_error(db);
        return -1;
    }
    return 0;
}

int db_begin(db_t *db)    { return simple(db, "START TRANSACTION"); }
int db_commit(db_t *db)   { return simple(db, "COMMIT"); }
int db_rollback(db_t *db) { return simple(db, "ROLLBACK"); }

unsigned db_errno(const db_t *db)      { return db->err_no; }
const char *db_error(const db_t *db)   { return db->err_msg; }
bool db_broken(const db_t *db)         { return db->broken; }
