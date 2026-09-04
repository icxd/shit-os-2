// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- the 8253/8254 programmable interval timer.
//
// The PIT is ancient, imprecise and present on everything, which makes it the
// right thing to start with. A local APIC timer driver will replace it; the
// callback interface here is what the scheduler binds to either way.

#pragma once

#include <kernel/arch/x86_64/interrupts.h>
#include <shitos/types.h>

namespace kernel::arch {

// 250 Hz: fine enough for a 4 ms scheduling quantum and millisecond-ish
// timekeeping, coarse enough that emulated machines are not spending all their
// time in the interrupt path.
inline constexpr u32 TIMER_FREQUENCY_HZ = 250;
inline constexpr u64 MILLISECONDS_PER_TICK = 1000 / TIMER_FREQUENCY_HZ;

// Returns the frame to resume, so the scheduler can preempt from the tick.
using TimerCallback = InterruptFrame* (*)(InterruptFrame*);

void pit_initialize(u32 frequency_hz = TIMER_FREQUENCY_HZ);
void pit_set_callback(TimerCallback callback);

u64 pit_ticks();
u64 pit_uptime_ms();
u32 pit_frequency();

// Busy-waits. Only for early boot, before there is a scheduler to sleep on.
void pit_busy_wait_ms(u64 milliseconds);

} // namespace kernel::arch
