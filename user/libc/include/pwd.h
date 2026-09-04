/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- the user database.
 *
 * There are no users. Everything runs as uid 0 and no mode bit is ever
 * checked, so this reports one account and says so honestly rather than
 * failing every lookup -- a shell that cannot find its own $HOME behaves much
 * worse than one told it is root.
 *
 * When there are real users this reads /etc/passwd and nothing above it
 * changes.
 */

#ifndef _PWD_H
#define _PWD_H

#include <sys/types.h>

struct passwd {
    char* pw_name;
    char* pw_passwd;
    uid_t pw_uid;
    gid_t pw_gid;
    char* pw_gecos;
    char* pw_dir;
    char* pw_shell;
};

struct passwd* getpwnam(const char* name);
struct passwd* getpwuid(uid_t uid);

void setpwent(void);
struct passwd* getpwent(void);
void endpwent(void);

#endif /* _PWD_H */
