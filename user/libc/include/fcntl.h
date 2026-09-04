/* SPDX-License-Identifier: MIT */
#ifndef _FCNTL_H
#define _FCNTL_H

#include <shitos/abi/fcntl.h>

#include <sys/types.h>

int open(const char* path, int flags, ...);
int creat(const char* path, mode_t mode);

/*
 * Variadic because the third argument depends on the command: F_DUPFD and
 * F_SETFD/F_SETFL take an int, F_GETFD/F_GETFL take nothing. Passing one where
 * none is wanted is harmless.
 */
int fcntl(int fd, int command, ...);

#endif /* _FCNTL_H */
