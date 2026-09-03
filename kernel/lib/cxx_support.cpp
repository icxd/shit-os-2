// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- the bits of the C++ runtime a freestanding kernel still needs.
//
// We compile with -fno-exceptions and -fno-rtti, which removes most of libsupc++,
// but the compiler still emits references to a few symbols. Providing them here
// (rather than linking a runtime) keeps the kernel self-contained and makes it
// obvious when something has pulled in a feature we do not support.

#include <kernel/mm/heap.h>
#include <kernel/panic.h>
#include <shitos/types.h>

extern "C" {

// A call through a vtable slot that was never overridden. Always a bug.
void __cxa_pure_virtual()
{
    ::kernel::panic("pure virtual function called");
}

// -fno-use-cxa-atexit means we never register destructors for globals, but the
// symbol is still referenced by the ABI in some configurations.
void* __dso_handle = nullptr;

int __cxa_atexit(void (*)(void*), void*, void*)
{
    // The kernel never exits, so a destructor registered here would never run.
    return 0;
}

void __stack_chk_fail()
{
    ::kernel::panic("stack smashing detected");
}
}

// Global operator new/delete.
//
// A virtual destructor makes the compiler emit a deleting destructor, which
// references operator delete even when nothing ever deletes the object. These
// forward to the kernel heap; before the heap exists, using them is a bug and
// says so rather than corrupting memory quietly.

void* operator new(usize size) { return ::kernel::kmalloc(size); }
void* operator new[](usize size) { return ::kernel::kmalloc(size); }
void operator delete(void* ptr) noexcept { ::kernel::kfree(ptr); }
void operator delete[](void* ptr) noexcept { ::kernel::kfree(ptr); }
void operator delete(void* ptr, usize) noexcept { ::kernel::kfree(ptr); }
void operator delete[](void* ptr, usize) noexcept { ::kernel::kfree(ptr); }

// Placement new, which the kernel uses to construct objects in memory it has
// already allocated (slabs, per-CPU blocks, page-aligned buffers).
void* operator new(usize, void* where) noexcept { return where; }
void* operator new[](usize, void* where) noexcept { return where; }

// The kernel's global constructors, run once before kernel_main.
extern "C" {
extern void (*__init_array_start[])();
extern void (*__init_array_end[])();
}

namespace kernel {

void run_global_constructors()
{
    for (auto** entry = __init_array_start; entry != __init_array_end; ++entry)
        (*entry)();
}

} // namespace kernel
