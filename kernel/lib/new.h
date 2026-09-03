// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- placement new.
//
// The kernel constructs objects in memory it has already obtained (slabs,
// per-CPU blocks, statically reserved storage) often enough to need this, and
// there is no <new> to include in a freestanding build.

#pragma once

#include <shitos/types.h>

inline void* operator new(usize, void* where) noexcept { return where; }
inline void* operator new[](usize, void* where) noexcept { return where; }
inline void operator delete(void*, void*) noexcept { }
inline void operator delete[](void*, void*) noexcept { }
