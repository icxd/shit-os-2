/* SPDX-License-Identifier: MIT */
#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#include <shitos/abi/wait.h>
#include <sys/types.h>

pid_t wait(int* status);
pid_t waitpid(pid_t pid, int* status, int options);

#endif /* _SYS_WAIT_H */
