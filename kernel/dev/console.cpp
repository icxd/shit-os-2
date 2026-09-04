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

void sink_put(void*, char c)
{
    kputchar(c);
}

char const* level_tag(LogLevel level)
{
    switch (level) {
    case LOG_DEBUG: return "dbg";
    case LOG_INFO: return "   ";
    case LOG_WARN: return "WRN";
    case LOG_ERROR: return "ERR";
    }
    return "???";
}

// ANSI, because both sinks understand it: a real terminal on the other end of
// the serial port, and our own framebuffer console, which parses SGR for
// exactly this. A boot log is the one place where the difference between "a
// subsystem said something" and "a subsystem is unhappy" should be visible
// without reading a word of it.
char const* level_color(LogLevel level)
{
    switch (level) {
    case LOG_DEBUG: return "\033[90m"; // grey, so debug recedes
    case LOG_INFO: return "\033[32m"; // green
    case LOG_WARN: return "\033[33m"; // yellow
    case LOG_ERROR: return "\033[1;31m"; // bold red
    }
    return "";
}

constexpr char const* COLOR_SUBSYSTEM = "\033[36m"; // cyan
constexpr char const* COLOR_MESSAGE_ERROR = "\033[31m";
constexpr char const* COLOR_RESET = "\033[0m";

bool s_color_enabled = true;

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

void console_set_min_level(LogLevel level)
{
    s_min_level = level;
}

void console_set_color_enabled(bool enabled)
{
    s_color_enabled = enabled;
}

bool console_color_enabled()
{
    return s_color_enabled;
}

void kvlog(LogLevel level, char const* subsystem, char const* format, va_list args)
{
    if (level < s_min_level)
        return;

    InterruptLockGuard guard(s_console_lock);

    if (!s_color_enabled) {
        ::kernel::format(sink_put, nullptr, "[%s] %-8s ", level_tag(level), subsystem);
        vformat(sink_put, nullptr, format, args);
        kputchar('\n');
        return;
    }

    // The tag carries the level, the subsystem is always cyan so the eye can
    // follow one subsystem down the log, and only an error colours its message
    // -- colouring every line would make none of them stand out.
    ::kernel::format(sink_put, nullptr, "%s[%s]%s %s%-8s%s ", level_color(level), level_tag(level),
        COLOR_RESET, COLOR_SUBSYSTEM, subsystem, COLOR_RESET);

    if (level >= LOG_WARN)
        ::kernel::format(
            sink_put, nullptr, "%s", level == LOG_WARN ? "\033[33m" : COLOR_MESSAGE_ERROR);
    vformat(sink_put, nullptr, format, args);
    if (level >= LOG_WARN)
        ::kernel::format(sink_put, nullptr, "%s", COLOR_RESET);

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
