/* SPDX-License-Identifier: MIT */
#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <shitos/abi/signal.h>
#include <sys/types.h>

typedef void (*sighandler_t)(int);

int kill(pid_t pid, int signal);
int raise(int signal);
sighandler_t signal(int number, sighandler_t handler);
int sigaction(int number, const struct sigaction* action, struct sigaction* old);

#endif /* _SIGNAL_H */
