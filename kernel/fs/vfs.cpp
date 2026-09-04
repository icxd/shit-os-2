// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- the virtual filesystem.

#include <kernel/dev/console.h>
#include <kernel/fs/vfs.h>
#include <kernel/lib/new.h>
#include <kernel/lib/spinlock.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>
#include <kernel/panic.h>

namespace kernel::fs {

namespace {

constexpr usize MAX_MOUNTS = 8;

struct MountEntry {
    Inode* covered; // the directory this filesystem is mounted over
    FileSystem* filesystem;
    char path[FILENAME_MAX_LENGTH];
};

MountEntry s_mounts[MAX_MOUNTS];
usize s_mount_count = 0;
FileSystem* s_root_filesystem = nullptr;
SpinLock s_mount_lock;

// If `inode` has a filesystem mounted over it, the caller wants that
// filesystem's root instead.
Inode* follow_mount(Inode* inode)
{
    for (usize i = 0; i < s_mount_count; ++i) {
        if (s_mounts[i].covered == inode)
            return &s_mounts[i].filesystem->root();
    }
    return inode;
}

// Copies the next '/'-delimited component out of `path`, advancing it past
// the separator. Returns false when there is nothing left.
bool next_component(char const*& path, char (&out)[FILENAME_MAX_LENGTH])
{
    while (*path == '/')
        ++path;
    if (*path == '\0')
        return false;

    usize length = 0;
    while (*path != '\0' && *path != '/') {
        if (length + 1 < FILENAME_MAX_LENGTH)
            out[length++] = *path;
        ++path;
    }
    out[length] = '\0';
    return true;
}

} // namespace

u32 mode_bits_for(InodeType type)
{
    switch (type) {
    case InodeType::Regular: return S_IFREG;
    case InodeType::Directory: return S_IFDIR;
    case InodeType::CharacterDevice: return S_IFCHR;
    case InodeType::BlockDevice: return S_IFBLK;
    case InodeType::Fifo: return S_IFIFO;
    case InodeType::SymbolicLink: return S_IFLNK;
    }
    return 0;
}

u8 dirent_type_for(InodeType type)
{
    switch (type) {
    case InodeType::Regular: return DT_REG;
    case InodeType::Directory: return DT_DIR;
    case InodeType::CharacterDevice: return DT_CHR;
    case InodeType::BlockDevice: return DT_BLK;
    case InodeType::Fifo: return DT_FIFO;
    case InodeType::SymbolicLink: return DT_LNK;
    }
    return DT_UNKNOWN;
}

// --- Inode defaults -----------------------------------------------------
//
// Everything fails by default with the errno POSIX specifies for that
// operation on that kind of object, so a filesystem only implements what it
// genuinely supports and unsupported operations are correct rather than absent.

ErrorOr<usize> Inode::read(u64, void*, usize)
{
    return Error::from_errno(EINVAL);
}
ErrorOr<usize> Inode::write(u64, void const*, usize)
{
    return Error::from_errno(EROFS);
}
ErrorOr<void> Inode::truncate(u64)
{
    return Error::from_errno(EROFS);
}
ErrorOr<Inode*> Inode::lookup(char const*)
{
    return Error::from_errno(ENOTDIR);
}
ErrorOr<bool> Inode::read_directory(usize, DirectoryEntry&)
{
    return Error::from_errno(ENOTDIR);
}
ErrorOr<Inode*> Inode::create(char const*, InodeType, u32)
{
    return Error::from_errno(EROFS);
}
ErrorOr<void> Inode::unlink(char const*)
{
    return Error::from_errno(EROFS);
}
ErrorOr<int> Inode::ioctl(u32, void*)
{
    return Error::from_errno(ENOTTY);
}

void Inode::destroy()
{
    // The destructor is virtual, so this dispatches to the most derived one
    // before the memory goes back to the heap.
    this->~Inode();
    kfree(this);
}

void Inode::unref()
{
    u32 const remaining = __atomic_sub_fetch(&m_reference_count, 1, __ATOMIC_ACQ_REL);
    if (remaining == 0)
        destroy();
}

ErrorOr<void> Inode::stat(struct stat& out) const
{
    memset(&out, 0, sizeof(out));
    out.st_ino = m_inode_number;
    out.st_mode = mode_bits_for(m_type) | (m_mode & 07777);
    out.st_nlink = 1;
    out.st_size = static_cast<i64>(size());
    out.st_blksize = static_cast<i64>(PAGE_SIZE);
    out.st_blocks = static_cast<i64>(div_round_up<u64>(size(), 512));
    return {};
}

// --- FileDescription ----------------------------------------------------

ErrorOr<usize> FileDescription::read(void* buffer, usize length)
{
    if (!is_readable())
        return Error::from_errno(EBADF);
    if (m_inode->is_directory())
        return Error::from_errno(EISDIR);

    usize const bytes = TRY(m_inode->read(m_offset, buffer, length));
    m_offset += bytes;
    return bytes;
}

ErrorOr<usize> FileDescription::write(void const* buffer, usize length)
{
    if (!is_writable())
        return Error::from_errno(EBADF);
    if (m_inode->is_directory())
        return Error::from_errno(EISDIR);

    // O_APPEND means every write goes to the current end, not to wherever
    // this description's offset happens to be.
    if ((m_flags & O_APPEND) != 0)
        m_offset = m_inode->size();

    usize const bytes = TRY(m_inode->write(m_offset, buffer, length));
    m_offset += bytes;
    return bytes;
}

ErrorOr<u64> FileDescription::seek(i64 offset, int whence)
{
    i64 base = 0;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = static_cast<i64>(m_offset); break;
    case SEEK_END: base = static_cast<i64>(m_inode->size()); break;
    default: return Error::from_errno(EINVAL);
    }

