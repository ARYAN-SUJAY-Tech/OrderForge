/*
 * harden.h - process hardening shared by every OrderForge daemon.
 *
 * of_drop_privileges  Nothing in OrderForge needs root. If a daemon is started as root
 *                     anyway (a careless sudo, an init script without User=), it switches
 *                     to the account named by run.user before it opens a single file:
 *                     creates and chowns the directories it will write, then
 *                     initgroups + setgid + setuid, and finally checks that setuid(0)
 *                     fails, i.e. root cannot be regained. With no run.user it refuses
 *                     to run as root at all. When not root it does nothing.
 *
 * of_harden           umask 027 (nothing we create is world-readable); no core dumps
 *                     (RLIMIT_CORE = 0 and PR_SET_DUMPABLE = 0), so the database password,
 *                     the payment HMAC key and session data can never end up in a core
 *                     file, and other processes of the same user cannot ptrace us or read
 *                     /proc/<pid>/mem; and a loud warning when the config file, which
 *                     holds those secrets, is readable by group or others.
 *                     Set run.core_dumps = 1 to keep core dumps while debugging.
 */
#ifndef OF_HARDEN_H
#define OF_HARDEN_H

#include <stddef.h>

/* 1 when privileges were dropped, 0 when not running as root, -1 with err on failure. */
int of_drop_privileges(const void *cfg, const char *proc, char *err, size_t errlen);

/* Always succeeds; prints warnings to stderr. */
void of_harden(const char *conf_path, const void *cfg, const char *proc);

#endif
