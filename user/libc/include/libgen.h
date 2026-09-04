/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- path splitting.
 *
 * Both of these may modify the string they are given and may return a pointer
 * into it. That is the POSIX contract, it is a bad one, and the alternative --
 * a static buffer -- is worse, so it is honoured exactly.
 */

#ifndef _LIBGEN_H
#define _LIBGEN_H

char* basename(char* path);
char* dirname(char* path);

#endif /* _LIBGEN_H */
