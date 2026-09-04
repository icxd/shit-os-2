/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- the BSD grab bag.
 *
 * Nothing belongs here on purpose; it is where portable software looks for
 * MAXPATHLEN and PIPE_BUF, so it exists to be found.
 */

#ifndef _SYS_PARAM_H
#define _SYS_PARAM_H

#include <limits.h>

/* Software still tests for these to decide which BSD-era interface to use. */
#define BSD 199506
#define BSD4_3 1
#define BSD4_4 1

#define MAXPATHLEN 1024
#define PATH_MAX 1024
#define MAXHOSTNAMELEN 64
#define NOFILE 64

/* The kernel's pipe buffer, less the one slot that is always left empty so a
 * full buffer is distinguishable from an empty one. */
#define PIPE_BUF 4095

#define NBBY 8

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))

#endif /* _SYS_PARAM_H */
