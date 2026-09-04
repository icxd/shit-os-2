/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- the group database, which is one group called root.
 *
 * Same story as <pwd.h>: reporting one account honestly beats failing every
 * lookup, because a program told "there is no such group" behaves much worse
 * than one told "you are root".
 */

#ifndef _GRP_H
#define _GRP_H

#include <sys/types.h>

struct group {
    char* gr_name;
    char* gr_passwd;
    gid_t gr_gid;
    char** gr_mem;
};

struct group* getgrnam(const char* name);
struct group* getgrgid(gid_t gid);

void setgrent(void);
struct group* getgrent(void);
void endgrent(void);

#endif /* _GRP_H */
