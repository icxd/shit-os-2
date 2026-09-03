// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- the end of the line.

#include <kernel/arch/x86_64/io.h>
#include <kernel/dev/console.h>
#include <kernel/dev/framebuffer.h>
#include <kernel/lib/format.h>
#include <kernel/lib/spinlock.h>
#include <kernel/panic.h>

namespace kernel {

namespace {

bool s_panicking = false;

void print_banner()
{
    // A nod to gen 1, which turned the screen blue and asked you to write in.
    auto& fb = dev::framebuffer_console();
    if (fb.is_usable()) {
        fb.set_colors({ 0xff, 0xff, 0xff }, { 0x1b, 0x2a, 0x6b });
        fb.clear();
    }

    kprintf("\n");
    kprintf("  shit os 2 has stopped, which is arguably on brand.\n");
    kprintf("  ------------------------------------------------------------\n");
}

void print_registers()
{
    u64 rsp, rbp, cr0, cr2, cr3, cr4, rflags;
    asm volatile("movq %%rsp, %0" : "=r"(rsp));
    asm volatile("movq %%rbp, %0" : "=r"(rbp));
    asm volatile("movq %%cr0, %0" : "=r"(cr0));
    asm volatile("movq %%cr2, %0" : "=r"(cr2));
    asm volatile("movq %%cr3, %0" : "=r"(cr3));
    asm volatile("movq %%cr4, %0" : "=r"(cr4));
    asm volatile("pushfq; popq %0" : "=r"(rflags));

    kprintf("\n  rsp=%p rbp=%p rflags=%p\n", reinterpret_cast<void*>(rsp),
        reinterpret_cast<void*>(rbp), reinterpret_cast<void*>(rflags));
    kprintf("  cr0=%p cr2=%p\n", reinterpret_cast<void*>(cr0), reinterpret_cast<void*>(cr2));
    kprintf("  cr3=%p cr4=%p\n", reinterpret_cast<void*>(cr3), reinterpret_cast<void*>(cr4));
}

// Walks saved rbp values. Only valid while frame pointers are being emitted,
// which is why the kernel is built with -fno-omit-frame-pointer.
void print_backtrace()
{
    struct StackFrame {
        StackFrame* previous;
        u64 return_address;
    };

    StackFrame* frame;
    asm volatile("movq %%rbp, %0" : "=r"(frame));

    kprintf("\n  backtrace:\n");
    for (int depth = 0; depth < 16 && frame != nullptr && frame->return_address != 0; ++depth) {
        kprintf("    #%-2d %p\n", depth, reinterpret_cast<void*>(frame->return_address));
        // Anything below the higher half is not a kernel frame; stop rather
        // than chase a wild pointer while already in trouble.
        if (reinterpret_cast<u64>(frame->previous) < KERNEL_VMA)
            break;
        frame = frame->previous;
    }
}

} // namespace

[[noreturn]] void panic(char const* format, ...)
{
    interrupts_disable();

    // A fault inside the panic handler must not loop forever.
    if (s_panicking) {
        kprintf("\n  ...and then it panicked while panicking. Giving up.\n");
        arch::halt_forever();
    }
    s_panicking = true;

    print_banner();

    kprintf("  ");
    va_list args;
    va_start(args, format);
    kvprintf(format, args);
    va_end(args);
    kprintf("\n");

    print_registers();
    print_backtrace();

    kprintf("\n  ------------------------------------------------------------\n");
    kprintf("  Machine halted. There is no reboot key; this is not that kind\n");
    kprintf("  of operating system yet. See docs/roadmap.md.\n");

    arch::halt_forever();
}

[[noreturn]] void panic_with_frame(InterruptFrame const*, char const* format, ...)
{
    // The frame-aware variant is wired up once the IDT exists; until then this
    // degrades to the plain path rather than pretending to know more.
    interrupts_disable();
    s_panicking = true;
    print_banner();
    kprintf("  ");
    va_list args;
    va_start(args, format);
    kvprintf(format, args);
    va_end(args);
    kprintf("\n");
    print_registers();
    arch::halt_forever();
}

} // namespace kernel
