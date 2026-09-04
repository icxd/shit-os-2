// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- floating point and vector state.

#include <kernel/arch/x86_64/cpu.h>
#include <kernel/arch/x86_64/fpu.h>
#include <kernel/dev/console.h>
#include <kernel/lib/string.h>

namespace kernel::arch {

namespace {

constexpr u64 CR0_MP = 1ULL << 1; // monitor coprocessor
constexpr u64 CR0_EM = 1ULL << 2; // emulate: must be clear, we have real SSE
constexpr u64 CR0_TS = 1ULL << 3; // task switched: unused, we save eagerly
constexpr u64 CR0_NE = 1ULL << 5; // native x87 exception reporting

constexpr u64 CR4_OSFXSR = 1ULL << 9; // fxsave/fxrstor and SSE are legal
constexpr u64 CR4_OSXMMEXCPT = 1ULL << 10; // SIMD faults raise #XM, not #UD

// The state a new thread starts from. Captured once from a freshly reset FPU
// so that a thread never inherits the creating thread's rounding mode or a
// stale x87 stack.
alignas(FPU_STATE_ALIGNMENT) u8 s_pristine_state[FPU_STATE_SIZE];

u64 read_cr0()
{
    u64 value;
    asm volatile("movq %%cr0, %0" : "=r"(value));
    return value;
}

void write_cr0(u64 value)
{
    asm volatile("movq %0, %%cr0" ::"r"(value) : "memory");
}

u64 read_cr4()
{
    u64 value;
    asm volatile("movq %%cr4, %0" : "=r"(value));
    return value;
}

void write_cr4(u64 value)
{
    asm volatile("movq %0, %%cr4" ::"r"(value) : "memory");
}

} // namespace

void fpu_initialize()
{
    u64 cr0 = read_cr0();
    cr0 &= ~CR0_EM; // never emulate; there is no emulator
    cr0 &= ~CR0_TS; // eager saving means this must stay clear
    cr0 |= CR0_MP | CR0_NE;
    write_cr0(cr0);

    write_cr4(read_cr4() | CR4_OSFXSR | CR4_OSXMMEXCPT);

    // Reset the x87 unit, then capture what a clean state looks like.
    asm volatile("fninit");

    // MXCSR: all exceptions masked, round to nearest. Unmasked SIMD exceptions
    // would turn an ordinary overflow in a user program into a fault.
    u32 const mxcsr = 0x1F80;
    asm volatile("ldmxcsr %0" ::"m"(mxcsr));

    memset(s_pristine_state, 0, sizeof(s_pristine_state));
    fpu_save(s_pristine_state);

    klog(LOG_INFO, "fpu", "x87 and SSE enabled, %zu-byte state saved per thread", FPU_STATE_SIZE);
}

void fpu_initialize_state(void* area)
{
    memcpy(area, s_pristine_state, FPU_STATE_SIZE);
}

} // namespace kernel::arch
