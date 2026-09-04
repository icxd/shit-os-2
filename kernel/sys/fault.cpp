// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- CPU faults raised by userspace.
//
// Without this, a null dereference in a shell command takes the whole machine
// down, which is the behaviour gen 1 was famous for. A fault in ring 3 is the
// process's problem: it becomes a signal, and the default action for all of
// these is to kill that process and nothing else.
//
// A fault in ring 0 is still a panic, because it means the kernel is already
// wrong and continuing would only obscure where.

#include <kernel/arch/x86_64/interrupts.h>
#include <kernel/arch/x86_64/io.h>
#include <kernel/dev/console.h>
#include <kernel/panic.h>
#include <kernel/sched/process.h>
#include <kernel/sys/syscall.h>

#include <shitos/abi/signal.h>

namespace kernel::sys {

namespace {

struct FaultMapping {
    u8 vector;
    int signal;
    char const* description;
};

// The faults a user program can plausibly cause, and what POSIX says each one
// turns into.
constexpr FaultMapping FAULT_MAPPINGS[] = {
    { 0, SIGFPE, "divide by zero" },
    { 4, SIGSEGV, "overflow" },
    { 5, SIGSEGV, "bound range exceeded" },
    { 6, SIGILL, "invalid opcode" },
    { 12, SIGSEGV, "stack-segment fault" },
    { 13, SIGSEGV, "general protection fault" },
    { 14, SIGSEGV, "page fault" },
    { 17, SIGBUS, "alignment check" },
};

int signal_for_vector(u8 vector)
{
    for (auto const& mapping : FAULT_MAPPINGS) {
        if (mapping.vector == vector)
            return mapping.signal;
    }
    return SIGSEGV;
}

char const* description_for_vector(u8 vector)
{
    for (auto const& mapping : FAULT_MAPPINGS) {
        if (mapping.vector == vector)
            return mapping.description;
    }
    return "fault";
}

InterruptFrame* on_fault(InterruptFrame* frame)
{
    auto const vector = static_cast<u8>(frame->vector);

    if (!frame->from_userspace()) {
        // Kernel fault. Let the generic handler print the full diagnosis and
        // stop; there is nothing to isolate here.
        kprintf("\n  cpu exception %llu: %s in kernel mode\n", frame->vector,
            description_for_vector(vector));
        if (vector == 14) {
            kprintf("  faulting address: %p\n", reinterpret_cast<void*>(arch::read_cr2()));
            kprintf("  error code: %p\n", reinterpret_cast<void*>(frame->error_code));
        }
        arch::dump_interrupt_frame(frame);
        panic("unhandled %s at %p", description_for_vector(vector),
            reinterpret_cast<void*>(frame->rip));
    }

    auto* process = Process::current();
    int const signal = signal_for_vector(vector);

    if (vector == 14) {
        klog(LOG_INFO, "fault", "%s[%d]: %s at %p (rip %p)", process->name(), process->pid(),
            description_for_vector(vector), reinterpret_cast<void*>(arch::read_cr2()),
            reinterpret_cast<void*>(frame->rip));
    } else {
        klog(LOG_INFO, "fault", "%s[%d]: %s at rip %p", process->name(), process->pid(),
            description_for_vector(vector), reinterpret_cast<void*>(frame->rip));
    }

    process->raise_signal(signal);

    // Interrupt gates arrive with interrupts masked. Delivery may block or
    // switch away, so let the timer back in first.
    interrupts_enable();

    return deliver_pending_signal(frame);
}

} // namespace

void faults_initialize()
{
    for (auto const& mapping : FAULT_MAPPINGS)
        arch::register_trap_handler(mapping.vector, on_fault);

    klog(LOG_INFO, "fault", "%zu user-recoverable faults routed to signals",
        sizeof(FAULT_MAPPINGS) / sizeof(FAULT_MAPPINGS[0]));
}

} // namespace kernel::sys
