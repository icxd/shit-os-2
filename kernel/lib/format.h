// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- printf-style formatting, with the output sink left to the caller.

#pragma once

#include <shitos/types.h>
#include <stdarg.h>

namespace kernel {

// Called once per character. Sinks are the serial port, the framebuffer
// console, and a bounded buffer for the snprintf-alikes.
using FormatSink = void (*)(void* context, char c);

// Supports: %s %c %d %i %u %x %X %o %p %% with the length modifiers l, ll and
// z, a field width, '0' and '-' flags, and '+' for signed conversions.
usize vformat(FormatSink sink, void* context, char const* format, va_list args);
usize format(FormatSink sink, void* context, char const* format, ...) __attribute__((format(printf, 3, 4)));

usize vsnprintf(char* buffer, usize size, char const* format, va_list args);
usize snprintf(char* buffer, usize size, char const* format, ...) __attribute__((format(printf, 3, 4)));

} // namespace kernel
