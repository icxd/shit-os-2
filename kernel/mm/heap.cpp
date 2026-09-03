// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- the kernel heap. Stage B replaces this with the real thing.

#include <kernel/mm/heap.h>
#include <kernel/panic.h>

namespace kernel {

void heap_initialize() { }

void* kmalloc(usize) { panic("kmalloc before the heap exists"); }
void* kzalloc(usize) { panic("kzalloc before the heap exists"); }
void* krealloc(void*, usize) { panic("krealloc before the heap exists"); }
void kfree(void*) { panic("kfree before the heap exists"); }
void* kmalloc_aligned(usize, usize) { panic("kmalloc_aligned before the heap exists"); }
void kfree_aligned(void*) { panic("kfree_aligned before the heap exists"); }
HeapStats heap_stats() { return {}; }

} // namespace kernel
