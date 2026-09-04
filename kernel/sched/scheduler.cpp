// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- the scheduler.

#include <kernel/arch/x86_64/gdt.h>
#include <kernel/arch/x86_64/percpu.h>
#include <kernel/arch/x86_64/pit.h>
#include <kernel/dev/console.h>
#include <kernel/lib/spinlock.h>
#include <kernel/mm/heap.h>
#include <kernel/panic.h>
#include <kernel/sched/process.h>
#include <kernel/sched/scheduler.h>

namespace kernel {

namespace {

IntrusiveList<Thread, &Thread::run_queue_node> s_run_queue;
IntrusiveList<Thread, &Thread::global_node> s_all_threads;
IntrusiveList<Thread, &Thread::run_queue_node> s_zombies;

// Taken from the timer interrupt, so it must mask interrupts rather than spin.
InterruptSpinLock s_lock;

bool s_running = false;
Thread* s_idle_thread = nullptr;

// A thread that has gone to sleep is kept here with its deadline, rather than
// in a sorted timer wheel. With a handful of threads a linear scan on each
// tick is cheaper than maintaining the ordering.
IntrusiveList<Thread, &Thread::wait_queue_node> s_sleepers;

void idle_loop(void*)
{
    for (;;) {
        // Reap anything that exited. Doing it here rather than in exit_current
        // means a thread is never freeing the stack it is standing on.
        for (;;) {
            Thread* zombie = nullptr;
            {
                InterruptLockGuard guard(s_lock);
                zombie = s_zombies.take_first();
                if (zombie != nullptr)
                    s_all_threads.remove(zombie);
            }
            if (zombie == nullptr)
                break;
            zombie->~Thread();
            kfree(zombie);
        }

        // hlt until the next interrupt. Without this the idle loop would spin
        // the host CPU at 100% for no reason.
        asm volatile("sti; hlt");
    }
}

} // namespace

void Scheduler::initialize()
{
    auto* cpu = arch::this_cpu();

    auto boot_thread = Thread::adopt_current_context("kmain");
    if (boot_thread.is_error())
        panic("could not adopt the boot context as a thread");

    auto idle = Thread::create_kernel_thread("idle", idle_loop, nullptr);
    if (idle.is_error())
        panic("could not create the idle thread");

    s_idle_thread = idle.value();
    // The idle thread is never on the run queue: it is what runs when the run
    // queue is empty, so queueing it would let it compete with real work.
    s_all_threads.append(s_idle_thread);
    s_all_threads.append(boot_thread.value());

    cpu->current_thread = boot_thread.value();
    cpu->idle_thread = s_idle_thread;

    arch::register_trap_handler(VECTOR_YIELD, on_yield_request);
    arch::pit_set_callback(on_timer_tick);

    s_running = true;

    klog(LOG_INFO, "sched", "round robin, %u tick quantum (%llu ms), running as '%s'",
        DEFAULT_QUANTUM_TICKS,
        static_cast<u64>(DEFAULT_QUANTUM_TICKS) * arch::MILLISECONDS_PER_TICK,
        cpu->current_thread->name());
}

bool Scheduler::is_running() { return s_running; }

Thread* Scheduler::current() { return arch::this_cpu()->current_thread; }

void Scheduler::enqueue(Thread* thread)
{
    InterruptLockGuard guard(s_lock);
    if (!thread->global_node.linked)
        s_all_threads.append(thread);
    thread->m_state = ThreadState::Ready;
    s_run_queue.append(thread);
}

InterruptFrame* Scheduler::switch_to_next(InterruptFrame* frame, bool requeue_current)
{
    auto* cpu = arch::this_cpu();
    Thread* previous = cpu->current_thread;

    if (previous != nullptr) {
        // Save where this thread was, so resuming it later is just a matter of
        // handing this pointer back to isr.S.
        previous->m_frame = frame;
        if (requeue_current && previous->m_state == ThreadState::Running) {
            previous->m_state = ThreadState::Ready;
            if (previous != s_idle_thread)
                s_run_queue.append(previous);
        }
    }

    Thread* next = s_run_queue.take_first();
    if (next == nullptr)
        next = s_idle_thread;

    next->m_state = ThreadState::Running;
    next->m_quantum_remaining = DEFAULT_QUANTUM_TICKS;
    cpu->current_thread = next;

    if (next != previous)
        ++cpu->context_switches;

    // A trap or syscall taken while this thread is in userspace has to land on
    // this thread's kernel stack, not on whichever one was there before. The
    // TSS is what the CPU reads for a trap; the per-CPU copy is what the
    // syscall stub reads, since syscall does not consult the TSS at all.
    arch::tss_set_kernel_stack(next->kernel_stack_top());
    cpu->kernel_stack_top = next->kernel_stack_top();

    // Switching to a thread in a different address space means switching CR3.
    // Kernel threads have no address space of their own and simply keep
    // whichever one was already loaded -- the kernel half is identical in all
    // of them, so that is safe.
    if (auto* process = next->process(); process != nullptr) {
        auto* space = process->address_space();
        if (space != nullptr && (previous == nullptr || previous->process() == nullptr
                || previous->process()->address_space() != space)) {
            space->activate();
        }
    }

    return next->m_frame;
}

InterruptFrame* Scheduler::on_timer_tick(InterruptFrame* frame)
{
    if (!s_running)
        return frame;

    auto* cpu = arch::this_cpu();
    ++cpu->ticks;

    u64 const now = arch::pit_ticks();

    InterruptLockGuard guard(s_lock);

    // Wake anything whose deadline has passed.
    for (Thread* sleeper : s_sleepers) {
        if (sleeper->m_wake_at_tick <= now) {
            s_sleepers.remove(sleeper);
            sleeper->m_state = ThreadState::Ready;
            s_run_queue.append(sleeper);
        }
    }

    Thread* current_thread = cpu->current_thread;
    if (current_thread != nullptr) {
        ++current_thread->m_cpu_ticks;
        if (current_thread->m_quantum_remaining > 0)
            --current_thread->m_quantum_remaining;
    }

    bool const quantum_expired = current_thread == nullptr || current_thread->m_quantum_remaining == 0;
    bool const someone_waiting = !s_run_queue.is_empty();

    // Only pay for a switch when there is both a reason and somewhere to go.
    if (!quantum_expired && !(current_thread == s_idle_thread && someone_waiting))
        return frame;
    if (!someone_waiting && current_thread != nullptr && current_thread->m_state == ThreadState::Running) {
        current_thread->m_quantum_remaining = DEFAULT_QUANTUM_TICKS;
        return frame;
    }

    return switch_to_next(frame, true);
}

InterruptFrame* Scheduler::on_yield_request(InterruptFrame* frame)
{
    if (!s_running)
        return frame;
    InterruptLockGuard guard(s_lock);
    return switch_to_next(frame, true);
}

void Scheduler::yield()
{
    if (!s_running)
        return;
    // Going through a software interrupt means voluntary and involuntary
    // switches share one code path, so there is only one of them to get right.
    asm volatile("int %0" ::"i"(VECTOR_YIELD) : "memory");
}

void Scheduler::block_current(ThreadState reason)
{
    {
        InterruptLockGuard guard(s_lock);
        auto* thread = arch::this_cpu()->current_thread;
        VERIFY(thread != nullptr);
        thread->m_state = reason;
    }
    // The thread is no longer Running, so switch_to_next will not requeue it.
    yield();
}

void Scheduler::unblock(Thread* thread)
{
    InterruptLockGuard guard(s_lock);
    if (thread->m_state == ThreadState::Running || thread->m_state == ThreadState::Ready)
        return;
    if (thread->wait_queue_node.linked)
        s_sleepers.remove(thread);
    thread->m_state = ThreadState::Ready;
    s_run_queue.append(thread);
}

void Scheduler::sleep_ms(u64 milliseconds)
{
    if (!s_running) {
        arch::pit_busy_wait_ms(milliseconds);
        return;
    }

    u64 const ticks_to_wait = (milliseconds * arch::pit_frequency() + 999) / 1000;
    {
        InterruptLockGuard guard(s_lock);
        auto* thread = arch::this_cpu()->current_thread;
        thread->m_wake_at_tick = arch::pit_ticks() + ticks_to_wait;
        thread->m_state = ThreadState::Sleeping;
        s_sleepers.append(thread);
    }
    yield();
}

[[noreturn]] void Scheduler::exit_current()
{
    {
        InterruptLockGuard guard(s_lock);
        auto* thread = arch::this_cpu()->current_thread;
        thread->m_state = ThreadState::Zombie;
        // The idle thread frees it later. Doing it here would mean unmapping
        // the stack this code is currently executing on.
        s_zombies.append(thread);
    }

    yield();
    panic("a zombie thread was scheduled again");
}

u64 Scheduler::ticks() { return arch::pit_ticks(); }
u64 Scheduler::uptime_ms() { return arch::pit_uptime_ms(); }
u64 Scheduler::context_switches() { return arch::this_cpu()->context_switches; }

usize Scheduler::thread_count()
{
    InterruptLockGuard guard(s_lock);
    return s_all_threads.size();
}

usize Scheduler::runnable_count()
{
    InterruptLockGuard guard(s_lock);
    return s_run_queue.size();
}

void Scheduler::for_each_thread(void (*callback)(Thread*, void*), void* context)
{
    InterruptLockGuard guard(s_lock);
    for (Thread* thread : s_all_threads)
        callback(thread, context);
}

} // namespace kernel
