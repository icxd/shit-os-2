# System calls

Numbers and structures live in
[`include/shitos/abi/syscall.h`](../include/shitos/abi/syscall.h), shared
verbatim between the kernel and the C library.

## Calling convention

The x86-64 SysV syscall convention:

```
  rax = syscall number          rax = return value
  rdi = arg0                          negative value == -errno
  rsi = arg1
  rdx = arg2                    clobbered: rcx (saved rip), r11 (saved rflags)
  r10 = arg3                    preserved: everything else
  r8  = arg4
  r9  = arg5
```

`rcx` is deliberately not an argument register: the `syscall` instruction
overwrites it with the return address, which is why arg3 lives in `r10`.

A return value in `[-4095, -1]` is an error. Anything else is a real result —
which matters, because `mmap` and `brk` can legitimately return values with
the top bit set.

## POSIX surface

| # | Name | Signature | Notes |
|---:|---|---|---|
| 0 | `exit` | `(int status)` | Does not return. |
| 1 | `read` | `(int fd, void* buf, size_t n)` | Blocks on a terminal or an empty pipe. |
| 2 | `write` | `(int fd, const void* buf, size_t n)` | |
| 3 | `open` | `(const char* path, int flags, mode_t mode)` | `EROFS` at open time on a read-only mount. |
| 4 | `close` | `(int fd)` | |
| 5 | `lseek` | `(int fd, off_t off, int whence)` | |
| 6 | `fork` | `()` | Eager copy of the address space; COW is on the roadmap. |
| 7 | `execve` | `(const char* path, char* const argv[], char* const envp[])` | Static ET_EXEC only. |
| 8 | `waitpid` | `(pid_t pid, int* status, int options)` | `WNOHANG`, `WUNTRACED`, `WCONTINUED`. A negative pid waits on a process group. |
| 9 | `getpid` | `()` | |
| 10 | `getppid` | `()` | Orphans are reparented to pid 1. |
| 11 | `brk` | `(void* address)` | `brk(0)` reports the current break. |
| 12 | `mmap` | `(addr, len, prot, flags, fd, off)` | Anonymous, `MAP_SHARED` and `MAP_PRIVATE`. A shared mapping of a file points at the inode's own pages, so it is shared memory and survives fork as one; a private one is an eager copy. Only tmpfs and `/dev/fb0` can back a mapping -- everything else is `ENODEV`. |
| 13 | `munmap` | `(void* address, size_t length)` | Address space is not reclaimed. |
| 14 | `dup` | `(int fd)` | Lowest free descriptor. |
| 15 | `dup2` | `(int fd, int to)` | |
| 16 | `pipe` | `(int fds[2])` | 4 KiB buffer. |
| 17 | `stat` | `(const char* path, struct stat*)` | |
| 18 | `fstat` | `(int fd, struct stat*)` | |
| 19 | `getdents` | `(int fd, void* buf, size_t n)` | Packed records, `d_reclen` 8-aligned. |
| 20 | `mkdir` | `(const char* path, mode_t mode)` | `EEXIST` beats `EROFS`. |
| 21 | `rmdir` | `(const char* path)` | `ENOTEMPTY` if not empty. |
| 22 | `unlink` | `(const char* path)` | |
| 23 | `chdir` | `(const char* path)` | |
| 24 | `getcwd` | `(char* buf, size_t size)` | Rebuilt by walking parent pointers. |
| 25 | `ioctl` | `(int fd, unsigned request, void* arg)` | Bounced through the kernel; devices never see a user pointer. `TIOCGPGRP`/`TIOCSPGRP` are how the terminal changes hands, and `TIOCSCTTY` is how a session gets one at all -- see `/dev/tty` below. |
| 26 | `kill` | `(pid_t pid, int signal)` | Signal 0 is the existence check. A negative pid addresses a process group; `-1` is everything but init. |
| 27 | `sigaction` | `(int sig, const struct sigaction*, struct sigaction*)` | libc fills in `sa_restorer`. |
| 28 | `sigreturn` | `()` | Called by the libc restorer, never directly. |
| 29 | `nanosleep` | `(time_t sec, long nsec)` | Rounded to the 4 ms tick. Interruptible, and never restarted -- POSIX says so, and restarting would sleep longer than asked. |
| 30 | `uname` | `(struct utsname*)` | |
| 31 | `sched_yield` | `()` | |
| 32 | `isatty` | `(int fd)` | |
| 33 | `clock_gettime` | `(clockid_t, struct timespec*)` | `CLOCK_REALTIME` and `CLOCK_MONOTONIC`. Realtime reads as boot time until a driver registers a clock. |
| 34 | `fcntl` | `(int fd, int cmd, ...)` | `F_DUPFD`, `F_DUPFD_CLOEXEC`, `F_GETFD`, `F_SETFD`, `F_GETFL`, `F_SETFL`. |
| 35 | `rename` | `(const char* from, const char* to)` | `EXDEV` across filesystems; replaces an existing file atomically. |
| 36 | `setpgid` | `(pid_t pid, pid_t pgid)` | Self or a not-yet-exec'd child, within one session. |
| 37 | `getpgid` | `(pid_t pid)` | |
| 38 | `setsid` | `()` | `EPERM` for a group leader. |
| 39 | `getsid` | `(pid_t pid)` | |
| 40 | `poll` | `(struct pollfd*, nfds_t, int timeout_ms)` | `POLLIN`/`POLLOUT`/`POLLHUP`/`POLLNVAL`. `select` is a libc translation onto it. |
| 41 | `sigprocmask` | `(int how, const sigset_t*, sigset_t*)` | `SIGKILL` and `SIGSTOP` cannot be blocked. Inherited across fork and exec. |
| 42 | `umask` | `(mode_t mask)` | Returns the previous mask. |
| 43 | `ftruncate` | `(int fd, off_t length)` | Grows with zeroes, shrinks by discarding. `EBADF` if the description is not writable. |
| 44 | `chmod` | `(const char* path, mode_t mode)` | Records the bits; nothing checks them yet. Recording the wrong thing now means the whole tree is wrong when something finally does. |
| 45 | `openat` | `(int dirfd, const char* path, int flags, mode_t mode)` | |
| 46 | `fstatat` | `(int dirfd, const char* path, struct stat*, int flags)` | No symlinks exist, so `AT_SYMLINK_NOFOLLOW` is accepted and means nothing. |
| 47 | `unlinkat` | `(int dirfd, const char* path, int flags)` | `AT_REMOVEDIR` selects `rmdir` semantics; without it a directory is `EISDIR`. |
| 48 | `mkdirat` | `(int dirfd, const char* path, mode_t mode)` | |
| 49 | `fchmodat` | `(int dirfd, const char* path, mode_t mode, int flags)` | |
| 50 | `mkfifo` | `(const char* path, mode_t mode)` | A name in the tree with a pipe behind it. Opening one end blocks until the other opens, unless `O_NONBLOCK`; `O_RDWR` is both ends and never blocks. |
| 51 | `openpty` | `(int* master_out, int* slave_out)` | Both ends of a pseudo-terminal, in one call. There is no `/dev/pts`: the pair is reachable only through the two descriptors, which is why they are returned together rather than opened by name. The slave carries a line discipline of its own -- canonical mode, echo, `^C` -- and answers the terminal ioctls; so does the master, because the emulator is the only side that knows how big the window is. |

