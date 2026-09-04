/* SPDX-License-Identifier: MIT */
#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <shitos/abi/fcntl.h>
#include <shitos/abi/stat.h>

#include <sys/types.h>
#include <time.h>

int stat(const char* path, struct stat* out);
int fstat(int fd, struct stat* out);
/*
 * Nothing in this filesystem is a symbolic link, so there is no difference to
 * report: lstat gives the same answer as stat. It exists because portable
 * software calls it, and when symlinks arrive this is where they land.
 */
int lstat(const char* path, struct stat* out);
int mkdir(const char* path, mode_t mode);
int chmod(const char* path, mode_t mode);
int fchmod(int fd, mode_t mode);
mode_t umask(mode_t mask);

/*
 * Nothing here has a link count above one, a symbolic link, or a device
 * number, so these report ENOSYS rather than pretending. When a disk
 * filesystem arrives they become real.
 */
int mkfifo(const char* path, mode_t mode);
int mknod(const char* path, mode_t mode, dev_t device);

/* The directory-relative calls. AT_FDCWD and the flag values come from
 * <shitos/abi/fcntl.h>, which sys/stat.h already pulls in. */

#define UTIME_NOW ((1L << 30) - 1L)
#define UTIME_OMIT ((1L << 30) - 2L)

int fstatat(int directory, const char* path, struct stat* out, int flags);
int fchmodat(int directory, const char* path, mode_t mode, int flags);
int mkdirat(int directory, const char* path, mode_t mode);
int utimensat(int directory, const char* path, const struct timespec times[2], int flags);

#endif /* _SYS_STAT_H */
