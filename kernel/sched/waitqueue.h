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

    // Blocks only if nothing has woken this queue since `generation` was read.
    //
    // A caller that tests a condition and then sleeps has a window between the
    // two, and a wake that lands in it is lost -- with no timeout, lost
    // forever. Reading the counter *before* the test and passing it here
    // closes that: the comparison and the enqueue happen under the same lock
    // the wake takes, so the wake is either seen by the test or seen here.
    void wait_since(u64 generation);

    // Bumped by every wake. Read it before testing whatever you are waiting on.
    u64 generation() const;

    void wake_one();

    // Safe to call from an interrupt handler, which is the usual case: a
    // device interrupt arrives and releases whoever was waiting for it.
    void wake_all();

    usize waiter_count() const;

private:
    void detach_self();

    IntrusiveList<Thread, &Thread::wait_queue_node> m_waiters;
    mutable InterruptSpinLock m_lock;
    u64 m_generation { 0 };
};

/*
 * The queue every poller waits on.
 *
 * A thread can only be on one wait list -- the node is shared with the sleeper
 * list -- so a poll over several descriptors cannot register on each of them.
 * The alternative to a timer was one queue that everything wakes: any wake_all
 * anywhere is a readiness change somewhere, and a poller that wakes for an
 * event that was not its own simply re-checks and sleeps again.
 *
 * The cost is a spurious wake; the cost of the timer it replaced was four
 * milliseconds of latency per hop, and a keystroke crosses three of them
 * before a pixel changes.
 */
WaitQueue& poll_queue();

} // namespace kernel
