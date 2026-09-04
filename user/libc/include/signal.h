/* SPDX-License-Identifier: MIT */
#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <shitos/abi/signal.h>

#include <sys/types.h>

typedef void (*sighandler_t)(int);

/* Writable without tearing even from a signal handler. On x86-64 an aligned
 * int qualifies. */
typedef volatile int sig_atomic_t;

int kill(pid_t pid, int signal);
int raise(int signal);
sighandler_t signal(int number, sighandler_t handler);
int sigaction(int number, const struct sigaction* action, struct sigaction* old);

#endif /* _SIGNAL_H */
