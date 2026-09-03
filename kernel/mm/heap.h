// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- the kernel heap.
//
// A segregated free-list allocator over pages taken from the physical
// allocator. Small requests come from per-size-class slabs; anything larger
// than half a page is served by whole pages. It is not a great allocator, but
// unlike gen 1's it can actually free memory.

#pragma once

#include <shitos/types.h>

namespace kernel {

void heap_initialize();

void* kmalloc(usize size);
void* kzalloc(usize size);
void* krealloc(void* ptr, usize size);
void kfree(void* ptr);

// Page-aligned allocation, for things the hardware cares about the alignment
// of (page tables, DMA buffers, task stacks).
void* kmalloc_aligned(usize size, usize alignment);
void kfree_aligned(void* ptr);

struct HeapStats {
    usize bytes_in_use;
    usize bytes_reserved;
    usize allocation_count;
    usize free_count;
};

HeapStats heap_stats();

} // namespace kernel
