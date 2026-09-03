// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- interrupt descriptor table and dispatch.

#include <kernel/arch/x86_64/gdt.h>
#include <kernel/arch/x86_64/idt.h>
#include <kernel/arch/x86_64/interrupts.h>
#include <kernel/arch/x86_64/io.h>
#include <kernel/arch/x86_64/pic.h>
#include <kernel/dev/console.h>
#include <kernel/lib/spinlock.h>
#include <kernel/lib/string.h>
#include <kernel/panic.h>

extern "C" {
// Generated in isr.S at a fixed stride, so entry N is table + N * stride.
extern u8 isr_stub_table[];
}

namespace kernel::arch {

namespace {

constexpr usize IDT_ENTRIES = 256;
constexpr usize STUB_STRIDE = 16;

constexpr u8 GATE_INTERRUPT = 0x0E;
constexpr u8 GATE_PRESENT = 0x80;
constexpr u8 GATE_DPL3 = 0x60;

struct [[gnu::packed]] IdtEntry {
    u16 offset_low;
    u16 selector;
    u8 ist;
    u8 type_attributes;
    u16 offset_middle;
    u32 offset_high;
    u32 reserved;
};

struct [[gnu::packed]] IdtPointer {
    u16 limit;
    u64 base;
};

alignas(16) IdtEntry s_idt[IDT_ENTRIES];

TrapHandler s_trap_handlers[IDT_ENTRIES];
u64 s_counts[IDT_ENTRIES];

struct IrqRegistration {
    IrqHandler handler;
    void* self;
};

IrqRegistration s_irq_handlers[IRQ_COUNT][MAX_HANDLERS_PER_IRQ];
InterruptSpinLock s_irq_lock;

char const* const EXCEPTION_NAMES[32] = {
    "divide by zero",
    "debug",
    "non-maskable interrupt",
    "breakpoint",
    "overflow",
    "bound range exceeded",
    "invalid opcode",
    "device not available",
    "double fault",
    "coprocessor segment overrun",
    "invalid TSS",
    "segment not present",
    "stack-segment fault",
    "general protection fault",
    "page fault",
    "reserved",
    "x87 floating-point exception",
    "alignment check",
    "machine check",
    "SIMD floating-point exception",
    "virtualisation exception",
    "control protection exception",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "hypervisor injection exception",
    "VMM communication exception",
    "security exception",
    "reserved",
    "reserved",
};

void set_gate(usize vector, void* handler, u8 ist, bool user_callable)
{
    auto const address = reinterpret_cast<u64>(handler);
    s_idt[vector] = IdtEntry {
        .offset_low = static_cast<u16>(address),
        .selector = SELECTOR_KERNEL_CODE,
        .ist = ist,
        .type_attributes = static_cast<u8>(GATE_PRESENT | GATE_INTERRUPT | (user_callable ? GATE_DPL3 : 0)),
        .offset_middle = static_cast<u16>(address >> 16),
        .offset_high = static_cast<u32>(address >> 32),
        .reserved = 0,
    };
}

void describe_page_fault(InterruptFrame const* frame)
{
    u64 const address = read_cr2();
    u64 const code = frame->error_code;

    kprintf("  faulting address: %p\n", reinterpret_cast<void*>(address));
    kprintf("  cause: %s %s in %s mode%s\n",
        (code & 0x1) ? "protection violation on" : "page not present for",
        (code & 0x2) ? "write" : ((code & 0x10) ? "instruction fetch" : "read"),
        (code & 0x4) ? "user" : "kernel",
        (code & 0x8) ? ", reserved bit set in a page table entry" : "");

    if (address < 0x1000)
        kprintf("  (that is a null pointer dereference)\n");
    else if (address >= 0xFFFF800000000000ULL && address < 0xFFFFFFFF80000000ULL)
        kprintf("  (that is inside the direct map -- a bad physical address?)\n");
}

} // namespace

void dump_interrupt_frame(InterruptFrame const* frame)
{
    kprintf("  rax=%p rbx=%p rcx=%p\n", reinterpret_cast<void*>(frame->rax),
        reinterpret_cast<void*>(frame->rbx), reinterpret_cast<void*>(frame->rcx));
    kprintf("  rdx=%p rsi=%p rdi=%p\n", reinterpret_cast<void*>(frame->rdx),
        reinterpret_cast<void*>(frame->rsi), reinterpret_cast<void*>(frame->rdi));
    kprintf("  rbp=%p rsp=%p rip=%p\n", reinterpret_cast<void*>(frame->rbp),
        reinterpret_cast<void*>(frame->rsp), reinterpret_cast<void*>(frame->rip));
    kprintf("  r8 =%p r9 =%p r10=%p\n", reinterpret_cast<void*>(frame->r8),
        reinterpret_cast<void*>(frame->r9), reinterpret_cast<void*>(frame->r10));
    kprintf("  r11=%p r12=%p r13=%p\n", reinterpret_cast<void*>(frame->r11),
        reinterpret_cast<void*>(frame->r12), reinterpret_cast<void*>(frame->r13));
    kprintf("  r14=%p r15=%p\n", reinterpret_cast<void*>(frame->r14),
        reinterpret_cast<void*>(frame->r15));
    kprintf("  cs=%p ss=%p rflags=%p err=%p\n", reinterpret_cast<void*>(frame->cs),
        reinterpret_cast<void*>(frame->ss), reinterpret_cast<void*>(frame->rflags),
        reinterpret_cast<void*>(frame->error_code));
}

void register_trap_handler(u8 vector, TrapHandler handler) { s_trap_handlers[vector] = handler; }

u64 interrupt_count(u8 vector) { return s_counts[vector]; }

ModuleResult register_irq_handler(u8 irq, IrqHandler handler, void* self)
{
    if (irq >= IRQ_COUNT || handler == nullptr)
        return MODULE_ERR_INVALID;

    InterruptLockGuard guard(s_irq_lock);
    for (u8 slot = 0; slot < MAX_HANDLERS_PER_IRQ; ++slot) {
        if (s_irq_handlers[irq][slot].handler == nullptr) {
            s_irq_handlers[irq][slot] = { handler, self };
            pic_unmask(irq);
            return MODULE_OK;
        }
    }
    return MODULE_ERR_BUSY;
}

void unregister_irq_handler(u8 irq, void* self)
{
    if (irq >= IRQ_COUNT)
        return;

    InterruptLockGuard guard(s_irq_lock);
    bool any_left = false;
    for (u8 slot = 0; slot < MAX_HANDLERS_PER_IRQ; ++slot) {
        auto& registration = s_irq_handlers[irq][slot];
        if (registration.handler != nullptr && registration.self == self)
            registration = {};
        else if (registration.handler != nullptr)
            any_left = true;
    }
    if (!any_left)
        pic_mask(irq);
}

void irq_mask(u8 irq) { pic_mask(irq); }
void irq_unmask(u8 irq) { pic_unmask(irq); }

void idt_initialize()
{
    memset(s_idt, 0, sizeof(s_idt));
    memset(s_trap_handlers, 0, sizeof(s_trap_handlers));
    memset(s_counts, 0, sizeof(s_counts));
    memset(s_irq_handlers, 0, sizeof(s_irq_handlers));

    for (usize vector = 0; vector < IDT_ENTRIES; ++vector)
        set_gate(vector, isr_stub_table + vector * STUB_STRIDE, 0, false);

    IdtPointer const pointer {
        .limit = static_cast<u16>(sizeof(s_idt) - 1),
        .base = reinterpret_cast<u64>(s_idt),
    };
    asm volatile("lidt %0" ::"m"(pointer) : "memory");

    pic_initialize();

    klog(LOG_INFO, "idt", "%zu vectors installed, PIC remapped to %u..%u", IDT_ENTRIES,
        IRQ_BASE_VECTOR, IRQ_BASE_VECTOR + IRQ_COUNT - 1);
}

} // namespace kernel::arch

