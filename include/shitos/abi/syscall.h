/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- system call numbers.
 *
 * Numbers 0..0xff are the POSIX surface. Numbers from SYS_EXT_BASE up are shit
 * os extensions with no POSIX equivalent; ported software must never need one.
 *
 * The calling convention is the x86-64 SysV syscall convention:
 *
 *   rax = syscall number      rax = return value
 *   rdi = arg0                (negative value == -errno)
 *   rsi = arg1
 *   rdx = arg2                clobbered: rcx (saved rip), r11 (saved rflags)
 *   r10 = arg3                preserved: everything else
 *   r8  = arg4
 *   r9  = arg5
 *
 * Note rcx is *not* an argument register here: the `syscall` instruction
 * overwrites it with the return address, which is why arg3 lives in r10.
 */

#pragma once

#include <shitos/types.h>

#define SYS_exit 0
#define SYS_read 1
#define SYS_write 2
#define SYS_open 3
#define SYS_close 4
#define SYS_lseek 5
#define SYS_fork 6
#define SYS_execve 7
#define SYS_waitpid 8
#define SYS_getpid 9
#define SYS_getppid 10
#define SYS_brk 11
#define SYS_mmap 12
#define SYS_munmap 13
#define SYS_dup 14
#define SYS_dup2 15
#define SYS_pipe 16
#define SYS_stat 17
#define SYS_fstat 18
#define SYS_getdents 19
#define SYS_mkdir 20
#define SYS_rmdir 21
#define SYS_unlink 22
#define SYS_chdir 23
#define SYS_getcwd 24
#define SYS_ioctl 25
#define SYS_kill 26
#define SYS_sigaction 27
#define SYS_sigreturn 28
#define SYS_nanosleep 29
#define SYS_uname 30
#define SYS_sched_yield 31
#define SYS_isatty 32
#define SYS_clock_gettime 33
#define SYS_fcntl 34
#define SYS_rename 35
#define SYS_setpgid 36
#define SYS_getpgid 37
#define SYS_setsid 38
#define SYS_getsid 39
#define SYS_poll 40
#define SYS_sigprocmask 41
#define SYS_umask 42
#define SYS_ftruncate 43
#define SYS_chmod 44
#define SYS_openat 45
#define SYS_fstatat 46
#define SYS_unlinkat 47
#define SYS_mkdirat 48
#define SYS_fchmodat 49

#define SYS_EXT_BASE 0x100

/* Machine-readable system statistics, for `free` and friends. */
#define SYS_shitos_sysinfo (SYS_EXT_BASE + 0)
/* Enumerate processes, for `ps`. A procfs will replace this; see docs/roadmap.md. */
#define SYS_shitos_procs (SYS_EXT_BASE + 1)
/* Enumerate loaded modules, for `lsmod`. */
#define SYS_shitos_modules (SYS_EXT_BASE + 2)
/* Halt, reboot or power off the machine. */
#define SYS_shitos_shutdown (SYS_EXT_BASE + 3)

#define SYS_MAX_POSIX 50
#define SYS_MAX_EXT 4

/* --- SYS_shitos_sysinfo ------------------------------------------------- */

struct shitos_sysinfo {
    u64 uptime_ms;
    u64 mem_total_bytes;
    u64 mem_free_bytes;
    u64 mem_kernel_heap_bytes;
    u64 page_size;
    u64 process_count;
    u64 thread_count;
    u64 module_count;
    u64 context_switches;
};

/* --- SYS_shitos_procs --------------------------------------------------- */

#define SHITOS_PROC_NAME_MAX 32

struct shitos_procinfo {
    i32 pid;
    i32 ppid;
    u32 state; /* see SHITOS_PROC_STATE_* */
    u32 thread_count;
    u64 rss_bytes;
    u64 cpu_ticks;
    char name[SHITOS_PROC_NAME_MAX];
};

#define SHITOS_PROC_STATE_RUNNING 0
#define SHITOS_PROC_STATE_READY 1
#define SHITOS_PROC_STATE_BLOCKED 2
#define SHITOS_PROC_STATE_ZOMBIE 3

/* --- SYS_shitos_modules ------------------------------------------------- */

#define SHITOS_MODULE_NAME_MAX 32

struct shitos_moduleinfo {
    char name[SHITOS_MODULE_NAME_MAX];
    u32 abi_version;
    u32 flags;
    u64 base;
    u64 size;
};

/* --- SYS_shitos_shutdown ------------------------------------------------ */

#define SHITOS_SHUTDOWN_HALT 0
#define SHITOS_SHUTDOWN_REBOOT 1
#define SHITOS_SHUTDOWN_POWEROFF 2
