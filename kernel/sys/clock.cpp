// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- the wall clock.

#include <kernel/arch/x86_64/pit.h>
#include <kernel/dev/console.h>
#include <kernel/sys/clock.h>

namespace kernel {

namespace {

// Nanoseconds to add to the monotonic clock to get real time. Zero until a
// driver tells us what year it is.
u64 s_realtime_offset_ns = 0;
bool s_realtime_is_set = false;

ClockReadFunction s_source_read = nullptr;
void* s_source_self = nullptr;

} // namespace

void clock_initialize()
{
    s_realtime_offset_ns = 0;
    s_realtime_is_set = false;
    s_source_read = nullptr;
    s_source_self = nullptr;
}

u64 clock_monotonic_ns()
{
    return arch::pit_uptime_ms() * 1'000'000ull;
}

u64 clock_realtime_ns()
{
    return clock_monotonic_ns() + __atomic_load_n(&s_realtime_offset_ns, __ATOMIC_ACQUIRE);
}

bool clock_realtime_is_set()
{
    return __atomic_load_n(&s_realtime_is_set, __ATOMIC_ACQUIRE);
}

i64 clock_realtime_seconds()
{
    return static_cast<i64>(clock_realtime_ns() / 1'000'000'000ull);
}

bool clock_register_source(ClockReadFunction read, void* self)
{
    if (read == nullptr)
        return false;
    if (s_source_read != nullptr)
        return false;

    // Read before pinning the offset, and read the uptime as close to it as we
    // can: everything between the two reads is error in the wall clock.
    i64 const epoch_seconds = read(self);
    if (epoch_seconds < 0) {
        klog(LOG_WARN, "clock", "time source refused to answer; staying on boot time");
        return false;
    }

    u64 const monotonic = clock_monotonic_ns();
    u64 const realtime = static_cast<u64>(epoch_seconds) * 1'000'000'000ull;
    if (realtime < monotonic) {
        // A clock that says the machine booted before the epoch is broken, and
        // an offset that wraps would be worse than no clock at all.
        klog(LOG_WARN, "clock", "time source reported %lld s, before this boot; ignored",
            static_cast<long long>(epoch_seconds));
        return false;
    }

    s_source_read = read;
    s_source_self = self;
    __atomic_store_n(&s_realtime_offset_ns, realtime - monotonic, __ATOMIC_RELEASE);
    __atomic_store_n(&s_realtime_is_set, true, __ATOMIC_RELEASE);

    klog(LOG_INFO, "clock", "real time set from a driver: %lld seconds since the epoch",
        static_cast<long long>(epoch_seconds));
    return true;
}

void clock_unregister_source(void* self)
{
    if (s_source_self != self)
        return;
    s_source_read = nullptr;
    s_source_self = nullptr;
    // The offset stays. The driver told us what time it was; unloading it does
    // not make that untrue, and a clock that stops when a module unloads would
    // be a worse answer than a slightly drifting one.
}

} // namespace kernel
