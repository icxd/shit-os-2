/* SPDX-License-Identifier: GPL-3.0-or-later */
/* shit os 2 -- open(2) flags and file modes. */

#pragma once

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR 0x0002
#define O_ACCMODE 0x0003

#define O_CREAT 0x0040
#define O_EXCL 0x0080
#define O_TRUNC 0x0200
#define O_APPEND 0x0400
#define O_NONBLOCK 0x0800
#define O_DIRECTORY 0x10000
#define O_CLOEXEC 0x80000

/* --- fcntl(2) ----------------------------------------------------------- */

#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_DUPFD_CLOEXEC 1030

/* The only descriptor flag there is. */
#define FD_CLOEXEC 1

/*
 * The bits fcntl(F_SETFL) is allowed to change. Access mode and creation flags
 * are fixed once the file is open; POSIX says the rest are silently ignored
 * rather than an error.
 */
#define O_SETFL_MASK (O_APPEND | O_NONBLOCK)

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define S_IFMT 0170000
#define S_IFIFO 0010000
#define S_IFCHR 0020000
#define S_IFDIR 0040000
#define S_IFBLK 0060000
#define S_IFREG 0100000
#define S_IFLNK 0120000
/* No sockets exist yet, but software tests for them unconditionally and a
 * missing macro is a compile error rather than a false answer. */
#define S_IFSOCK 0140000

#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

/* Recorded but never checked, like every other mode bit here. */
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000

#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXG 0070
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IRWXO 0007
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001
