#include "common/util.h"

#include "common/config.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

int of_random_bytes(void *buf, size_t len)
{
    unsigned char *p = buf;
    while (len > 0) {
        ssize_t n = getrandom(p, len, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

void of_hex(char *out, const void *data, size_t len)
{
    static const char digits[] = "0123456789abcdef";
    const unsigned char *p = data;
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = digits[p[i] >> 4];
        out[2 * i + 1] = digits[p[i] & 0xF];
    }
    out[2 * len] = '\0';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int of_query_get(const char *query, const char *key, char *out, size_t outlen)
{
    if (!query || outlen == 0)
        return -1;
    size_t klen = strlen(key);
    const char *p = query;

    while (*p) {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(end - p));
        const char *kend = eq ? eq : end;

        if ((size_t)(kend - p) == klen && memcmp(p, key, klen) == 0) {
            size_t o = 0;
            for (const char *v = eq ? eq + 1 : end; v < end; v++) {
                char c = *v;
                if (c == '+') {
                    c = ' ';
                } else if (c == '%') {
                    if (end - v < 3)
                        return -1;
                    int hi = hexval(v[1]), lo = hexval(v[2]);
                    if (hi < 0 || lo < 0 || (hi == 0 && lo == 0))
                        return -1;
                    c = (char)(hi << 4 | lo);
                    v += 2;
                }
                if (o + 1 >= outlen)
                    return -1;
                out[o++] = c;
            }
            out[o] = '\0';
            return 0;
        }
        p = amp ? amp + 1 : end;
    }
    return -1;
}

int of_parse_i64(const char *s, int64_t min, int64_t max, int64_t *out)
{
    if (!s || !*s)
        return -1;
    char *end;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno != 0 || *end != '\0' || v < min || v > max)
        return -1;
    *out = v;
    return 0;
}

int64_t of_query_get_int(const char *query, const char *key, int64_t def,
                         int64_t min, int64_t max)
{
    char tmp[32];
    int64_t v;
    if (of_query_get(query, key, tmp, sizeof tmp) != 0 || tmp[0] == '\0')
        return def;
    if (of_parse_i64(tmp, min, max, &v) != 0)
        return def;
    return v;
}

int of_cookie_get(const char *header, const char *name, char *out, size_t outlen)
{
    if (!header)
        return -1;
    size_t nlen = strlen(name);
    const char *p = header;
    while (*p) {
        while (*p == ' ' || *p == ';')
            p++;
        const char *semi = strchr(p, ';');
        const char *end = semi ? semi : p + strlen(p);
        if ((size_t)(end - p) > nlen && memcmp(p, name, nlen) == 0 && p[nlen] == '=') {
            size_t vlen = (size_t)(end - (p + nlen + 1));
            if (vlen + 1 > outlen)
                return -1;
            memcpy(out, p + nlen + 1, vlen);
            out[vlen] = '\0';
            return 0;
        }
        p = end;
    }
    return -1;
}

int of_mkdir_p(const char *path, unsigned mode)
{
    char buf[512];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof buf) {
        errno = n ? ENAMETOOLONG : ENOENT;
        return -1;
    }
    memcpy(buf, path, n + 1);

    /* Create each prefix in turn: "a", "a/b", "a/b/c". */
    for (char *p = buf + 1; ; p++) {
        if (*p == '/' || *p == '\0') {
            char saved = *p;
            *p = '\0';
            if (mkdir(buf, (mode_t)mode) != 0 && errno != EEXIST)
                return -1;
            *p = saved;
            if (saved == '\0')
                break;
        }
    }

    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

int64_t of_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static pid_t read_pid(int fd)
{
    char buf[32];
    ssize_t n = pread(fd, buf, sizeof buf - 1, 0);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    return (pid_t)strtol(buf, NULL, 10);
}

int of_pidfile_acquire(const char *path, pid_t *holder)
{
    char dir[512];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        of_mkdir_p(dir, 0750);
    }
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int e = errno;
        if (holder)
            *holder = read_pid(fd);
        close(fd);
        errno = e;
        return -1;
    }
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%d\n", (int)getpid());
    if (ftruncate(fd, 0) != 0 || pwrite(fd, buf, (size_t)n, 0) != n) {
        close(fd);
        return -1;
    }
    return fd;
}

int of_pidfile_read(const char *path, pid_t *pid)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    /* If we can take a shared lock, nobody holds the exclusive one: the file is stale. */
    if (flock(fd, LOCK_SH | LOCK_NB) == 0) {
        close(fd);
        return -1;
    }
    *pid = read_pid(fd);
    close(fd);
    return *pid > 0 ? 0 : -1;
}

void of_pidfile_path(const void *cfgp, const char *name, char *out, size_t outlen)
{
    const of_config_t *cfg = cfgp;
    snprintf(out, outlen, "%s/%s.pid", of_config_get(cfg, "run.dir", "run"), name);
}
