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

    InterruptLockGuard guard(m_lock);
    if (thread->wait_queue_node.linked)
        m_waiters.remove(thread);
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

void WaitQueue::wake_all()
{
    for (;;) {
        Thread* thread = nullptr;
        {
            InterruptLockGuard guard(m_lock);
            thread = m_waiters.take_first();
        }
        if (thread == nullptr)
            return;
        Scheduler::unblock(thread);
    }
}

usize WaitQueue::waiter_count() const
{
    InterruptLockGuard guard(m_lock);
    return m_waiters.size();
}

} // namespace kernel
