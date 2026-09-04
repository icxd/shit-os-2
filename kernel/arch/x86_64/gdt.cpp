// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- the global descriptor table and task state segment.

#include <kernel/arch/x86_64/gdt.h>
#include <kernel/dev/console.h>
#include <kernel/lib/string.h>

namespace kernel::arch {

namespace {

// Access byte bits.
constexpr u8 ACCESS_PRESENT = 0x80;
constexpr u8 ACCESS_DPL3 = 0x60;
constexpr u8 ACCESS_SEGMENT = 0x10; // 0 for system descriptors such as the TSS
constexpr u8 ACCESS_EXECUTABLE = 0x08;
constexpr u8 ACCESS_RW = 0x02;
constexpr u8 ACCESS_TSS_AVAILABLE = 0x09;

// Flags nibble bits (high half of the granularity byte).
constexpr u8 FLAG_GRANULARITY_4K = 0x80;
constexpr u8 FLAG_LONG_MODE = 0x20;
constexpr u8 FLAG_SIZE_32 = 0x40;

struct [[gnu::packed]] Descriptor {
    u16 limit_low;
    u16 base_low;
    u8 base_middle;
    u8 access;
    u8 limit_high_and_flags;
    u8 base_high;
};

struct [[gnu::packed]] SystemDescriptor {
    Descriptor low;
    u32 base_upper;
    u32 reserved;
};

struct [[gnu::packed]] GdtPointer {
    u16 limit;
    u64 base;
};

// Six 8-byte slots: null, kernel code, kernel data, user data, user code, and
// two consumed by the 16-byte TSS descriptor.
struct [[gnu::packed]] Gdt {
    Descriptor null;
    Descriptor kernel_code;
    Descriptor kernel_data;
    Descriptor user_data;
    Descriptor user_code;
    SystemDescriptor tss;
};

alignas(16) Gdt s_gdt;
alignas(16) TaskStateSegment s_tss;

constexpr Descriptor make_descriptor(u8 access, u8 flags)
{
    // In long mode the base and limit of a code or data segment are ignored,
    // so they are set to the traditional flat values purely out of habit.
    return Descriptor {
        .limit_low = 0xFFFF,
        .base_low = 0,
        .base_middle = 0,
        .access = access,
        .limit_high_and_flags = static_cast<u8>(0x0F | flags),
        .base_high = 0,
    };
}

void load_gdt(GdtPointer const& pointer)
{
    asm volatile("lgdt %0" ::"m"(pointer) : "memory");

    // The data selectors take effect on the next load; CS only changes via a
    // far transfer, which here is a far return to the label after the jump.
    //
    // Careful: writing %fs or %gs here resets that segment's base to zero, so
    // anything that has stashed a base in FS_BASE or GS_BASE -- the per-CPU
    // pointer, thread-local storage -- must be installed after this runs, not
    // before.
    asm volatile(
        "movw %[data], %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        "movw %%ax, %%ss\n"
        "pushq %[code]\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        :
        : [data] "i"(SELECTOR_KERNEL_DATA), [code] "i"(static_cast<u64>(SELECTOR_KERNEL_CODE))
        : "rax", "memory");
}

} // namespace

TaskStateSegment& tss() { return s_tss; }

void tss_set_kernel_stack(u64 rsp0) { s_tss.rsp0 = rsp0; }

void gdt_initialize()
{
    memset(&s_gdt, 0, sizeof(s_gdt));
    memset(&s_tss, 0, sizeof(s_tss));

    s_gdt.kernel_code = make_descriptor(
        ACCESS_PRESENT | ACCESS_SEGMENT | ACCESS_EXECUTABLE | ACCESS_RW,
        FLAG_GRANULARITY_4K | FLAG_LONG_MODE);
    s_gdt.kernel_data = make_descriptor(
        ACCESS_PRESENT | ACCESS_SEGMENT | ACCESS_RW,
        FLAG_GRANULARITY_4K | FLAG_SIZE_32);
    s_gdt.user_data = make_descriptor(
        ACCESS_PRESENT | ACCESS_DPL3 | ACCESS_SEGMENT | ACCESS_RW,
        FLAG_GRANULARITY_4K | FLAG_SIZE_32);
    s_gdt.user_code = make_descriptor(
        ACCESS_PRESENT | ACCESS_DPL3 | ACCESS_SEGMENT | ACCESS_EXECUTABLE | ACCESS_RW,
        FLAG_GRANULARITY_4K | FLAG_LONG_MODE);

    // No I/O permission bitmap: setting the base past the segment limit is the
    // documented way to say "there isn't one".
    s_tss.iomap_base = sizeof(TaskStateSegment);

    auto const tss_base = reinterpret_cast<u64>(&s_tss);
    s_gdt.tss.low.limit_low = static_cast<u16>(sizeof(TaskStateSegment) - 1);
    s_gdt.tss.low.base_low = static_cast<u16>(tss_base);
    s_gdt.tss.low.base_middle = static_cast<u8>(tss_base >> 16);
    s_gdt.tss.low.access = ACCESS_PRESENT | ACCESS_TSS_AVAILABLE;
    s_gdt.tss.low.limit_high_and_flags = 0;
    s_gdt.tss.low.base_high = static_cast<u8>(tss_base >> 24);
    s_gdt.tss.base_upper = static_cast<u32>(tss_base >> 32);
    s_gdt.tss.reserved = 0;

    GdtPointer const pointer {
        .limit = static_cast<u16>(sizeof(Gdt) - 1),
        .base = reinterpret_cast<u64>(&s_gdt),
    };
    load_gdt(pointer);

    asm volatile("ltr %0" ::"r"(SELECTOR_TSS));

    klog(LOG_INFO, "gdt", "loaded, tss at %p", &s_tss);
}

} // namespace kernel::arch
