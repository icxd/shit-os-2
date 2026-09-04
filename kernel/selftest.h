// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- boot-time self tests.
//
// Each subsystem gets a handful of assertions that run once during boot. They
// exist because an operating system has no test harness to run under: the only
// way to find out that the physical allocator hands out the same page twice is
// to check, on the machine, before anything depends on it.
//
// Disable with `nocheck` on the kernel command line if they ever get in the way.

#pragma once

namespace kernel {

void run_boot_selftests();

// Needs a running scheduler, so it runs later than the rest.
void run_scheduler_selftests();

} // namespace kernel
