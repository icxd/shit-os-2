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

    u64 kernel_stack_top;
};

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
