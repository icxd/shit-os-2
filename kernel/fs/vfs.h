// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- the virtual filesystem.
//
// Three filesystems exist at boot and they have nothing in common: a
// read-only tar image, a writable heap-backed one, and one that is entirely
// synthetic. They all implement Inode, which is what makes `cat /dev/kbd0`
// and `cat /etc/motd` the same code path from the caller's side.
//
// Lifetime: inodes are reference counted. A directory holds one reference to
// each child it names; an open FileDescription holds one; so does anything
// else that keeps a pointer for a while, such as a process's working
// directory. Removing a name drops the directory's reference but destroys
// nothing while somebody still has the file open, which is what POSIX
// promises and what the earlier design got wrong -- see the entry in
// docs/roadmap.md for the reproduction it cost.
//
// There is still no cache eviction; a live inode stays in memory. That is
// fine for RAM-backed filesystems and will need revisiting for a disk.

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

    // --- lifetime ---
    //
    // Atomic because two threads can open and close the same file at once,
    // and because a driver may drop a device node from an interrupt-adjacent
    // path.

    void ref() { __atomic_add_fetch(&m_reference_count, 1, __ATOMIC_ACQ_REL); }

    // Drops a reference and destroys the inode if that was the last one.
    // After this returns the pointer may be dead; callers must not touch it.
    void unref();

    u32 reference_count() const { return __atomic_load_n(&m_reference_count, __ATOMIC_ACQUIRE); }

    // True once the last directory entry naming this inode has gone. The
    // inode stays alive for whoever still has it open, but it is no longer
    // reachable by name.
    bool is_unlinked() const { return m_unlinked; }

    // Called by a filesystem when the last directory entry naming this inode
    // has been removed. Does not free anything; the reference count decides.
    // The parent link goes with the name: the directory may be removed too,
    // and a dangling m_parent is exactly the bug this change is about.
    void mark_unlinked()
    {
        m_unlinked = true;
        m_parent = nullptr;
    }

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

    // For an inode that never had a name -- a pipe -- to start from zero.
    void drop_initial_link_reference() { m_reference_count = 0; }

    // Destroys and frees. Virtual so a filesystem that pools its inodes can
    // reclaim rather than free; the default suits everything allocated with
    // kzalloc, which is all of them today.
    virtual void destroy();

    FileSystem* m_filesystem { nullptr };
    Inode* m_parent { nullptr };

    // Starts at one: the directory entry that names it. An inode created
    // without a name -- a pipe -- starts at zero and is kept alive purely by
    // its open descriptions.
    u32 m_reference_count { 1 };
    bool m_unlinked { false };
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
    // Takes a reference on the inode; release_description gives it back.
    FileDescription(Inode& inode, int flags)
        : m_inode(&inode)
        , m_flags(flags)
    {
        m_inode->ref();
        m_inode->on_description_opened(m_flags);
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
    // are reference counted too -- a second count, above the inode's: three
    // descriptors onto one description still hold the inode just once.
    void ref() { __atomic_add_fetch(&m_reference_count, 1, __ATOMIC_ACQ_REL); }
    bool unref() { return __atomic_sub_fetch(&m_reference_count, 1, __ATOMIC_ACQ_REL) == 0; }

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

// Drops one reference to a description. The last one closes the file, which
// may in turn destroy the inode if the name has already been removed.
// Everything that lets go of a FileDescription goes through here, so the
// unref pairing lives in one place rather than at four call sites.
void release_description(FileDescription* description);

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
