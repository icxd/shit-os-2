// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- the initial ramdisk, as a tar file.

#include <kernel/dev/console.h>
#include <kernel/fs/ustar.h>
#include <kernel/lib/new.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>

namespace kernel::fs {

namespace {

constexpr usize BLOCK_SIZE = 512;

struct [[gnu::packed]] TarHeader {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char type_flag;
    char link_name[100];
    char magic[6];
    char version[2];
    char user_name[32];
    char group_name[32];
    char device_major[8];
    char device_minor[8];
    char prefix[155];
    char padding[12];
};
static_assert(sizeof(TarHeader) == BLOCK_SIZE, "a tar header is exactly one block");

// tar stores numbers as octal ASCII, space or NUL terminated.
u64 parse_octal(char const* field, usize length)
{
    u64 value = 0;
    for (usize i = 0; i < length; ++i) {
        char const c = field[i];
        if (c == '\0' || c == ' ')
            break;
        if (c < '0' || c > '7')
            continue;
        value = value * 8 + static_cast<u64>(c - '0');
    }
    return value;
}

bool is_ustar(TarHeader const& header)
{
    return memcmp(header.magic, "ustar", 5) == 0;
}

bool is_zero_block(u8 const* block)
{
    for (usize i = 0; i < BLOCK_SIZE; ++i) {
        if (block[i] != 0)
            return false;
    }
    return true;
}

} // namespace

ErrorOr<usize> UstarInode::read(u64 offset, void* buffer, usize length)
{
    if (m_type != InodeType::Regular)
        return Error::from_errno(EISDIR);
    if (offset >= m_size)
        return static_cast<usize>(0);

    usize const available = static_cast<usize>(m_size - offset);
    usize const to_copy = min(length, available);
    memcpy(buffer, m_data + offset, to_copy);
    return to_copy;
}

ErrorOr<Inode*> UstarInode::lookup(char const* name)
{
    if (m_type != InodeType::Directory)
        return Error::from_errno(ENOTDIR);

    for (auto* child : m_children) {
        if (strcmp(child->m_name, name) == 0)
            return static_cast<Inode*>(child);
    }
    return Error::from_errno(ENOENT);
}

ErrorOr<bool> UstarInode::read_directory(usize index, DirectoryEntry& out)
{
    if (m_type != InodeType::Directory)
        return Error::from_errno(ENOTDIR);

    // Synthesise "." and ".." rather than storing them, so the tree has no
    // cycles for anything walking it to trip over.
    if (index == 0) {
        strcpy(out.name, ".");
        out.inode_number = m_inode_number;
        out.type = InodeType::Directory;
        return true;
    }
    if (index == 1) {
        strcpy(out.name, "..");
        auto* parent = static_cast<UstarInode*>(m_parent);
        out.inode_number = parent != nullptr ? parent->inode_number() : m_inode_number;
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

ErrorOr<UstarFileSystem*> UstarFileSystem::create(u8 const* data, usize length)
{
    auto* filesystem = static_cast<UstarFileSystem*>(kzalloc(sizeof(UstarFileSystem)));
    if (filesystem == nullptr)
        return Error::from_errno(ENOMEM);
    new (filesystem) UstarFileSystem();

    auto* root = static_cast<UstarInode*>(kzalloc(sizeof(UstarInode)));
    if (root == nullptr) {
        kfree(filesystem);
        return Error::from_errno(ENOMEM);
    }
    new (root) UstarInode(filesystem, InodeType::Directory, 0755);
    root->m_inode_number = filesystem->m_next_inode_number++;
    strcpy(root->m_name, "/");
    filesystem->m_root = root;

    TRY(filesystem->parse(data, length));
    return filesystem;
}

ErrorOr<UstarInode*> UstarFileSystem::ensure_path(char const* path, InodeType type, u32 mode)
{
    UstarInode* current = m_root;
    char const* cursor = path;
    char component[FILENAME_MAX_LENGTH];

    for (;;) {
        while (*cursor == '/')
            ++cursor;
        if (*cursor == '\0')
            return current;

        usize length = 0;
        while (*cursor != '\0' && *cursor != '/') {
            if (length + 1 < FILENAME_MAX_LENGTH)
                component[length++] = *cursor;
            ++cursor;
        }
        component[length] = '\0';

        bool const is_last = (*cursor == '\0') || (*(cursor + 1) == '\0' && *cursor == '/');

        UstarInode* child = nullptr;
        for (auto* candidate : current->m_children) {
            if (strcmp(candidate->m_name, component) == 0) {
                child = candidate;
                break;
            }
        }

        if (child == nullptr) {
            // Intermediate components are directories even if the archive
            // never listed them; GNU tar omits them often enough to matter.
            auto const child_type = is_last ? type : InodeType::Directory;
            auto const child_mode = is_last ? mode : 0755u;

            child = static_cast<UstarInode*>(kzalloc(sizeof(UstarInode)));
            if (child == nullptr)
                return Error::from_errno(ENOMEM);
            new (child) UstarInode(this, child_type, child_mode);

            strncpy(child->m_name, component, FILENAME_MAX_LENGTH - 1);
            child->m_parent = current;
            child->m_inode_number = m_next_inode_number++;
            TRY(current->m_children.append(child));
        }

        if (is_last)
            return child;
        current = child;
    }
}

ErrorOr<void> UstarFileSystem::parse(u8 const* data, usize length)
{
    usize offset = 0;

    while (offset + BLOCK_SIZE <= length) {
        auto const* header = reinterpret_cast<TarHeader const*>(data + offset);

        // Two consecutive zero blocks mark the end of the archive; one is
        // enough to stop on, since nothing valid follows.
        if (is_zero_block(data + offset))
            break;

        if (!is_ustar(*header)) {
            klog(LOG_WARN, "ustar", "block at offset %zu is not ustar, stopping", offset);
            break;
        }

        // A ustar path may be split across prefix and name.
        char path[256];
        path[0] = '\0';
        if (header->prefix[0] != '\0') {
            usize const prefix_length = strnlen(header->prefix, sizeof(header->prefix));
            memcpy(path, header->prefix, prefix_length);
            path[prefix_length] = '/';
            path[prefix_length + 1] = '\0';
        }
        usize const used = strlen(path);
        usize const name_length = strnlen(header->name, sizeof(header->name));
        memcpy(path + used, header->name, name_length);
        path[used + name_length] = '\0';

        u64 const size = parse_octal(header->size, sizeof(header->size));
        u32 const mode = static_cast<u32>(parse_octal(header->mode, sizeof(header->mode)));

        // tar paths from our mkinitrd start with "./"; strip it so the tree is
        // rooted where the mount expects.
        char const* clean_path = path;
        if (clean_path[0] == '.' && clean_path[1] == '/')
            clean_path += 2;

        offset += BLOCK_SIZE;

        if (clean_path[0] != '\0') {
            switch (header->type_flag) {
            case '5': {
                TRY(ensure_path(clean_path, InodeType::Directory, mode != 0 ? mode : 0755));
                break;
            }
            case '0':
            case '\0': {
                auto* inode = TRY(ensure_path(clean_path, InodeType::Regular, mode != 0 ? mode : 0644));
                inode->m_data = data + offset;
                inode->m_size = size;
                ++m_file_count;
                m_total_bytes += size;
                break;
            }
            default:
                // Symlinks, hard links, device nodes and the GNU extensions
                // are skipped rather than half-supported.
                klog(LOG_DEBUG, "ustar", "skipping '%s' (type '%c')", clean_path, header->type_flag);
                break;
            }
        }

        // File data is padded out to a whole number of blocks.
        offset += align_up<usize>(static_cast<usize>(size), BLOCK_SIZE);
    }

    klog(LOG_INFO, "ustar", "%zu files, %zu bytes of content", m_file_count, m_total_bytes);
    return {};
}

} // namespace kernel::fs
