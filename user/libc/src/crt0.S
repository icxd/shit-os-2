/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- program entry.
 *
 * The kernel enters here with rsp pointing at argc, followed by argv, a null,
 * envp, another null, and then the auxiliary vector. Nothing is in a register:
 * everything the program needs is on the stack, which is what the SysV ABI
 * specifies and what a dynamic linker would also expect to find.
 */

.section .text
.global _start
.type _start, @function
_start:
    /* A zero rbp terminates any backtrace that walks up here. */
    xorq %rbp, %rbp

    movq (%rsp), %rdi           /* argc */
    leaq 8(%rsp), %rsi          /* argv */
    /* envp sits just past argv's terminating null: argv + (argc + 1) * 8 */
    leaq 16(%rsp,%rdi,8), %rdx

    /* The ABI wants rsp 16-aligned at a call; nothing pushed a return address
     * to get here, so align it by hand. */
    andq $-16, %rsp

    call __libc_start

    /* __libc_start never returns, but if it somehow did, do not fall into
     * whatever bytes follow. */
    hlt

/*
 * Where a signal handler returns to. The kernel puts the address of this in
 * the handler's return slot, so that finishing the handler runs sigreturn,
 * which puts back the context the signal interrupted.
 */
.global __libc_sigreturn_trampoline
.type __libc_sigreturn_trampoline, @function
__libc_sigreturn_trampoline:
    movq $28, %rax              /* SYS_sigreturn */
    syscall
    hlt

.section .note.GNU-stack, "", @progbits
