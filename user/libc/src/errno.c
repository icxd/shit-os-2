/* SPDX-License-Identifier: MIT */
/* shit os 2 libc -- error strings. */

#include <errno.h>
#include <stdio.h>
#include <string.h>

char* strerror(int code)
{
    switch (code) {
    case ESUCCESS: return (char*)"Success";
    case EPERM: return (char*)"Operation not permitted";
    case ENOENT: return (char*)"No such file or directory";
    case ESRCH: return (char*)"No such process";
    case EINTR: return (char*)"Interrupted system call";
    case EIO: return (char*)"Input/output error";
    case ENXIO: return (char*)"No such device or address";
    case E2BIG: return (char*)"Argument list too long";
    case ENOEXEC: return (char*)"Exec format error";
    case EBADF: return (char*)"Bad file descriptor";
    case ECHILD: return (char*)"No child processes";
    case EAGAIN: return (char*)"Resource temporarily unavailable";
    case ENOMEM: return (char*)"Cannot allocate memory";
    case EACCES: return (char*)"Permission denied";
    case EFAULT: return (char*)"Bad address";
    case EBUSY: return (char*)"Device or resource busy";
    case EEXIST: return (char*)"File exists";
    case ENODEV: return (char*)"No such device";
    case ENOTDIR: return (char*)"Not a directory";
    case EISDIR: return (char*)"Is a directory";
    case EINVAL: return (char*)"Invalid argument";
    case EMFILE: return (char*)"Too many open files";
    case ENOTTY: return (char*)"Inappropriate ioctl for device";
    case ENOSPC: return (char*)"No space left on device";
    case ESPIPE: return (char*)"Illegal seek";
    case EROFS: return (char*)"Read-only file system";
    case EPIPE: return (char*)"Broken pipe";
    case ERANGE: return (char*)"Numerical result out of range";
    case ENAMETOOLONG: return (char*)"File name too long";
    case ENOSYS: return (char*)"Function not implemented";
    case ENOTEMPTY: return (char*)"Directory not empty";
    case ENOTSUP: return (char*)"Operation not supported";
    default: return (char*)"Unknown error";
    }
}

void perror(const char* prefix)
{
    if (prefix && *prefix)
        fprintf(stderr, "%s: %s\n", prefix, strerror(errno));
    else
        fprintf(stderr, "%s\n", strerror(errno));
}
