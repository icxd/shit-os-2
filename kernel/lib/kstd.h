// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- the handful of utilities that would otherwise be <utility>.

#pragma once

#include <shitos/types.h>

namespace kernel {

inline constexpr usize PAGE_SIZE = 4096;
inline constexpr usize PAGE_SHIFT = 12;

// The bootstrap mapping of all physical memory. Physical address p is readable
// at HHDM_BASE + p for as long as the kernel is running.
inline constexpr u64 HHDM_BASE = 0xFFFF800000000000ULL;
inline constexpr u64 KERNEL_VMA = 0xFFFFFFFF80000000ULL;

template<typename T>
constexpr T min(T a, T b)
{
    return a < b ? a : b;
}

template<typename T>
constexpr T max(T a, T b)
{
    return a > b ? a : b;
}

template<typename T>
constexpr T align_up(T value, T alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

template<typename T>
constexpr T align_down(T value, T alignment)
{
    return value & ~(alignment - 1);
}

template<typename T>
constexpr T div_round_up(T value, T divisor)
{
    return (value + divisor - 1) / divisor;
}

template<typename T>
constexpr void swap(T& a, T& b)
{
    T tmp = static_cast<T&&>(a);
    a = static_cast<T&&>(b);
    b = static_cast<T&&>(tmp);
}

template<typename T>
struct RemoveReference {
    using Type = T;
};
template<typename T>
struct RemoveReference<T&> {
    using Type = T;
};
template<typename T>
struct RemoveReference<T&&> {
    using Type = T;
};

template<typename T>
constexpr typename RemoveReference<T>::Type&& move(T&& value)
{
    return static_cast<typename RemoveReference<T>::Type&&>(value);
}

// Physical <-> HHDM translation. Everything in the kernel that needs to touch
// physical memory goes through these rather than fabricating pointers.
inline void* phys_to_virt(PhysAddr p)
{
    return reinterpret_cast<void*>(HHDM_BASE + raw(p));
}
inline PhysAddr virt_to_phys(void* v)
{
    return phys(reinterpret_cast<u64>(v) - HHDM_BASE);
}

// For addresses inside the linked kernel image, which live at -2 GiB rather
// than in the direct map.
inline PhysAddr kernel_virt_to_phys(void* v)
{
    return phys(reinterpret_cast<u64>(v) - KERNEL_VMA);
}

class NonCopyable {
public:
    NonCopyable() = default;
    NonCopyable(NonCopyable const&) = delete;
    NonCopyable& operator=(NonCopyable const&) = delete;
};

} // namespace kernel
