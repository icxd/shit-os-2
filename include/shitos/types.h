/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- fixed-width integer types.
 *
 * This header is shared verbatim between the kernel, loadable modules and the
 * C library, so it must stay valid in both C and freestanding C++ and must not
 * depend on anything else.
 */

#pragma once

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

typedef signed char i8;
typedef signed short i16;
typedef signed int i32;
typedef signed long long i64;

typedef unsigned long usize;
typedef signed long isize;

#ifdef __cplusplus

static_assert(sizeof(u8) == 1, "u8 must be 1 byte");
static_assert(sizeof(u16) == 2, "u16 must be 2 bytes");
static_assert(sizeof(u32) == 4, "u32 must be 4 bytes");
static_assert(sizeof(u64) == 8, "u64 must be 8 bytes");
static_assert(sizeof(usize) == 8, "shit os 2 is 64-bit only");

/*
 * Physical and virtual addresses are deliberately distinct types. Mixing them
 * up is the single most common way to lose an afternoon in a kernel, so we let
 * the compiler catch it instead.
 */
enum class PhysAddr : u64 {};
enum class VirtAddr : u64 {};

constexpr u64 raw(PhysAddr a)
{
    return static_cast<u64>(a);
}
constexpr u64 raw(VirtAddr a)
{
    return static_cast<u64>(a);
}

constexpr PhysAddr phys(u64 v)
{
    return static_cast<PhysAddr>(v);
}
constexpr VirtAddr virt(u64 v)
{
    return static_cast<VirtAddr>(v);
}

constexpr PhysAddr operator+(PhysAddr a, u64 n)
{
    return phys(raw(a) + n);
}
constexpr VirtAddr operator+(VirtAddr a, u64 n)
{
    return virt(raw(a) + n);
}

#endif /* __cplusplus */
