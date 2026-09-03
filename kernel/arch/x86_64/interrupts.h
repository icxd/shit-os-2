// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- interrupt descriptor table, traps and IRQ routing.

#pragma once

#include <shitos/module/api.h>
#include <shitos/types.h>

namespace kernel {

// Exactly what the ISR stubs and the CPU leave on the stack, in memory order.
// The scheduler also builds these by hand when it starts a new thread, so the
// layout is part of the kernel's internal contract -- see isr.S.
struct InterruptFrame {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vector;
    u64 error_code;
    // Pushed by the CPU. rsp and ss are always present in long mode, even for
    // a trap that did not change privilege level.
    u64 rip, cs, rflags, rsp, ss;

    bool from_userspace() const { return (cs & 3) != 0; }
};

} // namespace kernel

namespace kernel::arch {

inline constexpr u8 IRQ_BASE_VECTOR = 32;
inline constexpr u8 IRQ_COUNT = 16;
inline constexpr u8 MAX_HANDLERS_PER_IRQ = 4;

// A handler returns the frame that should be resumed. That is almost always
// the one it was handed; returning a different one is how the scheduler
// preempts, since isr_common simply switches rsp to whatever comes back.
using TrapHandler = InterruptFrame* (*)(InterruptFrame*);

void idt_initialize();

// Low-level: claim a raw vector. Used by the kernel itself (timer, page fault,
// syscall) rather than by drivers.
void register_trap_handler(u8 vector, TrapHandler handler);

// The IRQ layer drivers see through KernelApi. Several modules may share one
// line; handlers are called in registration order until one claims it.
ModuleResult register_irq_handler(u8 irq, IrqHandler handler, void* self);
void unregister_irq_handler(u8 irq, void* self);

void irq_mask(u8 irq);
void irq_unmask(u8 irq);

u64 interrupt_count(u8 vector);

// Prints a register dump for `frame`. Used by the fault handlers and by panic.
void dump_interrupt_frame(InterruptFrame const* frame);

} // namespace kernel::arch
