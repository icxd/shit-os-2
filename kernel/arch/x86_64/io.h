// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- x86 port I/O. The only file outside arch/ that may include this
// is nothing: core code reaches hardware through drivers, not directly.

#pragma once

#include <shitos/types.h>

namespace kernel::arch {

inline u8 inb(u16 port)
{
    u8 value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

inline u16 inw(u16 port)
{
    u16 value;
    asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

inline u32 inl(u16 port)
{
    u32 value;
    asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

inline void outb(u16 port, u8 value)
{
    asm volatile("outb %0, %1" ::"a"(value), "Nd"(port) : "memory");
}
inline void outw(u16 port, u16 value)
{
    asm volatile("outw %0, %1" ::"a"(value), "Nd"(port) : "memory");
}
inline void outl(u16 port, u32 value)
{
    asm volatile("outl %0, %1" ::"a"(value), "Nd"(port) : "memory");
}

// A write to an unused port; the bus cycle is the delay. Needed between back
// to back writes to slow devices like the legacy PIC.
inline void io_wait()
{
    outb(0x80, 0);
}

inline void halt()
{
    asm volatile("hlt");
}

[[noreturn]] inline void halt_forever()
{
    for (;;) {
        asm volatile("cli; hlt");
    }
}

inline u64 read_msr(u32 msr)
{
    u32 low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (static_cast<u64>(high) << 32) | low;
}

inline void write_msr(u32 msr, u64 value)
{
    asm volatile(
        "wrmsr" ::"c"(msr), "a"(static_cast<u32>(value)), "d"(static_cast<u32>(value >> 32)));
}

inline u64 read_cr2()
{
    u64 value;
    asm volatile("movq %%cr2, %0" : "=r"(value));
    return value;
}

inline u64 read_cr3()
{
    u64 value;
    asm volatile("movq %%cr3, %0" : "=r"(value));
    return value;
}

inline void write_cr3(u64 value)
{
    asm volatile("movq %0, %%cr3" ::"r"(value) : "memory");
}

inline void invlpg(u64 address)
{
    asm volatile("invlpg (%0)" ::"r"(address) : "memory");
}

} // namespace kernel::arch
