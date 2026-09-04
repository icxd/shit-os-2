/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- advisory whole-file locks.
 *
 * There is no locking. flock reports success for a lock and for an unlock,
 * which is the correct answer on a system where nothing contends: an advisory
 * lock nobody else can take is a lock you already hold.
 */

#ifndef _SYS_FILE_H
#define _SYS_FILE_H

#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

int flock(int fd, int operation);

#endif /* _SYS_FILE_H */
