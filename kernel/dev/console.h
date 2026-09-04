// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- kernel diagnostic output.
//
// Output fans out to every registered sink. In practice that is the serial
// port (which is how anything automated reads the machine) and the framebuffer
// console (which is how a human does). Registration is deliberately allocation
// free so the very first line of boot can already be printed.

#pragma once

#include <kernel/lib/format.h>

#include <shitos/module/api.h>
#include <shitos/types.h>

#include <stdarg.h>

namespace kernel {

class ConsoleSink {
public:
    virtual ~ConsoleSink() = default;
    virtual void write_char(char c) = 0;
    virtual void write(char const* data, usize length)
    {
        for (usize i = 0; i < length; ++i)
            write_char(data[i]);
    }
    virtual char const* name() const = 0;
};

void console_register(ConsoleSink* sink);
void console_write(char const* data, usize length);

void kputchar(char c);
void kprintf(char const* format, ...) __attribute__((format(printf, 1, 2)));
void kvprintf(char const* format, va_list args);

// Tagged logging. `subsystem` shows up in brackets, which is what makes a boot
// log readable when six subsystems are talking at once.
void klog(LogLevel level, char const* subsystem, char const* format, ...)
    __attribute__((format(printf, 3, 4)));
void kvlog(LogLevel level, char const* subsystem, char const* format, va_list args);

void console_set_min_level(LogLevel level);

// Colour is on by default. Both sinks understand ANSI -- the serial port
// because whatever is reading it does, and the framebuffer console because it
// parses SGR itself -- so there is nowhere for the escapes to leak as text.
// Turning it off exists for a log being piped somewhere that does not.
void console_set_color_enabled(bool enabled);
bool console_color_enabled();

} // namespace kernel
