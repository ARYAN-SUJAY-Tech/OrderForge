/*
 * Spike 2: run a prepared statement against MySQL with libmysqlclient.
 *
 * Reads db.* keys from a config file (default conf/orderforge.conf), binds one integer
 * parameter, and fetches typed results. No SQL is ever built with string formatting.
 *
 *   usage: mysql_stmt [config-file]
 */
#include "common/config.h"

#include <mysql.h>
#include <stdio.h>
#include <string.h>

static of_config_t cfg;

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "conf/orderforge.conf";
    char err[256];
    if (of_config_load(&cfg, path, err, sizeof err) != 0) {
        fprintf(stderr, "config: %s\n", err);
        return 1;
    }

    int ok;
    unsigned port = (unsigned)of_config_get_long(&cfg, "db.port", 3306, 1, 65535, &ok);
    if (!ok) {
        fprintf(stderr, "config: db.port out of range\n");
        return 1;
    }

    MYSQL *db = mysql_init(NULL);
    if (!db) {
        fprintf(stderr, "mysql_init failed\n");
        return 1;
    }
    if (!mysql_real_connect(db, of_config_get(&cfg, "db.host", "127.0.0.1"),
                            of_config_get(&cfg, "db.user", "orderforge"),
                            of_config_get(&cfg, "db.password", ""),
                            of_config_get(&cfg, "db.name", "orderforge"), port,
                            of_config_get(&cfg, "db.socket", NULL), 0)) {
        fprintf(stderr, "connect: %s\n", mysql_error(db));
        mysql_close(db);
        return 1;
    }

    MYSQL_STMT *st = mysql_stmt_init(db);
    const char *sql = "SELECT ? + 1, VERSION()";
    if (!st || mysql_stmt_prepare(st, sql, strlen(sql)) != 0) {
        fprintf(stderr, "prepare: %s\n", st ? mysql_stmt_error(st) : mysql_error(db));
        return 1;
    }

    long long in = 41;
    MYSQL_BIND param;
    memset(&param, 0, sizeof param);
    param.buffer_type = MYSQL_TYPE_LONGLONG;
    param.buffer = &in;
    if (mysql_stmt_bind_param(st, &param) != 0 || mysql_stmt_execute(st) != 0) {
        fprintf(stderr, "execute: %s\n", mysql_stmt_error(st));
        return 1;
    }

    long long answer = 0;
    char version[64] = "";
    unsigned long version_len = 0;
    MYSQL_BIND res[2];
    memset(res, 0, sizeof res);
    res[0].buffer_type = MYSQL_TYPE_LONGLONG;
    res[0].buffer = &answer;
    res[1].buffer_type = MYSQL_TYPE_STRING;
    res[1].buffer = version;
    res[1].buffer_length = sizeof version - 1;
    res[1].length = &version_len;

    if (mysql_stmt_bind_result(st, res) != 0 || mysql_stmt_fetch(st) != 0) {
        fprintf(stderr, "fetch: %s\n", mysql_stmt_error(st));
        return 1;
    }
    version[version_len < sizeof version ? version_len : sizeof version - 1] = '\0';

    printf("prepared statement ok: 41 + 1 = %lld, server %s\n", answer, version);
    mysql_stmt_close(st);
    mysql_close(db);
    return answer == 42 ? 0 : 1;
}
