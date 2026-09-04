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
#include <sys/stat.h>
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
    (void)remaining;
    if (request == 0)
        return -1;
    return (int)__syscall_return(
        __syscall2(SYS_nanosleep, (long)request->tv_sec, request->tv_nsec));
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
         * The kernel needs somewhere for the handler to return to. libc owns
         * that trampoline, so fill it in here rather than making every caller
         * know it exists.
         */
        if (copy.sa_restorer == 0)
            copy.sa_restorer = __libc_sigreturn_trampoline;
        to_install = &copy;
    }

    return (int)__syscall_return(__syscall3(SYS_sigaction, number, (long)to_install, (long)old));
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
