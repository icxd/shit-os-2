// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- where the kernel starts thinking in C++.
//
// boot.S hands us the multiboot2 magic and info pointer and nothing else; by
// the time this returns, the machine is a running operating system.

#include <kernel/arch/x86_64/io.h>
#include <kernel/arch/x86_64/serial.h>
#include <kernel/boot/boot_info.h>
#include <kernel/dev/console.h>
#include <kernel/dev/framebuffer.h>
#include <kernel/panic.h>

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
    kprintf("      _     _ _                 ___  \n");
    kprintf("  ___| |__ (_) |_    ___  ___  |_  ) \n");
    kprintf(" (_-<| '_ \\| |  _|  / _ \\/ _ \\  / /  \n");
    kprintf(" /__/|_.__/|_|\\__|  \\___/\\___/ /___| \n");
    kprintf("\n");
    kprintf(" a hybrid x86_64 kernel that is not based on Linux,\n");
    kprintf(" and is not based on much else either.\n");
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

    klog(LOG_INFO, "boot", "stage A complete: long mode, console, memory map");

    arch::halt_forever();
}
