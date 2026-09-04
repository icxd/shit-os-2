// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- loading a userland program.
//
// Static ET_EXEC only, for now. The auxiliary vector is filled in properly
// even though nothing reads it yet, because getting AT_PHDR and friends right
// now is what lets a dynamic linker be added later without the kernel
// changing at all.

#pragma once

#include <kernel/fs/vfs.h>
#include <kernel/lib/error.h>
#include <kernel/mm/address_space.h>

#include <shitos/types.h>

namespace kernel::sys {

struct LoadedExecutable {
    u64 entry { 0 };
    u64 phdr_address { 0 }; // AT_PHDR: where the program headers landed
    u16 phdr_count { 0 };
    u16 phdr_entry_size { 0 };
    u64 brk_start { 0 }; // first page past the last PT_LOAD
};

// Maps the program's segments into `space`, which must not be the active one.
ErrorOr<LoadedExecutable> load_executable(fs::Inode& inode, mm::AddressSpace& space);

// Maps a stack and lays out argc/argv/envp/auxv on it. Returns the rsp the
// program should start with.
ErrorOr<u64> setup_user_stack(mm::AddressSpace& space, LoadedExecutable const& executable,
    char const* const* argv, char const* const* envp);

// Copies into an address space that is not currently active, by translating
// each page and going through the direct map.
ErrorOr<void> copy_into_space(
    mm::AddressSpace& space, u64 destination, void const* source, usize length);
ErrorOr<void> copy_out_of_space(
    mm::AddressSpace& space, void* destination, u64 source, usize length);

} // namespace kernel::sys
