#include "check.h"
#include "common/config.h"

#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

static of_config_t cfg; /* large struct: keep it off the stack */

static void test_basic(void)
{
    char err[128];
    const char *text =
        "# comment\n"
        "; another comment\n"
        "\n"
        "db.host = 127.0.0.1\n"
        "  db.port=3306  \r\n"
        "db.password = \"with spaces\"\n"
        "core.workers = 8\n"
        "core.workers = 12\n"; /* later duplicate wins */
    CHECK(of_config_parse(&cfg, text, err, sizeof err) == 0);
    CHECK(cfg.count == 4);
    CHECK_STR(of_config_get(&cfg, "db.host", NULL), "127.0.0.1");
    CHECK_STR(of_config_get(&cfg, "db.port", NULL), "3306");
    CHECK_STR(of_config_get(&cfg, "db.password", NULL), "with spaces");
    CHECK_STR(of_config_get(&cfg, "missing", "dflt"), "dflt");

    int ok;
    CHECK(of_config_get_long(&cfg, "core.workers", 4, 1, 256, &ok) == 12 && ok);
    CHECK(of_config_get_long(&cfg, "missing", 4, 1, 256, &ok) == 4 && ok);
    CHECK(of_config_get_long(&cfg, "core.workers", 4, 1, 10, &ok) == -1 && !ok);
    CHECK(of_config_get_long(&cfg, "db.host", 4, 0, LONG_MAX, &ok) == -1 && !ok);
}

static void test_errors(void)
{
    char err[128];
    CHECK(of_config_parse(&cfg, "no equals sign\n", err, sizeof err) == -1);
    CHECK(strstr(err, "line 1") != NULL);
    CHECK(of_config_parse(&cfg, "a = 1\n = 2\n", err, sizeof err) == -1);
    CHECK(strstr(err, "line 2") != NULL);
    CHECK(of_config_parse(&cfg, "bad key = 1\n", err, sizeof err) == -1);

    char big[600];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    big[0] = 'k';
    big[1] = '=';
    CHECK(of_config_parse(&cfg, big, err, sizeof err) == -1);
}

static void test_file(void)
{
    char path[] = "/tmp/of_test_config_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    const char *body = "log.level = debug\n";
    CHECK(write(fd, body, strlen(body)) == (ssize_t)strlen(body));
    close(fd);

    char err[128];
    CHECK(of_config_load(&cfg, path, err, sizeof err) == 0);
    CHECK_STR(of_config_get(&cfg, "log.level", NULL), "debug");
    unlink(path);

    CHECK(of_config_load(&cfg, "/nonexistent/of.conf", err, sizeof err) == -1);
    CHECK(strstr(err, "No such file") != NULL);
}

int main(void)
{
    test_basic();
    test_errors();
    test_file();
    CHECK_DONE();
}
