// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- the kernel heap.
//
// Small allocations come from per-size-class slabs carved out of single pages;
// anything bigger gets whole pages of its own. Both carry a 16-byte header, so
// kfree does not need to be told how big the allocation was and a corrupted
// header is caught rather than acted on.
//
// Gen 1's heap was a bump pointer whose free() moved the pointer backwards by
// an amount unrelated to the allocation. This one can actually free memory.

#include <kernel/dev/console.h>
#include <kernel/lib/spinlock.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/physical.h>
#include <kernel/panic.h>

namespace kernel {

namespace {

constexpr u32 HEADER_MAGIC = 0x5348'4954; // "SHIT"
constexpr u16 CLASS_LARGE = 0xFFFF;

// Total chunk sizes, header included. Payload is 16 bytes less than each.
constexpr usize SIZE_CLASSES[] = { 32, 64, 128, 256, 512, 1024, 2048 };
constexpr usize CLASS_COUNT = sizeof(SIZE_CLASSES) / sizeof(SIZE_CLASSES[0]);
constexpr usize MAX_SMALL_PAYLOAD = 2048 - 16;

struct AllocationHeader {
    u32 magic;
    u16 size_class;
    u16 flags;
    u64 payload_size;
};
static_assert(sizeof(AllocationHeader) == 16, "the header must keep payloads 16-byte aligned");

// Free chunks are threaded through their own payload space, so an empty free
// list costs nothing beyond the head pointer.
struct FreeChunk {
    FreeChunk* next;
};

FreeChunk* s_free_lists[CLASS_COUNT];
InterruptSpinLock s_lock;

usize s_bytes_in_use = 0;
usize s_bytes_reserved = 0;
usize s_allocation_count = 0;
usize s_free_count = 0;
bool s_initialized = false;

// Page-aligned allocations cannot carry a header in front of the payload
// without giving up the alignment, so their sizes are recorded here instead.
constexpr usize ALIGNED_BUCKETS = 64;

struct AlignedAllocation {
    void* address;
    usize pages;
    AlignedAllocation* next;
};

AlignedAllocation* s_aligned_buckets[ALIGNED_BUCKETS];

usize bucket_of(void* address)
{
    // The low twelve bits are always zero for these, so hash the page number.
    return (reinterpret_cast<u64>(address) >> 12) % ALIGNED_BUCKETS;
}

usize class_for(usize payload_size)
{
    for (usize i = 0; i < CLASS_COUNT; ++i) {
        if (payload_size + sizeof(AllocationHeader) <= SIZE_CLASSES[i])
            return i;
    }
    return CLASS_LARGE;
}

// Carves one fresh page into chunks of the given class and pushes them all
// onto that class's free list.
bool refill_class(usize class_index)
{
    auto const page = mm::allocate_page();
    if (page.is_error())
        return false;

    auto* base = static_cast<u8*>(phys_to_virt(page.value()));
    usize const chunk_size = SIZE_CLASSES[class_index];
    usize const chunk_count = PAGE_SIZE / chunk_size;

    for (usize i = 0; i < chunk_count; ++i) {
        auto* chunk = reinterpret_cast<FreeChunk*>(base + i * chunk_size);
        chunk->next = s_free_lists[class_index];
        s_free_lists[class_index] = chunk;
    }

    s_bytes_reserved += PAGE_SIZE;
    return true;
}

AllocationHeader* header_of(void* payload)
{
    return reinterpret_cast<AllocationHeader*>(
        static_cast<u8*>(payload) - sizeof(AllocationHeader));
}

} // namespace

void heap_initialize()
{
    memset(s_free_lists, 0, sizeof(s_free_lists));
    memset(s_aligned_buckets, 0, sizeof(s_aligned_buckets));
    s_bytes_in_use = 0;
    s_bytes_reserved = 0;
    s_allocation_count = 0;
    s_free_count = 0;
    s_initialized = true;

    klog(LOG_INFO, "heap", "ready: %zu size classes, %zu..%zu byte payloads", CLASS_COUNT,
        SIZE_CLASSES[0] - sizeof(AllocationHeader), MAX_SMALL_PAYLOAD);
}

void* kmalloc(usize size)
{
    if (size == 0)
        return nullptr;
    if (!s_initialized)
        panic("kmalloc(%zu) before heap_initialize()", size);

    InterruptLockGuard guard(s_lock);

    usize const class_index = class_for(size);

    if (class_index == CLASS_LARGE) {
        usize const pages = div_round_up<usize>(size + sizeof(AllocationHeader), PAGE_SIZE);
        auto const base = mm::allocate_contiguous(pages);
        if (base.is_error())
            return nullptr;

        auto* header = static_cast<AllocationHeader*>(phys_to_virt(base.value()));
        *header = { HEADER_MAGIC, CLASS_LARGE, 0, size };
        s_bytes_in_use += size;
        s_bytes_reserved += pages * PAGE_SIZE;
        ++s_allocation_count;
        return header + 1;
    }

    if (s_free_lists[class_index] == nullptr && !refill_class(class_index))
        return nullptr;

    auto* chunk = s_free_lists[class_index];
    s_free_lists[class_index] = chunk->next;

    auto* header = reinterpret_cast<AllocationHeader*>(chunk);
    *header = { HEADER_MAGIC, static_cast<u16>(class_index), 0, size };
    s_bytes_in_use += size;
    ++s_allocation_count;
    return header + 1;
}

void* kzalloc(usize size)
{
    void* pointer = kmalloc(size);
    if (pointer != nullptr)
        memset(pointer, 0, size);
    return pointer;
}

void kfree(void* pointer)
{
    if (pointer == nullptr)
        return;

    InterruptLockGuard guard(s_lock);

    auto* header = header_of(pointer);
    if (header->magic != HEADER_MAGIC) {
        // Either a double free, a pointer that never came from kmalloc, or a
        // buffer overrun in the allocation just below this one. All three are
        // worth stopping for.
        panic("kfree(%p): bad header magic %p (double free or heap corruption)", pointer,
            reinterpret_cast<void*>(static_cast<u64>(header->magic)));
    }

    usize const payload_size = header->payload_size;
    u16 const size_class = header->size_class;
    header->magic = 0; // so an immediate second free is caught

    s_bytes_in_use -= payload_size;
    ++s_free_count;

    if (size_class == CLASS_LARGE) {
        usize const pages = div_round_up<usize>(payload_size + sizeof(AllocationHeader), PAGE_SIZE);
        s_bytes_reserved -= pages * PAGE_SIZE;
        mm::free_contiguous(virt_to_phys(header), pages);
        return;
    }

    if (size_class >= CLASS_COUNT)
        panic("kfree(%p): impossible size class %u", pointer, size_class);

    // The page the chunk came from is not returned to the physical allocator.
    // Slab pages are recycled through the free list instead, which keeps the
    // common path cheap; reclaiming empty slabs is on the roadmap.
    auto* chunk = reinterpret_cast<FreeChunk*>(header);
    chunk->next = s_free_lists[size_class];
    s_free_lists[size_class] = chunk;
}

void* krealloc(void* pointer, usize size)
{
    if (pointer == nullptr)
        return kmalloc(size);
    if (size == 0) {
        kfree(pointer);
        return nullptr;
    }

    auto const* header = header_of(pointer);
    if (header->magic != HEADER_MAGIC)
        panic("krealloc(%p): bad header magic", pointer);

    usize const old_size = header->payload_size;
    if (size <= old_size)
        return pointer;

    void* replacement = kmalloc(size);
    if (replacement == nullptr)
        return nullptr;

    memcpy(replacement, pointer, old_size);
    kfree(pointer);
    return replacement;
}

void* kmalloc_aligned(usize size, usize alignment)
{
    if (size == 0)
        return nullptr;

    // Anything up to the header size is already guaranteed by kmalloc.
    if (alignment <= sizeof(AllocationHeader))
        return kmalloc(size);
    if (alignment > PAGE_SIZE)
        panic("kmalloc_aligned: alignment %zu exceeds a page", alignment);

    usize const pages = div_round_up<usize>(size, PAGE_SIZE);
    auto const base = mm::allocate_contiguous(pages);
    if (base.is_error())
        return nullptr;

    void* address = phys_to_virt(base.value());

    auto* record = static_cast<AlignedAllocation*>(kmalloc(sizeof(AlignedAllocation)));
    if (record == nullptr) {
        mm::free_contiguous(base.value(), pages);
        return nullptr;
    }

    InterruptLockGuard guard(s_lock);
    usize const bucket = bucket_of(address);
    *record = { address, pages, s_aligned_buckets[bucket] };
    s_aligned_buckets[bucket] = record;
    s_bytes_in_use += pages * PAGE_SIZE;
    s_bytes_reserved += pages * PAGE_SIZE;
    return address;
}

void kfree_aligned(void* pointer)
{
    if (pointer == nullptr)
        return;

    AlignedAllocation* record = nullptr;
    {
        InterruptLockGuard guard(s_lock);
        usize const bucket = bucket_of(pointer);
        AlignedAllocation** link = &s_aligned_buckets[bucket];
        while (*link != nullptr && (*link)->address != pointer)
            link = &(*link)->next;
        if (*link == nullptr)
            panic("kfree_aligned(%p): not an aligned allocation", pointer);
        record = *link;
        *link = record->next;
        s_bytes_in_use -= record->pages * PAGE_SIZE;
        s_bytes_reserved -= record->pages * PAGE_SIZE;
    }

    mm::free_contiguous(virt_to_phys(pointer), record->pages);
    kfree(record);
}

HeapStats heap_stats()
{
    InterruptLockGuard guard(s_lock);
    return { s_bytes_in_use, s_bytes_reserved, s_allocation_count, s_free_count };
}

} // namespace kernel
