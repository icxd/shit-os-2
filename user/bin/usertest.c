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
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

int main(int argc, char** argv, char** envp)
{
    (void)envp;

    /* Re-executed by test_close_on_exec to report what survived. */
    if (argc == 4 && strcmp(argv[1], "--report-descriptors") == 0)
        return report_descriptors(atoi(argv[2]), atoi(argv[3]));

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

    printf("%d passed, %d failed\n", s_checks - s_failures, s_failures);
    return s_failures == 0 ? 0 : 1;
}
