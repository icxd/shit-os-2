// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- threads.

#include <kernel/arch/x86_64/gdt.h>
#include <kernel/dev/console.h>
#include <kernel/lib/new.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/thread.h>
#include <kernel/panic.h>

extern "C" {
extern u8 boot_stack_top[];
}

namespace kernel {

namespace {

u32 s_next_tid = 1;

} // namespace

char const* to_string(ThreadState state)
{
    switch (state) {
    case ThreadState::Ready:
        return "ready";
    case ThreadState::Running:
        return "running";
    case ThreadState::Blocked:
        return "blocked";
    case ThreadState::Sleeping:
        return "sleeping";
    case ThreadState::Zombie:
        return "zombie";
    }
    return "?";
}

namespace {

// Every kernel thread starts here rather than at its entry point directly, so
// that a thread whose function simply returns is retired cleanly instead of
// popping a return address that was never pushed.
extern "C" void kernel_thread_trampoline(void (*entry)(void*), void* argument)
{
    entry(argument);
    Scheduler::exit_current();
}

} // namespace

ErrorOr<Thread*> Thread::create_kernel_thread(char const* name, void (*entry)(void*), void* argument)
{
    auto* thread = static_cast<Thread*>(kzalloc(sizeof(Thread)));
    if (thread == nullptr)
        return Error::from_errno(ENOMEM);
    new (thread) Thread();

    auto* stack = static_cast<u8*>(kmalloc_aligned(KERNEL_STACK_SIZE, PAGE_SIZE));
    if (stack == nullptr) {
        kfree(thread);
        return Error::from_errno(ENOMEM);
    }

    thread->m_tid = s_next_tid++;
    strncpy(thread->m_name, name, THREAD_NAME_MAX - 1);
    thread->m_kernel_stack = stack;
    thread->m_owns_kernel_stack = true;
    thread->m_kernel_stack_top = reinterpret_cast<u64>(stack) + KERNEL_STACK_SIZE;
    thread->m_state = ThreadState::Ready;

    // Build the frame the dispatcher will "resume" the first time this thread
    // is scheduled. iretq pops all five of rip/cs/rflags/rsp/ss even when the
    // privilege level does not change, so a kernel thread's first run looks
    // exactly like a return from an interrupt.
    u64 stack_top = align_down<u64>(thread->m_kernel_stack_top, 16);
    auto* frame = reinterpret_cast<InterruptFrame*>(stack_top - sizeof(InterruptFrame));
    memset(frame, 0, sizeof(InterruptFrame));

    frame->rip = reinterpret_cast<u64>(&kernel_thread_trampoline);
    frame->cs = arch::SELECTOR_KERNEL_CODE;
    frame->ss = arch::SELECTOR_KERNEL_DATA;
    // IF set, and bit 1 which is always set in rflags.
    frame->rflags = 0x202;
    // The SysV ABI expects rsp % 16 == 8 on entry to a function, because a
    // call would have pushed a return address. Nothing called us, so fake it.
    frame->rsp = stack_top - 8;
    frame->rdi = reinterpret_cast<u64>(entry);
    frame->rsi = reinterpret_cast<u64>(argument);

    thread->m_frame = frame;
    return thread;
}

ErrorOr<Thread*> Thread::adopt_current_context(char const* name)
{
    auto* thread = static_cast<Thread*>(kzalloc(sizeof(Thread)));
    if (thread == nullptr)
        return Error::from_errno(ENOMEM);
    new (thread) Thread();

    thread->m_tid = s_next_tid++;
    strncpy(thread->m_name, name, THREAD_NAME_MAX - 1);
    thread->m_state = ThreadState::Running;

    // This context is already running on the boot stack from boot.S. It does
    // not own that stack and must never try to free it.
    thread->m_kernel_stack = nullptr;
    thread->m_owns_kernel_stack = false;
    thread->m_kernel_stack_top = reinterpret_cast<u64>(boot_stack_top);
    // m_frame is filled in by the first context switch away from here.
    thread->m_frame = nullptr;

    return thread;
}

Thread::~Thread()
{
    if (m_owns_kernel_stack && m_kernel_stack != nullptr)
        kfree_aligned(m_kernel_stack);
}

} // namespace kernel
