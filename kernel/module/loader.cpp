// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- the loadable module loader.

#include <kernel/dev/console.h>
#include <kernel/fs/vfs.h>
#include <kernel/lib/elf.h>
#include <kernel/lib/new.h>
#include <kernel/lib/spinlock.h>
#include <kernel/lib/string.h>
#include <kernel/lib/vector.h>
#include <kernel/mm/address_space.h>
#include <kernel/mm/heap.h>
#include <kernel/module/loader.h>
#include <kernel/panic.h>

namespace kernel {

namespace {

using namespace elf;

IntrusiveList<LoadedModule, &LoadedModule::list_node> s_modules;
SpinLock s_modules_lock;

// The complete set of symbols a module may reference from outside itself.
//
// These are not kernel API. They are the freestanding C runtime that clang
// assumes exists: it lowers struct assignment and array initialisation to
// memcpy and memset calls no matter what the source says. Everything a module
// is actually allowed to *do* arrives through KernelApi.
struct IntrinsicSymbol {
    char const* name;
    void* address;
};

IntrinsicSymbol const INTRINSIC_SYMBOLS[] = {
    { "memcpy", reinterpret_cast<void*>(&memcpy) },
    { "memmove", reinterpret_cast<void*>(&memmove) },
    { "memset", reinterpret_cast<void*>(&memset) },
    { "memcmp", reinterpret_cast<void*>(&memcmp) },
    { "strlen", reinterpret_cast<void*>(&strlen) },
    { "strcmp", reinterpret_cast<void*>(&strcmp) },
    { "strncmp", reinterpret_cast<void*>(&strncmp) },
    { "strcpy", reinterpret_cast<void*>(&strcpy) },
    { "strncpy", reinterpret_cast<void*>(&strncpy) },
};

void* find_intrinsic(char const* name)
{
    for (auto const& symbol : INTRINSIC_SYMBOLS) {
        if (strcmp(symbol.name, name) == 0)
            return symbol.address;
    }
    return nullptr;
}

// Where each section ended up in the module window, indexed by section number.
struct SectionPlacement {
    u64 address; // 0 for sections that were not loaded
    usize size;
    bool executable;
    bool writable;
};

} // namespace

void ModuleLoader::initialize() { }

usize ModuleLoader::module_count()
{
    LockGuard guard(s_modules_lock);
    return s_modules.size();
}

void ModuleLoader::for_each(void (*callback)(LoadedModule const&, void*), void* context)
{
    LockGuard guard(s_modules_lock);
    for (LoadedModule* module : s_modules)
        callback(*module, context);
}

ErrorOr<LoadedModule*> ModuleLoader::load(char const* name, u8 const* image, usize length)
{
    if (length < sizeof(Elf64_Ehdr))
        return Error::from_errno(ENOEXEC);

    auto const& header = *reinterpret_cast<Elf64_Ehdr const*>(image);
    if (!is_valid_elf64(header)) {
        klog(LOG_ERROR, "module", "%s: not an x86-64 ELF64 object", name);
        return Error::from_errno(ENOEXEC);
    }
    if (header.e_type != ET_REL) {
        klog(LOG_ERROR, "module", "%s: expected a relocatable object, got type %u", name,
            header.e_type);
        return Error::from_errno(ENOEXEC);
    }

    auto const* sections = reinterpret_cast<Elf64_Shdr const*>(image + header.e_shoff);
    usize const section_count = header.e_shnum;

    // Lay the allocatable sections out contiguously, honouring each one's
    // alignment, so a single allocation covers the whole module.
    Vector<SectionPlacement> placements;
    TRY(placements.reserve(section_count));
    usize total_size = 0;

    for (usize i = 0; i < section_count; ++i) {
        auto const& section = sections[i];
        SectionPlacement placement { 0, 0, false, false };

        if ((section.sh_flags & SHF_ALLOC) != 0 && section.sh_size > 0) {
            bool const executable = (section.sh_flags & SHF_EXECINSTR) != 0;

            // Page permissions are page-granular, so an executable section has
            // to own whole pages. Packing .text and .data into one page means
            // dropping write permission on .text also drops it on .data, and
            // the module faults on its own first store.
            usize const alignment
                = executable ? PAGE_SIZE : (section.sh_addralign > 1 ? section.sh_addralign : 1);

            total_size = align_up<usize>(total_size, alignment);
            placement.address = total_size; // offset for now, rebased below
            placement.size = section.sh_size;
            placement.executable = executable;
            placement.writable = (section.sh_flags & SHF_WRITE) != 0;
            total_size += section.sh_size;

            if (executable)
                total_size = align_up<usize>(total_size, PAGE_SIZE);
        }

        TRY(placements.append(placement));
    }

    if (total_size == 0)
        return Error::from_errno(ENOEXEC);

    void* base = TRY(mm::allocate_module_memory(total_size));
    auto const base_address = reinterpret_cast<u64>(base);

    // Rebase the offsets and copy the contents in.
    for (usize i = 0; i < section_count; ++i) {
        auto& placement = placements[i];
        if (placement.size == 0)
            continue;

        placement.address += base_address;
        auto const& section = sections[i];

        if (section.sh_type == SHT_NOBITS)
            memset(reinterpret_cast<void*>(placement.address), 0, placement.size);
        else
            memcpy(reinterpret_cast<void*>(placement.address), image + section.sh_offset,
                placement.size);
    }

    // Resolve the symbol table once so relocation is a lookup rather than a
    // search, and so an unsatisfiable symbol is reported before anything runs.
    Elf64_Shdr const* symbol_section = nullptr;
    for (usize i = 0; i < section_count; ++i) {
        if (sections[i].sh_type == SHT_SYMTAB) {
            symbol_section = &sections[i];
            break;
        }
    }
    if (symbol_section == nullptr) {
        mm::free_module_memory(base, total_size);
        return Error::from_errno(ENOEXEC);
    }

    auto const* symbols = reinterpret_cast<Elf64_Sym const*>(image + symbol_section->sh_offset);
    usize const symbol_count = symbol_section->sh_size / sizeof(Elf64_Sym);
    auto const* symbol_strings
        = reinterpret_cast<char const*>(image + sections[symbol_section->sh_link].sh_offset);

    Vector<u64> symbol_addresses;
    TRY(symbol_addresses.reserve(symbol_count));

    bool unresolved = false;
    for (usize i = 0; i < symbol_count; ++i) {
        auto const& symbol = symbols[i];
        char const* symbol_name = symbol_strings + symbol.st_name;
        u64 address = 0;

        if (symbol.st_shndx == SHN_UNDEF) {
            if (symbol.st_name != 0) {
                void* intrinsic = find_intrinsic(symbol_name);
                if (intrinsic != nullptr) {
                    address = reinterpret_cast<u64>(intrinsic);
                } else {
                    klog(LOG_ERROR, "module", "%s: undefined symbol '%s'", name, symbol_name);
                    klog(LOG_ERROR, "module",
                        "  modules may only call through KernelApi; see docs/driver-abi.md");
                    unresolved = true;
                }
            }
        } else if (symbol.st_shndx == SHN_ABS) {
            address = symbol.st_value;
        } else if (symbol.st_shndx < section_count) {
            address = placements[symbol.st_shndx].address + symbol.st_value;
        }

        TRY(symbol_addresses.append(address));
    }

    if (unresolved) {
        mm::free_module_memory(base, total_size);
        return Error::from_errno(ENOEXEC);
    }

    // Apply relocations.
    for (usize i = 0; i < section_count; ++i) {
        auto const& section = sections[i];
        if (section.sh_type != SHT_RELA)
            continue;

        usize const target_index = section.sh_info;
        if (target_index >= section_count || placements[target_index].size == 0)
            continue;

        u64 const target_base = placements[target_index].address;
        auto const* relocations = reinterpret_cast<Elf64_Rela const*>(image + section.sh_offset);
        usize const relocation_count = section.sh_size / sizeof(Elf64_Rela);

        for (usize r = 0; r < relocation_count; ++r) {
            auto const& relocation = relocations[r];
            u32 const type = relocation_type(relocation.r_info);
            u32 const symbol_index = relocation_symbol(relocation.r_info);

            if (symbol_index >= symbol_count) {
                mm::free_module_memory(base, total_size);
                return Error::from_errno(ENOEXEC);
            }

            u64 const S = symbol_addresses[symbol_index];
            i64 const A = relocation.r_addend;
            u64 const P = target_base + relocation.r_offset;
            auto* where = reinterpret_cast<u8*>(P);

            switch (type) {
            case R_X86_64_NONE: break;

            case R_X86_64_64:
                *reinterpret_cast<u64*>(where) = static_cast<u64>(static_cast<i64>(S) + A);
                break;

            case R_X86_64_PC32:
            case R_X86_64_PLT32: {
                // Static linking has no PLT, so a PLT32 is just a PC-relative
                // call to the symbol itself.
                i64 const value = static_cast<i64>(S) + A - static_cast<i64>(P);
                if (value < -0x80000000LL || value > 0x7FFFFFFFLL) {
                    klog(LOG_ERROR, "module",
                        "%s: 32-bit displacement out of range (%p -> %p); is the module "
                        "window still within 2 GiB of the kernel?",
                        name, reinterpret_cast<void*>(P), reinterpret_cast<void*>(S));
                    mm::free_module_memory(base, total_size);
                    return Error::from_errno(ENOEXEC);
                }
                *reinterpret_cast<i32*>(where) = static_cast<i32>(value);
                break;
            }

            case R_X86_64_PC64:
                *reinterpret_cast<i64*>(where) = static_cast<i64>(S) + A - static_cast<i64>(P);
                break;

            case R_X86_64_32:
            case R_X86_64_32S: {
                i64 const value = static_cast<i64>(S) + A;
                if (type == R_X86_64_32S ? (value < -0x80000000LL || value > 0x7FFFFFFFLL)
                                         : (static_cast<u64>(value) > 0xFFFFFFFFULL)) {
                    klog(LOG_ERROR, "module", "%s: 32-bit absolute relocation out of range", name);
                    mm::free_module_memory(base, total_size);
                    return Error::from_errno(ENOEXEC);
                }
                *reinterpret_cast<u32*>(where) = static_cast<u32>(value);
                break;
            }

            default:
                klog(LOG_ERROR, "module", "%s: unsupported relocation type %u", name, type);
                mm::free_module_memory(base, total_size);
                return Error::from_errno(ENOEXEC);
            }
        }
    }

    // Find the one symbol the loader cares about.
    ModuleDescriptor const* descriptor = nullptr;
    for (usize i = 0; i < symbol_count; ++i) {
        char const* symbol_name = symbol_strings + symbols[i].st_name;
        if (strcmp(symbol_name, "shitos_module") == 0) {
            descriptor = reinterpret_cast<ModuleDescriptor const*>(symbol_addresses[i]);
            break;
        }
    }

    if (descriptor == nullptr) {
        klog(LOG_ERROR, "module", "%s: no shitos_module descriptor; did you use SHITOS_MODULE()?",
            name);
        mm::free_module_memory(base, total_size);
        return Error::from_errno(ENOEXEC);
    }

    // The table only ever grows at the end, so a newer kernel can serve an
    // older module: every entry that module knows about is still in place.
    // The other direction would have it call through a pointer past the end of
    // the struct, which is what the number is here to prevent.
    if (descriptor->abi_version < SHITOS_MODULE_ABI_MIN_VERSION
        || descriptor->abi_version > SHITOS_MODULE_ABI_VERSION) {
        klog(LOG_ERROR, "module", "%s: built against ABI v%u, kernel speaks v%u..v%u", name,
            descriptor->abi_version, SHITOS_MODULE_ABI_MIN_VERSION, SHITOS_MODULE_ABI_VERSION);
        mm::free_module_memory(base, total_size);
        return Error::from_errno(EABIVER);
    }

    // Relocations are done, so the executable sections can stop being writable.
    for (usize i = 0; i < section_count; ++i) {
        auto const& placement = placements[i];
        if (placement.size == 0 || !placement.executable)
            continue;

        // Read-execute: no write bit, no NX bit. The section starts on a page
        // boundary and owns every page it touches, so nothing else loses write
        // permission along with it.
        auto const flags = mm::PageFlags::Present | mm::PageFlags::Global;
        auto result = mm::AddressSpace::kernel_space().protect(
            virt(placement.address), align_up<usize>(placement.size, PAGE_SIZE), flags);
        if (result.is_error())
            klog(LOG_WARN, "module", "%s: could not drop write permission on .text", name);
    }

    auto* module = static_cast<LoadedModule*>(kzalloc(sizeof(LoadedModule)));
    if (module == nullptr) {
        mm::free_module_memory(base, total_size);
        return Error::from_errno(ENOMEM);
    }
    new (module) LoadedModule();

    strncpy(
        module->m_name, descriptor->name != nullptr ? descriptor->name : name, MODULE_NAME_MAX - 1);
    module->m_abi_version = descriptor->abi_version;
    module->m_base = base;
    module->m_size = total_size;
    module->m_descriptor = descriptor;

    if (descriptor->init != nullptr) {
        ModuleResult const result = descriptor->init(&kernel_api());
        if (result != MODULE_OK) {
            klog(LOG_ERROR, "module", "%s: init failed (%d)", module->m_name, result);
            mm::free_module_memory(base, total_size);
            kfree(module);
            return Error::from_errno(EIO);
        }
    }

    {
        LockGuard guard(s_modules_lock);
        s_modules.append(module);
    }

    klog(LOG_INFO, "module", "loaded %s v%u at %p (%zu bytes) -- %s", module->m_name,
        module->m_abi_version, base, total_size,
        descriptor->description != nullptr ? descriptor->description : "");

    return module;
}

ErrorOr<usize> ModuleLoader::load_all_from(char const* directory)
{
    auto resolved = fs::resolve(directory);
    if (resolved.is_error()) {
        klog(LOG_INFO, "module", "%s does not exist; no modules to load", directory);
        return static_cast<usize>(0);
    }

    auto* inode = resolved.value();
    if (!inode->is_directory())
        return Error::from_errno(ENOTDIR);

    usize loaded = 0;
    for (usize index = 0;; ++index) {
        fs::DirectoryEntry entry;
        auto have_entry = inode->read_directory(index, entry);
        if (have_entry.is_error() || !have_entry.value())
            break;

        usize const name_length = strlen(entry.name);
        if (name_length < 4 || strcmp(entry.name + name_length - 3, ".ko") != 0)
            continue;

        auto child = inode->lookup(entry.name);
        if (child.is_error())
            continue;

        usize const size = static_cast<usize>(child.value()->size());
        auto* image = static_cast<u8*>(kmalloc(size));
        if (image == nullptr) {
            klog(LOG_ERROR, "module", "out of memory reading %s", entry.name);
            continue;
        }

        auto read = child.value()->read(0, image, size);
        if (read.is_error() || read.value() != size) {
            kfree(image);
            continue;
        }

        auto result = load(entry.name, image, size);
        // The image was copied into the module window during load, so the
        // staging buffer is not needed either way.
        kfree(image);

        if (!result.is_error())
            ++loaded;
    }

    return loaded;
}

ErrorOr<void> ModuleLoader::unload(char const* name)
{
    LoadedModule* target = nullptr;
    {
        LockGuard guard(s_modules_lock);
        for (LoadedModule* module : s_modules) {
            if (strcmp(module->m_name, name) == 0) {
                target = module;
                s_modules.remove(module);
                break;
            }
        }
    }

    if (target == nullptr)
        return Error::from_errno(ENOENT);

    if (target->m_descriptor != nullptr && target->m_descriptor->fini != nullptr)
        target->m_descriptor->fini();

    mm::free_module_memory(target->m_base, target->m_size);
    kfree(target);

    klog(LOG_INFO, "module", "unloaded %s", name);
    return {};
}

} // namespace kernel
