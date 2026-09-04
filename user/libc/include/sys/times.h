/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- process times.
 *
 * There is no per-process CPU accounting, so the user and system fields are
 * zero and only the return value -- elapsed time in ticks -- means anything.
 * A shell's `times` builtin will report zeroes rather than something invented.
 */

#ifndef _SYS_TIMES_H
#define _SYS_TIMES_H

#include <sys/types.h>

typedef long clock_t_times;

struct tms {
    clock_t tms_utime;
    clock_t tms_stime;
    clock_t tms_cutime;
    clock_t tms_cstime;
};

/* Returns elapsed time since boot in clock ticks, or (clock_t)-1 on error. */
clock_t times(struct tms* out);

#endif /* _SYS_TIMES_H */
