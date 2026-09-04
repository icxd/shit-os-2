// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- the end of the line.

#pragma once

#include <shitos/types.h>

namespace kernel {

// Prints the message, dumps whatever machine state we can reach, and stops the
// CPU. Gen 1 called this a blue screen; the spirit is preserved.
[[noreturn]] void panic(char const* format, ...) __attribute__((format(printf, 1, 2)));

// Same, but with the interrupt frame of the fault that got us here.
struct InterruptFrame;
[[noreturn]] void panic_with_frame(InterruptFrame const* frame, char const* format, ...)
    __attribute__((format(printf, 2, 3)));

} // namespace kernel

#define VERIFY(expression)                                                                         \
    do {                                                                                           \
        if (!(expression)) [[unlikely]]                                                            \
            ::kernel::panic("VERIFY(%s) failed at %s:%d", #expression, __FILE__, __LINE__);        \
    } while (0)

#define VERIFY_NOT_REACHED()                                                                       \
    ::kernel::panic("reached unreachable code at %s:%d", __FILE__, __LINE__)

#define TODO() ::kernel::panic("not implemented yet: %s at %s:%d", __func__, __FILE__, __LINE__)
