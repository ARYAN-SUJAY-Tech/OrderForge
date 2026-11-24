/*
 * db.h - thin, safe layer over libmysqlclient prepared statements.
 *
 * Every query is a prepared statement; values are always bound, never formatted into
 * SQL. Statements are prepared once per connection and cached by their SQL text.
 *
 * Parameters are described by a type string, one character per placeholder:
 *   'i'  int64_t            (pass an int64_t, not an int: varargs do not convert;
 *                            INT64_MIN binds SQL NULL)
 *   's'  const char *       (NULL binds SQL NULL)
 *
 *   db_res_t r;
 *   if (db_query(db, &r, "SELECT id, name FROM products WHERE price_paise < ?",
 *                "i", (int64_t)100000) == 0) {
 *       while (db_fetch(&r) == 1)
 *           printf("%lld %s\n", (long long)db_int(&r, 0), db_str(&r, 1));
 *       db_res_free(&r);
 *   }
 *
 * One connection is used by one thread at a time. A connection-level failure (server
 * gone, lost connection) sets db_broken(); the owning worker should exit and let its
 * parent start a fresh one.
 */
#ifndef OF_DB_H
#define OF_DB_H

#include "common/config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct db db_t;

/* MySQL server error codes the store reacts to. */
#define DB_ER_DUP_ENTRY        1062
#define DB_ER_NO_REFERENCED    1452
#define DB_ER_CHECK_VIOLATED   3819
#define DB_ER_LOCK_DEADLOCK    1213
#define DB_ER_LOCK_WAIT        1205

typedef struct {
    void          *stmt;     /* MYSQL_STMT *, borrowed from the cache */
    unsigned       ncols;
    void          *binds;    /* MYSQL_BIND[ncols] */
    char         **bufs;
    unsigned long *lens;
    bool          *nulls;
    db_t          *db;
} db_res_t;

db_t *db_connect(const of_config_t *cfg, char *err, size_t errlen);
void  db_close(db_t *db);

/* SELECT: 0 and a result to iterate, or -1. Always pair a 0 return with db_res_free. */
int db_query(db_t *db, db_res_t *res, const char *sql, const char *types, ...);

/* 1 when a row is available, 0 at the end, -1 on error. */
int db_fetch(db_res_t *res);

/* Column accessors for the current row. db_str returns NULL for SQL NULL. */
const char *db_str(const db_res_t *res, unsigned col);
int64_t     db_int(const db_res_t *res, unsigned col);
bool        db_null(const db_res_t *res, unsigned col);

void db_res_free(db_res_t *res);

/* INSERT/UPDATE/DELETE: affected row count, or -1 on error. */
int64_t db_exec(db_t *db, const char *sql, const char *types, ...);

/* AUTO_INCREMENT id from the last db_exec on this connection. */
uint64_t db_insert_id(const db_t *db);

/* Single-integer SELECT convenience: 1 found (*out set), 0 no row, -1 error. */
int db_query_int(db_t *db, int64_t *out, const char *sql, const char *types, ...);

int db_begin(db_t *db);
int db_commit(db_t *db);
int db_rollback(db_t *db);

/* Last error from this connection. */
unsigned    db_errno(const db_t *db);
const char *db_error(const db_t *db);
bool        db_broken(const db_t *db);

#endif
