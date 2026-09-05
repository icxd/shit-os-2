/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- the file and process calls that portable software expects
 * to find, grouped by how honest each one can be.
 *
 * Three kinds live here. Some are real: ftruncate and chmod are syscalls.
 * Some are true statements about a system with one user and no links -- chown
 * succeeds when asked to make root the owner of something root already owns,
 * because that is not a lie. And some report ENOSYS, because the facility
 * genuinely is not here and a caller that cares should find out rather than
 * carry on believing it worked.
 *
 * The `at` family is its own case: there is no way to resolve a path against
 * an arbitrary open directory, so they accept AT_FDCWD and refuse the rest.
 * Guessing would silently look in the wrong place.
 */

#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pty.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* --- real ---------------------------------------------------------------- */

int ftruncate(int fd, off_t length)
{
    return (int)__syscall_return(__syscall2(SYS_ftruncate, fd, length));
}

int truncate(const char* path, off_t length)
{
    int const fd = open(path, O_WRONLY, 0);
    if (fd < 0)
        return -1;
    int const result = ftruncate(fd, length);
    int const saved = errno;
    close(fd);
    errno = saved;
    return result;
}

int chmod(const char* path, mode_t mode)
{
    return (int)__syscall_return(__syscall2(SYS_chmod, (long)path, mode));
}

int fchmod(int fd, mode_t mode)
{
    /*
     * No syscall takes a descriptor, and reconstructing the path from one
     * would race with a rename. Refusing is better than changing the mode of
     * whatever now has that name.
     */
    (void)fd;
    (void)mode;
    errno = ENOSYS;
    return -1;
}

/* --- true, given one user ------------------------------------------------ */

static int chown_common(uid_t owner, gid_t group)
{
    /* Everything is owned by root and there is nobody else to give it to.
     * Asking for what is already the case succeeds; asking for anything else
     * is a permission error, which is what it would be on a real system. */
    if ((owner == 0 || owner == (uid_t)-1) && (group == 0 || group == (gid_t)-1))
        return 0;
    errno = EPERM;
    return -1;
}

int chown(const char* path, uid_t owner, gid_t group)
{
    struct stat status;
    if (stat(path, &status) < 0)
        return -1;
    return chown_common(owner, group);
}

int lchown(const char* path, uid_t owner, gid_t group)
{
    return chown(path, owner, group);
}

int fchown(int fd, uid_t owner, gid_t group)
{
    struct stat status;
    if (fstat(fd, &status) < 0)
        return -1;
    return chown_common(owner, group);
}

char* getlogin(void)
{
    return (char*)"root";
}

int gethostname(char* buffer, size_t capacity)
{
    static const char name[] = "shit-os-2";
    if (!buffer || capacity == 0) {
        errno = EINVAL;
        return -1;
    }
    if (capacity < sizeof(name)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buffer, name, sizeof(name));
    return 0;
}

int sethostname(const char* name, size_t length)
{
    /* The name is compiled into uname, so there is nowhere to put a new one. */
    (void)name;
    (void)length;
    errno = EPERM;
    return -1;
}

char* ttyname(int fd)
{
    if (!isatty(fd))
        return NULL;
    /* One terminal, and this is its name. */
    return (char*)"/dev/tty0";
}

void sync(void)
{
    /* Every filesystem is in memory; there is nothing to flush anywhere. */
}

int getpriority(int which, id_t who)
{
    (void)which;
    (void)who;
    /* Round robin with one quantum for everyone: every process is at the
     * default priority, and 0 is what that is called. */
    return 0;
}

int setpriority(int which, id_t who, int value)
{
    (void)which;
    (void)who;
    (void)value;
    errno = EPERM;
    return -1;
}

/* --- not here ------------------------------------------------------------ */

int link(const char* from, const char* to)
{
    (void)from;
    (void)to;
    /* No filesystem here records more than one name per inode. */
    errno = ENOSYS;
    return -1;
}

int symlink(const char* target, const char* path)
{
    (void)target;
    (void)path;
    errno = ENOSYS;
    return -1;
}

ssize_t readlink(const char* path, char* buffer, size_t capacity)
{
    (void)buffer;
    (void)capacity;
    /* Nothing is a symbolic link, so the error is "not one" rather than
     * "cannot": EINVAL is what readlink reports for an ordinary file. */
    struct stat status;
    if (stat(path, &status) < 0)
        return -1;
    errno = EINVAL;
    return -1;
}

int chroot(const char* path)
{
    (void)path;
    errno = ENOSYS;
    return -1;
}

int mkfifo(const char* path, mode_t mode)
{
    return (int)__syscall_return(__syscall2(SYS_mkfifo, (long)path, (long)mode));
}

