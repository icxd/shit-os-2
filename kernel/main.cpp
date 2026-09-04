// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- where the kernel starts thinking in C++.
//
// boot.S hands us the multiboot2 magic and info pointer and nothing else; by
// the time this returns, the machine is a running operating system.

#include <kernel/arch/x86_64/cpu.h>
#include <kernel/arch/x86_64/gdt.h>
#include <kernel/arch/x86_64/interrupts.h>
#include <kernel/arch/x86_64/percpu.h>
#include <kernel/arch/x86_64/pit.h>
#include <kernel/arch/x86_64/io.h>
#include <kernel/arch/x86_64/serial.h>
#include <kernel/boot/boot_info.h>
#include <kernel/dev/console.h>
#include <kernel/dev/framebuffer.h>
#include <kernel/dev/tty.h>
#include <kernel/fs/boot_mounts.h>
#include <kernel/fs/vfs.h>
#include <kernel/lib/spinlock.h>
#include <kernel/mm/address_space.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/physical.h>
#include <kernel/module/loader.h>
#include <kernel/panic.h>
#include <kernel/sched/process.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sys/syscall.h>
#include <kernel/sys/userland.h>
#include <kernel/selftest.h>

namespace kernel {

void run_global_constructors();

namespace {

char const* memory_kind_name(boot::MemoryKind kind)
{
    switch (kind) {
    case boot::MemoryKind::Usable:
        return "usable";
    case boot::MemoryKind::Reserved:
        return "reserved";
    case boot::MemoryKind::AcpiReclaimable:
        return "acpi";
    case boot::MemoryKind::AcpiNvs:
        return "acpi-nvs";
    case boot::MemoryKind::Bad:
        return "bad";
    case boot::MemoryKind::KernelImage:
        return "kernel";
    case boot::MemoryKind::BootModule:
        return "module";
    }
    return "?";
}

void print_banner()
{
    kprintf("\n");
    kprintf("                                            #####\n");
    kprintf("  ####  #    # # #####     ####   ####     #     #\n");
    kprintf(" #      #    # #   #      #    # #               #\n");
    kprintf("  ####  ###### #   #      #    #  ####      #####\n");
    kprintf("      # #    # #   #      #    #      #    #\n");
    kprintf(" #    # #    # #   #      #    # #    #    #\n");
    kprintf("  ####  #    # #   #       ####   ####     #######\n");
    kprintf("\n");
    kprintf("  a hybrid x86_64 kernel. not based on Linux, and not\n");
    kprintf("  based on much else either.\n");
    kprintf("\n");
}

void print_boot_summary(boot::BootInfo const& info)
{
    klog(LOG_INFO, "boot", "bootloader: %s", info.bootloader_name);
    if (info.command_line != nullptr && info.command_line[0] != '\0')
        klog(LOG_INFO, "boot", "cmdline: %s", info.command_line);

    klog(LOG_INFO, "boot", "kernel image: %p..%p (%llu KiB)",
        reinterpret_cast<void*>(info.kernel_phys_start),
        reinterpret_cast<void*>(info.kernel_phys_end),
        (info.kernel_phys_end - info.kernel_phys_start) / 1024);

    klog(LOG_INFO, "mem", "%llu MiB usable across %zu regions",
        info.total_usable_bytes / (1024 * 1024), info.memory_region_count);

    for (usize i = 0; i < info.memory_region_count; ++i) {
        auto const& region = info.memory_regions[i];
        klog(LOG_DEBUG, "mem", "  %p..%p  %-9s %llu KiB",
            reinterpret_cast<void*>(region.base),
            reinterpret_cast<void*>(region.base + region.length),
            memory_kind_name(region.kind), region.length / 1024);
    }

    for (usize i = 0; i < info.module_count; ++i) {
        auto const& module = info.modules[i];
        klog(LOG_INFO, "boot", "module '%s' at %p..%p", module.name,
            reinterpret_cast<void*>(module.phys_start),
            reinterpret_cast<void*>(module.phys_end));
    }

    auto const& fb = info.framebuffer;
    switch (fb.format) {
    case boot::FramebufferFormat::Rgb:
        klog(LOG_INFO, "fbcon", "%ux%ux%u linear rgb at %p, %u cols x %u rows", fb.width,
            fb.height, fb.bits_per_pixel, reinterpret_cast<void*>(fb.phys_address),
            dev::framebuffer_console().columns(), dev::framebuffer_console().rows());
        break;
    case boot::FramebufferFormat::EgaText:
        klog(LOG_WARN, "fbcon", "no linear framebuffer; falling back to EGA text");
        break;
    case boot::FramebufferFormat::None:
        klog(LOG_WARN, "fbcon", "no usable framebuffer; serial console only");
        break;
    }
}

} // namespace
} // namespace kernel