    i64 const target = base + offset;
    if (target < 0)
        return Error::from_errno(EINVAL);

    m_offset = static_cast<u64>(target);
    // Seeking a directory description restarts enumeration, which is what
    // rewinddir() relies on.
    if (m_inode->is_directory() && target == 0)
        m_directory_index = 0;
    return m_offset;
}

ErrorOr<usize> FileDescription::get_directory_entries(void* buffer, usize capacity)
{
    if (!m_inode->is_directory())
        return Error::from_errno(ENOTDIR);

    auto* out = static_cast<u8*>(buffer);
    usize written = 0;

    for (;;) {
        DirectoryEntry entry;
        bool const have_entry = TRY(m_inode->read_directory(m_directory_index, entry));
        if (!have_entry)
            break;

        usize const name_length = strlen(entry.name);
        // Keep every record 8-aligned so the caller can walk the buffer by
        // stepping d_reclen without ever landing on a misaligned struct.
        usize const record_length = align_up<usize>(sizeof(struct dirent) + name_length + 1, 8);

        if (written + record_length > capacity) {
            // Not even one record fits: the caller's buffer is unusably small
            // and looping would spin forever.
            if (written == 0)
                return Error::from_errno(EINVAL);
            break;
        }

        auto* record = reinterpret_cast<struct dirent*>(out + written);
        record->d_ino = entry.inode_number;
        record->d_reclen = static_cast<u16>(record_length);
        record->d_type = dirent_type_for(entry.type);
        memcpy(record->d_name, entry.name, name_length + 1);

        written += record_length;
        ++m_directory_index;
    }

    return written;
}

// --- mounting -----------------------------------------------------------

void initialize()
{
    s_mount_count = 0;
    s_root_filesystem = nullptr;
}

ErrorOr<void> mount_root(FileSystem* filesystem)
{
    LockGuard guard(s_mount_lock);
    if (s_root_filesystem != nullptr)
        return Error::from_errno(EBUSY);

    s_root_filesystem = filesystem;
    s_mounts[s_mount_count++] = { nullptr, filesystem, "/" };

    klog(LOG_INFO, "vfs", "mounted %s on / (%s)", filesystem->name(),
        filesystem->is_read_only() ? "ro" : "rw");
    return {};
}

