// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- wait queues.
//
// The blocking primitive drivers get through KernelApi. A thread waiting on
// one is off the run queue entirely, so a driver waiting for a key press costs
// nothing until the key arrives.

#pragma once

#include <kernel/lib/intrusive_list.h>
#include <kernel/lib/spinlock.h>
#include <kernel/sched/thread.h>

namespace kernel {

class WaitQueue {
public:
    constexpr WaitQueue() = default;

    // Blocks the calling thread until somebody wakes this queue. Illegal from
    // interrupt context -- there is no thread there to block.
    void wait();

    void wake_one();

    // Safe to call from an interrupt handler, which is the usual case: a
    // device interrupt arrives and releases whoever was waiting for it.
    void wake_all();

    usize waiter_count() const;

private:
    IntrusiveList<Thread, &Thread::wait_queue_node> m_waiters;
    mutable InterruptSpinLock m_lock;
};

} // namespace kernel
