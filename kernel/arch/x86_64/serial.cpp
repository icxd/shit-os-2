// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- 16550 UART on COM1.

#include <kernel/arch/x86_64/io.h>
#include <kernel/arch/x86_64/serial.h>
#include <kernel/lib/spinlock.h>

namespace kernel::arch {

namespace {

// Register offsets from the port base.
constexpr u16 REG_DATA = 0;
constexpr u16 REG_INTERRUPT_ENABLE = 1;
constexpr u16 REG_DIVISOR_LOW = 0;
constexpr u16 REG_DIVISOR_HIGH = 1;
constexpr u16 REG_FIFO_CONTROL = 2;
constexpr u16 REG_LINE_CONTROL = 3;
constexpr u16 REG_MODEM_CONTROL = 4;
constexpr u16 REG_LINE_STATUS = 5;

constexpr u8 LINE_STATUS_DATA_READY = 0x01;
constexpr u8 LINE_STATUS_TRANSMIT_EMPTY = 0x20;

constexpr u8 LINE_CONTROL_8N1 = 0x03;
constexpr u8 LINE_CONTROL_DLAB = 0x80;

SerialPort s_com1(0x3F8);

} // namespace

bool SerialPort::initialize()
{
    outb(m_io_base + REG_INTERRUPT_ENABLE, 0x00); // polling only, for now
    outb(m_io_base + REG_LINE_CONTROL, LINE_CONTROL_DLAB);
    outb(m_io_base + REG_DIVISOR_LOW, 0x01); // divisor 1 => 115200 baud
    outb(m_io_base + REG_DIVISOR_HIGH, 0x00);
    outb(m_io_base + REG_LINE_CONTROL, LINE_CONTROL_8N1);
    outb(m_io_base + REG_FIFO_CONTROL, 0xC7); // enable + clear FIFOs, 14-byte trigger
    outb(m_io_base + REG_MODEM_CONTROL, 0x0B); // DTR + RTS + OUT2

    // Loopback test: if what we write does not come back, there is no UART
    // here and every later write would busy-wait forever.
    outb(m_io_base + REG_MODEM_CONTROL, 0x1E);
    outb(m_io_base + REG_DATA, 0xAE);
    if (inb(m_io_base + REG_DATA) != 0xAE) {
        m_present = false;
        return false;
    }

    outb(m_io_base + REG_MODEM_CONTROL, 0x0F);
    m_present = true;
    return true;
}

bool SerialPort::is_transmit_empty() const
{
    return (inb(m_io_base + REG_LINE_STATUS) & LINE_STATUS_TRANSMIT_EMPTY) != 0;
}

void SerialPort::write_char(char c)
{
    if (!m_present)
        return;

    // A bare newline on a terminal leaves the cursor in column N; terminals
    // that matter want CRLF.
    if (c == '\n')
        write_char('\r');

    while (!is_transmit_empty())
        cpu_relax();
    outb(m_io_base + REG_DATA, static_cast<u8>(c));
}

bool SerialPort::has_input() const
{
    if (!m_present)
        return false;
    return (inb(m_io_base + REG_LINE_STATUS) & LINE_STATUS_DATA_READY) != 0;
}

char SerialPort::read_char()
{
    while (!has_input())
        cpu_relax();
    return static_cast<char>(inb(m_io_base + REG_DATA));
}

SerialPort& serial_com1()
{
    return s_com1;
}

bool serial_initialize()
{
    return s_com1.initialize();
}

} // namespace kernel::arch
