/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- limits.
 *
 * The C limits (INT_MAX and friends) belong to the compiler, which knows the
 * widths; include_next reaches clang's freestanding copy rather than
 * duplicating a table that could disagree with the code generator. What is
 * added here is the POSIX half, which is a property of this system.
 */

#ifndef _LIMITS_H
#define _LIMITS_H

#include_next <limits.h>

/* Matches fs::PATH_MAX_LENGTH and fs::FILENAME_MAX_LENGTH in the kernel; a
 * longer path is ENAMETOOLONG rather than truncated. */
#define PATH_MAX 1024
#define NAME_MAX 255

/* MAX_ARGUMENT_BYTES and MAX_ARGUMENTS, from kernel/sched/process.h. */
#define ARG_MAX (32 * 1024)
#define _POSIX_ARG_MAX 4096

/* MAX_FILE_DESCRIPTORS. */
#define OPEN_MAX 64

/* One slot is always left empty, so a full pipe is distinguishable from an
 * empty one without a separate count. */
#define PIPE_BUF 4095

#define LINK_MAX 1
#define MAX_CANON 1024
#define MAX_INPUT 1024
#define SYMLOOP_MAX 8
#define HOST_NAME_MAX 64
#define LOGIN_NAME_MAX 32
#define TTY_NAME_MAX 32

#define NGROUPS_MAX 1
#define SSIZE_MAX 0x7fffffffffffffffL
/* The nice(2) offset. Nothing is nice; there are no priorities. */
#define NZERO 20

/* The minimums POSIX guarantees, which are what portable software sizes its
 * buffers to when it will not trust the real ones. */
#define _POSIX_PATH_MAX 256
#define _POSIX_NAME_MAX 14
#define _POSIX_OPEN_MAX 20
#define _POSIX_PIPE_BUF 512
#define CHILD_MAX 64

#endif /* _LIMITS_H */
