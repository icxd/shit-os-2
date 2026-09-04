/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- the syscall gate.
 *
 * The `syscall` instruction is fast because it does almost nothing: it loads
 * CS and SS from the STAR MSR, puts the return address in rcx and the old
 * rflags in r11, and jumps to LSTAR. In particular it does not switch stacks,
 * so the first thing this stub has to do is find one, which is what the
 * per-CPU scratch slot is for.
 *
 * Once a stack exists, the stub builds exactly the same InterruptFrame the
 * interrupt stubs build and joins the shared return path. Every entry into the
 * kernel then looks the same to everything above it.
 */

#include <kernel/arch/x86_64/percpu_offsets.h>

/* Selector values, mirroring kernel::arch::SELECTOR_* in gdt.h. */
#define SELECTOR_USER_CODE 0x23
#define SELECTOR_USER_DATA 0x1B

/* A pseudo-vector so a syscall frame is identifiable in a register dump. */
#define VECTOR_SYSCALL 0x100

.section .text, "ax", @progbits
.code64

.global syscall_entry
.type syscall_entry, @function
syscall_entry:
    /* Userspace owns the GS base until this instruction. */
    swapgs

    /* Park the user's rsp, then take this CPU's kernel stack. */
    movq %rsp, %gs:CPU_OFFSET_SYSCALL_SCRATCH_RSP
    movq %gs:CPU_OFFSET_KERNEL_STACK_TOP, %rsp

    /* Synthesise the five words the CPU would have pushed for an interrupt. */
    pushq $SELECTOR_USER_DATA
    pushq %gs:CPU_OFFSET_SYSCALL_SCRATCH_RSP
    pushq %r11                      /* syscall stashed rflags here */
    pushq $SELECTOR_USER_CODE
    pushq %rcx                      /* ...and the return address here */

    pushq $0                        /* error code: none for a syscall */
    pushq $VECTOR_SYSCALL

    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    cld

    movq %rsp, %rdi
    call syscall_dispatch

    /*
     * As in isr_common, the dispatcher returns the frame to resume, which is a
     * different one whenever the syscall blocked and something else ran.
     */
    movq %rax, %rsp
    jmp isr_return

.section .note.GNU-stack, "", @progbits
