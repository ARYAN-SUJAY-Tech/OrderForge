#include "common/config.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define OF_CONFIG_FILE_MAX (64 * 1024)

static int valid_key_char(int c)
{
    return isalnum(c) || c == '_' || c == '.' || c == '-';
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
        s[--n] = '\0';
    return s;
}

static int set_entry(of_config_t *cfg, const char *key, const char *val)
{
    for (size_t i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].key, key) == 0) {
            snprintf(cfg->entries[i].val, sizeof cfg->entries[i].val, "%s", val);
            return 0;
        }
    }
    if (cfg->count == OF_CONFIG_MAX_ENTRIES)
        return -1;
    of_config_entry_t *e = &cfg->entries[cfg->count++];
    snprintf(e->key, sizeof e->key, "%s", key);
    snprintf(e->val, sizeof e->val, "%s", val);
    return 0;
}

int of_config_parse(of_config_t *cfg, const char *text, char *err, size_t errlen)
{
    char line[OF_CONFIG_KEY_MAX + OF_CONFIG_VAL_MAX + 64];
    int lineno = 0;

    memset(cfg, 0, sizeof *cfg);
    while (*text) {
        lineno++;
        const char *nl = strchr(text, '\n');
        size_t len = nl ? (size_t)(nl - text) : strlen(text);
        if (len >= sizeof line) {
            snprintf(err, errlen, "line %d: too long", lineno);
            return -1;
        }
        memcpy(line, text, len);
        line[len] = '\0';
        text += len + (nl ? 1 : 0);

        char *s = trim(line);
        if (*s == '\0' || *s == '#' || *s == ';')
            continue;

        char *eq = strchr(s, '=');
        if (!eq) {
            snprintf(err, errlen, "line %d: expected 'key = value'", lineno);
            return -1;
        }
        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);

        if (*key == '\0') {
            snprintf(err, errlen, "line %d: empty key", lineno);
            return -1;
        }
        for (const char *p = key; *p; p++) {
            if (!valid_key_char((unsigned char)*p)) {
                snprintf(err, errlen, "line %d: invalid character in key", lineno);
                return -1;
            }
        }
        if (strlen(key) >= OF_CONFIG_KEY_MAX) {
            snprintf(err, errlen, "line %d: key too long", lineno);
            return -1;
        }

        size_t vlen = strlen(val);
        if (vlen >= 2 && val[0] == '"' && val[vlen - 1] == '"') {
            val[vlen - 1] = '\0';
            val++;
            vlen -= 2;
        }
        if (vlen >= OF_CONFIG_VAL_MAX) {
            snprintf(err, errlen, "line %d: value too long", lineno);
            return -1;
        }
        if (set_entry(cfg, key, val) != 0) {
            snprintf(err, errlen, "line %d: too many entries (max %d)", lineno,
                     OF_CONFIG_MAX_ENTRIES);
            return -1;
        }
    }
    return 0;
}

int of_config_load(of_config_t *cfg, const char *path, char *err, size_t errlen)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(err, errlen, "open %s: %s", path, strerror(errno));
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        snprintf(err, errlen, "fstat %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    if (st.st_size > OF_CONFIG_FILE_MAX) {
        snprintf(err, errlen, "%s: larger than %d bytes", path, OF_CONFIG_FILE_MAX);
        close(fd);
        return -1;
    }

    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf) {
        snprintf(err, errlen, "out of memory");
        close(fd);
        return -1;
    }

    size_t got = 0;
    while (got < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            snprintf(err, errlen, "read %s: %s", path, strerror(errno));
            free(buf);
            close(fd);
            return -1;
        }
        if (n == 0)
            break;
        got += (size_t)n;
    }
    buf[got] = '\0';
    close(fd);

    char perr[128];
    int rc = of_config_parse(cfg, buf, perr, sizeof perr);
    if (rc != 0)
        snprintf(err, errlen, "%s: %s", path, perr);
    free(buf);
    return rc;
}

const char *of_config_get(const of_config_t *cfg, const char *key, const char *def)
{
    for (size_t i = 0; i < cfg->count; i++)
        if (strcmp(cfg->entries[i].key, key) == 0)
            return cfg->entries[i].val;
    return def;
}

long of_config_get_long(const of_config_t *cfg, const char *key, long def,
                        long min, long max, int *ok)
{
    if (ok)
        *ok = 1;
    const char *v = of_config_get(cfg, key, NULL);
    if (!v)
        return def;

    char *end;
    errno = 0;
    long n = strtol(v, &end, 10);
    if (errno != 0 || end == v || *end != '\0' || n < min || n > max) {
        if (ok)
            *ok = 0;
        return -1;
    }
    return n;
}
