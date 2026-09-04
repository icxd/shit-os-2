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

/* kill(-pgid), spelled the way a shell means it. A pgid of 0 is the caller's
 * own group. */
int killpg(pid_t pgid, int signal);

/* Signal sets are a single word here, because there are 32 signals. The
 * functions exist rather than macros so a caller can take their address. */
int sigemptyset(sigset_t* set);
int sigfillset(sigset_t* set);
int sigaddset(sigset_t* set, int signal);
int sigdelset(sigset_t* set, int signal);
int sigismember(const sigset_t* set, int signal);

int sigprocmask(int how, const sigset_t* set, sigset_t* old);
int sigsuspend(const sigset_t* mask);
int sigpending(sigset_t* set);

/* Names for the signal numbers, for a shell reporting how a job died. */
const char* strsignal(int signal);
int raise(int signal);
sighandler_t signal(int number, sighandler_t handler);
int sigaction(int number, const struct sigaction* action, struct sigaction* old);

#endif /* _SIGNAL_H */
