// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- 16550 UART on COM1. This is the console CI reads.

#pragma once

#include <kernel/dev/console.h>

#include <shitos/types.h>

namespace kernel::arch {

class SerialPort final : public ConsoleSink {
public:
    explicit SerialPort(u16 io_base)
        : m_io_base(io_base)
    {
    }

    bool initialize();
    void write_char(char c) override;
    char const* name() const override { return "serial"; }

    bool has_input() const;
    char read_char();

private:
    bool is_transmit_empty() const;

    u16 m_io_base;
    bool m_present { false };
};

// COM1. Brought up before anything else so that a panic during early boot
// still says something.
SerialPort& serial_com1();
bool serial_initialize();

} // namespace kernel::arch
