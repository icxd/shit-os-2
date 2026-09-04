// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- threads.
//
// A thread is a kernel stack plus a saved InterruptFrame sitting on top of it.
// Switching threads means returning a different frame pointer from the
// interrupt dispatcher, which isr.S then loads into rsp -- there is no separate
// context-switch routine, because the interrupt entry path already saves and
// restores exactly the state a switch needs.

#pragma once

#include <kernel/arch/x86_64/interrupts.h>
#include <kernel/lib/error.h>
#include <kernel/lib/intrusive_list.h>

#include <shitos/types.h>

namespace kernel {

class Process;

enum class ThreadState : u32 {
    Ready, // runnable, waiting for a CPU
    Running, // on a CPU right now
    Blocked, // waiting on a wait queue
    Sleeping, // waiting for a deadline
    Zombie, // finished, waiting to be reaped
};

char const* to_string(ThreadState state);

inline constexpr usize KERNEL_STACK_SIZE = 16 * 1024;
inline constexpr usize THREAD_NAME_MAX = 32;

// How many timer ticks a thread gets before it is preempted, if something else
// is waiting. At 250 Hz this is 20 ms.
inline constexpr u32 DEFAULT_QUANTUM_TICKS = 5;

class Thread {
public:
    static ErrorOr<Thread*> create_kernel_thread(
        char const* name, void (*entry)(void*), void* argument);

    // Wraps the context the kernel is already running in, so that the very
    // first context switch has somewhere to save its state.
    static ErrorOr<Thread*> adopt_current_context(char const* name);

    // A thread that starts in ring 3 at `entry` with `user_stack`.
    static ErrorOr<Thread*> create_user_thread(char const* name, u64 entry, u64 user_stack);

    // A thread resuming from a copy of somebody else's frame, which is what
    // fork() is: same registers, different address space, rax zero.
    static ErrorOr<Thread*> create_from_frame(char const* name, InterruptFrame const& frame);

    ~Thread();

    u32 tid() const { return m_tid; }
    char const* name() const { return m_name; }
    ThreadState state() const { return m_state; }
    u64 cpu_ticks() const { return m_cpu_ticks; }
    Process* process() const { return m_process; }
    void set_process(Process* process) { m_process = process; }

    InterruptFrame* frame() const { return m_frame; }
    void set_frame(InterruptFrame* frame) { m_frame = frame; }

    u64 kernel_stack_top() const { return m_kernel_stack_top; }

    // Nodes for the lists a thread can be in. A thread is in at most one of
    // the run queue and a wait queue, but is always in the global list.
    ListNode<Thread> run_queue_node;
    ListNode<Thread> wait_queue_node;
    ListNode<Thread> global_node;

private:
    friend class Scheduler;
    friend class WaitQueue;

    Thread() = default;

    // Allocates a Thread plus its kernel stack and reserves a frame at the top
    // of that stack. Shared by every factory above.
    static ErrorOr<Thread*> allocate_with_stack(char const* name, InterruptFrame*& frame_out);

    u32 m_tid { 0 };
    char m_name[THREAD_NAME_MAX] {};
    ThreadState m_state { ThreadState::Ready };
    Process* m_process { nullptr };

    InterruptFrame* m_frame { nullptr };

    u8* m_kernel_stack { nullptr };
    u64 m_kernel_stack_top { 0 };
    bool m_owns_kernel_stack { false };

    u32 m_quantum_remaining { DEFAULT_QUANTUM_TICKS };
    u64 m_cpu_ticks { 0 };
    u64 m_wake_at_tick { 0 };
};

} // namespace kernel
