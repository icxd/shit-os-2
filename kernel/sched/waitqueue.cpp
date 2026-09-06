// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- wait queues.

#include <kernel/arch/x86_64/percpu.h>
#include <kernel/panic.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/waitqueue.h>

namespace kernel {

void WaitQueue::wait()
{
    auto* thread = Scheduler::current();
    VERIFY(thread != nullptr);

    {
        InterruptLockGuard guard(m_lock);
        m_waiters.append(thread);
    }

    // Between appending and blocking, a wake could arrive and find this thread
    // still Running, so it would not be moved to the run queue. block_current
    // marks the state and yields with interrupts masked around the state
    // change, and unblock() is idempotent for an already-runnable thread, so
    // the worst case is one spurious wake rather than a lost one.
    Scheduler::block_current(ThreadState::Blocked);
    detach_self();
}

void WaitQueue::detach_self()
{
    auto* thread = Scheduler::current();
    InterruptLockGuard guard(m_lock);
    if (thread != nullptr && thread->wait_queue_node.linked)
        m_waiters.remove(thread);
}

u64 WaitQueue::generation() const
{
    InterruptLockGuard guard(m_lock);
    return m_generation;
}

void WaitQueue::wait_since(u64 generation)
{
    auto* thread = Scheduler::current();
    VERIFY(thread != nullptr);

    {
        InterruptLockGuard guard(m_lock);
        if (m_generation != generation)
            return; // something happened while the caller was looking
        m_waiters.append(thread);
    }

    Scheduler::block_current(ThreadState::Blocked);
    detach_self();
}

void WaitQueue::wake_one()
{
    Thread* thread = nullptr;
    {
        InterruptLockGuard guard(m_lock);
        thread = m_waiters.take_first();
    }
    if (thread != nullptr)
        Scheduler::unblock(thread);
}

WaitQueue& poll_queue()
{
    static WaitQueue s_queue;
    return s_queue;
}

void WaitQueue::wake_all()
{
    {
        InterruptLockGuard guard(m_lock);
        ++m_generation;
    }

    for (;;) {
        Thread* thread = nullptr;
        {
            InterruptLockGuard guard(m_lock);
            thread = m_waiters.take_first();
        }
        if (thread == nullptr)
            break;
        Scheduler::unblock(thread);
    }

    /*
     * Anything becoming ready is something a poller might be waiting for, and
     * from here there is no way to know which poller or which descriptor. So
     * they are all woken to re-check. Hooking it here rather than at every
     * call site is deliberate: a waker that someone forgets to add is a poll
     * that never returns, and this cannot be forgotten.
     */
    if (this != &poll_queue())
        poll_queue().wake_all();
}

usize WaitQueue::waiter_count() const
{
    InterruptLockGuard guard(m_lock);
    return m_waiters.size();
}

} // namespace kernel