ErrorOr<void> mount(char const* path, FileSystem* filesystem)
{
    if (s_root_filesystem == nullptr)
        return Error::from_errno(ENOENT);

    auto* mount_point = TRY(resolve(path));
    if (!mount_point->is_directory())
        return Error::from_errno(ENOTDIR);

    LockGuard guard(s_mount_lock);
    if (s_mount_count >= MAX_MOUNTS)
        return Error::from_errno(ENOSPC);

    for (usize i = 0; i < s_mount_count; ++i) {
        if (s_mounts[i].covered == mount_point)
            return Error::from_errno(EBUSY);
    }

    // The mount table outlives whatever named the directory, so it holds a
    // reference of its own. There is no umount yet; when there is, it drops
    // this one.
    mount_point->ref();

    auto& entry = s_mounts[s_mount_count++];
    entry.covered = mount_point;
    entry.filesystem = filesystem;
    strncpy(entry.path, path, FILENAME_MAX_LENGTH - 1);
    entry.path[FILENAME_MAX_LENGTH - 1] = '\0';

    klog(LOG_INFO, "vfs", "mounted %s on %s (%s)", filesystem->name(), path,
        filesystem->is_read_only() ? "ro" : "rw");
    return {};
}

Inode* root_inode()
{
    if (s_root_filesystem == nullptr)
        return nullptr;
    return &s_root_filesystem->root();
}

usize mount_count()
{
    return s_mount_count;
}

MountInfo const& mount_at(usize index)
{
    static MountInfo info;
    auto const& entry = s_mounts[index < s_mount_count ? index : 0];
    strncpy(info.path, entry.path, FILENAME_MAX_LENGTH - 1);
    info.path[FILENAME_MAX_LENGTH - 1] = '\0';
    info.filesystem = entry.filesystem;
    return info;
}

// --- path resolution ----------------------------------------------------

ErrorOr<Inode*> resolve(char const* path, Inode* base)
{
    if (path == nullptr)
        return Error::from_errno(EINVAL);
    if (s_root_filesystem == nullptr)
        return Error::from_errno(ENOENT);

    Inode* current = (path[0] == '/' || base == nullptr) ? root_inode() : base;
    current = follow_mount(current);

    char component[FILENAME_MAX_LENGTH];
    char const* cursor = path;

    while (next_component(cursor, component)) {
        if (strcmp(component, ".") == 0)
            continue;

        if (strcmp(component, "..") == 0) {
            // Walking out of a mounted filesystem's root should step back over
            // the mount point. Doing that properly needs the mount stack; for
            // now the root of a mount is its own parent, as with chroot.
            Inode* parent = current->parent();
            current = parent != nullptr ? parent : current;
            continue;
        }

        if (!current->is_directory())
            return Error::from_errno(ENOTDIR);

        auto* next = TRY(current->lookup(component));
        current = follow_mount(next);
    }

    return current;
}

ErrorOr<Inode*> resolve_parent(
    char const* path, Inode* base, char (&final_component)[FILENAME_MAX_LENGTH])
{
    if (path == nullptr)
        return Error::from_errno(EINVAL);

    // Find the last component without copying the whole path around.
    char const* last_slash = nullptr;
    for (char const* p = path; *p != '\0'; ++p) {
        if (*p == '/' && *(p + 1) != '\0')
            last_slash = p;
    }

    if (last_slash == nullptr) {
        strncpy(final_component, path, FILENAME_MAX_LENGTH - 1);
        final_component[FILENAME_MAX_LENGTH - 1] = '\0';
        Inode* parent = (path[0] == '/') ? root_inode() : (base != nullptr ? base : root_inode());
        return follow_mount(parent);
    }

    char directory[PATH_MAX_LENGTH];
    usize const directory_length
        = min<usize>(static_cast<usize>(last_slash - path), PATH_MAX_LENGTH - 1);
    memcpy(directory, path, directory_length);
    directory[directory_length] = '\0';

    char const* name = last_slash + 1;
    strncpy(final_component, name, FILENAME_MAX_LENGTH - 1);
    final_component[FILENAME_MAX_LENGTH - 1] = '\0';

    // A path like "/foo" has an empty directory part, which means the root.
    if (directory_length == 0)
        return follow_mount(root_inode());

    return resolve(directory, base);
}

