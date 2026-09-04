/* SPDX-License-Identifier: MIT */
#ifndef _ERRNO_H
#define _ERRNO_H

#include <shitos/abi/errno.h>

/* A function so that making this thread-local later changes one line. */
int* __errno_location(void);
#define errno (*__errno_location())

/*
 * strerror lives in <string.h> and perror in <stdio.h>, where the standard
 * puts them. They are not redeclared here.
 */

#endif /* _ERRNO_H */
