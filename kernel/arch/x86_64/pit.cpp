// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- the 8253/8254 programmable interval timer.

#include <kernel/arch/x86_64/io.h>
#include <kernel/arch/x86_64/percpu.h>
#include <kernel/arch/x86_64/pic.h>
#include <kernel/arch/x86_64/pit.h>
#include <kernel/dev/console.h>

namespace kernel::arch {

namespace {

constexpr u16 PIT_CHANNEL0_DATA = 0x40;
constexpr u16 PIT_COMMAND = 0x43;
constexpr u32 PIT_BASE_FREQUENCY = 1193182;

// Channel 0, low byte then high byte, mode 2 (rate generator), binary.
constexpr u8 PIT_CONFIGURE = 0x34;

// Read from thread context and written from the interrupt handler, so it is
// an atomic rather than a volatile: volatile orders nothing and a torn read of
// a 64-bit counter is a real possibility on the way to SMP.
u64 s_ticks = 0;
u32 s_frequency = TIMER_FREQUENCY_HZ;
TimerCallback s_callback = nullptr;

InterruptFrame* on_timer_interrupt(InterruptFrame* frame)
{
    __atomic_add_fetch(&s_ticks, 1, __ATOMIC_RELAXED);

    // Acknowledge before running the callback: the callback may switch to a
    // different thread and not come back here for a while, and the PIC will
    // deliver nothing further until it has been told this one is finished.
    pic_send_eoi(0);

    if (s_callback != nullptr)
        return s_callback(frame);
    return frame;
}

} // namespace

void pit_initialize(u32 frequency_hz)
{
    s_frequency = frequency_hz;

    u32 divisor = PIT_BASE_FREQUENCY / frequency_hz;
    if (divisor == 0)
        divisor = 1;
    if (divisor > 0xFFFF)
        divisor = 0xFFFF;

    outb(PIT_COMMAND, PIT_CONFIGURE);
    outb(PIT_CHANNEL0_DATA, static_cast<u8>(divisor & 0xFF));
    outb(PIT_CHANNEL0_DATA, static_cast<u8>((divisor >> 8) & 0xFF));

    register_trap_handler(IRQ_BASE_VECTOR + 0, on_timer_interrupt);
    pic_unmask(0);

    klog(LOG_INFO, "pit", "%u Hz (divisor %u), %llu ms per tick", frequency_hz, divisor,
        static_cast<u64>(1000 / frequency_hz));
}

void pit_set_callback(TimerCallback callback) { s_callback = callback; }

u64 pit_ticks() { return __atomic_load_n(&s_ticks, __ATOMIC_RELAXED); }
u32 pit_frequency() { return s_frequency; }
u64 pit_uptime_ms() { return pit_ticks() * 1000 / s_frequency; }

void pit_busy_wait_ms(u64 milliseconds)
{
    u64 const target = pit_ticks() + (milliseconds * s_frequency + 999) / 1000;
    while (pit_ticks() < target)
        asm volatile("hlt");
}

} // namespace kernel::arch