int mknod(const char* path, mode_t mode, dev_t device)
{
    (void)path;
    (void)mode;
    (void)device;
    /* Device nodes are created by drivers registering with devfs, not by a
     * process asking for one. */
    errno = ENOSYS;
    return -1;
}

/* --- the directory-relative family --------------------------------------- */

/*
 * These are real syscalls: the kernel resolves the path against the inode the
 * descriptor refers to. That is what lets a recursive walk descend without
 * rebuilding a full path at every step, and without racing a rename of a
 * directory it has already passed -- which is why sbase's cp, rm, du and
 * chmod all go through them.
 */

int fstatat(int directory, const char* path, struct stat* out, int flags)
{
    /* Nothing here is a symbolic link, so NOFOLLOW asks for what it gets. */
    (void)flags;
    return (int)__syscall_return(__syscall3(SYS_fstatat, directory, (long)path, (long)out));
}

int fchmodat(int directory, const char* path, mode_t mode, int flags)
{
    (void)flags;
    return (int)__syscall_return(__syscall3(SYS_fchmodat, directory, (long)path, mode));
}

int mkdirat(int directory, const char* path, mode_t mode)
{
    return (int)__syscall_return(__syscall3(SYS_mkdirat, directory, (long)path, mode));
}

int unlinkat(int directory, const char* path, int flags)
{
    return (int)__syscall_return(__syscall3(SYS_unlinkat, directory, (long)path, flags));
}

int faccessat(int directory, const char* path, int mode, int flags)
{
    /* No permissions are enforced, so existence is the whole question. */
    (void)mode;
    (void)flags;
    struct stat status;
    return fstatat(directory, path, &status, 0);
}

int fchownat(int directory, const char* path, uid_t owner, gid_t group, int flags)
{
    (void)flags;
    struct stat status;
    if (fstatat(directory, path, &status, 0) < 0)
        return -1;
    return chown_common(owner, group);
}

int linkat(int from_directory, const char* from, int to_directory, const char* to, int flags)
{
    (void)from_directory;
    (void)from;
    (void)to_directory;
    (void)to;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

int utimensat(int directory, const char* path, const struct timespec times[2], int flags)
{
    (void)times;
    (void)flags;
    /* Same reason as utime: inodes are stamped when written and there is no
     * call to say otherwise. The path is still checked, so a caller learns
     * about a missing file before it learns about a missing feature. */
    struct stat status;
    if (fstatat(directory, path, &status, 0) < 0)
        return -1;
    errno = ENOSYS;
    return -1;
}

/* --- the variadic exec spellings ----------------------------------------- */

#define MAX_EXEC_ARGUMENTS 64

/* Collects a null-terminated argument list into a vector. Returns the count,
 * or -1 if there were too many. The environment, for the `e` forms, follows
 * the terminator. */
static int collect_arguments(
    const char* first, va_list arguments, char** vector, char*** environment)
{
    int count = 0;
    vector[count++] = (char*)first;

    while (count < MAX_EXEC_ARGUMENTS) {
        char* const next = va_arg(arguments, char*);
        if (!next)
            break;
        vector[count++] = next;
    }
    if (count >= MAX_EXEC_ARGUMENTS) {
        errno = E2BIG;
        return -1;
    }
    vector[count] = NULL;

    if (environment)
        *environment = va_arg(arguments, char**);
    return count;
}

int execl(const char* path, const char* argument, ...)
{
    char* vector[MAX_EXEC_ARGUMENTS + 1];
    va_list arguments;
    va_start(arguments, argument);
    int const count = collect_arguments(argument, arguments, vector, NULL);
    va_end(arguments);
    if (count < 0)
        return -1;
    return execve(path, vector, environ);
}

int execlp(const char* file, const char* argument, ...)
{
    char* vector[MAX_EXEC_ARGUMENTS + 1];
    va_list arguments;
    va_start(arguments, argument);
    int const count = collect_arguments(argument, arguments, vector, NULL);
    va_end(arguments);
    if (count < 0)
        return -1;
    return execvp(file, vector);
}

int execle(const char* path, const char* argument, ...)
{
    char* vector[MAX_EXEC_ARGUMENTS + 1];
    char** environment = NULL;
    va_list arguments;
    va_start(arguments, argument);
    int const count = collect_arguments(argument, arguments, vector, &environment);
    va_end(arguments);
    if (count < 0)
        return -1;
    return execve(path, vector, environment ? environment : environ);
}

int openat(int directory, const char* path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list arguments;
        va_start(arguments, flags);
        mode = (mode_t)va_arg(arguments, int);
        va_end(arguments);
    }
    return (int)__syscall_return(__syscall4(SYS_openat, directory, (long)path, flags, mode));
}

