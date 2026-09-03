// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- the physical page allocator.
//
// A bitmap over every 4 KiB frame in the machine, seeded from the multiboot2
// memory map. One bit per page is 32 KiB per gigabyte of RAM, which is cheap
// enough that the simplicity is worth more than the space.

#pragma once

#include <kernel/boot/boot_info.h>
#include <kernel/lib/error.h>
#include <shitos/types.h>

namespace kernel::mm {

void physical_initialize(boot::BootInfo const& info);

// A single page. The contents are whatever the last owner left behind; use
// allocate_zeroed_page for anything that will be mapped into a user process.
ErrorOr<PhysAddr> allocate_page();
ErrorOr<PhysAddr> allocate_zeroed_page();

// `count` physically contiguous pages, for DMA buffers and page tables that
// have to span more than one frame.
ErrorOr<PhysAddr> allocate_contiguous(usize count);

void free_page(PhysAddr page);
void free_contiguous(PhysAddr base, usize count);

// Marks a range as permanently unavailable. Used for memory-mapped hardware
// that the firmware did not report.
void reserve_range(PhysAddr base, usize length);

usize total_pages();
usize free_pages();
usize used_pages();

} // namespace kernel::mm
