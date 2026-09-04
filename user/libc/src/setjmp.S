/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- setjmp and longjmp.
 *
 * The jmp_buf holds only what the SysV ABI says a function call preserves:
 *
 *   0  rbx    16  r12    32  r14    48  rsp (as it will be after ret)
 *   8  rbp    24  r13    40  r15    56  rip (the return address)
 *
 * Caller-saved registers are deliberately absent. A longjmp lands as though
 * the original setjmp had just returned, and at that point the ABI permits
 * rax, rcx, rdx, rsi, rdi and r8-r11 to hold anything at all.
 */

.section .text

.global setjmp
.type setjmp, @function
setjmp:
    movq %rbx, 0(%rdi)
    movq %rbp, 8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)

    /* The stack pointer to restore is the one after this function returns,
     * which is the current rsp plus the return address sitting on it. */
    leaq 8(%rsp), %rax
    movq %rax, 48(%rdi)

    movq (%rsp), %rax
    movq %rax, 56(%rdi)

    xorl %eax, %eax             /* a direct call returns 0 */
    ret

.global longjmp
.type longjmp, @function
longjmp:
    movl %esi, %eax
    testl %eax, %eax
    jnz 1f
    movl $1, %eax               /* longjmp(env, 0) must still return 1 */
1:
    movq 0(%rdi), %rbx
    movq 8(%rdi), %rbp
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15
    movq 48(%rdi), %rsp

    /* Jumping to the saved return address makes setjmp appear to return a
     * second time, with rax holding the value passed here. */
    jmp *56(%rdi)

.section .note.GNU-stack, "", @progbits
