/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- the user database, and resource limits.
 *
 * Neither exists. There are no users, so the database has one entry and it is
 * root; nothing is limited or accounted, so every limit is infinite and every
 * usage is zero. Reporting that is better than failing: a shell that cannot
 * find its own $HOME misbehaves in ways that look nothing like "there is no
 * user database".
 */

#include "internal.h"

#include <errno.h>
#include <pwd.h>
#include <string.h>
#include <sys/resource.h>

static struct passwd s_root = {
    .pw_name = (char*)"root",
    .pw_passwd = (char*)"",
    .pw_uid = 0,
    .pw_gid = 0,
    .pw_gecos = (char*)"root",
    .pw_dir = (char*)"/",
    .pw_shell = (char*)"/bin/sh",
};

static int s_enumerated;

struct passwd* getpwnam(const char* name)
{
    if (name && strcmp(name, "root") == 0)
        return &s_root;
    return NULL;
}

struct passwd* getpwuid(uid_t uid)
{
    return uid == 0 ? &s_root : NULL;
}

void setpwent(void)
{
    s_enumerated = 0;
}

struct passwd* getpwent(void)
{
    return s_enumerated++ == 0 ? &s_root : NULL;
}

void endpwent(void)
{
    s_enumerated = 0;
}

/* --- resource limits ---------------------------------------------------- */

int getrlimit(int resource, struct rlimit* limit)
{
    if (resource < 0 || resource >= RLIM_NLIMITS || !limit) {
        errno = EINVAL;
        return -1;
    }
    limit->rlim_cur = RLIM_INFINITY;
    limit->rlim_max = RLIM_INFINITY;
    return 0;
}

int setrlimit(int resource, const struct rlimit* limit)
{
    if (resource < 0 || resource >= RLIM_NLIMITS || !limit) {
        errno = EINVAL;
        return -1;
    }
    /* Accepting a limit nothing enforces would be a lie a program could act
     * on. Refusing is the honest answer, and callers already handle it. */
    errno = EPERM;
    return -1;
}

int getrusage(int who, struct rusage* usage)
{
    if (!usage) {
        errno = EFAULT;
        return -1;
    }
    if (who != RUSAGE_SELF && who != RUSAGE_CHILDREN) {
        errno = EINVAL;
        return -1;
    }
    /* No per-process accounting exists yet; `times` in a shell will report
     * zero rather than something invented. */
    memset(usage, 0, sizeof(*usage));
    return 0;
}
