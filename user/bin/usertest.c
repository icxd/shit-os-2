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

    printf("%d passed, %d failed\n", s_checks - s_failures, s_failures);
    return s_failures == 0 ? 0 : 1;
}
