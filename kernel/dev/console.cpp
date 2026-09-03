// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- kernel diagnostic output.

#include <kernel/dev/console.h>
#include <kernel/lib/spinlock.h>
#include <kernel/lib/string.h>

namespace kernel {

namespace {

constexpr usize MAX_SINKS = 4;

ConsoleSink* s_sinks[MAX_SINKS];
usize s_sink_count = 0;
LogLevel s_min_level = LOG_DEBUG;

// Taken from interrupt context by drivers logging from a handler, so it has to
// mask interrupts rather than just spin.
InterruptSpinLock s_console_lock;

void sink_put(void*, char c) { kputchar(c); }

char const* level_tag(LogLevel level)
{
    switch (level) {
    case LOG_DEBUG:
        return "dbg";
    case LOG_INFO:
        return "   ";
    case LOG_WARN:
        return "WRN";
    case LOG_ERROR:
        return "ERR";
    }
    return "???";
}

} // namespace

void console_register(ConsoleSink* sink)
{
    InterruptLockGuard guard(s_console_lock);
    if (s_sink_count < MAX_SINKS)
        s_sinks[s_sink_count++] = sink;
}

void kputchar(char c)
{
    for (usize i = 0; i < s_sink_count; ++i)
        s_sinks[i]->write_char(c);
}

void console_write(char const* data, usize length)
{
    InterruptLockGuard guard(s_console_lock);
    for (usize i = 0; i < s_sink_count; ++i)
        s_sinks[i]->write(data, length);
}

void kvprintf(char const* format, va_list args)
{
    InterruptLockGuard guard(s_console_lock);
    vformat(sink_put, nullptr, format, args);
}

void kprintf(char const* format, ...)
{
    va_list args;
    va_start(args, format);
    kvprintf(format, args);
    va_end(args);
}

void console_set_min_level(LogLevel level) { s_min_level = level; }

void kvlog(LogLevel level, char const* subsystem, char const* format, va_list args)
{
    if (level < s_min_level)
        return;

    InterruptLockGuard guard(s_console_lock);
    ::kernel::format(sink_put, nullptr, "[%s] %-8s ", level_tag(level), subsystem);
    vformat(sink_put, nullptr, format, args);
    kputchar('\n');
}

void klog(LogLevel level, char const* subsystem, char const* format, ...)
{
    va_list args;
    va_start(args, format);
    kvlog(level, subsystem, format, args);
    va_end(args);
}

} // namespace kernel
