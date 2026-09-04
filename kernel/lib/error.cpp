// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- error names.

#include <kernel/lib/error.h>

namespace kernel {

char const* Error::to_string() const
{
    switch (m_code) {
    case ESUCCESS:
        return "success";
    case EPERM:
        return "operation not permitted";
    case ENOENT:
        return "no such file or directory";
    case ESRCH:
        return "no such process";
    case EINTR:
        return "interrupted";
    case EIO:
        return "input/output error";
    case ENXIO:
        return "no such device or address";
    case E2BIG:
        return "argument list too long";
    case ENOEXEC:
        return "not an executable";
    case EBADF:
        return "bad file descriptor";
    case ECHILD:
        return "no child processes";
    case EAGAIN:
        return "try again";
    case ENOMEM:
        return "out of memory";
    case EACCES:
        return "permission denied";
    case EFAULT:
        return "bad address";
    case EBUSY:
        return "device or resource busy";
    case EEXIST:
        return "file exists";
    case EXDEV:
        return "cross-device link";
    case ENODEV:
        return "no such device";
    case ENOTDIR:
        return "not a directory";
    case EISDIR:
        return "is a directory";
    case EINVAL:
        return "invalid argument";
    case ENFILE:
        return "too many open files in system";
    case EMFILE:
        return "too many open files";
    case ENOTTY:
        return "not a terminal";
    case EFBIG:
        return "file too large";
    case ENOSPC:
        return "no space left on device";
    case ESPIPE:
        return "illegal seek";
    case EROFS:
        return "read-only filesystem";
    case EMLINK:
        return "too many links";
    case EPIPE:
        return "broken pipe";
    case ERANGE:
        return "result out of range";
    case ENAMETOOLONG:
        return "name too long";
    case ENOSYS:
        return "not implemented";
    case ENOTEMPTY:
        return "directory not empty";
    case ELOOP:
        return "too many levels of symbolic links";
    case EOVERFLOW:
        return "value too large";
    case ENOTSUP:
        return "not supported";
    case ETIMEDOUT:
        return "timed out";
    case EABIVER:
        return "module built against a different kernel ABI";
    default:
        return "unknown error";
    }
}

} // namespace kernel
