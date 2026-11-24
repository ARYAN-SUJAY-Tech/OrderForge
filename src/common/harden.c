#include "common/harden.h"

#include "common/config.h"
#include "common/util.h"

#include <errno.h>
#include <grp.h>
#include <libgen.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

/* Make sure a directory exists and belongs to the service account. */
static int own_dir(const char *dir, uid_t uid, gid_t gid, char *err, size_t errlen)
{
    if (!dir || !*dir || strcmp(dir, "-") == 0)
        return 0;
    if (of_mkdir_p(dir, 0750) != 0 || chown(dir, uid, gid) != 0) {
        snprintf(err, errlen, "preparing %s: %s", dir, strerror(errno));
        return -1;
    }
    return 0;
}

static const char *parent_of(const char *path, char *buf, size_t n)
{
    snprintf(buf, n, "%s", path);
    return dirname(buf);
}

int of_drop_privileges(const void *cfgp, const char *proc, char *err, size_t errlen)
{
    const of_config_t *cfg = cfgp;
    if (geteuid() != 0)
        return 0;

    const char *user = of_config_get(cfg, "run.user", "");
    if (!*user) {
        snprintf(err, errlen, "%s refuses to run as root: set run.user = <service account> in "
                              "the config, or start it as that user", proc);
        return -1;
    }
    struct passwd *pw = getpwnam(user);
    if (!pw) {
        snprintf(err, errlen, "run.user '%s' does not exist", user);
        return -1;
    }
    if (pw->pw_uid == 0) {
        snprintf(err, errlen, "run.user must not be root");
        return -1;
    }

    /* Everything the daemons will create must be writable by the account. */
    char b1[512], b2[512];
    if (own_dir(of_config_get(cfg, "run.dir", "run"), pw->pw_uid, pw->pw_gid, err, errlen) ||
        own_dir(of_config_get(cfg, "log.dir", "run/log"), pw->pw_uid, pw->pw_gid, err, errlen) ||
        own_dir(of_config_get(cfg, "report.dir", "run/reports"), pw->pw_uid, pw->pw_gid, err,
                errlen) ||
        own_dir(parent_of(of_config_get(cfg, "core.wal_path", "run/of-core.wal"), b1, sizeof b1),
                pw->pw_uid, pw->pw_gid, err, errlen) ||
        own_dir(parent_of(of_config_get(cfg, "core.socket", "run/of-core.sock"), b2, sizeof b2),
                pw->pw_uid, pw->pw_gid, err, errlen))
        return -1;

    /* Order matters: supplementary groups and gid while we still may, uid last. */
    if (initgroups(pw->pw_name, pw->pw_gid) != 0 || setgid(pw->pw_gid) != 0 ||
        setuid(pw->pw_uid) != 0) {
        snprintf(err, errlen, "switching to %s: %s", user, strerror(errno));
        return -1;
    }
    if (setuid(0) == 0 || geteuid() == 0) {
        snprintf(err, errlen, "root could be regained after dropping to %s", user);
        return -1;
    }
    return 1;
}

void of_harden(const char *conf_path, const void *cfgp, const char *proc)
{
    const of_config_t *cfg = cfgp;
    umask(027);

    if (strcmp(of_config_get(cfg, "run.core_dumps", "0"), "1") != 0) {
        struct rlimit none = {0, 0};
        if (setrlimit(RLIMIT_CORE, &none) != 0 || prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0)
            fprintf(stderr, "%s: warning: could not disable core dumps: %s\n", proc,
                    strerror(errno));
    }

    struct stat st;
    if (conf_path && stat(conf_path, &st) == 0 && (st.st_mode & 077))
        fprintf(stderr,
                "%s: WARNING: %s holds the database password and the payment key but is "
                "readable by %s (mode %03o). Fix it with: chmod 600 %s\n",
                proc, conf_path, (st.st_mode & 007) ? "every user" : "its group",
                (unsigned)(st.st_mode & 0777), conf_path);
}
