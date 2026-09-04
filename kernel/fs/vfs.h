// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- the virtual filesystem.
//
// Three filesystems exist at boot and they have nothing in common: a
// read-only tar image, a writable heap-backed one, and one that is entirely
// synthetic. They all implement Inode, which is what makes `cat /dev/kbd0`
// and `cat /etc/motd` the same code path from the caller's side.
//
// Lifetime: inodes are owned by their filesystem and live as long as the
// mount does. There is no cache eviction and no reference counting, which is
// fine for RAM-backed filesystems and will need revisiting when a disk driver
// arrives -- see docs/roadmap.md.

#pragma once

#include <kernel/lib/error.h>
#include <kernel/lib/vector.h>

#include <shitos/abi/dirent.h>
#include <shitos/abi/fcntl.h>
#include <shitos/abi/stat.h>
#include <shitos/types.h>

namespace kernel::fs {

inline constexpr usize FILENAME_MAX_LENGTH = 128;
inline constexpr usize PATH_MAX_LENGTH = 1024;

enum class InodeType : u8 {
    Regular,
    Directory,
    CharacterDevice,
    BlockDevice,
    Fifo,
    SymbolicLink,
};

u32 mode_bits_for(InodeType type);
u8 dirent_type_for(InodeType type);

struct DirectoryEntry {
    char name[FILENAME_MAX_LENGTH];
    u64 inode_number;
    InodeType type;
};

class FileSystem;

class Inode {
public:
    virtual ~Inode() = default;

    virtual ErrorOr<usize> read(u64 offset, void* buffer, usize length);
    virtual ErrorOr<usize> write(u64 offset, void const* buffer, usize length);
    virtual ErrorOr<void> truncate(u64 size);

    // Directory operations. The default implementations return ENOTDIR, so a
    // filesystem only overrides what it actually supports.
    virtual ErrorOr<Inode*> lookup(char const* name);
    virtual ErrorOr<bool> read_directory(usize index, DirectoryEntry& out);
    virtual ErrorOr<Inode*> create(char const* name, InodeType type, u32 mode);
    virtual ErrorOr<void> unlink(char const* name);

    virtual ErrorOr<int> ioctl(u32 request, void* argument);

    // Called when a FileDescription onto this inode is created and destroyed.
    // Pipes use this to count their live ends: fork() shares a description
    // rather than duplicating it, so a description is exactly one "end".
    virtual void on_description_opened(int flags) { (void)flags; }
    virtual void on_description_closed(int flags) { (void)flags; }

    // Whether a read would return immediately. Used by the TTY and by poll.
    virtual bool can_read_without_blocking() const { return true; }

    virtual ErrorOr<void> stat(struct stat& out) const;

    InodeType type() const { return m_type; }
    bool is_directory() const { return m_type == InodeType::Directory; }
    u64 inode_number() const { return m_inode_number; }
    u32 mode() const { return m_mode; }
    virtual u64 size() const { return m_size; }

    FileSystem* filesystem() const { return m_filesystem; }
    Inode* parent() const { return m_parent; }

protected:
    Inode(FileSystem* filesystem, InodeType type, u32 mode)
        : m_filesystem(filesystem)
        , m_type(type)
        , m_mode(mode)
    {
    }

    FileSystem* m_filesystem { nullptr };
    Inode* m_parent { nullptr };
    InodeType m_type { InodeType::Regular };
    u32 m_mode { 0644 };
    u64 m_size { 0 };
    u64 m_inode_number { 0 };
};

class FileSystem {
public:
    virtual ~FileSystem() = default;
    virtual char const* name() const = 0;
    virtual Inode& root() = 0;
    virtual bool is_read_only() const { return false; }
};

// An open file. One per successful open(); several descriptors can share one
// after dup(), which is why the offset lives here and not in the fd table.
class FileDescription {
public:
    FileDescription(Inode& inode, int flags)
        : m_inode(&inode)
        , m_flags(flags)
    {
    }

    ErrorOr<usize> read(void* buffer, usize length);
    ErrorOr<usize> write(void const* buffer, usize length);
    ErrorOr<u64> seek(i64 offset, int whence);
    ErrorOr<usize> get_directory_entries(void* buffer, usize length);

    Inode& inode() { return *m_inode; }
    int flags() const { return m_flags; }
    void set_flags(int flags) { m_flags = flags; }
    u64 offset() const { return m_offset; }

    bool is_readable() const { return (m_flags & O_ACCMODE) != O_WRONLY; }
    bool is_writable() const { return (m_flags & O_ACCMODE) != O_RDONLY; }

    // Descriptions are shared by dup() and inherited across fork(), so they
    // are reference counted even though inodes are not.
    void ref() { ++m_reference_count; }
    bool unref() { return --m_reference_count == 0; }

private:
    Inode* m_inode;
    int m_flags { 0 };
    u64 m_offset { 0 };
    u32 m_reference_count { 1 };
    usize m_directory_index { 0 };
};

void initialize();

ErrorOr<void> mount(char const* path, FileSystem* filesystem);
ErrorOr<void> mount_root(FileSystem* filesystem);

// Resolves an absolute path, or one relative to `base`. Follows mount points.
ErrorOr<Inode*> resolve(char const* path, Inode* base = nullptr);

// Resolves everything but the last component, and hands back the parent
// directory plus that final name. Used by create, mkdir and unlink.
ErrorOr<Inode*> resolve_parent(
    char const* path, Inode* base, char (&final_component)[FILENAME_MAX_LENGTH]);

ErrorOr<FileDescription*> open(char const* path, int flags, u32 mode, Inode* base = nullptr);

Inode* root_inode();

// Reconstructs an absolute path by walking parent pointers. Used by getcwd.
ErrorOr<usize> absolute_path_of(Inode& inode, char* buffer, usize capacity);

struct MountInfo {
    char path[FILENAME_MAX_LENGTH];
    FileSystem* filesystem;
};

usize mount_count();
MountInfo const& mount_at(usize index);

} // namespace kernel::fs
