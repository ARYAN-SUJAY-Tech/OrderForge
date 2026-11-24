/*
 * config.h - "key = value" configuration files shared by every OrderForge binary.
 *
 * Format:
 *   # comment            (also ';' comments)
 *   db.host = 127.0.0.1
 *   db.password = "has spaces"   (optional double quotes are stripped)
 *
 * Keys are [A-Za-z0-9_.-]+. A later duplicate key overrides an earlier one.
 * The table is fixed-size and owned by the caller; nothing is allocated.
 */
#ifndef OF_CONFIG_H
#define OF_CONFIG_H

#include <stddef.h>

#define OF_CONFIG_MAX_ENTRIES 128
#define OF_CONFIG_KEY_MAX     64
#define OF_CONFIG_VAL_MAX     256

typedef struct {
    char key[OF_CONFIG_KEY_MAX];
    char val[OF_CONFIG_VAL_MAX];
} of_config_entry_t;

typedef struct {
    size_t            count;
    of_config_entry_t entries[OF_CONFIG_MAX_ENTRIES];
} of_config_t;

/* Parse text into cfg (cfg is cleared first). Returns 0, or -1 with a message in err. */
int of_config_parse(of_config_t *cfg, const char *text, char *err, size_t errlen);

/* Read and parse a file. Returns 0, or -1 with a message in err. */
int of_config_load(of_config_t *cfg, const char *path, char *err, size_t errlen);

/* Value for key, or def when absent. */
const char *of_config_get(const of_config_t *cfg, const char *key, const char *def);

/*
 * Integer value for key. Returns def when absent. Returns -1 and sets *ok = 0 when the
 * value is present but not a valid integer in [min, max]; *ok is 1 otherwise.
 * ok may be NULL.
 */
long of_config_get_long(const of_config_t *cfg, const char *key, long def,
                        long min, long max, int *ok);

#endif