int symlinkat(const char* target, int directory, const char* path)
{
    (void)target;
    (void)directory;
    (void)path;
    errno = ENOSYS;
    return -1;
}

ssize_t readlinkat(int directory, const char* path, char* buffer, size_t capacity)
{
    (void)buffer;
    (void)capacity;
    /* Nothing is a symbolic link, so the answer is "not one" rather than
     * "cannot", which is EINVAL. */
    struct stat status;
    if (fstatat(directory, path, &status, 0) < 0)
        return -1;
    errno = EINVAL;
    return -1;
}

/*
 * Resolves . and .. and collapses runs of slashes. Nothing here is a symbolic
 * link, so what remains is purely textual -- but the result is still checked
 * to exist, which is the half of realpath that callers actually depend on.
 */
char* realpath(const char* path, char* resolved)
{
    if (!path || !*path) {
        errno = ENOENT;
        return NULL;
    }

    char* const output = resolved ? resolved : malloc(PATH_MAX);
    if (!output) {
        errno = ENOMEM;
        return NULL;
    }

    /* Start from the working directory unless the path is already absolute. */
    size_t length = 0;
    if (path[0] != '/') {
        if (!getcwd(output, PATH_MAX)) {
            if (!resolved)
                free(output);
            return NULL;
        }
        length = strlen(output);
        if (length == 1 && output[0] == '/')
            length = 0; /* so the join below does not double the slash */
    }

    const char* cursor = path;
    while (*cursor) {
        while (*cursor == '/')
            ++cursor;
        if (!*cursor)
            break;

        const char* const start = cursor;
        while (*cursor && *cursor != '/')
            ++cursor;
        size_t const component = (size_t)(cursor - start);

        if (component == 1 && start[0] == '.')
            continue;

        if (component == 2 && start[0] == '.' && start[1] == '.') {
            /* Back up over the last component, if there is one. */
            while (length > 0 && output[length - 1] != '/')
                --length;
            if (length > 0)
                --length; /* drop the slash too */
            continue;
        }

        if (length + 1 + component >= PATH_MAX) {
            if (!resolved)
                free(output);
            errno = ENAMETOOLONG;
            return NULL;
        }
        output[length++] = '/';
        memcpy(output + length, start, component);
        length += component;
    }

    if (length == 0)
        output[length++] = '/';
    output[length] = '\0';

    struct stat status;
    if (stat(output, &status) < 0) {
        if (!resolved)
            free(output);
        return NULL;
    }
    return output;
}

/* --- pseudo-terminals ------------------------------------------------------ */

int openpty(int* master, int* slave, char* name, const struct termios* settings,
    const struct winsize* window)
{
    int fds[2];
    if ((int)__syscall_return(__syscall2(SYS_openpty, (long)&fds[0], (long)&fds[1])) < 0)
        return -1;

    /* There are no names, so there is nothing honest to write here. Callers
     * pass null in practice; the argument exists so that code written for
     * other systems compiles unchanged. */
    if (name != NULL)
        name[0] = '\0';

    if (settings != NULL)
        (void)ioctl(fds[1], TCSETS, (void*)settings);
    if (window != NULL)
        (void)ioctl(fds[1], TIOCSWINSZ, (void*)window);

    if (master != NULL)
        *master = fds[0];
    if (slave != NULL)
        *slave = fds[1];
    return 0;
}

pid_t forkpty(int* master, char* name, const struct termios* settings, const struct winsize* window)
{
    int master_fd = -1;
    int slave_fd = -1;
    if (openpty(&master_fd, &slave_fd, name, settings, window) < 0)
        return -1;

    pid_t child = fork();
    if (child < 0) {
        close(master_fd);
        close(slave_fd);
        return -1;
    }

    if (child == 0) {
        /*
         * A new session, so the child is not in the parent's job control and
         * the pty is its terminal rather than whatever the parent had. Without
         * this a shell in the child would keep trying to drive the console it
         * inherited.
         */
        close(master_fd);
        setsid();

        dup2(slave_fd, 0);
        dup2(slave_fd, 1);
        dup2(slave_fd, 2);
        if (slave_fd > 2)
            close(slave_fd);

        /*
         * Claim the pty as this session's controlling terminal. There is no
         * path to open it by, so TIOCSCTTY is the only way it can become one
         * -- and without it `/dev/tty` in anything we exec resolves to the
         * console, which is somebody else's terminal entirely.
         */
        (void)ioctl(0, TIOCSCTTY, 0);

        /* And claim the foreground, so that ^C reaches this process rather
         * than nobody. A shell will do it again for each job it runs. */
        pid_t group = getpid();
        (void)ioctl(0, TIOCSPGRP, &group);
        return 0;
    }

    close(slave_fd);
    if (master != NULL)
        *master = master_fd;
    return child;
}
