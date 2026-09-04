// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- a writable filesystem that lives in the heap.
//
// Mounted on /tmp, and the only place anything can be written until a disk
// driver exists. File contents grow geometrically in the kernel heap, so a
// tmpfs file costs roughly what it holds.

#pragma once

#include <kernel/fs/vfs.h>
#include <kernel/lib/vector.h>

namespace kernel::fs {

class TmpfsInode final : public Inode {
public:
    TmpfsInode(FileSystem* filesystem, InodeType type, u32 mode)
        : Inode(filesystem, type, mode)
    {
    }

    ~TmpfsInode() override;

    ErrorOr<usize> read(u64 offset, void* buffer, usize length) override;
    ErrorOr<usize> write(u64 offset, void const* buffer, usize length) override;
    ErrorOr<void> truncate(u64 size) override;

    ErrorOr<Inode*> lookup(char const* name) override;
    ErrorOr<bool> read_directory(usize index, DirectoryEntry& out) override;
    ErrorOr<Inode*> create(char const* name, InodeType type, u32 mode) override;
    ErrorOr<void> unlink(char const* name) override;

private:
    friend class TmpfsFileSystem;

    ErrorOr<void> ensure_capacity(usize wanted);

    char m_name[FILENAME_MAX_LENGTH] {};
    u8* m_data { nullptr };
    usize m_capacity { 0 };
    Vector<TmpfsInode*> m_children;
};

class TmpfsFileSystem final : public FileSystem {
public:
    static ErrorOr<TmpfsFileSystem*> create();

    char const* name() const override { return "tmpfs"; }
    Inode& root() override { return *m_root; }

    u64 allocate_inode_number() { return m_next_inode_number++; }

private:
    TmpfsFileSystem() = default;

    TmpfsInode* m_root { nullptr };
    u64 m_next_inode_number { 1 };
};

} // namespace kernel::fs
