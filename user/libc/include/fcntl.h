/* SPDX-License-Identifier: MIT */
#ifndef _FCNTL_H
#define _FCNTL_H

#include <shitos/abi/fcntl.h>

#include <sys/types.h>

int open(const char* path, int flags, ...);
int creat(const char* path, mode_t mode);

#endif /* _FCNTL_H */
