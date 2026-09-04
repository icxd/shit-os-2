// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- floating point and vector state.
//
// x86-64 has no software floating point: a double lives in an xmm register,
// and clang reaches for xmm to copy structs whether or not the program does
// any arithmetic. So every user thread has SSE state worth preserving, and a
// kernel that switches threads without preserving it corrupts them silently.
//
// The kernel itself is built with -mno-sse and never touches this state, which
// is what makes an eager save on switch safe: nothing between the save and the
// restore can disturb what was saved.
//
// Saving eagerly rather than lazily (CR0.TS and a #NM trap) costs a fixed ~100
// cycles per switch and avoids the whole class of lazy-restore bugs -- see
// CVE-2018-3665, where lazy state let one process read another's registers.

#pragma once

#include <shitos/types.h>

namespace kernel::arch {

// FXSAVE writes 512 bytes and requires 16-byte alignment. XSAVE would be
// bigger and variable; FXSAVE covers x87, MMX and SSE, which is everything
// userland can currently reach.
inline constexpr usize FPU_STATE_SIZE = 512;
inline constexpr usize FPU_STATE_ALIGNMENT = 16;

// Configures CR0 and CR4 for hardware floating point and captures a pristine
// state to stamp new threads with. Runs once per CPU.
void fpu_initialize();

// Fills `area` with the state a freshly created thread should start from:
// a clean x87 stack and a default MXCSR, not whatever the creating thread
// happened to be holding.
void fpu_initialize_state(void* area);

inline void fpu_save(void* area)
{
    asm volatile("fxsave (%0)" ::"r"(area) : "memory");
}
inline void fpu_restore(void const* area)
{
    asm volatile("fxrstor (%0)" ::"r"(area) : "memory");
}

} // namespace kernel::arch
