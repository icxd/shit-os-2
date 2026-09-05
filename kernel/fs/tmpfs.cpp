// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- a writable filesystem that lives in the heap.

#include <kernel/fs/tmpfs.h>
#include <kernel/lib/kstd.h>
#include <kernel/lib/new.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/physical.h>

namespace kernel::fs {

TmpfsInode::~TmpfsInode()
{
    release_pages_from(0);
    // Give back the reference this directory held on each child rather than
    // destroying them: a child a process still has open outlives its parent.
    for (auto* child : m_children)
        child->unref();
}

namespace {

constexpr usize pages_for(u64 bytes)
{
    return static_cast<usize>((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
}

} // namespace

ErrorOr<void> TmpfsInode::ensure_pages(u64 wanted_bytes)
{
    usize const wanted = pages_for(wanted_bytes);
    while (m_pages.size() < wanted) {
        // Zeroed, not merely allocated: these pages go straight into a user
        // address space on the next mmap, and whatever was in them before
        // belonged to somebody else.
        auto page = mm::allocate_zeroed_page();
        if (page.is_error()) {
            // Give back what this call added, so a failed write does not leave
            // the file owning pages it does not account for in m_size.
            release_pages_from(pages_for(m_size));
            return page.error();
        }
        if (auto appended = m_pages.append(page.value()); appended.is_error()) {
            mm::free_page(page.value());
            release_pages_from(pages_for(m_size));
            return appended.error();
        }
    }
    return {};
}

void TmpfsInode::release_pages_from(usize first_index)
{
    while (m_pages.size() > first_index) {
        mm::free_page(m_pages.last());
        m_pages.remove_at(m_pages.size() - 1);
    }
}

ErrorOr<usize> TmpfsInode::read(u64 offset, void* buffer, usize length)
{
    if (m_type == InodeType::Directory)
        return Error::from_errno(EISDIR);
    if (offset >= m_size)
        return static_cast<usize>(0);

    usize const to_copy = min(length, static_cast<usize>(m_size - offset));
    auto* out = static_cast<u8*>(buffer);

    usize copied = 0;
    while (copied < to_copy) {
        u64 const position = offset + copied;
        usize const index = static_cast<usize>(position / PAGE_SIZE);
        usize const within = static_cast<usize>(position % PAGE_SIZE);
        usize const run = min(to_copy - copied, PAGE_SIZE - within);

        auto* page = static_cast<u8*>(phys_to_virt(m_pages[index]));
        memcpy(out + copied, page + within, run);
        copied += run;
    }
    return copied;
}

ErrorOr<usize> TmpfsInode::write(u64 offset, void const* buffer, usize length)
{
    if (m_type == InodeType::Directory)
        return Error::from_errno(EISDIR);
    if (length == 0)
        return static_cast<usize>(0);

    TRY(ensure_pages(offset + length));

    // A write past the end leaves a hole, which POSIX says reads as zeroes.
    // Nothing needs doing for it: the pages arrived zeroed.
    auto const* in = static_cast<u8 const*>(buffer);

    usize written = 0;
    while (written < length) {
        u64 const position = offset + written;
        usize const index = static_cast<usize>(position / PAGE_SIZE);
        usize const within = static_cast<usize>(position % PAGE_SIZE);
        usize const run = min(length - written, PAGE_SIZE - within);

        auto* page = static_cast<u8*>(phys_to_virt(m_pages[index]));
        memcpy(page + within, in + written, run);
        written += run;
    }

    m_size = max<u64>(m_size, offset + length);
    touch();
    return length;
}

ErrorOr<void> TmpfsInode::truncate(u64 size)
{
    if (m_type == InodeType::Directory)
        return Error::from_errno(EISDIR);

    if (size > m_size) {
        TRY(ensure_pages(size));
    } else if (size < m_size) {
        // Clear the tail of the last surviving page: shrinking and growing
        // again must not bring the old bytes back.
        usize const keep = pages_for(size);
        if (usize const within = static_cast<usize>(size % PAGE_SIZE); within != 0 && keep > 0) {
            auto* page = static_cast<u8*>(phys_to_virt(m_pages[keep - 1]));
            memset(page + within, 0, PAGE_SIZE - within);
        }
        release_pages_from(keep);
    }

    m_size = size;
    touch();
    return {};
}

/*
 * What makes a file here mappable. The page has to already exist -- POSIX says
 * a shared mapping is of a file you have sized first, and reading past the end
 * of one is the caller's mistake rather than something to paper over by
 * allocating.
 */
ErrorOr<PhysAddr> TmpfsInode::physical_page(u64 offset, bool for_write)
{
    if (m_type == InodeType::Directory)
        return Error::from_errno(EISDIR);

    usize const index = static_cast<usize>(offset / PAGE_SIZE);
    if (for_write)
        TRY(ensure_pages(offset + PAGE_SIZE));
    if (index >= m_pages.size())
        return Error::from_errno(ENXIO);

    return m_pages[index];
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
