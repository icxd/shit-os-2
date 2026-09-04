// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- starting the first user process.

#pragma once

#include <kernel/lib/error.h>

namespace kernel::sys {

// Creates pid 1 from the given executable, wires its standard descriptors to
// the terminal, and makes it runnable. Everything else in userspace descends
// from this.
ErrorOr<void> start_init(char const* path);

} // namespace kernel::sys
