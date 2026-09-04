/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- setting a file's timestamps.
 *
 * Nothing can, yet: inodes are stamped when they are written and there is no
 * syscall to say otherwise. utime reports ENOSYS rather than pretending to
 * succeed, so a caller that cares finds out.
 */

#ifndef _UTIME_H
#define _UTIME_H

#include <sys/types.h>

struct utimbuf {
    time_t actime;
    time_t modtime;
};

int utime(const char* path, const struct utimbuf* times);

#endif /* _UTIME_H */
