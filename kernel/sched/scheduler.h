// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- the scheduler.
//
// Round-robin over a per-CPU run queue with a fixed quantum. Preemption
// happens on the timer tick; a thread can also give up the CPU voluntarily,
// which raises a software interrupt so that the switch goes through exactly
// the same code path as an involuntary one.
//
// There is one run queue because there is one CPU. It lives in the per-CPU
// block rather than in a global, so bringing up more CPUs is a matter of
// starting them, not of restructuring this.

#pragma once

#include <kernel/arch/x86_64/interrupts.h>
#include <kernel/lib/error.h>
#include <kernel/sched/thread.h>

#include <shitos/types.h>

namespace kernel {

// The vector a thread raises to ask for a reschedule. Chosen high enough to
// stay out of the way of the exception and IRQ ranges.
inline constexpr u8 VECTOR_YIELD = 0xFE;

class Scheduler {
public:
    static void initialize();
    static bool is_running();

    static Thread* current();

    // Makes a thread runnable and puts it on the queue.
    static void enqueue(Thread* thread);

    // Give up the rest of this quantum. Returns once scheduled again.
    static void yield();

    // Take the current thread off the run queue until somebody unblocks it.
    // The caller must have arranged for that to happen first.
    static void block_current(ThreadState reason);
    static void unblock(Thread* thread);

    static void sleep_ms(u64 milliseconds);

    [[noreturn]] static void exit_current();

    static u64 ticks();
    static u64 uptime_ms();
    static u64 context_switches();
    static usize thread_count();
    static usize runnable_count();

    // Calls `callback` for every thread that exists. Used by `ps` and by the
    // self tests; holds the scheduler lock throughout, so the callback must
    // not block or allocate.
    static void for_each_thread(void (*callback)(Thread*, void*), void* context);

private:
    friend class WaitQueue;

    static InterruptFrame* on_timer_tick(InterruptFrame* frame);
    static InterruptFrame* on_yield_request(InterruptFrame* frame);
    static InterruptFrame* switch_to_next(InterruptFrame* frame, bool requeue_current);
};

} // namespace kernel
