/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- the POSIX surface, checked from ring 3.
 *
 * The kernel's own self tests run before there is a process, so they cannot
 * reach anything that takes a descriptor table or a signal disposition. Lua
 * exercises a lot by accident but only what its own library surfaces. This is
 * where the rest goes: the syscalls a ported program will lean on, checked
 * from the far side of the syscall boundary where a caller actually sits.
 *
 * Run at boot by /etc/rc, so a regression is on the console rather than in a
 * test run nobody does.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int s_checks;
static int s_failures;

static void check(int condition, const char* what)
{
    ++s_checks;
    if (!condition) {
        ++s_failures;
        printf("  FAIL %s\n", what);
    }
}

static void check_errno(int condition, const char* what)
{
    if (!condition)
        printf("  (errno was %d: %s)\n", errno, strerror(errno));
    check(condition, what);
}

/* --- fcntl -------------------------------------------------------------- */

static void test_fcntl(void)
{
    int const fd = open("/tmp/fcntl.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    check_errno(fd >= 0, "open a file to fcntl");
    if (fd < 0)
        return;

    check(fcntl(fd, F_GETFD) == 0, "a fresh descriptor is not close-on-exec");
    check_errno(fcntl(fd, F_SETFD, FD_CLOEXEC) == 0, "F_SETFD accepts FD_CLOEXEC");
    check(fcntl(fd, F_GETFD) == FD_CLOEXEC, "F_GETFD reads it back");
    check_errno(fcntl(fd, F_SETFD, 0) == 0, "F_SETFD clears it again");
    check(fcntl(fd, F_GETFD) == 0, "and F_GETFD agrees");

    int const flags = fcntl(fd, F_GETFL);
    check(flags >= 0 && (flags & O_ACCMODE) == O_RDWR, "F_GETFL reports the access mode");
    check_errno(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0, "F_SETFL sets O_NONBLOCK");
    check((fcntl(fd, F_GETFL) & O_NONBLOCK) != 0, "and it reads back");
    /* The access mode is fixed at open time; POSIX says F_SETFL ignores it
     * rather than failing, so asking for O_RDONLY must change nothing. */
    check_errno(fcntl(fd, F_SETFL, O_RDONLY) == 0, "F_SETFL ignores the access mode");
    check((fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDWR, "the file is still read-write");

    int const duplicate = fcntl(fd, F_DUPFD, 10);
    check_errno(duplicate >= 10, "F_DUPFD returns a descriptor at or above the floor");
    check(fcntl(duplicate, F_GETFD) == 0, "F_DUPFD does not set close-on-exec");

    int const cloexec = fcntl(fd, F_DUPFD_CLOEXEC, 10);
    check_errno(cloexec >= 10, "F_DUPFD_CLOEXEC returns a descriptor");
    check(fcntl(cloexec, F_GETFD) == FD_CLOEXEC, "and marks it close-on-exec");

    /* The duplicate shares the file offset, which is what makes it a dup
     * rather than a second open. */
    check(write(fd, "abcd", 4) == 4, "writing through the original");
    check(lseek(duplicate, 0, SEEK_CUR) == 4, "moves the duplicate's offset too");

    check(close(duplicate) == 0, "closing the duplicate");
    check(close(cloexec) == 0, "closing the close-on-exec duplicate");
    check(close(fd) == 0, "closing the original");
    check(fcntl(fd, F_GETFD) == -1 && errno == EBADF, "fcntl on a closed descriptor is EBADF");

    unlink("/tmp/fcntl.txt");
}

/* O_CLOEXEC and FD_CLOEXEC are only worth anything if exec honours them. */
static void test_close_on_exec(void)
{
    int const kept = open("/tmp/cloexec-keep.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    int const dropped = open("/tmp/cloexec-drop.txt", O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    check_errno(kept >= 0 && dropped >= 0, "opening one plain and one O_CLOEXEC file");
    check(fcntl(dropped, F_GETFD) == FD_CLOEXEC, "O_CLOEXEC sets the flag at open time");

    /* The child reports which descriptors survived exec, through a file,
     * because a pipe would need poll to read without risking a block. */
    pid_t const child = fork();
    check_errno(child >= 0, "forking to test exec");
    if (child == 0) {
        char kept_text[16];
        char dropped_text[16];
        snprintf(kept_text, sizeof(kept_text), "%d", kept);
        snprintf(dropped_text, sizeof(dropped_text), "%d", dropped);
        char* argv[]
            = { (char*)"/bin/usertest", (char*)"--report-descriptors", kept_text, dropped_text, 0 };
        execve("/bin/usertest", argv, environ);
        _exit(127);
    }

    int status = 0;
    waitpid(child, &status, 0);
    check(WIFEXITED(status), "the exec'd child exited normally");
    /* The child exits 0 when the plain descriptor survived and the O_CLOEXEC
     * one did not, which is the whole contract. */
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "exec keeps a plain descriptor and closes an O_CLOEXEC one");

    close(kept);
    close(dropped);
    unlink("/tmp/cloexec-keep.txt");
    unlink("/tmp/cloexec-drop.txt");
}

static int report_descriptors(int kept, int dropped)
{
    int const kept_alive = fcntl(kept, F_GETFD) != -1;
    int const dropped_alive = fcntl(dropped, F_GETFD) != -1;
    return (kept_alive && !dropped_alive) ? 0 : 1;
}

/* --- rename ------------------------------------------------------------- */

static void test_rename(void)
{
    int fd = open("/tmp/ren-a.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    check_errno(fd >= 0, "creating a file to rename");
    if (fd < 0)
        return;
    write(fd, "contents", 8);
    close(fd);

    check_errno(rename("/tmp/ren-a.txt", "/tmp/ren-b.txt") == 0, "rename moves a file");
    check(open("/tmp/ren-a.txt", O_RDONLY, 0) < 0, "the old name is gone");

    fd = open("/tmp/ren-b.txt", O_RDONLY, 0);
    check_errno(fd >= 0, "the new name opens");
    if (fd >= 0) {
        char buffer[16] = { 0 };
        read(fd, buffer, sizeof(buffer) - 1);
        check(strcmp(buffer, "contents") == 0, "with the contents intact");
        close(fd);
    }

    /* Write-to-temp-then-rename is the reason rename has to replace silently. */
    fd = open("/tmp/ren-c.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    write(fd, "replaced", 8);
    close(fd);
    check_errno(
        rename("/tmp/ren-b.txt", "/tmp/ren-c.txt") == 0, "rename replaces an existing file");
    fd = open("/tmp/ren-c.txt", O_RDONLY, 0);
    if (fd >= 0) {
        char buffer[16] = { 0 };
        read(fd, buffer, sizeof(buffer) - 1);
        check(strcmp(buffer, "contents") == 0, "the replacement won, not the replaced");
        close(fd);
    }

    check(rename("/tmp/does-not-exist", "/tmp/ren-d.txt") == -1 && errno == ENOENT,
        "renaming a missing file is ENOENT");
    check(rename("/tmp/ren-c.txt", "/dev/ren-c.txt") == -1 && errno == EXDEV,
        "renaming across filesystems is EXDEV");

    unlink("/tmp/ren-c.txt");
}

/* --- the clock ---------------------------------------------------------- */

static void test_clock(void)
{
    struct timespec monotonic_first;
    check_errno(clock_gettime(CLOCK_MONOTONIC, &monotonic_first) == 0, "clock_gettime MONOTONIC");

    struct timespec wall;
    check_errno(clock_gettime(CLOCK_REALTIME, &wall) == 0, "clock_gettime REALTIME");
    check(clock_gettime(-1, &wall) == -1 && errno == EINVAL, "an unknown clock is EINVAL");

    /* The RTC module registers a real date at boot. Anything after 2020 means
     * the CMOS was read and the offset applied; without it the clock would
     * still be counting from boot and this would be a handful of seconds. */
    time_t const now = time(NULL);
    check(now > 1577836800, "time() is a real date, not seconds since boot");

    struct tm* const broken = gmtime(&now);
    check(broken != NULL && broken->tm_year + 1900 >= 2020, "gmtime agrees");

    /* And the round trip has to close, or every timestamp comparison is wrong. */
    struct tm copy = *broken;
    check(mktime(&copy) == now, "mktime inverts gmtime");

    char formatted[32];
    check(strftime(formatted, sizeof(formatted), "%Y-%m-%d", broken) == 10,
        "strftime renders a date");

    struct timeval timeval_now;
    check_errno(gettimeofday(&timeval_now, NULL) == 0, "gettimeofday");
    check(timeval_now.tv_sec >= now, "and agrees with time()");

    /* Monotonic must move forward across a sleep and never jump backwards. */
    usleep(20000);
    struct timespec monotonic_second;
    clock_gettime(CLOCK_MONOTONIC, &monotonic_second);
    check(monotonic_second.tv_sec > monotonic_first.tv_sec
            || (monotonic_second.tv_sec == monotonic_first.tv_sec
                && monotonic_second.tv_nsec > monotonic_first.tv_nsec),
        "the monotonic clock advances across a sleep");
}

/* --- file timestamps ---------------------------------------------------- */

static void test_stat_times(void)
{
    time_t const before = time(NULL);
    int const fd = open("/tmp/stamped.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    check_errno(fd >= 0, "creating a file to stat");
    if (fd < 0)
        return;
    write(fd, "x", 1);

    struct stat status;
    check_errno(fstat(fd, &status) == 0, "fstat");
    check(status.st_mtime >= before, "a new file is stamped with the current time");
    check(status.st_mtime == status.st_ctime, "and mtime matches ctime on creation");

    close(fd);
    unlink("/tmp/stamped.txt");
}

/* --- job control -------------------------------------------------------- */

/* Waits for a status change with a bound, so a kernel bug fails the test
 * rather than hanging the boot. */
static pid_t wait_bounded(pid_t pid, int* status, int options)
{
    for (int attempt = 0; attempt < 500; ++attempt) {
        pid_t const result = waitpid(pid, status, options | WNOHANG);
        if (result != 0)
            return result;
        usleep(4000);
    }
    return -1;
}

static void test_process_groups(void)
{
    pid_t const self = getpid();
    check(getpgrp() == getpgid(0), "getpgrp agrees with getpgid(0)");
    check(getsid(0) > 0, "the process is in a session");

    /* A child starts in its parent's job, which is what makes a pipeline one
     * group without anyone arranging it. */
    pid_t child = fork();
    if (child == 0)
        _exit(getpgid(0) == getpgid(getppid()) ? 0 : 1);
    int status = 0;
    wait_bounded(child, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "a child inherits its parent's group");

    /* ...and can leave it, which is what a shell does to make a job. */
    child = fork();
    if (child == 0) {
        if (setpgid(0, 0) < 0)
            _exit(1);
        _exit(getpgid(0) == getpid() ? 0 : 2);
    }
    /* Both sides call setpgid, because either may run first and the parent
     * must be able to signal the group immediately. */
    setpgid(child, child);
    check(getpgid(child) == child, "the parent sees the child's new group");
    wait_bounded(child, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "setpgid(0, 0) makes a child a leader");

    check(setpgid(999999, 0) == -1 && errno == ESRCH, "setpgid on a missing process is ESRCH");
    (void)self;
}

/* kill(-pgid) has to reach every member, which is the whole reason ^C works
 * on a pipeline rather than on whichever member last read the terminal. */
static void test_group_signals(void)
{
    pid_t group = 0;
    pid_t children[3];

    for (int i = 0; i < 3; ++i) {
        pid_t const child = fork();
        if (child == 0) {
            setpgid(0, group != 0 ? group : 0);
            /* Sleep long enough that the signal is what ends this, not the
             * clock. */
            for (int spin = 0; spin < 1000; ++spin)
                usleep(10000);
            _exit(0);
        }
        if (group == 0)
            group = child;
        setpgid(child, group);
        children[i] = child;
    }

    check(killpg(group, SIGTERM) == 0, "killpg signals the group");

    int reached = 0;
    for (int i = 0; i < 3; ++i) {
        int status = 0;
        if (wait_bounded(children[i], &status, 0) == children[i] && WIFSIGNALED(status)
            && WTERMSIG(status) == SIGTERM)
            ++reached;
    }
    check(reached == 3, "every process in the group got it");
}

static void test_stop_and_continue(void)
{
    pid_t const child = fork();
    if (child == 0) {
        setpgid(0, 0);
        /* Long enough that only a signal ends it. */
        for (int spin = 0; spin < 2000; ++spin)
            usleep(10000);
        _exit(3);
    }
    setpgid(child, child);

    check(kill(child, SIGSTOP) == 0, "SIGSTOP a running child");

    int status = 0;
    check(wait_bounded(child, &status, WUNTRACED) == child, "waitpid WUNTRACED reports the stop");
    check(WIFSTOPPED(status), "and the status says stopped");
    check(WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP, "with the signal that did it");
    check(!WIFEXITED(status) && !WIFSIGNALED(status),
        "a stopped status is neither exited nor signalled");

    /* A stopped process is not running to deliver its own SIGCONT, so the
     * kernel has to act on it at the moment it is raised. */
    check(kill(child, SIGCONT) == 0, "SIGCONT a stopped child");
    check(wait_bounded(child, &status, WCONTINUED) == child, "waitpid WCONTINUED reports it");
    check(WIFCONTINUED(status), "and the status says continued");

    /* Same for SIGKILL: a stopped process must still be killable. */
    check(kill(child, SIGSTOP) == 0, "stop it again");
    check(wait_bounded(child, &status, WUNTRACED) == child, "the second stop is reported");
    check(kill(child, SIGKILL) == 0, "SIGKILL a stopped child");
    check(wait_bounded(child, &status, 0) == child, "and it actually dies");
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "killed by SIGKILL");
}

static volatile int s_caught_tstp;

static void catch_tstp(int signal)
{
    (void)signal;
    s_caught_tstp = 1;
}

/* SIGTSTP stops by default but can be caught; SIGSTOP cannot be caught at
 * all. That difference is what lets an editor save its state on ^Z. */
static void test_stop_signal_dispositions(void)
{
    pid_t const child = fork();
    if (child == 0) {
        signal(SIGTSTP, catch_tstp);
        setpgid(0, 0);
        for (int spin = 0; spin < 500 && !s_caught_tstp; ++spin)
            usleep(4000);
        _exit(s_caught_tstp ? 0 : 1);
    }
    setpgid(child, child);
    usleep(20000);

    check(kill(child, SIGTSTP) == 0, "SIGTSTP a child that catches it");
    int status = 0;
    check(wait_bounded(child, &status, WUNTRACED) == child, "the child reports something");
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "a caught SIGTSTP runs the handler instead of stopping");
}

/* A background job that reads the terminal is stopped rather than allowed to
 * steal input the foreground job is waiting for. */
static void test_background_read(void)
{
    /*
     * /etc/rc runs this from a non-interactive shell, which by POSIX does no
     * job control -- so nothing has claimed the terminal and there is no
     * foreground group to be in the background of. Claim it here, which is
     * exactly what an interactive shell would have done.
     */
    if (!isatty(STDIN_FILENO))
        return;
    if (tcsetpgrp(STDIN_FILENO, getpgrp()) < 0)
        return;

    pid_t const foreground = tcgetpgrp(STDIN_FILENO);
    check(foreground == getpgrp(), "this process group owns the terminal");

    pid_t const child = fork();
    if (child == 0) {
        setpgid(0, 0); /* leaves the foreground group */
        char scratch[1];
        /* Must not block: the kernel signals rather than queues us behind the
         * foreground reader. */
        (void)read(STDIN_FILENO, scratch, sizeof(scratch));
        _exit(0);
    }
    setpgid(child, child);

    int status = 0;
    check(wait_bounded(child, &status, WUNTRACED) == child, "the background reader reports");
    check(WIFSTOPPED(status) && WSTOPSIG(status) == SIGTTIN,
        "reading the terminal from the background stops the reader with SIGTTIN");

    kill(child, SIGKILL);
    wait_bounded(child, &status, 0);

    /* The terminal must still belong to whoever had it. */
    check(tcgetpgrp(STDIN_FILENO) == foreground, "and the terminal did not change hands");
}

static void test_terminal_ownership(void)
{
    if (!isatty(STDIN_FILENO))
        return;

    pid_t const original = tcgetpgrp(STDIN_FILENO);
    check(tcsetpgrp(STDIN_FILENO, getpgrp()) == 0, "tcsetpgrp to our own group");
    check(tcgetpgrp(STDIN_FILENO) == getpgrp(), "tcgetpgrp reads it back");

    /* Handing the terminal to a group that does not exist would leave it owned
     * by nothing, and every later read would stop its caller. */
    check(tcsetpgrp(STDIN_FILENO, 999999) == -1, "tcsetpgrp refuses a group that does not exist");
    check(tcgetpgrp(STDIN_FILENO) == getpgrp(), "and left the owner alone");

    if (original > 0)
        tcsetpgrp(STDIN_FILENO, original);
}

static void test_sessions(void)
{
    /* A group leader cannot start a session: its group would end up split
     * across two, which is the one thing the hierarchy forbids. */
    pid_t const child = fork();
    if (child == 0) {
        setpgid(0, 0); /* now a group leader */
        if (setsid() != -1 || errno != EPERM)
            _exit(1);

        /* A grandchild is not a leader, so it can. */
        pid_t const grandchild = fork();
        if (grandchild == 0) {
            pid_t const session = setsid();
            _exit(session == getpid() && getsid(0) == getpid() ? 0 : 2);
        }
        int status = 0;
        waitpid(grandchild, &status, 0);
        _exit(WIFEXITED(status) ? WEXITSTATUS(status) : 3);
    }
    setpgid(child, child);

    int status = 0;
    wait_bounded(child, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "setsid is refused for a group leader and allowed for anyone else");
}

/* --- catching signals and coming back ------------------------------------
 *
 * Everything else here checks that a signal *arrives*. This checks that the
 * program is still standing afterwards: the handler runs, sigreturn puts the
 * interrupted context back, and the code that was running carries on with its
 * registers and its stack intact. Nothing tested that until dash caught a
 * SIGTERM and died in the next call it made.
 */

static volatile int s_handler_ran;
static volatile int s_handler_signal;

static void note_signal(int signal)
{
    s_handler_ran = 1;
    s_handler_signal = signal;
}

static void test_signal_return(void)
{
    struct sigaction action;
    struct sigaction previous;
    memset(&action, 0, sizeof(action));
    action.sa_handler = note_signal;
    action.sa_flags = SA_RESTART;
    check_errno(sigaction(SIGTERM, &action, &previous) == 0, "installing a SIGTERM handler");

    /* Values in callee-saved registers and on the stack, so that a handler
     * that clobbers them shows up here rather than three calls later. */
    volatile long guard = 0x0123456789ABCDEFL;
    char scratch[64];
    memset(scratch, 0x5A, sizeof(scratch));

    s_handler_ran = 0;
    check(raise(SIGTERM) == 0, "raising it at ourselves");
    check(s_handler_ran == 1, "the handler ran");
    check(s_handler_signal == SIGTERM, "with the right signal number");
    check(guard == 0x0123456789ABCDEFL, "and left our locals alone");
    check(scratch[0] == 0x5A && scratch[63] == 0x5A, "and our stack alone");

    /* The allocator is the first thing to notice a wrecked context, because
     * it is what the next call usually reaches for. */
    char* const allocated = malloc(128);
    check(allocated != NULL, "malloc still works after a handler returned");
    if (allocated) {
        memset(allocated, 'x', 128);
        free(allocated);
    }

    char* const copied = strdup("after the handler");
    check(copied != NULL && strcmp(copied, "after the handler") == 0,
        "and so does strdup, which is where dash died");
    free(copied);

    /* Twice, because a handler that half-restores may only show on the
     * second pass through. */
    s_handler_ran = 0;
    raise(SIGTERM);
    check(s_handler_ran == 1, "a second signal is caught too");
    check(guard == 0x0123456789ABCDEFL, "locals survive the second one");

    /*
     * The bug this test exists for. sa_restorer is a libc-internal field, so a
     * program that fills a struct sigaction in field by field -- as dash does,
     * and as most software does -- leaves stack garbage in it. If the libc
     * honours that instead of overwriting it, the handler returns to a random
     * address and the program dies somewhere unrelated.
     */
    struct sigaction dirty;
    memset(&dirty, 0xAB, sizeof(dirty));
    dirty.sa_handler = note_signal;
    dirty.sa_mask = 0;
    dirty.sa_flags = 0;
    check_errno(sigaction(SIGTERM, &dirty, NULL) == 0, "installing from an unzeroed struct");

    s_handler_ran = 0;
    raise(SIGTERM);
    check(s_handler_ran == 1, "the handler still ran");
    check(guard == 0x0123456789ABCDEFL, "and returning did not wreck the caller");

    char* const after_dirty = strdup("still here");
    check(after_dirty != NULL && strcmp(after_dirty, "still here") == 0,
        "a garbage sa_restorer is overwritten rather than honoured");
    free(after_dirty);

    /* And what comes back in `old` must not be the libc's own trampoline: a
     * caller that saves and restores a disposition would pass it back in. */
    struct sigaction saved;
    sigaction(SIGTERM, NULL, &saved);
    check(saved.sa_restorer == NULL, "sigaction does not hand back the internal restorer");

    sigaction(SIGTERM, &previous, NULL);
}

/* --- printf length modifiers ---------------------------------------------
 *
 * A length modifier the formatter does not recognise is not a cosmetic
 * problem. It never sees the conversion that follows it, so it consumes no
 * argument, and every later conversion in the same call reads the wrong one.
 * sbase's du printed a block count with %jd and the %s after it took that
 * number as a pointer; the crash was three frames away from the cause.
 */

static void check_format(const char* what, const char* rendered, const char* expected)
{
    ++s_checks;
    if (strcmp(rendered, expected) != 0) {
        ++s_failures;
        printf("  FAIL %s: expected [%s], got [%s]\n", what, expected, rendered);
    }
}

static void test_printf_lengths(void)
{
    char out[128];

    snprintf(out, sizeof(out), "%d", 42);
    check_format("%d", out, "42");
    snprintf(out, sizeof(out), "%ld", 42L);
    check_format("%ld", out, "42");
    snprintf(out, sizeof(out), "%lld", 42LL);
    check_format("%lld", out, "42");
    snprintf(out, sizeof(out), "%zu", (size_t)42);
    check_format("%zu", out, "42");
    snprintf(out, sizeof(out), "%jd", (intmax_t)-42);
    check_format("%jd", out, "-42");
    snprintf(out, sizeof(out), "%ju", (uintmax_t)42);
    check_format("%ju", out, "42");
    snprintf(out, sizeof(out), "%td", (ptrdiff_t)-42);
    check_format("%td", out, "-42");
    snprintf(out, sizeof(out), "%hd", (short)42);
    check_format("%hd", out, "42");
    snprintf(out, sizeof(out), "%hhd", (signed char)42);
    check_format("%hhd", out, "42");

    /* The failure that matters: a conversion after an unrecognised modifier
     * must still read its own argument. */
    snprintf(out, sizeof(out), "%jd\t%s", (intmax_t)7, "after");
    check_format("%jd then %s", out, "7\tafter");
    snprintf(out, sizeof(out), "%zu:%s:%d", (size_t)1, "mid", 2);
    check_format("%zu then %s then %d", out, "1:mid:2");
    snprintf(out, sizeof(out), "%ju %ju", (uintmax_t)1, (uintmax_t)2);
    check_format("two %ju in a row", out, "1 2");

    /* Widths and precisions alongside a modifier. */
    snprintf(out, sizeof(out), "%8jd|", (intmax_t)42);
    check_format("%8jd", out, "      42|");
    snprintf(out, sizeof(out), "%-8ju|", (uintmax_t)42);
    check_format("%-8ju", out, "42      |");
    snprintf(out, sizeof(out), "%08jx", (uintmax_t)0xABCD);
    check_format("%08jx", out, "0000abcd");
}

/* --- poll and select ---------------------------------------------------- */

static void test_poll(void)
{
    int fds[2];
    check_errno(pipe(fds) == 0, "pipe for poll");

    struct pollfd entries[2];
    entries[0].fd = fds[0];
    entries[0].events = POLLIN;
    entries[0].revents = 0;
    entries[1].fd = fds[1];
    entries[1].events = POLLOUT;
    entries[1].revents = 0;

    /* An empty pipe is not readable, and an empty pipe is writable. A zero
     * timeout makes this a question rather than a wait. */
    int ready = poll(entries, 2, 0);
    check_errno(ready >= 0, "poll with a zero timeout returns");
    check(ready == 1, "only the writable end is ready");
    check((entries[0].revents & POLLIN) == 0, "an empty pipe is not readable");
    check((entries[1].revents & POLLOUT) != 0, "an empty pipe is writable");

    check(write(fds[1], "x", 1) == 1, "writing a byte into the pipe");
    entries[0].revents = entries[1].revents = 0;
    ready = poll(entries, 2, 0);
    check(ready == 2, "now both ends are ready");
    check((entries[0].revents & POLLIN) != 0, "and the read end says so");

    /* Closing the writer is a hangup, and it must be reported whether or not
     * the caller asked -- otherwise a reader waits for data that cannot come. */
    char scratch[4];
    check(read(fds[0], scratch, 1) == 1, "draining the byte");
    close(fds[1]);
    entries[0].events = POLLIN;
    entries[0].revents = 0;
    ready = poll(entries, 1, 0);
    check(ready == 1, "a pipe with no writers is ready");
    check((entries[0].revents & POLLHUP) != 0, "and reports POLLHUP");
    close(fds[0]);

    /* A closed descriptor is POLLNVAL, not an error return. */
    entries[0].fd = fds[0];
    entries[0].events = POLLIN;
    entries[0].revents = 0;
    check(poll(entries, 1, 0) == 1, "polling a closed descriptor returns it as ready");
    check((entries[0].revents & POLLNVAL) != 0, "with POLLNVAL");

    /* A negative fd is how a caller skips an entry without shuffling. */
    entries[0].fd = -1;
    entries[0].events = POLLIN;
    entries[0].revents = 0;
    check(poll(entries, 1, 0) == 0, "a negative fd is skipped");
    check(entries[0].revents == 0, "and its revents is cleared");

    /* And a timeout has to actually elapse. */
    struct timespec before;
    struct timespec after;
    clock_gettime(CLOCK_MONOTONIC, &before);
    check(poll(NULL, 0, 40) == 0, "poll with nothing to watch times out");
    clock_gettime(CLOCK_MONOTONIC, &after);
    long const elapsed_ms = (long)((after.tv_sec - before.tv_sec) * 1000
        + (after.tv_nsec - before.tv_nsec) / 1000000);
    check(elapsed_ms >= 30, "and waited roughly the requested time");
}

static void test_select(void)
{
    int fds[2];
    check_errno(pipe(fds) == 0, "pipe for select");

    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(fds[0], &readable);

    struct timeval immediately = { 0, 0 };
    int ready = select(fds[0] + 1, &readable, NULL, NULL, &immediately);
    check_errno(ready >= 0, "select returns");
    check(ready == 0, "an empty pipe is not readable");
    check(!FD_ISSET(fds[0], &readable), "and the set was cleared");

    check(write(fds[1], "y", 1) == 1, "writing into the pipe");
    FD_ZERO(&readable);
    FD_SET(fds[0], &readable);
    immediately.tv_sec = 0;
    immediately.tv_usec = 0;
    ready = select(fds[0] + 1, &readable, NULL, NULL, &immediately);
    check(ready == 1, "now it is readable");
    check(FD_ISSET(fds[0], &readable), "and select says which");

    /* Both directions at once, on two different descriptors. */
    fd_set writable;
    FD_ZERO(&readable);
    FD_ZERO(&writable);
    FD_SET(fds[0], &readable);
    FD_SET(fds[1], &writable);
    immediately.tv_sec = 0;
    immediately.tv_usec = 0;
    ready = select(fds[1] + 1, &readable, &writable, NULL, &immediately);
    check(ready == 2, "select counts both sets");
    check(FD_ISSET(fds[0], &readable) && FD_ISSET(fds[1], &writable), "and reports both");

    close(fds[0]);
    close(fds[1]);
}

/*
 * Shared memory, which here is just a tmpfs file mapped MAP_SHARED -- which is
 * what POSIX shared memory has always actually been. The window server passes
 * every window's pixels this way, so all of it matters:
 *
 *   - two mappings of one file are the same memory, not two copies;
 *   - a write through the mapping is a write to the file;
 *   - the mapping survives fork as shared memory, so parent and child see each
 *     other's stores;
 *   - and unmapping, or exiting, does not corrupt anything -- if the frames
 *     were wrongly freed back to the allocator this test would pass and the
 *     ones after it would fail strangely, which is why it runs before them.
 */
static void test_shared_memory(void)
{
    const char* path = "/tmp/shm.bin";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    check(fd >= 0, "creating a file to share");
    if (fd < 0)
        return;

    size_t const size = 8192; /* two pages, so boundaries are exercised */
    check(ftruncate(fd, size) == 0, "sizing it before mapping");

    volatile unsigned char* a = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(a != MAP_FAILED, "mapping it shared");
    if (a == MAP_FAILED) {
        close(fd);
        return;
    }

    volatile unsigned char* b = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(b != MAP_FAILED, "mapping the same file a second time");
    check(a != b, "the two mappings are at different addresses");

    if (b != MAP_FAILED) {
        a[0] = 0x11;
        a[4095] = 0x22; /* last byte of page one */
        a[4096] = 0x33; /* first byte of page two */
        a[8191] = 0x44;
        check(b[0] == 0x11 && b[4095] == 0x22, "a store is visible through the other mapping");
        check(b[4096] == 0x33 && b[8191] == 0x44, "and across the page boundary");

        b[100] = 0x55;
        check(a[100] == 0x55, "and the other way round");
    }

    /* A write through the mapping is a write to the file. */
    unsigned char scratch[4] = { 0, 0, 0, 0 };
    check(lseek(fd, 0, SEEK_SET) == 0, "seeking the shared file");
    check(read(fd, scratch, 1) == 1, "reading the shared file");
    check(scratch[0] == 0x11, "the mapping and the file are the same bytes");

    /* And it survives fork as shared memory rather than being copied. */
    a[200] = 0x66;
    pid_t child = fork();
    if (child == 0) {
        a[201] = 0x77;
        _exit(a[200] == 0x66 ? 0 : 1);
    }
    check(child > 0, "forking with a shared mapping");
    if (child > 0) {
        int status = 0;
        waitpid(child, &status, 0);
        check(
            WIFEXITED(status) && WEXITSTATUS(status) == 0, "the child sees what the parent wrote");
        check(a[201] == 0x77, "and the parent sees what the child wrote");
    }

    check(munmap((void*)a, size) == 0, "unmapping");
    if (b != MAP_FAILED)
        check(munmap((void*)b, size) == 0, "unmapping the second one");
    close(fd);
    unlink(path);

    /*
     * A read-only private mapping. This looks like a weaker case than the
     * writable one below and is actually the one that broke: the kernel has to
     * fill the pages in before handing them over, which it cannot do if it has
     * already made them read-only. The symptom was a mapping full of zeroes
     * that reported success -- the window server mapped its font that way and
     * quietly drew nothing.
     */
    fd = open("/etc/rc", O_RDONLY);
    if (fd >= 0) {
        char first[8];
        memset(first, 0, sizeof(first));
        check(read(fd, first, 4) == 4, "reading the head of a file");
        check(lseek(fd, 0, SEEK_SET) == 0, "rewinding it");

        const char* readonly_map = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
        check(readonly_map != MAP_FAILED, "mapping a file PROT_READ, MAP_PRIVATE");
        if (readonly_map != MAP_FAILED) {
            check(memcmp(readonly_map, first, 4) == 0,
                "a read-only private mapping holds the file, not zeroes");
            munmap((void*)readonly_map, 4096);
        }
        close(fd);
    }

    /* A private file mapping is a copy: writing to it must not touch the file. */
    fd = open("/etc/rc", O_RDONLY);
    if (fd >= 0) {
        char* private_map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
        check(private_map != MAP_FAILED, "mapping a file private");
        if (private_map != MAP_FAILED) {
            check(private_map[0] == '#', "a private mapping starts as the file's contents");
            private_map[0] = 'X';
            char first = 0;
            check(read(fd, &first, 1) == 1, "reading the file behind a private mapping");
            check(first == '#', "writing to a private mapping does not touch the file");
            munmap(private_map, 4096);
        }
        close(fd);
    }
}

/*
 * Named FIFOs. The point of them is that two processes which never shared a
 * descriptor can still find each other, so the test forks a child that knows
 * nothing but the path -- an inherited pipe would prove nothing.
 */
static void test_fifo(void)
{
    const char* path = "/tmp/fifo.sock";
    unlink(path);

    check(mkfifo(path, 0600) == 0, "creating a named fifo");

    struct stat status;
    check(stat(path, &status) == 0, "the fifo is visible by path");
    check(S_ISFIFO(status.st_mode), "and stat says it is a fifo");

    /* mkfifo over an existing name is EEXIST, not a second fifo. */
    check(mkfifo(path, 0600) == -1 && errno == EEXIST, "mkfifo refuses an existing name");

    /*
     * The rendezvous. The child opens for writing and will block there until
     * this process opens for reading -- so the parent deliberately opens
     * second, which is the case that deadlocks if the pending-opener
     * bookkeeping is wrong.
     */
    pid_t child = fork();
    if (child == 0) {
        int out = open(path, O_WRONLY);
        if (out < 0)
            _exit(1);
        if (write(out, "hello fifo", 10) != 10)
            _exit(2);
        close(out);
        _exit(0);
    }
    check(child > 0, "forking a writer");

    int in = open(path, O_RDONLY);
    check(in >= 0, "opening the read end after the writer was already waiting");

    char buffer[32];
    memset(buffer, 0, sizeof(buffer));
    ssize_t got = read(in, buffer, sizeof(buffer) - 1);
    check(got == 10, "reading what the unrelated process wrote");
    check(strcmp(buffer, "hello fifo") == 0, "and the bytes are the same ones");

    /* With the writer gone, the next read is end of file rather than a stall. */
    check(read(in, buffer, sizeof(buffer)) == 0, "end of file once the writer closes");
    close(in);

    int status_code = 0;
    waitpid(child, &status_code, 0);
    check(WIFEXITED(status_code) && WEXITSTATUS(status_code) == 0, "the writer exited cleanly");

    /* O_RDWR is both ends at once and must never block, which is how a server
     * holds a fifo open across clients coming and going. */
    int both = open(path, O_RDWR);
    check(both >= 0, "opening a fifo O_RDWR does not block");
    if (both >= 0) {
        check(write(both, "x", 1) == 1, "writing to a fifo held open both ways");
        char one = 0;
        check(read(both, &one, 1) == 1 && one == 'x', "and reading it back");
        close(both);
    }

    /* O_NONBLOCK is the escape hatch: a reader with no writer gets the
     * descriptor rather than being held. */
    int lonely = open(path, O_RDONLY | O_NONBLOCK);
    check(lonely >= 0, "O_NONBLOCK skips the rendezvous");
    if (lonely >= 0)
        close(lonely);

    unlink(path);
}

/*
 * A signal has to reach a process that is asleep in a syscall. This looks like
 * a strange thing to test until you have watched a `kill` return success
 * against a process that then sat there forever: the pending bit was set and
 * nobody was running to look at it.
 *
 * A blocked pipe read is the case that went wrong, so it is the case checked
 * here, along with a FIFO -- which is the same code, reached the other way.
 */
static void test_signal_wakes_blocked_reader(void)
{
    int fds[2];
    check(pipe(fds) == 0, "making a pipe to block on");

    pid_t child = fork();
    if (child == 0) {
        close(fds[1]);
        char byte = 0;
        /* Nothing will ever be written, so this blocks until the signal. */
        read(fds[0], &byte, 1);
        _exit(7); /* only reached if the read returns rather than the signal killing us */
    }
    check(child > 0, "forking a reader that will block");

    /* Long enough that the child is certainly asleep in the read rather than
     * still on its way there. */
    struct timespec settle = { 0, 60 * 1000 * 1000 };
    nanosleep(&settle, NULL);

    check(kill(child, SIGTERM) == 0, "signalling the blocked reader");

    int status = 0;
    check(waitpid(child, &status, 0) == child, "the blocked reader can be killed");
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM,
        "and it died of the signal rather than returning");

    close(fds[0]);
    close(fds[1]);

    /* The same, through a named FIFO, which is what the window server uses. */
    const char* path = "/tmp/killme.fifo";
    unlink(path);
    check(mkfifo(path, 0600) == 0, "making a fifo to block on");

    child = fork();
    if (child == 0) {
        int fd = open(path, O_RDWR);
        if (fd < 0)
            _exit(1);
        char byte = 0;
        read(fd, &byte, 1);
        _exit(7);
    }
    check(child > 0, "forking a reader blocked on a fifo");

    nanosleep(&settle, NULL);
    check(kill(child, SIGTERM) == 0, "signalling it");
    check(waitpid(child, &status, 0) == child, "a fifo reader can be killed too");
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM, "and by the signal sent");

    unlink(path);
}

int main(int argc, char** argv, char** envp)
{
    (void)envp;

    /* Re-executed by test_close_on_exec to report what survived. */
    if (argc == 4 && strcmp(argv[1], "--report-descriptors") == 0)
        return report_descriptors(atoi(argv[2]), atoi(argv[3]));

    test_shared_memory();
    test_fifo();
    test_signal_wakes_blocked_reader();
    test_fcntl();
    test_close_on_exec();
    test_rename();
    test_clock();
    test_stat_times();
    test_process_groups();
    test_group_signals();
    test_stop_and_continue();
    test_stop_signal_dispositions();
    test_background_read();
    test_terminal_ownership();
    test_sessions();
    test_signal_return();
    test_printf_lengths();
    test_poll();
    test_select();

    printf("%d passed, %d failed\n", s_checks - s_failures, s_failures);
    return s_failures == 0 ? 0 : 1;
}
