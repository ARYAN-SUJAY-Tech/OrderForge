/*
 * log.h - JSON-lines logger used by every OrderForge process.
 *
 * Each record is one line, written with a single write(2) to a file opened with
 * O_APPEND, so lines from many threads never interleave and no lock is needed:
 *
 *   {"ts":"2026-11-24T10:15:02.123Z","lvl":"info","proc":"of-core","pid":4121,
 *    "tid":4125,"event":"listen","msg":"socket ready"}
 *
 * of_log_reopen() opens the path again and dup2()s it over the old descriptor, which
 * swaps the file atomically. Call it from the main loop after SIGHUP so logrotate
 * works without a restart. Never call any of_log function from a signal handler.
 */
#ifndef OF_LOG_H
#define OF_LOG_H

#include <stddef.h>

typedef enum {
    OF_LOG_DEBUG = 0,
    OF_LOG_INFO  = 1,
    OF_LOG_WARN  = 2,
    OF_LOG_ERROR = 3,
} of_log_level_t;

#define OF_LOG_LINE_MAX 2048

/* path NULL or "-" logs to stderr. Returns 0, or -1 with errno set. */
int  of_log_open(const char *path, const char *proc, of_log_level_t min_level);
int  of_log_reopen(void);
void of_log_close(void);
void of_log_set_level(of_log_level_t min_level);

/* Parse "debug" / "info" / "warn" / "error". Returns -1 for anything else. */
int of_log_level_parse(const char *s);

/* msg is printf-style; the result is JSON-escaped and truncated to fit one line. */
void of_log(of_log_level_t lvl, const char *event, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Escape src as the body of a JSON string. Always NUL-terminates dst (dstlen > 0).
 * Returns the number of bytes written, excluding the NUL. */
size_t of_json_escape(char *dst, size_t dstlen, const char *src);

#define LOG_DEBUG(ev, ...) of_log(OF_LOG_DEBUG, ev, __VA_ARGS__)
#define LOG_INFO(ev, ...)  of_log(OF_LOG_INFO, ev, __VA_ARGS__)
#define LOG_WARN(ev, ...)  of_log(OF_LOG_WARN, ev, __VA_ARGS__)
#define LOG_ERROR(ev, ...) of_log(OF_LOG_ERROR, ev, __VA_ARGS__)

#endif
