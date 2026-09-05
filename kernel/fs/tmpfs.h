// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- a writable filesystem that lives in the heap.
//
// Mounted on /tmp, and the only place anything can be written until a disk
// driver exists.
//
// File contents are a list of whole physical pages rather than one growing
// heap buffer. That costs a little padding on small files and buys two things:
// a file is no longer limited by the largest contiguous allocation the heap
// can find, and -- the reason it changed -- a page never moves once it has
// been handed out, so a process can map one and keep it. MAP_SHARED over a
// file here is what POSIX shared memory actually is, and it is how the window
// server passes pixels to its clients.

#pragma once

#include <kernel/fs/vfs.h>
#include <kernel/lib/vector.h>
#include <kernel/mm/physical.h>

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
    ErrorOr<PhysAddr> physical_page(u64 offset, bool for_write) override;

    ErrorOr<Inode*> lookup(char const* name) override;
    ErrorOr<bool> read_directory(usize index, DirectoryEntry& out) override;
    ErrorOr<Inode*> create(char const* name, InodeType type, u32 mode) override;
    ErrorOr<void> unlink(char const* name) override;
    ErrorOr<void> rename(char const* name, Inode& new_parent, char const* new_name) override;

private:
    friend class TmpfsFileSystem;

    // Both round to whole pages: a file holding one byte owns one page.
    ErrorOr<void> ensure_pages(u64 wanted_bytes);
    void release_pages_from(usize first_index);

    // Renaming inside one directory mutates the child vector twice, and an
    // index found before the first mutation does not survive it -- so both of
    // these work by identity or by name, never by a remembered index.
    static bool remove_child(Vector<TmpfsInode*>& children, TmpfsInode* child);
    static TmpfsInode* find_child(Vector<TmpfsInode*>& children, char const* name);

    char m_name[FILENAME_MAX_LENGTH] {};
    Vector<PhysAddr> m_pages;
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