extern "C" kernel::InterruptFrame* interrupt_dispatch(kernel::InterruptFrame* frame)
{
    using namespace kernel;
    using namespace kernel::arch;

    auto const vector = static_cast<u8>(frame->vector);
    ++s_counts[vector];

    // A handler registered for this exact vector wins, whatever it is. This is
    // how the timer, the page-fault handler and the syscall gate attach.
    if (auto handler = s_trap_handlers[vector]; handler != nullptr)
        return handler(frame);

    if (vector >= IRQ_BASE_VECTOR && vector < IRQ_BASE_VECTOR + IRQ_COUNT) {
        u8 const irq = static_cast<u8>(vector - IRQ_BASE_VECTOR);

        if (pic_is_spurious(irq))
            return frame;

        bool claimed = false;
        for (u8 slot = 0; slot < MAX_HANDLERS_PER_IRQ; ++slot) {
            auto const& registration = s_irq_handlers[irq][slot];
            if (registration.handler == nullptr)
                continue;
            if (registration.handler(registration.self, irq)) {
                claimed = true;
                break;
            }
        }

        if (!claimed) {
            // Not fatal, but it means a line is enabled with nothing behind it,
            // which will keep firing. Say so once per occurrence and mask it.
            klog(LOG_WARN, "irq", "unclaimed interrupt on line %u, masking", irq);
            pic_mask(irq);
        }

        pic_send_eoi(irq);
        return frame;
    }

    if (vector < 32) {
        kprintf("\n");
        kprintf("  cpu exception %u: %s\n", vector, EXCEPTION_NAMES[vector]);
        if (vector == 14)
            describe_page_fault(frame);
        dump_interrupt_frame(frame);
        panic("unhandled %s at %p", EXCEPTION_NAMES[vector], reinterpret_cast<void*>(frame->rip));
    }

    klog(LOG_WARN, "idt", "unexpected interrupt on vector %u, ignoring", vector);
    return frame;
}
