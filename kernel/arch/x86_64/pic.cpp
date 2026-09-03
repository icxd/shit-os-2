// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- the legacy 8259 pair.

#include <kernel/arch/x86_64/interrupts.h>
#include <kernel/arch/x86_64/io.h>
#include <kernel/arch/x86_64/pic.h>

namespace kernel::arch {

namespace {

constexpr u16 PIC1_COMMAND = 0x20;
constexpr u16 PIC1_DATA = 0x21;
constexpr u16 PIC2_COMMAND = 0xA0;
constexpr u16 PIC2_DATA = 0xA1;

constexpr u8 ICW1_INIT = 0x11; // initialise, expect ICW4
constexpr u8 ICW4_8086 = 0x01;
constexpr u8 COMMAND_EOI = 0x20;
constexpr u8 COMMAND_READ_ISR = 0x0B;

} // namespace

void pic_initialize()
{
    // Remap: master to 32..39, slave to 40..47.
    outb(PIC1_COMMAND, ICW1_INIT);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT);
    io_wait();
    outb(PIC1_DATA, IRQ_BASE_VECTOR);
    io_wait();
    outb(PIC2_DATA, IRQ_BASE_VECTOR + 8);
    io_wait();
    outb(PIC1_DATA, 0x04); // slave is wired to master IRQ2
    io_wait();
    outb(PIC2_DATA, 0x02); // slave's cascade identity
    io_wait();
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    pic_mask_all();
}

void pic_mask_all()
{
    // Everything off except the cascade line, which must stay open or the
    // slave chip can never deliver anything.
    outb(PIC1_DATA, 0xFB);
    outb(PIC2_DATA, 0xFF);
}

void pic_mask(u8 irq)
{
    u16 const port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    u8 const bit = static_cast<u8>(1 << (irq & 7));
    outb(port, static_cast<u8>(inb(port) | bit));
}

void pic_unmask(u8 irq)
{
    u16 const port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    u8 const bit = static_cast<u8>(1 << (irq & 7));
    outb(port, static_cast<u8>(inb(port) & ~bit));

    // Unmasking a slave line is useless while the cascade is masked.
    if (irq >= 8)
        outb(PIC1_DATA, static_cast<u8>(inb(PIC1_DATA) & ~(1 << 2)));
}

void pic_send_eoi(u8 irq)
{
    // The slave needs its own acknowledgement before the master's.
    if (irq >= 8)
        outb(PIC2_COMMAND, COMMAND_EOI);
    outb(PIC1_COMMAND, COMMAND_EOI);
}

bool pic_is_spurious(u8 irq)
{
    if (irq != 7 && irq != 15)
        return false;

    u16 const command_port = irq == 7 ? PIC1_COMMAND : PIC2_COMMAND;
    outb(command_port, COMMAND_READ_ISR);
    u8 const in_service = inb(command_port);
    bool const real = (in_service & (1 << (irq & 7))) != 0;

    // A spurious IRQ15 still requires the master to be acknowledged, because
    // the master genuinely did forward a cascade interrupt.
    if (!real && irq == 15)
        outb(PIC1_COMMAND, COMMAND_EOI);

    return !real;
}

} // namespace kernel::arch
