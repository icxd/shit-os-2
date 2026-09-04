// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- CPU feature configuration.
//
// boot.S turns on the bare minimum needed to reach long mode. This turns on
// everything else we want, once, on the bootstrap processor. When application
// processors arrive they run the same function.

#pragma once

#include <shitos/types.h>

namespace kernel::arch {

struct CpuFeatures {
    bool nx;
    bool pge; // global pages survive a CR3 reload
    bool smep; // ring 0 cannot execute a user-accessible page
    bool smap; // ring 0 cannot read or write a user page without STAC
    bool gigabyte_pages;
    bool syscall;
    bool tsc;
    bool fsgsbase;
    char vendor[13];
    char brand[49];
};

void cpu_initialize();
CpuFeatures const& cpu_features();

} // namespace kernel::arch