extern "C" [[noreturn]] void kernel_entry(u32 magic, u32 multiboot_info_phys)
{
    using namespace kernel;

    // Serial first, unconditionally: if anything below this line goes wrong we
    // still want to be able to say so.
    arch::serial_initialize();
    console_register(&arch::serial_com1());

    auto& info = boot::boot_info();
    if (!boot::parse_multiboot2(magic, multiboot_info_phys, info)) {
        kprintf("shit os 2: not booted by a multiboot2 loader (magic %p)\n",
            reinterpret_cast<void*>(static_cast<u64>(magic)));
        arch::halt_forever();
    }

    if (dev::framebuffer_initialize())
        console_register(&dev::framebuffer_console());

    run_global_constructors();

    print_banner();
    print_boot_summary(info);

    arch::cpu_initialize();

    // Descriptor tables before memory: a fault during the memory bring-up is
    // exactly when a working IDT is worth the most.
    //
    // The per-CPU block has to be installed *after* the GDT, not before:
    // loading a segment register in long mode resets that segment's base to
    // zero, so gdt_initialize() writing %gs would wipe the GS base MSR that
    // this_cpu() reads.
    arch::gdt_initialize();
    arch::percpu_initialize_bootstrap();
    arch::idt_initialize();

    mm::physical_initialize(info);
    mm::virtual_memory_initialize(info);
    heap_initialize();

    // Safe to take interrupts now: the IDT is real and every PIC line is
    // still masked, so nothing can fire until a driver asks for it.
    interrupts_enable();

    run_boot_selftests();

    arch::pit_initialize();
    Scheduler::initialize();
    run_scheduler_selftests();

    if (auto mounted = fs::mount_boot_filesystems(info); mounted.is_error())
        panic("could not mount the boot filesystems: %s", mounted.error().to_string());
    run_filesystem_selftests();

    ModuleLoader::initialize();
    if (auto loaded = ModuleLoader::load_all_from("/lib/modules"); loaded.is_error())
        klog(LOG_WARN, "module", "could not scan /lib/modules: %s", loaded.error().to_string());
    else
        klog(LOG_INFO, "module", "%zu module(s) loaded", loaded.value());
    run_module_selftests();

    Process::initialize();
    sys::syscall_initialize();
    sys::faults_initialize();

    if (auto tty = dev::Tty::initialize(); tty.is_error())
        klog(LOG_WARN, "tty", "no terminal: %s", tty.error().to_string());
    else {
        if (auto input = Thread::create_kernel_thread("tty-kbd", dev::tty_input_thread, nullptr);
            !input.is_error())
            Scheduler::enqueue(input.value());
        if (auto serial = Thread::create_kernel_thread("tty-serial", dev::tty_serial_input_thread,
                nullptr);
            !serial.is_error())
            Scheduler::enqueue(serial.value());
    }

    klog(LOG_INFO, "boot", "stage E complete: runtime-loadable driver modules");
    klog(LOG_INFO, "boot", "uptime %llu ms, %zu threads, %llu context switches",
        Scheduler::uptime_ms(), Scheduler::thread_count(), Scheduler::context_switches());

    if (auto started = sys::start_init("/bin/init"); started.is_error()) {
        klog(LOG_ERROR, "init", "could not start /bin/init: %s", started.error().to_string());
        klog(LOG_ERROR, "init", "there is no userland, so there is nothing else to do");
    }

    // kmain has nothing left to do. It cannot exit -- something has to be the
    // thread the boot stack belongs to -- so it becomes a second idle loop.
    for (;;)
        Scheduler::sleep_ms(1000);
}
