/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- byte offsets into the per-CPU block.
 *
 * Included by both percpu.h and the assembly entry stubs, which cannot see
 * C++ types. percpu.h static_asserts every value here against the real struct,
 * so a field moving is a build failure rather than a boot failure.
 */

#pragma once

#define CPU_OFFSET_SELF 0
#define CPU_OFFSET_KERNEL_STACK_TOP 56
#define CPU_OFFSET_SYSCALL_SCRATCH_RSP 64
