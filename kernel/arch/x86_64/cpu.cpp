// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- CPU feature configuration.

#include <kernel/arch/x86_64/cpu.h>
#include <kernel/arch/x86_64/io.h>
#include <kernel/dev/console.h>
#include <kernel/lib/string.h>

namespace kernel::arch {

namespace {

CpuFeatures s_features;

constexpr u64 CR0_WP = 1ULL << 16;
constexpr u64 CR4_PGE = 1ULL << 7;
constexpr u64 CR4_SMEP = 1ULL << 20;
constexpr u64 CR4_OSFXSR = 1ULL << 9;

constexpr u32 MSR_EFER = 0xC0000080;
constexpr u64 EFER_NXE = 1ULL << 11;
constexpr u64 EFER_SCE = 1ULL << 0;

struct CpuidResult {
    u32 eax, ebx, ecx, edx;
};

CpuidResult cpuid(u32 leaf, u32 subleaf = 0)
{
    CpuidResult result {};
    asm volatile("cpuid"
                 : "=a"(result.eax), "=b"(result.ebx), "=c"(result.ecx), "=d"(result.edx)
                 : "a"(leaf), "c"(subleaf));
    return result;
}

u64 read_cr0()
{
    u64 value;
    asm volatile("movq %%cr0, %0" : "=r"(value));
    return value;
}

void write_cr0(u64 value) { asm volatile("movq %0, %%cr0" ::"r"(value) : "memory"); }

u64 read_cr4()
{
    u64 value;
    asm volatile("movq %%cr4, %0" : "=r"(value));
    return value;
}

void write_cr4(u64 value) { asm volatile("movq %0, %%cr4" ::"r"(value) : "memory"); }

void detect()
{
    auto const vendor = cpuid(0);
    memcpy(s_features.vendor + 0, &vendor.ebx, 4);
    memcpy(s_features.vendor + 4, &vendor.edx, 4);
    memcpy(s_features.vendor + 8, &vendor.ecx, 4);
    s_features.vendor[12] = '\0';

    auto const basic = cpuid(1);
    s_features.tsc = (basic.edx & (1u << 4)) != 0;
    s_features.pge = (basic.edx & (1u << 13)) != 0;

    auto const extended = cpuid(7, 0);
    s_features.smep = (extended.ebx & (1u << 7)) != 0;
    s_features.smap = (extended.ebx & (1u << 20)) != 0;
    s_features.fsgsbase = (extended.ebx & (1u << 0)) != 0;

    auto const extended_info = cpuid(0x80000001);
    s_features.nx = (extended_info.edx & (1u << 20)) != 0;
    s_features.gigabyte_pages = (extended_info.edx & (1u << 26)) != 0;
    s_features.syscall = (extended_info.edx & (1u << 11)) != 0;

    // The brand string, if the CPU has one, is three cpuid leaves of raw text.
    s_features.brand[0] = '\0';
    if (cpuid(0x80000000).eax >= 0x80000004) {
        u32 words[12];
        for (u32 i = 0; i < 3; ++i) {
            auto const leaf = cpuid(0x80000002 + i);
            words[i * 4 + 0] = leaf.eax;
            words[i * 4 + 1] = leaf.ebx;
            words[i * 4 + 2] = leaf.ecx;
            words[i * 4 + 3] = leaf.edx;
        }
        memcpy(s_features.brand, words, sizeof(words));
        s_features.brand[48] = '\0';
    }
}

} // namespace

CpuFeatures const& cpu_features() { return s_features; }

void cpu_initialize()
{
    detect();

    // Write protect. Without this a ring 0 write to a read-only page silently
    // succeeds, which would make the kernel's own W^X mapping decorative and
    // make copy-on-write impossible to implement.
    write_cr0(read_cr0() | CR0_WP);

    u64 cr4 = read_cr4();
    if (s_features.pge)
        cr4 |= CR4_PGE; // without this the Global bit in a PTE does nothing
    if (s_features.smep)
        cr4 |= CR4_SMEP; // a kernel jump into user memory now faults
    cr4 |= CR4_OSFXSR;
    write_cr4(cr4);

    // NXE was set in boot.S so that early page tables could use the NX bit;
    // SCE is what makes the syscall instruction legal at all.
    u64 efer = read_msr(MSR_EFER);
    efer |= EFER_NXE;
    if (s_features.syscall)
        efer |= EFER_SCE;
    write_msr(MSR_EFER, efer);

    klog(LOG_INFO, "cpu", "%s", s_features.brand[0] != '\0' ? s_features.brand : s_features.vendor);
    klog(LOG_INFO, "cpu", "nx=%s pge=%s smep=%s smap=%s 1g-pages=%s syscall=%s",
        s_features.nx ? "yes" : "no", s_features.pge ? "yes" : "no",
        s_features.smep ? "on" : "no", s_features.smap ? "available" : "no",
        s_features.gigabyte_pages ? "yes" : "no", s_features.syscall ? "yes" : "no");
}

} // namespace kernel::arch
