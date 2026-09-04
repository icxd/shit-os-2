/* SPDX-License-Identifier: MIT */
#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <shitos/abi/fcntl.h>
#include <shitos/abi/stat.h>

#include <sys/types.h>

int stat(const char* path, struct stat* out);
int fstat(int fd, struct stat* out);
/*
 * Nothing in this filesystem is a symbolic link, so there is no difference to
 * report: lstat gives the same answer as stat. It exists because portable
 * software calls it, and when symlinks arrive this is where they land.
 */
int lstat(const char* path, struct stat* out);
int mkdir(const char* path, mode_t mode);

#endif /* _SYS_STAT_H */
