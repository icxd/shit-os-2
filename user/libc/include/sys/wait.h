/* SPDX-License-Identifier: MIT */
#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#include <shitos/abi/wait.h>

#include <sys/types.h>

pid_t wait(int* status);
pid_t waitpid(pid_t pid, int* status, int options);

/* The BSD spelling, still what a shell reaches for. Resource usage is always
 * zeroed; there is no per-process accounting to report. */
struct rusage;
pid_t wait3(int* status, int options, struct rusage* usage);
pid_t wait4(pid_t pid, int* status, int options, struct rusage* usage);

#endif /* _SYS_WAIT_H */
