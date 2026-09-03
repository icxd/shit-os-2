// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- virtual memory.

#include <kernel/arch/x86_64/io.h>
#include <kernel/dev/console.h>
#include <kernel/lib/new.h>
#include <kernel/lib/spinlock.h>
#include <kernel/lib/string.h>
#include <kernel/mm/address_space.h>
#include <kernel/mm/physical.h>
#include <kernel/panic.h>

extern "C" {
extern u8 __text_start[], __text_end[];
extern u8 __rodata_start[], __rodata_end[];
extern u8 __data_start[], __bss_end[];
}

namespace kernel::mm {

namespace {

constexpr u64 ADDRESS_MASK = 0x000FFFFFFFFFF000ULL;
constexpr usize ENTRIES_PER_TABLE = 512;
constexpr u64 HUGE_PAGE_SIZE = 2 * 1024 * 1024;

// The direct map always covers at least the low 4 GiB so that legacy MMIO --
// the framebuffer above all -- stays reachable even on a machine with very
// little RAM.
constexpr u64 MINIMUM_HHDM_SIZE = 4ULL * 1024 * 1024 * 1024;

AddressSpace* s_kernel_space = nullptr;
alignas(AddressSpace) u8 s_kernel_space_storage[sizeof(AddressSpace)];

u64 s_mmio_next = MMIO_WINDOW_BASE;
InterruptSpinLock s_mmio_lock;

constexpr usize index_of(u64 address, int level)
{
    // level 4 = PML4, 3 = PDPT, 2 = PD, 1 = PT
    return (address >> (12 + 9 * (level - 1))) & 0x1FF;
}

u64* table_at(u64 entry) { return static_cast<u64*>(phys_to_virt(phys(entry & ADDRESS_MASK))); }

void flush(u64 address) { arch::invlpg(address); }

} // namespace

ErrorOr<u64*> AddressSpace::ensure_table(u64* table, usize index, bool user_accessible)
{
    u64& entry = table[index];

    if (!(entry & static_cast<u64>(PageFlags::Present))) {
        auto const page = TRY(allocate_zeroed_page());
        entry = raw(page) | static_cast<u64>(PageFlags::Present) | static_cast<u64>(PageFlags::Writable);
    }

    // An intermediate entry has to be at least as permissive as the leaf below
    // it: the CPU ANDs the permission bits down the whole walk, so a kernel-only
    // PDPT would make a user leaf unreachable from ring 3.
    if (user_accessible)
        entry |= static_cast<u64>(PageFlags::User);

    if (entry & static_cast<u64>(PageFlags::Huge))
        return Error::from_errno(EEXIST);

    return table_at(entry);
}

ErrorOr<void> AddressSpace::map(VirtAddr address, PhysAddr page, PageFlags flags)
{
    u64 const virtual_address = align_down<u64>(raw(address), PAGE_SIZE);
    bool const user = has_flag(flags, PageFlags::User);

    u64* pml4 = static_cast<u64*>(phys_to_virt(m_root));
    u64* pdpt = TRY(ensure_table(pml4, index_of(virtual_address, 4), user));
    u64* pd = TRY(ensure_table(pdpt, index_of(virtual_address, 3), user));
    u64* pt = TRY(ensure_table(pd, index_of(virtual_address, 2), user));

    u64& entry = pt[index_of(virtual_address, 1)];
    if (entry & static_cast<u64>(PageFlags::Present))
        return Error::from_errno(EEXIST);

    entry = (raw(page) & ADDRESS_MASK) | static_cast<u64>(flags | PageFlags::Present);
    ++m_resident_pages;
    flush(virtual_address);
    return {};
}

ErrorOr<void> AddressSpace::map_range(VirtAddr address, PhysAddr base, usize length, PageFlags flags)
{
    u64 const start = align_down<u64>(raw(address), PAGE_SIZE);
    u64 const end = align_up<u64>(raw(address) + length, PAGE_SIZE);

    for (u64 offset = 0; start + offset < end; offset += PAGE_SIZE)
        TRY(map(virt(start + offset), base + offset, flags));

    return {};
}

ErrorOr<void> AddressSpace::map_anonymous(VirtAddr address, usize length, PageFlags flags)
{
    u64 const start = align_down<u64>(raw(address), PAGE_SIZE);
    u64 const end = align_up<u64>(raw(address) + length, PAGE_SIZE);

    for (u64 page_address = start; page_address < end; page_address += PAGE_SIZE) {
        // Already mapped is fine here: growing a heap or a stack repeatedly
        // touches pages that are partly present already.
        if (is_mapped(virt(page_address)))
            continue;

        auto const frame_or_error = allocate_zeroed_page();
        if (frame_or_error.is_error()) {
            // Roll back what this call added, so a failed mmap leaves nothing
            // half-mapped behind it.
            unmap_range(virt(start), page_address - start);
            return frame_or_error.error();
        }
        auto const frame = frame_or_error.value();

        if (auto result = map(virt(page_address), frame, flags); result.is_error()) {
            free_page(frame);
            unmap_range(virt(start), page_address - start);
            return result.error();
        }
    }

    return {};
}

u64* AddressSpace::walk_to_leaf_table(u64 address) const
{
    u64* table = static_cast<u64*>(phys_to_virt(m_root));

    for (int level = 4; level > 1; --level) {
        u64 const entry = table[index_of(address, level)];
        if (!(entry & static_cast<u64>(PageFlags::Present)))
            return nullptr;
        if (entry & static_cast<u64>(PageFlags::Huge))
            return nullptr; // caller must handle huge mappings itself
        table = table_at(entry);
    }

    return table;
}

ErrorOr<PhysAddr> AddressSpace::translate(VirtAddr address) const
{
    u64 const virtual_address = raw(address);
    u64* table = static_cast<u64*>(phys_to_virt(m_root));

    for (int level = 4; level > 1; --level) {
        u64 const entry = table[index_of(virtual_address, level)];
        if (!(entry & static_cast<u64>(PageFlags::Present)))
            return Error::from_errno(EFAULT);

        if (entry & static_cast<u64>(PageFlags::Huge)) {
            // A huge mapping ends the walk early; the remaining index bits are
            // the offset within the large page.
            u64 const size = 1ULL << (12 + 9 * (level - 1));
            return phys((entry & ADDRESS_MASK) + (virtual_address & (size - 1)));
        }
        table = table_at(entry);
    }

    u64 const entry = table[index_of(virtual_address, 1)];
    if (!(entry & static_cast<u64>(PageFlags::Present)))
        return Error::from_errno(EFAULT);

    return phys((entry & ADDRESS_MASK) + (virtual_address & (PAGE_SIZE - 1)));
}

bool AddressSpace::is_mapped(VirtAddr address) const { return !translate(address).is_error(); }

ErrorOr<PageFlags> AddressSpace::query(VirtAddr address) const
{
    u64 const virtual_address = raw(address);
    u64 const* table = static_cast<u64 const*>(phys_to_virt(m_root));

    for (int level = 4; level > 1; --level) {
        u64 const entry = table[index_of(virtual_address, level)];
        if (!(entry & static_cast<u64>(PageFlags::Present)))
            return Error::from_errno(EFAULT);
        if (entry & static_cast<u64>(PageFlags::Huge))
            return static_cast<PageFlags>(entry & ~ADDRESS_MASK);
        table = table_at(entry);
    }

    u64 const entry = table[index_of(virtual_address, 1)];
    if (!(entry & static_cast<u64>(PageFlags::Present)))
        return Error::from_errno(EFAULT);
    return static_cast<PageFlags>(entry & ~ADDRESS_MASK);
}

bool AddressSpace::validate_user_range(VirtAddr address, usize length, bool for_write) const
{
    u64 const start = raw(address);
    // A range that wraps, or that reaches into the kernel half, is never valid
    // no matter what the page tables say.
    if (start + length < start || start + length > USER_SPACE_END)
        return false;
    if (length == 0)
        return true;

    u64 const first = align_down<u64>(start, PAGE_SIZE);
    u64 const last = align_up<u64>(start + length, PAGE_SIZE);

    for (u64 page_address = first; page_address < last; page_address += PAGE_SIZE) {
        u64* pt = walk_to_leaf_table(page_address);
        if (pt == nullptr)
            return false;
        u64 const entry = pt[index_of(page_address, 1)];
        if (!(entry & static_cast<u64>(PageFlags::Present)))
            return false;
        if (!(entry & static_cast<u64>(PageFlags::User)))
            return false;
        if (for_write && !(entry & static_cast<u64>(PageFlags::Writable)))
            return false;
    }

    return true;
}

void AddressSpace::unmap(VirtAddr address)
{
    u64 const virtual_address = align_down<u64>(raw(address), PAGE_SIZE);
    u64* pt = walk_to_leaf_table(virtual_address);
    if (pt == nullptr)
        return;

    u64& entry = pt[index_of(virtual_address, 1)];
    if (!(entry & static_cast<u64>(PageFlags::Present)))
        return;

    entry = 0;
    --m_resident_pages;
    flush(virtual_address);
}

void AddressSpace::unmap_range(VirtAddr address, usize length)
{
    u64 const start = align_down<u64>(raw(address), PAGE_SIZE);
    u64 const end = align_up<u64>(raw(address) + length, PAGE_SIZE);
    for (u64 page_address = start; page_address < end; page_address += PAGE_SIZE)
        unmap(virt(page_address));
}

void AddressSpace::destroy_user_mappings()
{
    u64* pml4 = static_cast<u64*>(phys_to_virt(m_root));

    // Only the lower half. Entries 256..511 are the shared kernel mappings and
    // freeing those would take every other process down with this one.
    for (usize pml4_index = 0; pml4_index < 256; ++pml4_index) {
        u64 const pml4_entry = pml4[pml4_index];
        if (!(pml4_entry & static_cast<u64>(PageFlags::Present)))
            continue;

        u64* pdpt = table_at(pml4_entry);
        for (usize pdpt_index = 0; pdpt_index < ENTRIES_PER_TABLE; ++pdpt_index) {
            u64 const pdpt_entry = pdpt[pdpt_index];
            if (!(pdpt_entry & static_cast<u64>(PageFlags::Present)))
                continue;

            u64* pd = table_at(pdpt_entry);
            for (usize pd_index = 0; pd_index < ENTRIES_PER_TABLE; ++pd_index) {
                u64 const pd_entry = pd[pd_index];
                if (!(pd_entry & static_cast<u64>(PageFlags::Present)))
                    continue;

                if (!(pd_entry & static_cast<u64>(PageFlags::Huge))) {
                    u64* pt = table_at(pd_entry);
                    for (usize pt_index = 0; pt_index < ENTRIES_PER_TABLE; ++pt_index) {
                        u64 const pt_entry = pt[pt_index];
                        if (pt_entry & static_cast<u64>(PageFlags::Present))
                            free_page(phys(pt_entry & ADDRESS_MASK));
                    }
                    free_page(phys(pd_entry & ADDRESS_MASK));
                }
            }
            free_page(phys(pdpt_entry & ADDRESS_MASK));
        }
        free_page(phys(pml4_entry & ADDRESS_MASK));
        pml4[pml4_index] = 0;
    }

    m_resident_pages = 0;
}

void AddressSpace::activate() const { arch::write_cr3(raw(m_root)); }

AddressSpace& AddressSpace::kernel_space()
{
    VERIFY(s_kernel_space != nullptr);
    return *s_kernel_space;
}

ErrorOr<AddressSpace*> AddressSpace::create_user_space()
{
    auto const root = TRY(allocate_zeroed_page());

    // Share the kernel half by copying the PML4 entries. Because the tables
    // *below* those entries are shared rather than copied, a kernel mapping
    // made later is visible in every process automatically -- which is why
    // kernel_space() pre-allocates all 256 upper-half PDPTs up front.
    u64* new_pml4 = static_cast<u64*>(phys_to_virt(root));
    u64 const* kernel_pml4 = static_cast<u64 const*>(phys_to_virt(s_kernel_space->m_root));
    for (usize i = 256; i < ENTRIES_PER_TABLE; ++i)
        new_pml4[i] = kernel_pml4[i];

    auto* space = new AddressSpace(root);
    if (space == nullptr) {
        free_page(root);
        return Error::from_errno(ENOMEM);
    }
    return space;
}

ErrorOr<void*> map_mmio(PhysAddr base, usize length)
{
    if (length == 0)
        return Error::from_errno(EINVAL);

    InterruptLockGuard guard(s_mmio_lock);

    u64 const page_offset = raw(base) & (PAGE_SIZE - 1);
    u64 const aligned_base = align_down<u64>(raw(base), PAGE_SIZE);
    usize const mapped_length = align_up<usize>(length + page_offset, PAGE_SIZE);

    if (s_mmio_next + mapped_length > MMIO_WINDOW_BASE + MMIO_WINDOW_SIZE)
        return Error::from_errno(ENOMEM);

    u64 const window = s_mmio_next;
    auto const flags = PageFlags::Present | PageFlags::Writable | PageFlags::NoExecute
        | PageFlags::CacheDisable | PageFlags::Global;

    TRY(AddressSpace::kernel_space().map_range(virt(window), phys(aligned_base), mapped_length, flags));
    s_mmio_next += mapped_length;

    return reinterpret_cast<void*>(window + page_offset);
}

void unmap_mmio(void* address, usize length)
{
    u64 const start = align_down<u64>(reinterpret_cast<u64>(address), PAGE_SIZE);
    // The window is a bump allocator, so the address space is not reclaimed;
    // only the mapping goes away. Drivers unloading and reloading are rare
    // enough that a real allocator here would be premature.
    AddressSpace::kernel_space().unmap_range(virt(start), length);
}

void virtual_memory_initialize(boot::BootInfo const& info)
{
    auto const root = allocate_zeroed_page();
    if (root.is_error())
        panic("could not allocate a PML4 for the kernel address space");

    s_kernel_space = new (s_kernel_space_storage) AddressSpace(root.value());
    u64* pml4 = static_cast<u64*>(phys_to_virt(root.value()));

    // Pre-allocate every upper-half PDPT so that user address spaces can share
    // the kernel half by copying 256 PML4 entries once, at creation, and still
    // see kernel mappings added afterwards. One megabyte, spent once.
    for (usize i = 256; i < ENTRIES_PER_TABLE; ++i) {
        auto const table = allocate_zeroed_page();
        if (table.is_error())
            panic("could not allocate kernel PDPT %zu", i);
        pml4[i] = raw(table.value()) | static_cast<u64>(PageFlags::Present)
            | static_cast<u64>(PageFlags::Writable);
    }

    // The direct map, with 2 MiB pages. It is writable and never executable:
    // nothing should ever jump into the direct map, and marking it NX means an
    // attempt to faults instead of running.
    u64 highest_usable = MINIMUM_HHDM_SIZE;
    for (usize i = 0; i < info.memory_region_count; ++i) {
        auto const& region = info.memory_regions[i];
        if (region.kind == boot::MemoryKind::Usable)
            highest_usable = max(highest_usable, region.base + region.length);
    }
    u64 const hhdm_size = align_up<u64>(highest_usable, HUGE_PAGE_SIZE);

    auto const hhdm_flags = static_cast<u64>(PageFlags::Present) | static_cast<u64>(PageFlags::Writable)
        | static_cast<u64>(PageFlags::NoExecute) | static_cast<u64>(PageFlags::Global)
        | static_cast<u64>(PageFlags::Huge);

    for (u64 offset = 0; offset < hhdm_size; offset += HUGE_PAGE_SIZE) {
        u64 const address = HHDM_BASE + offset;
        u64* pdpt = table_at(pml4[index_of(address, 4)]);

        u64& pdpt_entry = pdpt[index_of(address, 3)];
        if (!(pdpt_entry & static_cast<u64>(PageFlags::Present))) {
            auto const table = allocate_zeroed_page();
            if (table.is_error())
                panic("out of memory building the direct map at offset %p",
                    reinterpret_cast<void*>(offset));
            pdpt_entry = raw(table.value()) | static_cast<u64>(PageFlags::Present)
                | static_cast<u64>(PageFlags::Writable);
        }

        u64* pd = table_at(pdpt_entry);
        pd[index_of(address, 2)] = offset | hhdm_flags;
    }

    // The kernel image, one section at a time, so that .text is not writable
    // and .data is not executable.
    struct Section {
        u8* start;
        u8* end;
        PageFlags flags;
        char const* name;
    };

    Section const sections[] = {
        { __text_start, __text_end, PageFlags::Present | PageFlags::Global, ".text" },
        { __rodata_start, __rodata_end,
            PageFlags::Present | PageFlags::NoExecute | PageFlags::Global, ".rodata" },
        { __data_start, __bss_end,
            PageFlags::Present | PageFlags::Writable | PageFlags::NoExecute | PageFlags::Global,
            ".data+.bss" },
    };

    for (auto const& section : sections) {
        u64 const start = align_down<u64>(reinterpret_cast<u64>(section.start), PAGE_SIZE);
        u64 const end = align_up<u64>(reinterpret_cast<u64>(section.end), PAGE_SIZE);
        for (u64 address = start; address < end; address += PAGE_SIZE) {
            auto result = s_kernel_space->map(virt(address), phys(address - KERNEL_VMA), section.flags);
            if (result.is_error())
                panic("could not map kernel section %s at %p", section.name,
                    reinterpret_cast<void*>(address));
        }
    }

    // The .boot section is deliberately not carried over. From the moment the
    // next line executes, the low identity mapping is gone.
    s_kernel_space->activate();

    klog(LOG_INFO, "vmm", "kernel address space active: %llu MiB direct map, image mapped W^X",
        hhdm_size / (1024 * 1024));
}

} // namespace kernel::mm
