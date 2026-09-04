/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- signal sets, masking, and names.
 *
 * A sigset_t is one 64-bit word, because there are 32 signals and no realtime
 * ones. That makes every set operation a bit test, and means the mask can be
 * passed to the kernel by value rather than by pointer chase.
 */

#include "internal.h"

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <time.h>
#include <unistd.h>

static int valid_signal(int signal)
{
    if (signal <= 0 || signal >= NSIG) {
        errno = EINVAL;
        return 0;
    }
    return 1;
}

int sigemptyset(sigset_t* set)
{
    if (!set) {
        errno = EINVAL;
        return -1;
    }
    *set = 0;
    return 0;
}

int sigfillset(sigset_t* set)
{
    if (!set) {
        errno = EINVAL;
        return -1;
    }
    *set = ~(sigset_t)0;
    return 0;
}

int sigaddset(sigset_t* set, int signal)
{
    if (!set || !valid_signal(signal))
        return -1;
    *set |= (sigset_t)1 << signal;
    return 0;
}

int sigdelset(sigset_t* set, int signal)
{
    if (!set || !valid_signal(signal))
        return -1;
    *set &= ~((sigset_t)1 << signal);
    return 0;
}

int sigismember(const sigset_t* set, int signal)
{
    if (!set || !valid_signal(signal))
        return -1;
    return (*set & ((sigset_t)1 << signal)) != 0;
}

int sigprocmask(int how, const sigset_t* set, sigset_t* old)
{
    return (int)__syscall_return(__syscall3(SYS_sigprocmask, how, (long)set, (long)old));
}

int sigpending(sigset_t* set)
{
    /* Nothing reports the pending set yet. Saying so beats reporting an empty
     * one, which a caller would read as "nothing is waiting". */
    (void)set;
    errno = ENOSYS;
    return -1;
}

int sigsuspend(const sigset_t* mask)
{
    /*
     * Replace the mask, wait, put it back. There is no kernel call that does
     * all three atomically, and the window between them is exactly where this
     * goes wrong if you are not careful: a signal unblocked by the first step
     * is delivered on the way out of that very syscall, before there is
     * anything here to notice it.
     *
     * So this does not wait for a signal. It waits *briefly* and returns,
     * every time, reporting EINTR as sigsuspend always does. Every caller of
     * sigsuspend is already a loop around a condition it re-tests -- that is
     * the only way the interface can be used correctly -- so a return with
     * nothing delivered costs a re-test, while waiting for a signal that has
     * already arrived costs everything.
     *
     * An earlier version looped until nanosleep reported EINTR. It hung dash
     * whenever SIGCHLD landed during the unblock rather than during the wait,
     * which is most of the time.
     */
    sigset_t previous;
    if (sigprocmask(SIG_SETMASK, mask, &previous) < 0)
        return -1;

    struct timespec interval = { 0, 4000000 };
    nanosleep(&interval, NULL);

    sigprocmask(SIG_SETMASK, &previous, NULL);
    errno = EINTR; /* sigsuspend never reports success */
    return -1;
}

static const char* const SIGNAL_NAMES[NSIG] = {
    [0] = "Unknown signal",
    [SIGHUP] = "Hangup",
    [SIGINT] = "Interrupt",
    [SIGQUIT] = "Quit",
    [SIGILL] = "Illegal instruction",
    [SIGTRAP] = "Trace/breakpoint trap",
    [SIGABRT] = "Aborted",
    [SIGBUS] = "Bus error",
    [SIGFPE] = "Arithmetic exception",
    [SIGKILL] = "Killed",
    [SIGUSR1] = "User defined signal 1",
    [SIGSEGV] = "Segmentation fault",
    [SIGUSR2] = "User defined signal 2",
    [SIGPIPE] = "Broken pipe",
    [SIGALRM] = "Alarm clock",
    [SIGTERM] = "Terminated",
    [SIGCHLD] = "Child exited",
    [SIGCONT] = "Continued",
    [SIGSTOP] = "Stopped (signal)",
    [SIGTSTP] = "Stopped",
    [SIGTTIN] = "Stopped (tty input)",
    [SIGTTOU] = "Stopped (tty output)",
    [SIGURG] = "Urgent I/O condition",
    [SIGWINCH] = "Window changed",
};

const char* strsignal(int signal)
{
    if (signal <= 0 || signal >= NSIG || !SIGNAL_NAMES[signal])
        return "Unknown signal";
    return SIGNAL_NAMES[signal];
}
