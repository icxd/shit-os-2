/* SPDX-License-Identifier: MIT */
/* shit os 2 libc -- internals shared between translation units. */

#ifndef _LIBC_INTERNAL_H
#define _LIBC_INTERNAL_H

#include <shitos/abi/syscall.h>
#include <sys/types.h>

/*
 * The x86-64 syscall convention. r10 stands in for rcx because the syscall
 * instruction overwrites rcx with the return address.
 */

static inline long __syscall0(long number)
{
    long result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number) : "rcx", "r11", "memory");
    return result;
}

static inline long __syscall1(long number, long a)
{
    long result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(a) : "rcx", "r11", "memory");
    return result;
}

static inline long __syscall2(long number, long a, long b)
{
    long result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return result;
}

static inline long __syscall3(long number, long a, long b, long c)
{
    long result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return result;
}

static inline long __syscall4(long number, long a, long b, long c, long d)
{
    long result;
    register long r10 __asm__("r10") = d;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory");
    return result;
}

static inline long __syscall6(long number, long a, long b, long c, long d, long e, long f)
{
    long result;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return result;
}

/* Turns a kernel return value into a POSIX one, setting errno on failure. */
long __syscall_return(long value);

void __stdio_initialize(void);
void __stdio_flush_all(void);

extern char** environ;

#endif /* _LIBC_INTERNAL_H */