The `at` family resolves against the descriptor's inode rather than against a
rebuilt path, which is the entire point of it: `du`, `rm -r` and `cp -r`
descend by opening each directory and walking from there, so they never build
a path that a concurrent rename could invalidate. `AT_FDCWD` means the working
directory. `chmod`, `mkdir`, `open`, `stat` and `unlink` keep their own numbers
rather than becoming libc wrappers over these, because they are the calls
almost everything makes and an extra argument on every one of them buys
nothing.

## Extensions

Numbered from `SYS_EXT_BASE` (0x100). These have no POSIX equivalent, and
ported software should never need one — they exist so that `ps`, `free` and
`lsmod` can work before there is a procfs.

| # | Name | Purpose |
|---:|---|---|
| 0x100 | `shitos_sysinfo` | Memory, uptime, process/thread/module counts, context switches. |
| 0x101 | `shitos_procs` | Enumerate processes. |
| 0x102 | `shitos_modules` | Enumerate loaded modules. |
| 0x103 | `shitos_shutdown` | Halt, reboot or power off. |

A `/proc` filesystem should replace the first three; see
[roadmap.md](roadmap.md).

## `/dev/tty`

Not a device: it resolves, on every call, to whichever terminal the calling
process's session is attached to, and reports `ENXIO` when there is none. A
program whose output is in a pipe still has a terminal, and this is the only
way to reach it -- which is why every shell opens it before deciding whether it
can do job control.

A session gets a controlling terminal by claiming one with `TIOCSCTTY`, which
only a session leader may do. `init` claims the console; `forkpty` claims the
pty in the child after `setsid`. It is inherited across `fork` and `exec` and
dropped by `setsid`, because a new session is by definition attached to
nothing yet.

The implicit POSIX rule -- that a session leader opening a terminal by name
acquires it -- is deliberately *not* implemented. `TIOCSCTTY` is explicit, a
pty has no name to be opened by anyway, and the implicit rule mostly surprises
people.

## Not implemented

Deliberately absent rather than stubbed, so a caller finds out at build time
rather than by getting a plausible wrong answer:

- Users and permissions. Everything runs as uid 0 and mode bits are recorded --
  `umask` is applied at creation -- but never checked. `getuid` and friends live
  in the libc and answer 0, because that is true rather than a placeholder.
- `sigpending`, and `sigsuspend` as a syscall. The libc has a sigsuspend that
  polls; see the comment on it for why that is race-free and a real one would
  be better.
- `link` and `symlink`. No filesystem here records more than one name per
  inode, so both fail with `ENOSYS` rather than pretending; `readlink` reports
  `EINVAL`, which is what it says about an ordinary file, after checking the
  file exists.
- Ownership. `chown`, `lchown` and `fchown` live in the libc, check the file
  exists and then succeed without a syscall, because everything already runs as
  the only uid there is. `chmod` is different and is a real call: mode bits are
  recorded even though nothing checks them.
- `settimeofday` and `clock_settime`. The clock is read once at boot from
  whatever driver offers one and never written.
- Threads. One thread per process today, though the Thread/Process split is
  real.
