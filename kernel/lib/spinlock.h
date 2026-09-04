// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- spinlocks and interrupt discipline.
//
// Only one CPU is brought up today, but these are real locks rather than
// no-ops, and the kernel uses them everywhere shared state is touched. That is
// the whole point of the "SMP-ready" decision: when the application processors
// come online, this file changes and the callers do not.

#pragma once

#include <kernel/lib/kstd.h>

#include <shitos/types.h>

namespace kernel {

inline void cpu_relax()
{
    asm volatile("pause" ::: "memory");
}

inline bool interrupts_enabled()
{
    u64 flags;
    asm volatile("pushfq; popq %0" : "=r"(flags)::"memory");
    return (flags & (1 << 9)) != 0;
}

inline void interrupts_disable()
{
    asm volatile("cli" ::: "memory");
}
inline void interrupts_enable()
{
    asm volatile("sti" ::: "memory");
}

// Restores the previous interrupt state on scope exit rather than blindly
// re-enabling, so nesting these is safe.
class InterruptDisabler : public NonCopyable {
public:
    InterruptDisabler()
        : m_was_enabled(interrupts_enabled())
    {
        interrupts_disable();
    }

    ~InterruptDisabler()
    {
        if (m_was_enabled)
            interrupts_enable();
    }

private:
    bool m_was_enabled;
};

class SpinLock : public NonCopyable {
public:
    constexpr SpinLock() = default;

    void lock()
    {
        while (__atomic_test_and_set(&m_locked, __ATOMIC_ACQUIRE)) {
            // Spin on a plain load so we are not hammering the cache line with
            // read-for-ownership traffic while somebody else holds it.
            while (__atomic_load_n(&m_locked, __ATOMIC_RELAXED))
                cpu_relax();
        }
    }

    bool try_lock() { return !__atomic_test_and_set(&m_locked, __ATOMIC_ACQUIRE); }

    void unlock() { __atomic_clear(&m_locked, __ATOMIC_RELEASE); }

    bool is_locked() const { return __atomic_load_n(&m_locked, __ATOMIC_RELAXED); }

private:
    bool m_locked { false };
};

template<typename LockType>
class LockGuard : public NonCopyable {
public:
    explicit LockGuard(LockType& lock)
        : m_lock(lock)
    {
        m_lock.lock();
    }

    ~LockGuard() { m_lock.unlock(); }

private:
    LockType& m_lock;
};

// A lock that is also taken from interrupt handlers must mask interrupts on
// the local CPU first, or the handler will spin forever on a lock this very
// CPU is holding.
class InterruptSpinLock : public NonCopyable {
public:
    constexpr InterruptSpinLock() = default;

    u64 lock_irqsave()
    {
        u64 const was_enabled = interrupts_enabled() ? 1 : 0;
        interrupts_disable();
        m_lock.lock();
        return was_enabled;
    }

    void unlock_irqrestore(u64 state)
    {
        m_lock.unlock();
        if (state != 0)
            interrupts_enable();
    }

private:
    SpinLock m_lock;
};

class InterruptLockGuard : public NonCopyable {
public:
    explicit InterruptLockGuard(InterruptSpinLock& lock)
        : m_lock(lock)
        , m_state(lock.lock_irqsave())
    {
    }

    ~InterruptLockGuard() { m_lock.unlock_irqrestore(m_state); }

private:
    InterruptSpinLock& m_lock;
    u64 m_state;
};

} // namespace kernel