ErrorOr<FileDescription*> open(char const* path, int flags, u32 mode, Inode* base)
{
    Inode* inode = nullptr;
    bool const wants_write = (flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC)) != 0;

    auto resolved = resolve(path, base);
    if (resolved.is_error()) {
        if (resolved.error().code() != ENOENT || (flags & O_CREAT) == 0)
            return resolved.error();

        char name[FILENAME_MAX_LENGTH];
        auto* parent = TRY(resolve_parent(path, base, name));
        if (parent->filesystem() != nullptr && parent->filesystem()->is_read_only())
            return Error::from_errno(EROFS);
        inode = TRY(parent->create(name, InodeType::Regular, mode));
    } else {
        inode = resolved.value();
        if ((flags & O_CREAT) != 0 && (flags & O_EXCL) != 0)
            return Error::from_errno(EEXIST);
    }

    // POSIX wants EROFS from open(), not from the first write. Checking here
    // also means O_CREAT and O_TRUNC on a read-only mount fail up front.
    if (wants_write && inode->filesystem() != nullptr && inode->filesystem()->is_read_only())
        return Error::from_errno(EROFS);

    if ((flags & O_DIRECTORY) != 0 && !inode->is_directory())
        return Error::from_errno(ENOTDIR);
    if (inode->is_directory() && (flags & O_ACCMODE) != O_RDONLY)
        return Error::from_errno(EISDIR);

    if ((flags & O_TRUNC) != 0 && (flags & O_ACCMODE) != O_RDONLY)
        TRY(inode->truncate(0));

    auto* description = static_cast<FileDescription*>(kmalloc(sizeof(FileDescription)));
    if (description == nullptr)
        return Error::from_errno(ENOMEM);
    new (description) FileDescription(*inode, flags);

    if ((flags & O_APPEND) != 0)
        (void)description->seek(0, SEEK_END);

    return description;
}

void release_description(FileDescription* description)
{
    if (description == nullptr)
        return;
    if (!description->unref())
        return;

    // Last holder. Tell the inode the end is closing before letting go of it,
    // because a pipe decides whether it has reached EOF from exactly that.
    Inode& inode = description->inode();
    int const flags = description->flags();

    description->~FileDescription();
    kfree(description);

    inode.on_description_closed(flags);
    inode.unref();
}

ErrorOr<usize> absolute_path_of(Inode& inode, char* buffer, usize capacity)
{
    // Build the path backwards from the leaf, since parent pointers are the
    // only direction available, then reverse it in place.
    char scratch[PATH_MAX_LENGTH];
    usize position = PATH_MAX_LENGTH;
    scratch[--position] = '\0';

    Inode* current = &inode;
    Inode* root = root_inode();

    if (current == root) {
        if (capacity < 2)
            return Error::from_errno(ERANGE);
        buffer[0] = '/';
        buffer[1] = '\0';
        return 1;
    }

    while (current != nullptr && current != root) {
        Inode* parent = current->parent();
        if (parent == nullptr)
            break;

        // Find this inode's name by asking its parent to enumerate.
        char name[FILENAME_MAX_LENGTH] = {};
        for (usize index = 0;; ++index) {
            DirectoryEntry entry;
            auto have = parent->read_directory(index, entry);
            if (have.is_error() || !have.value())
                break;
            auto child = parent->lookup(entry.name);
            if (!child.is_error() && child.value() == current) {
                strncpy(name, entry.name, FILENAME_MAX_LENGTH - 1);
                break;
            }
        }
        if (name[0] == '\0')
            return Error::from_errno(ENOENT);

        usize const name_length = strlen(name);
        if (position < name_length + 1)
            return Error::from_errno(ENAMETOOLONG);

        position -= name_length;
        memcpy(&scratch[position], name, name_length);
        scratch[--position] = '/';

        current = parent;
    }

    // Reaching anything but the root means the chain was cut -- an unlinked
    // directory that a process is still sitting in. POSIX says getcwd fails
    // with ENOENT there rather than inventing a path.
    if (current != root)
        return Error::from_errno(ENOENT);

    usize const length = PATH_MAX_LENGTH - position - 1;
    if (length + 1 > capacity)
        return Error::from_errno(ERANGE);
    memcpy(buffer, &scratch[position], length + 1);
    return length;
}

} // namespace kernel::fs
