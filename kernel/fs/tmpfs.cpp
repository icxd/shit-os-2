// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- a writable filesystem that lives in the heap.

#include <kernel/fs/tmpfs.h>
#include <kernel/lib/new.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>

namespace kernel::fs {

TmpfsInode::~TmpfsInode()
{
    kfree(m_data);
    // Give back the reference this directory held on each child rather than
    // destroying them: a child a process still has open outlives its parent.
    for (auto* child : m_children)
        child->unref();
}

ErrorOr<void> TmpfsInode::ensure_capacity(usize wanted)
{
    if (wanted <= m_capacity)
        return {};

    // Double until it fits, so appending a byte at a time to a large file does
    // not turn into a quadratic copy.
    usize new_capacity = m_capacity == 0 ? 64 : m_capacity;
    while (new_capacity < wanted)
        new_capacity *= 2;

    auto* replacement = static_cast<u8*>(kmalloc(new_capacity));
    if (replacement == nullptr)
        return Error::from_errno(ENOMEM);

    if (m_data != nullptr) {
        memcpy(replacement, m_data, static_cast<usize>(m_size));
        kfree(m_data);
    }
    // Zero the tail so a write past the end followed by a read of the gap
    // returns zeroes rather than whatever the heap had.
    memset(replacement + m_size, 0, new_capacity - static_cast<usize>(m_size));

    m_data = replacement;
    m_capacity = new_capacity;
    return {};
}

ErrorOr<usize> TmpfsInode::read(u64 offset, void* buffer, usize length)
{
    if (m_type == InodeType::Directory)
        return Error::from_errno(EISDIR);
    if (offset >= m_size)
        return static_cast<usize>(0);

    usize const to_copy = min(length, static_cast<usize>(m_size - offset));
    memcpy(buffer, m_data + offset, to_copy);
    return to_copy;
}

ErrorOr<usize> TmpfsInode::write(u64 offset, void const* buffer, usize length)
{
    if (m_type == InodeType::Directory)
        return Error::from_errno(EISDIR);
    if (length == 0)
        return static_cast<usize>(0);

    TRY(ensure_capacity(static_cast<usize>(offset) + length));

    // Writing past the end leaves a hole, which POSIX says reads as zeroes.
    if (offset > m_size)
        memset(m_data + m_size, 0, static_cast<usize>(offset - m_size));

    memcpy(m_data + offset, buffer, length);
    m_size = max<u64>(m_size, offset + length);
    touch();
    return length;
}

ErrorOr<void> TmpfsInode::truncate(u64 size)
{
    if (m_type == InodeType::Directory)
        return Error::from_errno(EISDIR);

    if (size > m_size) {
        TRY(ensure_capacity(static_cast<usize>(size)));
        memset(m_data + m_size, 0, static_cast<usize>(size - m_size));
    }
    m_size = size;
    touch();
    return {};
}

ErrorOr<Inode*> TmpfsInode::lookup(char const* name)
{
    if (m_type != InodeType::Directory)
        return Error::from_errno(ENOTDIR);

    for (auto* child : m_children) {
        if (strcmp(child->m_name, name) == 0)
            return static_cast<Inode*>(child);
    }
    return Error::from_errno(ENOENT);
}

ErrorOr<bool> TmpfsInode::read_directory(usize index, DirectoryEntry& out)
{
    if (m_type != InodeType::Directory)
        return Error::from_errno(ENOTDIR);

    if (index == 0) {
        strcpy(out.name, ".");
        out.inode_number = m_inode_number;
        out.type = InodeType::Directory;
        return true;
    }
    if (index == 1) {
        strcpy(out.name, "..");
        out.inode_number = m_parent != nullptr ? m_parent->inode_number() : m_inode_number;
        out.type = InodeType::Directory;
        return true;
    }

    usize const child_index = index - 2;
    if (child_index >= m_children.size())
        return false;

    auto* child = m_children[child_index];
    strncpy(out.name, child->m_name, FILENAME_MAX_LENGTH - 1);
    out.name[FILENAME_MAX_LENGTH - 1] = '\0';
    out.inode_number = child->inode_number();
    out.type = child->type();
    return true;
}

ErrorOr<Inode*> TmpfsInode::create(char const* name, InodeType type, u32 mode)
{
    if (m_type != InodeType::Directory)
        return Error::from_errno(ENOTDIR);
    if (strlen(name) >= FILENAME_MAX_LENGTH)
        return Error::from_errno(ENAMETOOLONG);

    for (auto* child : m_children) {
        if (strcmp(child->m_name, name) == 0)
            return Error::from_errno(EEXIST);
    }

    auto* child = static_cast<TmpfsInode*>(kzalloc(sizeof(TmpfsInode)));
    if (child == nullptr)
        return Error::from_errno(ENOMEM);
    new (child) TmpfsInode(m_filesystem, type, mode);

    strncpy(child->m_name, name, FILENAME_MAX_LENGTH - 1);
    child->m_parent = this;
    child->m_inode_number = static_cast<TmpfsFileSystem*>(m_filesystem)->allocate_inode_number();
    child->touch();
    // Adding an entry changes this directory too.
    touch();

    if (auto result = m_children.append(child); result.is_error()) {
        child->unref();
        return result.error();
    }

    return static_cast<Inode*>(child);
}

ErrorOr<void> TmpfsInode::unlink(char const* name)
{
    if (m_type != InodeType::Directory)
        return Error::from_errno(ENOTDIR);

    for (usize i = 0; i < m_children.size(); ++i) {
        auto* child = m_children[i];
        if (strcmp(child->m_name, name) != 0)
            continue;

        // Refusing to remove a non-empty directory is what rmdir promises;
        // unlink on a directory is refused outright.
        if (child->is_directory() && !child->m_children.is_empty())
            return Error::from_errno(ENOTEMPTY);

        // Remove the name and drop the reference that went with it. If the
        // file is still open somewhere it stays alive, unreachable by name,
        // until the last descriptor closes.
        m_children.remove_at(i);
        child->mark_unlinked();
        child->unref();
        return {};
    }

    return Error::from_errno(ENOENT);
}

bool TmpfsInode::remove_child(Vector<TmpfsInode*>& children, TmpfsInode* child)
{
    for (usize i = 0; i < children.size(); ++i) {
        if (children[i] == child) {
            children.remove_at(i);
            return true;
        }
    }
    return false;
}

TmpfsInode* TmpfsInode::find_child(Vector<TmpfsInode*>& children, char const* name)
{
    for (auto* child : children) {
        if (strcmp(child->m_name, name) == 0)
            return child;
    }
    return nullptr;
}

ErrorOr<void> TmpfsInode::rename(char const* name, Inode& new_parent, char const* new_name)
{
    if (m_type != InodeType::Directory || !new_parent.is_directory())
        return Error::from_errno(ENOTDIR);
    if (strlen(new_name) >= FILENAME_MAX_LENGTH)
        return Error::from_errno(ENAMETOOLONG);

    // fs::rename already established both sides are on this filesystem, so the
    // cast is the same one lookup and create make.
    auto& destination = static_cast<TmpfsInode&>(new_parent);

    auto* source = find_child(m_children, name);
    if (source == nullptr)
        return Error::from_errno(ENOENT);

    // Renaming a name onto itself is a no-op that must not destroy anything.
    if (&destination == this && strcmp(name, new_name) == 0)
        return {};

    // A destination that already exists is replaced, but only by something
    // compatible: POSIX will not let a file overwrite a directory or the
    // reverse, and will not replace a directory that still has entries.
    auto* replaced = find_child(destination.m_children, new_name);
    if (replaced == source)
        return {};
    if (replaced != nullptr) {
        if (replaced->is_directory() != source->is_directory())
            return Error::from_errno(source->is_directory() ? ENOTDIR : EISDIR);
        if (replaced->is_directory() && !replaced->m_children.is_empty())
            return Error::from_errno(ENOTEMPTY);
    }

    // Everything that can fail has to fail before anything moves, or a half
    // done rename loses the file. Reserving the destination slot is the only
    // allocation left. It is unnecessary in the cases that free a slot first,
    // and asking for it anyway costs one pointer and no reasoning.
    TRY(destination.m_children.reserve(destination.m_children.size() + 1));

    if (replaced != nullptr)
        (void)remove_child(destination.m_children, replaced);
    (void)remove_child(m_children, source);

    strncpy(source->m_name, new_name, FILENAME_MAX_LENGTH - 1);
    source->m_name[FILENAME_MAX_LENGTH - 1] = '\0';
    source->m_parent = &destination;

    // Cannot fail: the capacity was reserved above, and two entries just left.
    (void)destination.m_children.append(source);

    touch();
    destination.touch();

    // Only now, once the move cannot fail, does the replaced file lose its
    // last name.
    if (replaced != nullptr) {
        replaced->mark_unlinked();
        replaced->unref();
    }

    return {};
}

ErrorOr<TmpfsFileSystem*> TmpfsFileSystem::create()
{
    auto* filesystem = static_cast<TmpfsFileSystem*>(kzalloc(sizeof(TmpfsFileSystem)));
    if (filesystem == nullptr)
        return Error::from_errno(ENOMEM);
    new (filesystem) TmpfsFileSystem();

    auto* root = static_cast<TmpfsInode*>(kzalloc(sizeof(TmpfsInode)));
    if (root == nullptr) {
        kfree(filesystem);
        return Error::from_errno(ENOMEM);
    }
    new (root) TmpfsInode(filesystem, InodeType::Directory, 0777);
    root->m_inode_number = filesystem->allocate_inode_number();
    strcpy(root->m_name, "/");

    filesystem->m_root = root;
    return filesystem;
}

} // namespace kernel::fs
