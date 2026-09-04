/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- non-local jumps.
 *
 * Only the callee-saved registers, the stack pointer and the return address
 * are preserved, which is exactly what the ABI guarantees a function to keep.
 * Anything in a caller-saved register at the time of setjmp is undefined after
 * a longjmp, which is why the standard says to declare such variables volatile.
 */

#ifndef _SETJMP_H
#define _SETJMP_H

/* rbx, rbp, r12, r13, r14, r15, rsp, rip */
typedef unsigned long jmp_buf[8];
typedef unsigned long sigjmp_buf[8];

int setjmp(jmp_buf environment) __attribute__((returns_twice));
__attribute__((noreturn)) void longjmp(jmp_buf environment, int value);

/*
 * No signal mask is saved or restored: this system has no sigprocmask yet, so
 * the two forms cannot differ. They are aliases rather than a pretence.
 */
#define sigsetjmp(env, savemask) setjmp(env)
#define siglongjmp(env, value) longjmp(env, value)

#endif /* _SETJMP_H */
