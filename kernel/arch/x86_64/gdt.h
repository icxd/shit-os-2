// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- the global descriptor table and task state segment.
//
// Long mode barely uses segmentation, but the descriptors still have to exist
// and their *order* is load-bearing: the syscall/sysret instructions derive
// the selectors they load from a single base in the STAR MSR, which forces
// this exact arrangement.
//
//   0x08  kernel code   <- STAR[47:32] points here for syscall entry
//   0x10  kernel data      (syscall SS = kernel code + 8)
//   0x18  user data     <- sysret SS = STAR[63:48] + 8
//   0x20  user code        sysret CS = STAR[63:48] + 16
//   0x28  TSS (16 bytes, two GDT slots)

#pragma once

#include <shitos/types.h>

namespace kernel::arch {

inline constexpr u16 SELECTOR_KERNEL_CODE = 0x08;
inline constexpr u16 SELECTOR_KERNEL_DATA = 0x10;
inline constexpr u16 SELECTOR_USER_DATA = 0x18 | 3;
inline constexpr u16 SELECTOR_USER_CODE = 0x20 | 3;
inline constexpr u16 SELECTOR_TSS = 0x28;

// The value syscall/sysret needs in STAR[63:48]: the CPU adds 8 for SS and 16
// for CS and forces RPL 3 on both.
inline constexpr u16 STAR_USER_BASE = 0x10;

struct [[gnu::packed]] TaskStateSegment {
    u32 reserved0;
    u64 rsp0; // stack the CPU switches to on a ring 3 -> ring 0 transition
    u64 rsp1;
    u64 rsp2;
    u64 reserved1;
    u64 ist[7]; // ist[0] is IST1 as far as the IDT is concerned
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
};

void gdt_initialize();

// Called on every context switch into a user thread so that a later trap
// lands on that thread's kernel stack rather than the previous one's.
void tss_set_kernel_stack(u64 rsp0);

TaskStateSegment& tss();

} // namespace kernel::arch
