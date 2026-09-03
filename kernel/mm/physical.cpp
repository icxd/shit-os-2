// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- the physical page allocator.

#include <kernel/dev/console.h>
#include <kernel/lib/spinlock.h>
#include <kernel/lib/string.h>
#include <kernel/mm/physical.h>
#include <kernel/panic.h>

namespace kernel::mm {

namespace {

u64* s_bitmap = nullptr; // one bit per page; set means in use
usize s_bitmap_words = 0;
usize s_total_pages = 0;
usize s_used_pages = 0;

// Where the last successful search finished. Starting from here rather than
// zero turns the common allocate-many-pages-in-a-row case from quadratic into
// something closer to linear.
usize s_search_hint = 0;

InterruptSpinLock s_lock;

constexpr usize BITS_PER_WORD = 64;

bool test_bit(usize index) { return (s_bitmap[index / BITS_PER_WORD] >> (index % BITS_PER_WORD)) & 1; }

void set_bit(usize index) { s_bitmap[index / BITS_PER_WORD] |= 1ULL << (index % BITS_PER_WORD); }

void clear_bit(usize index) { s_bitmap[index / BITS_PER_WORD] &= ~(1ULL << (index % BITS_PER_WORD)); }

void mark_used(usize first, usize count)
{
    for (usize i = first; i < first + count && i < s_total_pages; ++i) {
        if (!test_bit(i)) {
            set_bit(i);
            ++s_used_pages;
        }
    }
}

void mark_free(usize first, usize count)
{
    for (usize i = first; i < first + count && i < s_total_pages; ++i) {
        if (test_bit(i)) {
            clear_bit(i);
            --s_used_pages;
        }
    }
}

// Marks every page overlapping [base, base+length) with the given operation,
// rounding outward so a partially covered page is never treated as free.
void apply_to_range(u64 base, u64 length, void (*operation)(usize, usize))
{
    if (length == 0)
        return;
    u64 const first = align_down<u64>(base, PAGE_SIZE) / PAGE_SIZE;
    u64 const last = align_up<u64>(base + length, PAGE_SIZE) / PAGE_SIZE;
    if (first >= s_total_pages)
        return;
    operation(first, static_cast<usize>(min<u64>(last, s_total_pages) - first));
}

// Only RAM the firmware called usable is a candidate for the bitmap itself,
// and it must not land on top of the kernel or a boot module.
bool range_is_clear(boot::BootInfo const& info, u64 base, u64 length)
{
    u64 const end = base + length;
    for (usize i = 0; i < info.memory_region_count; ++i) {
        auto const& region = info.memory_regions[i];
        if (region.kind != boot::MemoryKind::KernelImage && region.kind != boot::MemoryKind::BootModule)
            continue;
        u64 const region_end = region.base + region.length;
        if (base < region_end && region.base < end)
            return false;
    }
    // Never place it in the first megabyte; that space is full of firmware
    // structures nobody reports and everybody assumes.
    return base >= 0x100000;
}

} // namespace

void physical_initialize(boot::BootInfo const& info)
{
    // Size the bitmap from the top of usable RAM, not from the top of the
    // address space: the PCI hole above it is not memory we will ever hand out.
    u64 highest_usable = 0;
    for (usize i = 0; i < info.memory_region_count; ++i) {
        auto const& region = info.memory_regions[i];
        if (region.kind == boot::MemoryKind::Usable)
            highest_usable = max(highest_usable, region.base + region.length);
    }

    s_total_pages = static_cast<usize>(align_up<u64>(highest_usable, PAGE_SIZE) / PAGE_SIZE);
    s_bitmap_words = div_round_up<usize>(s_total_pages, BITS_PER_WORD);
    usize const bitmap_bytes = s_bitmap_words * sizeof(u64);

    // Find somewhere to put the bitmap. It has to come out of memory we are
    // about to start managing, which is the usual chicken-and-egg.
    u64 bitmap_phys = 0;
    for (usize i = 0; i < info.memory_region_count; ++i) {
        auto const& region = info.memory_regions[i];
        if (region.kind != boot::MemoryKind::Usable || region.length < bitmap_bytes)
            continue;

        u64 candidate = align_up<u64>(max<u64>(region.base, 0x100000), PAGE_SIZE);
        // Step past anything already spoken for inside this region.
        while (candidate + bitmap_bytes <= region.base + region.length) {
            if (range_is_clear(info, candidate, bitmap_bytes)) {
                bitmap_phys = candidate;
                break;
            }
            candidate += PAGE_SIZE;
        }
        if (bitmap_phys != 0)
            break;
    }

    if (bitmap_phys == 0)
        panic("no room for a %zu KiB page bitmap in %llu MiB of RAM", bitmap_bytes / 1024,
            info.total_usable_bytes / (1024 * 1024));

    s_bitmap = static_cast<u64*>(phys_to_virt(phys(bitmap_phys)));

    // Start from "everything is taken" and hand back only what the firmware
    // explicitly called usable. Being wrong in this direction merely wastes
    // memory; being wrong the other way corrupts it.
    memset(s_bitmap, 0xFF, bitmap_bytes);
    s_used_pages = s_total_pages;

    for (usize i = 0; i < info.memory_region_count; ++i) {
        auto const& region = info.memory_regions[i];
        if (region.kind == boot::MemoryKind::Usable)
            apply_to_range(region.base, region.length, mark_free);
    }

    // Now carve back out everything that is inside a usable region but not
    // actually available: the kernel, the modules, and the bitmap itself.
    for (usize i = 0; i < info.memory_region_count; ++i) {
        auto const& region = info.memory_regions[i];
        if (region.kind == boot::MemoryKind::KernelImage || region.kind == boot::MemoryKind::BootModule)
            apply_to_range(region.base, region.length, mark_used);
    }
    apply_to_range(bitmap_phys, bitmap_bytes, mark_used);

    // Page zero stays reserved so that a null pointer dereference faults
    // instead of quietly reading whatever happens to live at address zero.
    apply_to_range(0, PAGE_SIZE, mark_used);

    s_search_hint = 0;

    klog(LOG_INFO, "pmm", "%zu pages (%llu MiB), %zu free, bitmap %zu KiB at %p", s_total_pages,
        static_cast<u64>(s_total_pages) * PAGE_SIZE / (1024 * 1024), s_total_pages - s_used_pages,
        bitmap_bytes / 1024, reinterpret_cast<void*>(bitmap_phys));
}

ErrorOr<PhysAddr> allocate_page()
{
    InterruptLockGuard guard(s_lock);

    for (usize attempt = 0; attempt < 2; ++attempt) {
        usize const start = attempt == 0 ? s_search_hint : 0;
        usize const limit = attempt == 0 ? s_total_pages : s_search_hint;

        for (usize word = start / BITS_PER_WORD; word < s_bitmap_words; ++word) {
            if (s_bitmap[word] == ~0ULL)
                continue;

            // __builtin_ctzll on the inverted word finds the first zero bit,
            // which is the first free page in this 64-page span.
            usize const bit = static_cast<usize>(__builtin_ctzll(~s_bitmap[word]));
            usize const index = word * BITS_PER_WORD + bit;
            if (index >= s_total_pages || (attempt == 1 && index >= limit))
                break;

            set_bit(index);
            ++s_used_pages;
            s_search_hint = index + 1;
            return phys(static_cast<u64>(index) * PAGE_SIZE);
        }
    }

    return Error::from_errno(ENOMEM);
}

ErrorOr<PhysAddr> allocate_zeroed_page()
{
    auto page = TRY(allocate_page());
    memset(phys_to_virt(page), 0, PAGE_SIZE);
    return page;
}

ErrorOr<PhysAddr> allocate_contiguous(usize count)
{
    if (count == 0)
        return Error::from_errno(EINVAL);
    if (count == 1)
        return allocate_page();

    InterruptLockGuard guard(s_lock);

    usize run_start = 0;
    usize run_length = 0;
    for (usize index = 0; index < s_total_pages; ++index) {
        if (test_bit(index)) {
            run_length = 0;
            continue;
        }
        if (run_length == 0)
            run_start = index;
        if (++run_length == count) {
            mark_used(run_start, count);
            return phys(static_cast<u64>(run_start) * PAGE_SIZE);
        }
    }

    return Error::from_errno(ENOMEM);
}

void free_page(PhysAddr page) { free_contiguous(page, 1); }

void free_contiguous(PhysAddr base, usize count)
{
    usize const first = static_cast<usize>(raw(base) / PAGE_SIZE);
    if (first >= s_total_pages)
        return;

    InterruptLockGuard guard(s_lock);
    for (usize i = first; i < first + count && i < s_total_pages; ++i) {
        if (!test_bit(i)) {
            panic("double free of physical page %p", reinterpret_cast<void*>(i * PAGE_SIZE));
        }
        clear_bit(i);
        --s_used_pages;
    }
    s_search_hint = min(s_search_hint, first);
}

void reserve_range(PhysAddr base, usize length)
{
    InterruptLockGuard guard(s_lock);
    apply_to_range(raw(base), length, mark_used);
}

usize total_pages() { return s_total_pages; }
usize free_pages() { return s_total_pages - s_used_pages; }
usize used_pages() { return s_used_pages; }

} // namespace kernel::mm
