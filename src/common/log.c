#include "common/log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static const char *const level_names[] = {"debug", "info", "warn", "error"};

/* The descriptor never changes after open: reopen swaps the file underneath with dup2. */
static int         log_fd = STDERR_FILENO;
static char        log_path[512];
static char        log_proc[32] = "orderforge";
static atomic_int  log_min = OF_LOG_INFO;

int of_log_open(const char *path, const char *proc, of_log_level_t min_level)
{
    snprintf(log_proc, sizeof log_proc, "%s", proc ? proc : "orderforge");
    atomic_store(&log_min, (int)min_level);

    if (!path || strcmp(path, "-") == 0) {
        log_path[0] = '\0';
        log_fd = STDERR_FILENO;
        return 0;
    }
    if (strlen(path) >= sizeof log_path) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
    if (fd < 0)
        return -1;
    snprintf(log_path, sizeof log_path, "%s", path);
    log_fd = fd;
    return 0;
}

int of_log_reopen(void)
{
    if (log_path[0] == '\0')
        return 0;
    int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
    if (fd < 0)
        return -1;
    /* dup2 atomically replaces log_fd; concurrent writers see either old or new file. */
    if (dup3(fd, log_fd, O_CLOEXEC) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    close(fd);
    return 0;
}

void of_log_close(void)
{
    if (log_fd != STDERR_FILENO) {
        close(log_fd);
        log_fd = STDERR_FILENO;
    }
    log_path[0] = '\0';
}

void of_log_set_level(of_log_level_t min_level)
{
    atomic_store(&log_min, (int)min_level);
}

int of_log_level_parse(const char *s)
{
    for (int i = 0; i < 4; i++)
        if (s && strcmp(s, level_names[i]) == 0)
            return i;
    return -1;
}

size_t of_json_escape(char *dst, size_t dstlen, const char *src)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        char tmp[8];
        const char *rep = NULL;
        size_t rlen = 1;

        switch (*p) {
        case '"':  rep = "\\\""; rlen = 2; break;
        case '\\': rep = "\\\\"; rlen = 2; break;
        case '\n': rep = "\\n";  rlen = 2; break;
        case '\r': rep = "\\r";  rlen = 2; break;
        case '\t': rep = "\\t";  rlen = 2; break;
        default:
            if (*p < 0x20) {
                snprintf(tmp, sizeof tmp, "\\u%04x", *p);
                rep = tmp;
                rlen = 6;
            }
        }
        if (o + rlen >= dstlen)
            break;
        if (rep) {
            memcpy(dst + o, rep, rlen);
        } else {
            dst[o] = (char)*p;
        }
        o += rlen;
    }
    dst[o] = '\0';
    return o;
}

void of_log(of_log_level_t lvl, const char *event, const char *fmt, ...)
{
    if ((int)lvl < atomic_load(&log_min) || lvl > OF_LOG_ERROR)
        return;

    char msg[OF_LOG_LINE_MAX / 2];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    char emsg[OF_LOG_LINE_MAX / 2];
    char eevent[64];
    of_json_escape(emsg, sizeof emsg, msg);
    of_json_escape(eevent, sizeof eevent, event ? event : "");

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);

    char line[OF_LOG_LINE_MAX];
    int n = snprintf(line, sizeof line,
                     "{\"ts\":\"%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ\",\"lvl\":\"%s\","
                     "\"proc\":\"%s\",\"pid\":%d,\"tid\":%ld,\"event\":\"%s\",\"msg\":\"%s\"}\n",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                     tm.tm_sec, ts.tv_nsec / 1000000, level_names[lvl], log_proc,
                     (int)getpid(), (long)syscall(SYS_gettid), eevent, emsg);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof line) {
        n = (int)sizeof line - 1;
        line[n - 1] = '\n';
    }

    /* One write per record keeps lines whole under O_APPEND. A short write or error is
     * dropped: logging must never take the process down. */
    ssize_t w;
    do {
        w = write(log_fd, line, (size_t)n);
    } while (w < 0 && errno == EINTR);
}
