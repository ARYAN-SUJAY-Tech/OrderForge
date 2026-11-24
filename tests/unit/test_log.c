#include "check.h"
#include "common/log.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

static char *slurp(const char *path)
{
    static char buf[1 << 20];
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    buf[n < 0 ? 0 : n] = '\0';
    return buf;
}

static int count_lines(const char *s)
{
    int n = 0;
    for (; *s; s++)
        n += *s == '\n';
    return n;
}

static void test_escape(void)
{
    char out[64];
    of_json_escape(out, sizeof out, "a\"b\\c\nd\x01");
    CHECK_STR(out, "a\\\"b\\\\c\\nd\\u0001");

    /* Truncation never splits an escape sequence and always terminates. */
    char tiny[4];
    of_json_escape(tiny, sizeof tiny, "ab\"cd");
    CHECK_STR(tiny, "ab");
}

static void test_levels(void)
{
    CHECK(of_log_level_parse("debug") == OF_LOG_DEBUG);
    CHECK(of_log_level_parse("error") == OF_LOG_ERROR);
    CHECK(of_log_level_parse("loud") == -1);
    CHECK(of_log_level_parse(NULL) == -1);
}

#define THREADS 8
#define PER_THREAD 500

static void *writer(void *arg)
{
    (void)arg;
    for (int i = 0; i < PER_THREAD; i++)
        LOG_INFO("tick", "thread line %d with \"quotes\"", i);
    return NULL;
}

static void test_file_and_threads(void)
{
    char path[] = "/tmp/of_test_log_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);

    CHECK(of_log_open(path, "of-test", OF_LOG_INFO) == 0);
    LOG_DEBUG("hidden", "below the minimum level");
    LOG_INFO("start", "hello %d", 42);

    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++)
        pthread_create(&t[i], NULL, writer, NULL);
    for (int i = 0; i < THREADS; i++)
        pthread_join(t[i], NULL);

    char *s = slurp(path);
    CHECK(s != NULL);
    CHECK(strstr(s, "hidden") == NULL);
    CHECK(strstr(s, "\"proc\":\"of-test\"") != NULL);
    CHECK(strstr(s, "\"msg\":\"hello 42\"") != NULL);
    /* Every record is a whole line: no interleaving across threads. */
    CHECK(count_lines(s) == 1 + THREADS * PER_THREAD);

    /* Simulate logrotate: rename the file, reopen, and check new lines go to a new file. */
    char rotated[64];
    snprintf(rotated, sizeof rotated, "%s.1", path);
    CHECK(rename(path, rotated) == 0);
    CHECK(of_log_reopen() == 0);
    LOG_WARN("after", "rotated");
    s = slurp(path);
    CHECK(s != NULL && count_lines(s) == 1 && strstr(s, "\"lvl\":\"warn\"") != NULL);

    of_log_close();
    unlink(path);
    unlink(rotated);
}

int main(void)
{
    test_escape();
    test_levels();
    test_file_and_threads();
    CHECK_DONE();
}
