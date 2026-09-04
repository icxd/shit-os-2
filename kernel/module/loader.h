// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- the loadable module loader.
//
// A .ko is an ordinary ELF64 relocatable object. Loading one means placing its
// allocatable sections in the module window, applying relocations, and calling
// the init function named by its single exported descriptor.
//
// What the loader deliberately does *not* do is resolve kernel symbols. The
// only symbols it will satisfy from outside the object are the handful of
// compiler intrinsics clang emits whether you use them or not (memcpy and
// friends). Everything else a module wants comes through the KernelApi table
// it is handed at init, which is what keeps the kernel free to refactor.

#pragma once

#include <kernel/lib/error.h>
#include <kernel/lib/intrusive_list.h>

#include <shitos/module/api.h>
#include <shitos/types.h>

namespace kernel {

inline constexpr usize MODULE_NAME_MAX = 32;

class LoadedModule {
public:
    char const* name() const { return m_name; }
    u32 abi_version() const { return m_abi_version; }
    void* base() const { return m_base; }
    usize size() const { return m_size; }
    ModuleDescriptor const* descriptor() const { return m_descriptor; }

    ListNode<LoadedModule> list_node;

private:
    friend class ModuleLoader;

    char m_name[MODULE_NAME_MAX] {};
    u32 m_abi_version { 0 };
    void* m_base { nullptr };
    usize m_size { 0 };
    ModuleDescriptor const* m_descriptor { nullptr };
};

class ModuleLoader {
public:
    static void initialize();

    // Loads and initialises one module from an in-memory .ko image.
    static ErrorOr<LoadedModule*> load(char const* name, u8 const* image, usize length);

    // Loads everything in /lib/modules. Called once during boot.
    static ErrorOr<usize> load_all_from(char const* directory);

    static ErrorOr<void> unload(char const* name);

    static usize module_count();
    static void for_each(void (*callback)(LoadedModule const&, void*), void* context);

    // The one table every module is handed. Exposed so the self tests can
    // check it is fully populated.
    static KernelApi const& kernel_api();
};

} // namespace kernel
