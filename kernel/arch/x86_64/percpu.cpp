// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- per-CPU state.

#include <kernel/arch/x86_64/io.h>
#include <kernel/arch/x86_64/percpu.h>
#include <kernel/dev/console.h>
#include <kernel/lib/string.h>
#include <kernel/panic.h>

namespace kernel::arch {

namespace {

constexpr u32 MSR_GS_BASE = 0xC0000101;
constexpr u32 MSR_KERNEL_GS_BASE = 0xC0000102;

Cpu s_cpus[MAX_CPUS];
usize s_cpu_count = 1;

} // namespace

void percpu_initialize_bootstrap()
{
    memset(&s_cpus[0], 0, sizeof(Cpu));
    s_cpus[0].self = &s_cpus[0];
    s_cpus[0].id = 0;

    // Both bases point at the same block for now. Once ring 3 exists, GS_BASE
    // holds the user's TLS pointer while KERNEL_GS_BASE holds this, and the
    // swapgs in isr.S exchanges them on every kernel entry and exit.
    auto const base = reinterpret_cast<u64>(&s_cpus[0]);
    write_msr(MSR_GS_BASE, base);
    write_msr(MSR_KERNEL_GS_BASE, base);

    // Read it straight back. If the GDT is loaded after this point, the write
    // above is silently undone, and finding that out here is much cheaper than
    // finding it out as a null dereference several subsystems later.
    if (this_cpu() != &s_cpus[0]) {
        panic("per-cpu block did not stick: gs:0 reads %p, expected %p", this_cpu(), &s_cpus[0]);
    }

    klog(LOG_INFO, "cpu", "per-cpu block for cpu 0 at %p", &s_cpus[0]);
}

usize cpu_count()
{
    return s_cpu_count;
}
Cpu& cpu_by_index(usize index)
{
    return s_cpus[index < MAX_CPUS ? index : 0];
}

} // namespace kernel::arch
