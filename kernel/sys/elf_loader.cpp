// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- loading a userland program.

#include <kernel/dev/console.h>
#include <kernel/lib/elf.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/physical.h>
#include <kernel/sched/process.h>
#include <kernel/sys/elf_loader.h>

namespace kernel::sys {

using namespace elf;

namespace {

mm::PageFlags flags_for_segment(Elf64_Word p_flags)
{
    auto flags = mm::PageFlags::Present | mm::PageFlags::User;
    if ((p_flags & PF_W) != 0)
        flags = flags | mm::PageFlags::Writable;
    if ((p_flags & PF_X) == 0)
        flags = flags | mm::PageFlags::NoExecute;
    return flags;
}

} // namespace

ErrorOr<void> copy_into_space(
    mm::AddressSpace& space, u64 destination, void const* source, usize length)
{
    auto const* bytes = static_cast<u8 const*>(source);
    usize copied = 0;

    // The target space is not on CR3, so its pages are only reachable by
    // translating each one and going the long way round through the direct map.
    while (copied < length) {
        u64 const address = destination + copied;
        u64 const page_offset = address & (PAGE_SIZE - 1);
        usize const chunk = min<usize>(PAGE_SIZE - page_offset, length - copied);

        auto frame = TRY(space.translate(virt(address)));
        memcpy(phys_to_virt(frame), bytes + copied, chunk);
        copied += chunk;
    }

    return {};
}

ErrorOr<void> copy_out_of_space(
    mm::AddressSpace& space, void* destination, u64 source, usize length)
{
    auto* bytes = static_cast<u8*>(destination);
    usize copied = 0;

    while (copied < length) {
        u64 const address = source + copied;
        u64 const page_offset = address & (PAGE_SIZE - 1);
        usize const chunk = min<usize>(PAGE_SIZE - page_offset, length - copied);

        auto frame = TRY(space.translate(virt(address)));
        memcpy(bytes + copied, phys_to_virt(frame), chunk);
        copied += chunk;
    }

    return {};
}

ErrorOr<LoadedExecutable> load_executable(fs::Inode& inode, mm::AddressSpace& space)
{
    Elf64_Ehdr header;
    usize const header_bytes = TRY(inode.read(0, &header, sizeof(header)));
    if (header_bytes != sizeof(header))
        return Error::from_errno(ENOEXEC);

    if (!is_valid_elf64(header))
        return Error::from_errno(ENOEXEC);
    if (header.e_type != ET_EXEC) {
        // ET_DYN would need a dynamic linker, which is the next step rather
        // than something to fake here.
        return Error::from_errno(ENOEXEC);
    }
    if (header.e_phnum == 0 || header.e_phentsize != sizeof(Elf64_Phdr))
        return Error::from_errno(ENOEXEC);

    usize const program_headers_size = static_cast<usize>(header.e_phnum) * header.e_phentsize;
    auto* program_headers = static_cast<Elf64_Phdr*>(kmalloc(program_headers_size));
    if (program_headers == nullptr)
        return Error::from_errno(ENOMEM);

    usize const read_bytes = TRY(inode.read(header.e_phoff, program_headers, program_headers_size));
    if (read_bytes != program_headers_size) {
        kfree(program_headers);
        return Error::from_errno(ENOEXEC);
    }

    LoadedExecutable result;
    result.entry = header.e_entry;
    result.phdr_count = header.e_phnum;
    result.phdr_entry_size = header.e_phentsize;

    u64 highest_address = 0;

    for (u16 i = 0; i < header.e_phnum; ++i) {
        auto const& segment = program_headers[i];
        if (segment.p_type != PT_LOAD || segment.p_memsz == 0)
            continue;

        if (segment.p_vaddr >= mm::USER_SPACE_END
            || segment.p_vaddr + segment.p_memsz > mm::USER_SPACE_END) {
            kfree(program_headers);
            return Error::from_errno(ENOEXEC);
        }

        u64 const start = align_down<u64>(segment.p_vaddr, PAGE_SIZE);
        u64 const end = align_up<u64>(segment.p_vaddr + segment.p_memsz, PAGE_SIZE);

        // Map writable first so the contents can be copied in, then tighten
        // the permissions once the segment is populated.
        auto const staging_flags = mm::PageFlags::Present | mm::PageFlags::Writable
            | mm::PageFlags::User | mm::PageFlags::NoExecute;
        if (auto mapped = space.map_anonymous(virt(start), end - start, staging_flags);
            mapped.is_error()) {
            kfree(program_headers);
            return mapped.error();
        }

        if (segment.p_filesz > 0) {
            auto* staging = static_cast<u8*>(kmalloc(static_cast<usize>(segment.p_filesz)));
            if (staging == nullptr) {
                kfree(program_headers);
                return Error::from_errno(ENOMEM);
            }

            auto file_bytes
                = inode.read(segment.p_offset, staging, static_cast<usize>(segment.p_filesz));
            if (file_bytes.is_error() || file_bytes.value() != segment.p_filesz) {
                kfree(staging);
                kfree(program_headers);
                return Error::from_errno(ENOEXEC);
            }

            auto copied = copy_into_space(
                space, segment.p_vaddr, staging, static_cast<usize>(segment.p_filesz));
            kfree(staging);
            if (copied.is_error()) {
                kfree(program_headers);
                return copied.error();
            }
        }

        // map_anonymous hands back zeroed pages, so the .bss tail is already
        // zero and there is nothing more to do for p_memsz > p_filesz.

        if (auto protected_result
            = space.protect(virt(start), end - start, flags_for_segment(segment.p_flags));
            protected_result.is_error()) {
            kfree(program_headers);
            return protected_result.error();
        }

        highest_address = max(highest_address, end);

        // If the program headers happen to be inside a mapped segment, the
        // auxiliary vector can point at them where they already are.
        if (header.e_phoff >= segment.p_offset
            && header.e_phoff + program_headers_size <= segment.p_offset + segment.p_filesz) {
            result.phdr_address = segment.p_vaddr + (header.e_phoff - segment.p_offset);
        }
    }

    kfree(program_headers);

    if (highest_address == 0)
        return Error::from_errno(ENOEXEC);

    result.brk_start = highest_address;
    return result;
}

ErrorOr<u64> setup_user_stack(mm::AddressSpace& space, LoadedExecutable const& executable,
    char const* const* argv, char const* const* envp)
{
    u64 const stack_bottom = USER_STACK_TOP - USER_STACK_SIZE;
    auto const flags = mm::PageFlags::Present | mm::PageFlags::Writable | mm::PageFlags::User
        | mm::PageFlags::NoExecute;
    TRY(space.map_anonymous(virt(stack_bottom), USER_STACK_SIZE, flags));

    usize argument_count = 0;
    usize environment_count = 0;
    usize string_bytes = 0;

    for (; argv != nullptr && argv[argument_count] != nullptr; ++argument_count) {
        if (argument_count >= MAX_ARGUMENTS)
            return Error::from_errno(E2BIG);
        string_bytes += strlen(argv[argument_count]) + 1;
    }
    for (; envp != nullptr && envp[environment_count] != nullptr; ++environment_count) {
        if (environment_count >= MAX_ARGUMENTS)
            return Error::from_errno(E2BIG);
        string_bytes += strlen(envp[environment_count]) + 1;
    }
    if (string_bytes > MAX_ARGUMENT_BYTES)
        return Error::from_errno(E2BIG);

    // The auxiliary vector. A static binary ignores most of this, but getting
    // it right now is what lets a dynamic linker slot in later without the
    // kernel changing.
    struct AuxiliaryEntry {
        u64 type;
        u64 value;
    };
    AuxiliaryEntry auxiliary[] = {
        { AT_PHDR, executable.phdr_address },
        { AT_PHENT, executable.phdr_entry_size },
        { AT_PHNUM, executable.phdr_count },
        { AT_PAGESZ, PAGE_SIZE },
        { AT_ENTRY, executable.entry },
        { AT_BASE, 0 },
        { AT_FLAGS, 0 },
        { AT_UID, 0 },
        { AT_EUID, 0 },
        { AT_GID, 0 },
        { AT_EGID, 0 },
        { AT_SECURE, 0 },
        { AT_NULL, 0 },
    };
    usize const auxiliary_count = sizeof(auxiliary) / sizeof(auxiliary[0]);

    usize const pointer_bytes = (argument_count + 1 + environment_count + 1) * sizeof(u64);
    usize const auxiliary_bytes = auxiliary_count * sizeof(AuxiliaryEntry);
    usize const string_area = align_up<usize>(string_bytes, 16);

    // Lay the whole thing out in a kernel buffer first, then copy it across in
    // one pass; poking a not-currently-mapped address space a word at a time
    // would be needlessly slow and much easier to get wrong.
    usize const total
        = align_up<usize>(sizeof(u64) + pointer_bytes + auxiliary_bytes + string_area, 16);
    if (total > USER_STACK_SIZE / 2)
        return Error::from_errno(E2BIG);

    auto* image = static_cast<u8*>(kzalloc(total));
    if (image == nullptr)
        return Error::from_errno(ENOMEM);

    u64 const rsp = USER_STACK_TOP - total;

    // Strings live at the very top; everything else points into them.
    usize string_cursor = total - string_area;
    auto write_string = [&](char const* text) -> u64 {
        usize const length = strlen(text) + 1;
        memcpy(image + string_cursor, text, length);
        u64 const address = rsp + string_cursor;
        string_cursor += length;
        return address;
    };

    auto* argc_slot = reinterpret_cast<u64*>(image);
    *argc_slot = argument_count;

    auto* argv_slots = reinterpret_cast<u64*>(image + sizeof(u64));
    for (usize i = 0; i < argument_count; ++i)
        argv_slots[i] = write_string(argv[i]);
    argv_slots[argument_count] = 0;

    auto* envp_slots = argv_slots + argument_count + 1;
    for (usize i = 0; i < environment_count; ++i)
        envp_slots[i] = write_string(envp[i]);
    envp_slots[environment_count] = 0;

    auto* auxiliary_slots = reinterpret_cast<AuxiliaryEntry*>(envp_slots + environment_count + 1);
    for (usize i = 0; i < auxiliary_count; ++i)
        auxiliary_slots[i] = auxiliary[i];

    auto copied = copy_into_space(space, rsp, image, total);
    kfree(image);
    TRY(copied);

    // The SysV ABI wants rsp 16-byte aligned at the entry point, with argc
    // sitting exactly there.
    return rsp;
}

} // namespace kernel::sys
