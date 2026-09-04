// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- per-CPU state.
//
// Reached through the GS segment base rather than a global, so that when the
// application processors are brought up this file changes and its callers do
// not. That is the entire content of the "SMP-ready, one CPU" decision: today
// MAX_CPUS is effectively one, but nothing above here assumes it.
//
// The first field is a pointer to the block itself, which is what makes
// `movq %gs:0, %rax` able to produce a usable pointer in one instruction.

#pragma once

#include <kernel/arch/x86_64/percpu_offsets.h>
#include <kernel/lib/kstd.h>

#include <shitos/types.h>

namespace kernel {
class Thread;
}

namespace kernel::arch {

inline constexpr usize MAX_CPUS = 8;

struct Cpu {
    Cpu* self; // must stay first; gs:0 is read directly

    u32 id;
    u32 lapic_id;

    Thread* current_thread;
    Thread* idle_thread;

    u64 ticks;
    u64 context_switches;

    // Depth of nested interrupt-disabling sections. Preemption is only legal
    // at depth zero.
    u32 preempt_disable_count;

    // Where a trap or syscall from ring 3 should land. Updated on every
    // context switch, alongside the TSS's rsp0.
    u64 kernel_stack_top;

    // syscall gives us no stack, so the entry stub parks the user's rsp here
    // for the two instructions it takes to switch to the kernel one.
    u64 syscall_scratch_rsp;
};

// Offsets the assembly entry stubs use. Kept honest by the static_asserts
// below, so moving a field breaks the build rather than the kernel.
static_assert(__builtin_offsetof(Cpu, self) == 0, "gs:0 must be the self pointer");
static_assert(__builtin_offsetof(Cpu, kernel_stack_top) == CPU_OFFSET_KERNEL_STACK_TOP);
static_assert(__builtin_offsetof(Cpu, syscall_scratch_rsp) == CPU_OFFSET_SYSCALL_SCRATCH_RSP);

void percpu_initialize_bootstrap();

inline Cpu* this_cpu()
{
    Cpu* cpu;
    asm volatile("movq %%gs:0, %0" : "=r"(cpu)::"memory");
    return cpu;
}

usize cpu_count();
Cpu& cpu_by_index(usize index);

} // namespace kernel::arch
