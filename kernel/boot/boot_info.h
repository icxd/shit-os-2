// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- what the kernel knows about the machine after boot.
//
// The boot protocol is an arch/ concern. Core code consumes this instead, so
// swapping GRUB for something else touches one translation unit.

#pragma once

#include <kernel/lib/kstd.h>
#include <shitos/types.h>

namespace kernel::boot {

inline constexpr usize MAX_MEMORY_REGIONS = 64;
inline constexpr usize MAX_BOOT_MODULES = 8;

enum class MemoryKind {
    Usable,
    Reserved,
    AcpiReclaimable,
    AcpiNvs,
    Bad,
    KernelImage,
    BootModule,
};

struct MemoryRegion {
    u64 base;
    u64 length;
    MemoryKind kind;
};

struct BootModule {
    u64 phys_start;
    u64 phys_end;
    char const* name;
};

enum class FramebufferFormat {
    None,
    Rgb,
    EgaText,
};

struct FramebufferInfo {
    FramebufferFormat format { FramebufferFormat::None };
    u64 phys_address { 0 };
    u32 pitch { 0 };
    u32 width { 0 };
    u32 height { 0 };
    u8 bits_per_pixel { 0 };
    u8 red_shift { 0 };
    u8 red_bits { 0 };
    u8 green_shift { 0 };
    u8 green_bits { 0 };
    u8 blue_shift { 0 };
    u8 blue_bits { 0 };
};

struct BootInfo {
    char const* bootloader_name { "unknown" };
    char const* command_line { "" };

    MemoryRegion memory_regions[MAX_MEMORY_REGIONS] {};
    usize memory_region_count { 0 };
    u64 total_usable_bytes { 0 };
    u64 highest_address { 0 };

    BootModule modules[MAX_BOOT_MODULES] {};
    usize module_count { 0 };

    FramebufferInfo framebuffer {};

    u64 kernel_phys_start { 0 };
    u64 kernel_phys_end { 0 };

    u64 rsdp_phys { 0 };
};

// Parses the multiboot2 block at the given physical address into a BootInfo.
// Returns false if the magic does not match, in which case nothing was booted
// the way we expect and the caller should give up loudly.
bool parse_multiboot2(u32 magic, u64 info_phys, BootInfo& out);

// The one instance, filled in before anything else runs.
BootInfo& boot_info();

} // namespace kernel::boot
