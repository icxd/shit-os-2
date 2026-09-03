// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- virtual memory.
//
// One AddressSpace wraps one PML4. The upper half (entries 256..511) is shared
// by every address space and describes the kernel; the lower half belongs to
// whichever process owns the space. Sharing the upper half is what makes a
// syscall from ring 3 able to run kernel code without switching CR3.
//
//   0x0000000000000000 .. 0x00007FFFFFFFFFFF   user
//   0xFFFF800000000000 .. 0xFFFF807FFFFFFFFF   direct map of physical memory
//   0xFFFFC00000000000 .. 0xFFFFC0FFFFFFFFFF   MMIO window for drivers
//   0xFFFFFFFF80000000 .. 0xFFFFFFFFFFFFFFFF   the kernel image

#pragma once

#include <kernel/boot/boot_info.h>
#include <kernel/lib/error.h>
#include <kernel/lib/kstd.h>
#include <shitos/types.h>

namespace kernel::mm {

enum class PageFlags : u64 {
    None = 0,
    Present = 1ULL << 0,
    Writable = 1ULL << 1,
    User = 1ULL << 2,
    WriteThrough = 1ULL << 3,
    CacheDisable = 1ULL << 4,
    Accessed = 1ULL << 5,
    Dirty = 1ULL << 6,
    Huge = 1ULL << 7,
    Global = 1ULL << 8,
    NoExecute = 1ULL << 63,
};

constexpr PageFlags operator|(PageFlags a, PageFlags b)
{
    return static_cast<PageFlags>(static_cast<u64>(a) | static_cast<u64>(b));
}

constexpr PageFlags operator&(PageFlags a, PageFlags b)
{
    return static_cast<PageFlags>(static_cast<u64>(a) & static_cast<u64>(b));
}

constexpr bool has_flag(PageFlags value, PageFlags flag)
{
    return (static_cast<u64>(value) & static_cast<u64>(flag)) != 0;
}

inline constexpr u64 USER_SPACE_END = 0x0000800000000000ULL;
inline constexpr u64 MMIO_WINDOW_BASE = 0xFFFFC00000000000ULL;
inline constexpr u64 MMIO_WINDOW_SIZE = 0x0000010000000000ULL;

class AddressSpace {
public:
    // The kernel's own space, built during boot to replace the bootstrap
    // tables from boot.S with correct per-section permissions.
    static AddressSpace& kernel_space();

    // A fresh space for a user process. The kernel half is shared with
    // kernel_space(), so mappings made there are visible here immediately.
    static ErrorOr<AddressSpace*> create_user_space();

    ErrorOr<void> map(VirtAddr address, PhysAddr page, PageFlags flags);
    ErrorOr<void> map_range(VirtAddr address, PhysAddr base, usize length, PageFlags flags);

    // Maps `length` bytes of freshly allocated, zeroed anonymous memory.
    ErrorOr<void> map_anonymous(VirtAddr address, usize length, PageFlags flags);

    void unmap(VirtAddr address);
    void unmap_range(VirtAddr address, usize length);

    ErrorOr<PhysAddr> translate(VirtAddr address) const;
    bool is_mapped(VirtAddr address) const;

    // The effective flags on the leaf entry for this address. Used by the self
    // tests to check W^X without having to trigger a fault to find out.
    ErrorOr<PageFlags> query(VirtAddr address) const;

    // True when [address, address+length) is entirely mapped with at least the
    // requested access. Every syscall that takes a user pointer goes through
    // this before touching it.
    bool validate_user_range(VirtAddr address, usize length, bool for_write) const;

    void activate() const;
    PhysAddr root() const { return m_root; }

    // Releases every user-half mapping and the page tables behind them. The
    // kernel half is shared and is deliberately left alone.
    void destroy_user_mappings();

    usize resident_bytes() const { return m_resident_pages * PAGE_SIZE; }

private:
    // Builds the one kernel space during boot, in statically reserved storage.
    friend void virtual_memory_initialize(boot::BootInfo const& info);

    explicit AddressSpace(PhysAddr root)
        : m_root(root)
    {
    }

    ErrorOr<u64*> ensure_table(u64* table, usize index, bool user_accessible);
    u64* walk_to_leaf_table(u64 address) const;

    PhysAddr m_root;
    usize m_resident_pages { 0 };
};

// Builds the kernel address space and switches to it. Must run after the
// physical allocator is up and before anything relies on NX or on the
// bootstrap identity map being gone.
void virtual_memory_initialize(boot::BootInfo const& info);

// Maps device memory into the kernel MMIO window, uncached. This is what
// KernelApi::map_mmio is wired to.
ErrorOr<void*> map_mmio(PhysAddr base, usize length);
void unmap_mmio(void* address, usize length);

} // namespace kernel::mm
