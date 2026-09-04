// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- the initial ramdisk, as a tar file.
//
// GRUB loads the initrd as a multiboot2 module and we mount it read-only at
// root. Using plain ustar means the build needs no bespoke tooling and a
// broken image can be inspected with `tar -tvf` like anything else.
//
// The archive is parsed once at mount time into a tree of inodes. File
// contents are not copied: a regular file's inode points straight into the
// module GRUB loaded, so mounting a 10 MiB initrd costs a tree of inodes and
// nothing more.

#pragma once

#include <kernel/fs/vfs.h>
#include <kernel/lib/vector.h>

namespace kernel::fs {

class UstarInode final : public Inode {
public:
    UstarInode(FileSystem* filesystem, InodeType type, u32 mode)
        : Inode(filesystem, type, mode)
    {
    }

    ErrorOr<usize> read(u64 offset, void* buffer, usize length) override;
    ErrorOr<Inode*> lookup(char const* name) override;
    ErrorOr<bool> read_directory(usize index, DirectoryEntry& out) override;

    char const* entry_name() const { return m_name; }

private:
    friend class UstarFileSystem;

    char m_name[FILENAME_MAX_LENGTH] {};
    u8 const* m_data { nullptr }; // into the initrd; never owned
    Vector<UstarInode*> m_children;
};

class UstarFileSystem final : public FileSystem {
public:
    // `data` must stay valid for the life of the mount, which for the initrd
    // means the pages must be reserved in the physical allocator.
    static ErrorOr<UstarFileSystem*> create(u8 const* data, usize length);

    char const* name() const override { return "ustar"; }
    Inode& root() override { return *m_root; }
    bool is_read_only() const override { return true; }

    usize file_count() const { return m_file_count; }
    usize total_bytes() const { return m_total_bytes; }

private:
    UstarFileSystem() = default;

    ErrorOr<void> parse(u8 const* data, usize length);
    ErrorOr<UstarInode*> ensure_path(char const* path, InodeType type, u32 mode);

    UstarInode* m_root { nullptr };
    usize m_file_count { 0 };
    usize m_total_bytes { 0 };
    u64 m_next_inode_number { 1 };
};

} // namespace kernel::fs
