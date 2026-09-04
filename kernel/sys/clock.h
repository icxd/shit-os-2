// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- the wall clock.
//
// The kernel keeps time in two ways and they answer different questions.
//
// Monotonic time is the tick counter: it starts at zero when the PIT does,
// never goes backwards, and is what a timeout should be measured against.
//
// Real time is a calendar date, which the machine cannot know on its own. A
// driver offers one through the module ABI's time_source_register, and the
// kernel pins the difference between it and the monotonic clock once. Every
// later query is that offset plus the current uptime, so reading the date is
// free and does not depend on the driver still being loaded.
//
// Before any source registers, real time is boot time and says so: callers
// get an epoch of zero, and `clock_realtime_is_set()` is how you find out
// whether a date is real or a placeholder.

#pragma once

#include <shitos/types.h>

namespace kernel {

using ClockReadFunction = i64 (*)(void* self);

void clock_initialize();

// Nanoseconds since boot. Never goes backwards, always available.
u64 clock_monotonic_ns();

// Nanoseconds since the Unix epoch, or since boot if no source has registered.
u64 clock_realtime_ns();

bool clock_realtime_is_set();

// Registers the first source that offers one; a later one is refused. Reading
// happens here, once, in the caller's context -- so a driver may block.
bool clock_register_source(ClockReadFunction read, void* self);
void clock_unregister_source(void* self);

// Seconds since the epoch, for stamping inodes.
i64 clock_realtime_seconds();

} // namespace kernel
