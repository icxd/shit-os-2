/* SPDX-License-Identifier: MIT */
/* shit os 2 libc -- the thin POSIX wrappers. */

#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

char** environ = 0;

ssize_t read(int fd, void* buffer, size_t count)
{
    return __syscall_return(__syscall3(SYS_read, fd, (long)buffer, (long)count));
}

ssize_t write(int fd, const void* buffer, size_t count)
{
    return __syscall_return(__syscall3(SYS_write, fd, (long)buffer, (long)count));
}

int open(const char* path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, unsigned int);
        va_end(args);
    }
    return (int)__syscall_return(__syscall3(SYS_open, (long)path, flags, (long)mode));
}

int creat(const char* path, mode_t mode)
{
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

int close(int fd)
{
    return (int)__syscall_return(__syscall1(SYS_close, fd));
}

off_t lseek(int fd, off_t offset, int whence)
{
    return __syscall_return(__syscall3(SYS_lseek, fd, offset, whence));
}

pid_t fork(void)
{
    return (pid_t)__syscall_return(__syscall0(SYS_fork));
}

int execve(const char* path, char* const argv[], char* const envp[])
{
    return (int)__syscall_return(__syscall3(SYS_execve, (long)path, (long)argv, (long)envp));
}

int execv(const char* path, char* const argv[])
{
    return execve(path, argv, environ);
}

int execvp(const char* file, char* const argv[])
{
    /* An explicit path is used as given; a bare name is looked up in PATH. */
    if (strchr(file, '/') != 0)
        return execve(file, argv, environ);

    const char* path = getenv("PATH");
    if (path == 0)
        path = "/bin";

    char candidate[256];
    while (*path != '\0') {
        size_t length = 0;
        while (path[length] != '\0' && path[length] != ':')
            ++length;

        if (length + 1 + strlen(file) + 1 < sizeof(candidate)) {
            memcpy(candidate, path, length);
            candidate[length] = '/';
            strcpy(candidate + length + 1, file);
            execve(candidate, argv, environ);
            /* Only a genuine "not here" is worth trying the next entry for. */
            if (errno != ENOENT)
                return -1;
        }

        path += length;
        if (*path == ':')
            ++path;
    }

    errno = ENOENT;
    return -1;
}

void _exit(int status)
{
    __syscall1(SYS_exit, status);
    __builtin_unreachable();
}

pid_t getpid(void)
{
    return (pid_t)__syscall0(SYS_getpid);
}
pid_t getppid(void)
{
    return (pid_t)__syscall0(SYS_getppid);
}

int dup(int fd)
{
    return (int)__syscall_return(__syscall1(SYS_dup, fd));
}
int dup2(int fd, int to)
{
    return (int)__syscall_return(__syscall2(SYS_dup2, fd, to));
}
int pipe(int fds[2])
{
    return (int)__syscall_return(__syscall1(SYS_pipe, (long)fds));
}

int chdir(const char* path)
{
    return (int)__syscall_return(__syscall1(SYS_chdir, (long)path));
}

char* getcwd(char* buffer, size_t size)
{
    long result = __syscall_return(__syscall2(SYS_getcwd, (long)buffer, (long)size));
    return result < 0 ? 0 : buffer;
}

int rmdir(const char* path)
{
    return (int)__syscall_return(__syscall1(SYS_rmdir, (long)path));
}
int unlink(const char* path)
{
    return (int)__syscall_return(__syscall1(SYS_unlink, (long)path));
}
int mkdir(const char* path, mode_t mode)
{
    return (int)__syscall_return(__syscall2(SYS_mkdir, (long)path, mode));
}

int stat(const char* path, struct stat* out)
{
    return (int)__syscall_return(__syscall2(SYS_stat, (long)path, (long)out));
}

int fstat(int fd, struct stat* out)
{
    return (int)__syscall_return(__syscall2(SYS_fstat, fd, (long)out));
}

pid_t waitpid(pid_t pid, int* status, int options)
{
    return (pid_t)__syscall_return(__syscall3(SYS_waitpid, pid, (long)status, options));
}

pid_t wait(int* status)
{
    return waitpid(-1, status, 0);
}

int isatty(int fd)
{
    long result = __syscall1(SYS_isatty, fd);
    if (result < 0) {
        errno = (int)-result;
        return 0;
    }
    return 1;
}

int ioctl(int fd, unsigned long request, void* argument)
{
    return (int)__syscall_return(__syscall3(SYS_ioctl, fd, (long)request, (long)argument));
}

int tcgetattr(int fd, struct termios* out)
{
    return ioctl(fd, TCGETS, out);
}
int tcsetattr(int fd, int actions, const struct termios* in)
{
    (void)actions;
    return ioctl(fd, TCSETS, (void*)in);
}

int uname(struct utsname* out)
{
    return (int)__syscall_return(__syscall1(SYS_uname, (long)out));
}

void* mmap(void* address, size_t length, int protection, int flags, int fd, off_t offset)
{
    long result = __syscall6(SYS_mmap, (long)address, (long)length, protection, flags, fd, offset);
    if (result < 0 && result > -4096) {
        errno = (int)-result;
        return MAP_FAILED;
    }
    return (void*)result;
}

int munmap(void* address, size_t length)
{
    return (int)__syscall_return(__syscall2(SYS_munmap, (long)address, (long)length));
}

int nanosleep(const struct timespec* request, struct timespec* remaining)
{
    if (request == 0 || request->tv_nsec < 0 || request->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }

    /*
     * The kernel does not report how much of the sleep was left, and it does
     * not need to: the monotonic clock says. Taking the deadline before the
     * call and subtracting after is exact to the tick, which is as good as the
     * sleep itself.
     */
    struct timespec started;
    int const have_clock = clock_gettime(CLOCK_MONOTONIC, &started) == 0;

    int const result
        = (int)__syscall_return(__syscall2(SYS_nanosleep, (long)request->tv_sec, request->tv_nsec));

    if (result == 0 || !remaining)
        return result;

    /* Interrupted. Work out what is left so a caller can go back to sleep for
     * the rest rather than starting over. */
    remaining->tv_sec = 0;
    remaining->tv_nsec = 0;
    if (!have_clock)
        return result;

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return result;

    long long const asked = (long long)request->tv_sec * 1000000000LL + request->tv_nsec;
    long long const slept
        = ((long long)now.tv_sec - started.tv_sec) * 1000000000LL + (now.tv_nsec - started.tv_nsec);
    long long left = asked - slept;
    if (left < 0)
        left = 0;

    remaining->tv_sec = (time_t)(left / 1000000000LL);
    remaining->tv_nsec = (long)(left % 1000000000LL);
    return result;
}

unsigned int sleep(unsigned int seconds)
{
    struct timespec request = { (time_t)seconds, 0 };
    nanosleep(&request, 0);
    return 0;
}

int usleep(unsigned int microseconds)
{
    struct timespec request = { 0, (long)microseconds * 1000 };
    return nanosleep(&request, 0);
}

void* sbrk(long increment)
{
    /* brk(0) reports the current break; brk(new) moves it. */
    long current = __syscall1(SYS_brk, 0);
    if (increment == 0)
        return (void*)current;

    long wanted = current + increment;
    long result = __syscall1(SYS_brk, wanted);
    if (result < 0 && result > -4096) {
        errno = (int)-result;
        return (void*)-1;
    }
    if (result < wanted) {
        errno = ENOMEM;
        return (void*)-1;
    }
    return (void*)current;
}

int kill(pid_t pid, int signal)
{
    return (int)__syscall_return(__syscall2(SYS_kill, pid, signal));
}

int raise(int signal)
{
    return kill(getpid(), signal);
}

extern void __libc_sigreturn_trampoline(void);

int sigaction(int number, const struct sigaction* action, struct sigaction* old)
{
    struct sigaction copy;
    const struct sigaction* to_install = action;

    if (action != 0) {
        copy = *action;
        /*
         * The kernel needs somewhere for the handler to return to, and libc
         * owns that trampoline. Overwrite whatever the caller had there,
         * always -- sa_restorer is not a field a portable program sets, so a
         * value in it is stack garbage from a struct that was filled in field
         * by field rather than zeroed. Honouring it means returning from the
         * handler to a random address.
         *
         * dash does exactly that, and the crash it caused was three calls
         * later and looked like a corrupt heap.
         */
        copy.sa_restorer = __libc_sigreturn_trampoline;
        to_install = &copy;
    }

    int const result
        = (int)__syscall_return(__syscall3(SYS_sigaction, number, (long)to_install, (long)old));

    /* The trampoline is ours, not the caller's business, and handing it back
     * invites it being passed to a later sigaction as if it meant something. */
    if (old != 0)
        old->sa_restorer = 0;

    return result;
}

sighandler_t signal(int number, sighandler_t handler)
{
    struct sigaction action;
    struct sigaction old;
    memset(&action, 0, sizeof(action));
    memset(&old, 0, sizeof(old));
    action.sa_handler = handler;
    action.sa_flags = SA_RESTART;

    if (sigaction(number, &action, &old) < 0)
        return SIG_ERR;
    return old.sa_handler;
}

int access(const char* path, int mode)
{
    struct stat status;
    if (stat(path, &status) < 0)
        return -1;

    /* Everything runs as root and no mode bits are enforced, so the only
     * question left is whether a directory was asked to be executable --
     * which it always is -- or a plain file, which is never refused. */
    (void)mode;
    return 0;
}

int fcntl(int fd, int command, ...)
{
    /*
     * Every command this kernel implements takes at most one integer, so read
     * one unconditionally. Reading an argument that was not passed is defined
     * behaviour for va_arg only in the sense that the value is garbage -- and
     * the commands that ignore it do exactly that.
     */
    va_list arguments;
    va_start(arguments, command);
    long const argument = (long)va_arg(arguments, int);
    va_end(arguments);

    return (int)__syscall_return(__syscall3(SYS_fcntl, fd, command, argument));
}

/* --- job control --------------------------------------------------------
 *
 * A shell needs all of this to run a pipeline as one job: put the children in
 * a group of their own, hand that group the terminal, and take it back when
 * they stop or finish. Without it ^C reaches whichever process last read the
 * terminal, which is nearly right and wrong in exactly the cases that matter.
 */

int setpgid(pid_t pid, pid_t pgid)
{
    return (int)__syscall_return(__syscall2(SYS_setpgid, pid, pgid));
}

pid_t getpgid(pid_t pid)
{
    return (pid_t)__syscall_return(__syscall1(SYS_getpgid, pid));
}

pid_t getpgrp(void)
{
    return getpgid(0);
}

pid_t setsid(void)
{
    return (pid_t)__syscall_return(__syscall0(SYS_setsid));
}

pid_t getsid(pid_t pid)
{
    return (pid_t)__syscall_return(__syscall1(SYS_getsid, pid));
}

pid_t tcgetpgrp(int fd)
{
    pid_t group = 0;
    if (ioctl(fd, TIOCGPGRP, &group) < 0)
        return -1;
    return group;
}

int tcsetpgrp(int fd, pid_t pgid)
{
    return ioctl(fd, TIOCSPGRP, &pgid);
}

int killpg(pid_t pgid, int signal)
{
    if (pgid < 0) {
        errno = EINVAL;
        return -1;
    }
    /* kill() reads a negative pid as a group; 0 already means "my group". */
    return kill(pgid == 0 ? 0 : -pgid, signal);
}

/* --- users, such as they are --------------------------------------------
 *
 * There are none. Everything runs as root and no mode bit is ever checked, so
 * these report 0 because that is what is true, not as a placeholder.
 */

uid_t getuid(void)
{
    return 0;
}
uid_t geteuid(void)
{
    return 0;
}
gid_t getgid(void)
{
    return 0;
}
gid_t getegid(void)
{
    return 0;
}

mode_t umask(mode_t mask)
{
    return (mode_t)__syscall_return(__syscall1(SYS_umask, mask));
}

long sysconf(int name)
{
    switch (name) {
    case _SC_OPEN_MAX: return 64; /* MAX_FILE_DESCRIPTORS in the kernel */
    case _SC_PAGESIZE: return 4096;
    case _SC_CLK_TCK: return 250; /* the PIT runs at 250 Hz */
    case _SC_NPROCESSORS_ONLN: return 1;
    case _SC_ARG_MAX: return 4096; /* MAX_ARGUMENT_BYTES */
    default: errno = EINVAL; return -1;
    }
}

clock_t times(struct tms* out)
{
    if (out) {
        /* No per-process CPU accounting exists, so reporting zero is the
         * honest answer rather than a plausible invented one. */
        out->tms_utime = 0;
        out->tms_stime = 0;
        out->tms_cutime = 0;
        out->tms_cstime = 0;
    }

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return (clock_t)-1;
    /* In ticks, as sysconf(_SC_CLK_TCK) reports them. */
    return (clock_t)(now.tv_sec * 250 + now.tv_nsec / 4000000);
}

int lstat(const char* path, struct stat* out)
{
    /* No symbolic links exist, so there is nothing for lstat to decline to
     * follow. When they arrive, this stops being a forward. */
    return stat(path, out);
}

pid_t wait4(pid_t pid, int* status, int options, struct rusage* usage)
{
    if (usage)
        memset(usage, 0, sizeof(*usage));
    return waitpid(pid, status, options);
}

pid_t wait3(int* status, int options, struct rusage* usage)
{
    return wait4(-1, status, options, usage);
}

int getgroups(int count, gid_t* groups)
{
    /* Everything runs as root, which is in exactly one group. Asking for zero
     * is how a caller sizes its array first. */
    if (count == 0)
        return 1;
    if (count < 1 || !groups) {
        errno = EINVAL;
        return -1;
    }
    groups[0] = 0;
    return 1;
}
