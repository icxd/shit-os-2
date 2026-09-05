// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- /dev.

#include <kernel/dev/console.h>
#include <kernel/dev/fbdev.h>
#include <kernel/fs/devfs.h>
#include <kernel/lib/new.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>

namespace kernel::fs {

namespace {

DevfsFileSystem* s_instance = nullptr;

// /dev/null and /dev/zero are not worth a loadable module, so devfs provides
// them itself through the same DeviceOps interface a driver would use.

isize null_read(void*, void*, usize, u64)
{
    return 0;
}
isize null_write(void*, void const*, usize length, u64)
{
    return static_cast<isize>(length);
}

isize zero_read(void*, void* buffer, usize length, u64)
{
    memset(buffer, 0, length);
    return static_cast<isize>(length);
}

constexpr DeviceOps NULL_OPS { null_read, null_write, nullptr, nullptr };
constexpr DeviceOps ZERO_OPS { zero_read, null_write, nullptr, nullptr };

} // namespace

ErrorOr<usize> DevfsInode::read(u64 offset, void* buffer, usize length)
{
    if (m_type == InodeType::Directory)
        return Error::from_errno(EISDIR);
    if (is_unlinked())
        return Error::from_errno(ENODEV);
    if (m_ops == nullptr || m_ops->read == nullptr)
        return Error::from_errno(ENOTSUP);

    isize const result = m_ops->read(m_device_self, buffer, length, offset);
    if (result < 0)
        return Error::from_errno(static_cast<int>(-result));
    return static_cast<usize>(result);
}

ErrorOr<usize> DevfsInode::write(u64 offset, void const* buffer, usize length)
{
    if (m_type == InodeType::Directory)
        return Error::from_errno(EISDIR);
    if (is_unlinked())
        return Error::from_errno(ENODEV);
    if (m_ops == nullptr || m_ops->write == nullptr)
        return Error::from_errno(ENOTSUP);

    isize const result = m_ops->write(m_device_self, buffer, length, offset);
    if (result < 0)
        return Error::from_errno(static_cast<int>(-result));
    return static_cast<usize>(result);
}

ErrorOr<int> DevfsInode::ioctl(u32 request, void* argument)
{
    if (is_unlinked())
        return Error::from_errno(ENODEV);
    if (m_ops == nullptr || m_ops->ioctl == nullptr)
        return Error::from_errno(ENOTTY);

    int const result = m_ops->ioctl(m_device_self, request, argument);
    if (result < 0)
        return Error::from_errno(-result);
    return result;
}

bool DevfsInode::can_read_without_blocking() const
{
    if (m_ops == nullptr || m_ops->poll_readable == nullptr)
        return true;
    return m_ops->poll_readable(m_device_self);
}

/*
 * Device memory is a fixed physical range, so the page for an offset is
 * arithmetic rather than a lookup, and `for_write` means nothing: the memory
 * is already there and its permissions came from the mapping.
 */
/*
 * The framebuffer is the one device where letting go matters: a compositor
 * that takes the screen and then dies would otherwise leave the console
 * suspended and the machine looking hung.
 *
 * It is the *last* descriptor that gives the screen back, not any descriptor,
 * which is why this counts them rather than acting on each close. A process
 * that opens /dev/fb0 a second time and closes it again is not done with the
 * screen.
 */
void DevfsInode::on_description_opened(int)
{
    if (strcmp(m_name, "fb0") == 0)
        ++m_open_descriptions;
}

void DevfsInode::on_description_closed(int)
{
    if (strcmp(m_name, "fb0") != 0)
        return;
    if (m_open_descriptions > 0 && --m_open_descriptions == 0)
        dev::framebuffer_device_release();
}

ErrorOr<PhysAddr> DevfsInode::physical_page(u64 offset, bool)
{
    if (is_unlinked())
        return Error::from_errno(ENODEV);
    if (m_memory_length == 0)
        return Error::from_errno(ENODEV);
    if (offset >= m_memory_length)
        return Error::from_errno(ENXIO);

    return m_memory_base + align_down<u64>(offset, PAGE_SIZE);
}

ErrorOr<Inode*> DevfsInode::lookup(char const* name)
{
    if (m_type != InodeType::Directory)
        return Error::from_errno(ENOTDIR);

    for (auto* child : m_children) {
        if (strcmp(child->m_name, name) == 0)
            return static_cast<Inode*>(child);
    }
    return Error::from_errno(ENOENT);
}

ErrorOr<bool> DevfsInode::read_directory(usize index, DirectoryEntry& out)
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

DevfsFileSystem* DevfsFileSystem::the()
{
    return s_instance;
}

ErrorOr<DevfsFileSystem*> DevfsFileSystem::create()
{
    auto* filesystem = static_cast<DevfsFileSystem*>(kzalloc(sizeof(DevfsFileSystem)));
    if (filesystem == nullptr)
        return Error::from_errno(ENOMEM);
    new (filesystem) DevfsFileSystem();

    auto* root = static_cast<DevfsInode*>(kzalloc(sizeof(DevfsInode)));
    if (root == nullptr) {
        kfree(filesystem);
        return Error::from_errno(ENOMEM);
    }
    new (root) DevfsInode(filesystem, InodeType::Directory, 0755);
    root->m_inode_number = filesystem->m_next_inode_number++;
    strcpy(root->m_name, "/");

    filesystem->m_root = root;
    s_instance = filesystem;

    TRY(filesystem->register_device({ "null", DEVICE_TYPE_CHAR, nullptr, &NULL_OPS }));
    TRY(filesystem->register_device({ "zero", DEVICE_TYPE_CHAR, nullptr, &ZERO_OPS }));

    return filesystem;
}

ErrorOr<void> DevfsFileSystem::register_device(DeviceDescriptor const& device)
{
    return register_memory_device(device, PhysAddr(0), 0);
}

ErrorOr<void> DevfsFileSystem::register_memory_device(
    DeviceDescriptor const& device, PhysAddr memory_base, u64 memory_length)
{
    if (device.name == nullptr || device.ops == nullptr)
        return Error::from_errno(EINVAL);
    if (strlen(device.name) >= FILENAME_MAX_LENGTH)
        return Error::from_errno(ENAMETOOLONG);

    for (auto* existing : m_root->m_children) {
        if (strcmp(existing->m_name, device.name) == 0)
            return Error::from_errno(EEXIST);
    }

    auto const type
        = device.type == DEVICE_TYPE_BLOCK ? InodeType::BlockDevice : InodeType::CharacterDevice;

    auto* node = static_cast<DevfsInode*>(kzalloc(sizeof(DevfsInode)));
    if (node == nullptr)
        return Error::from_errno(ENOMEM);
    new (node) DevfsInode(this, type, 0666);

    strncpy(node->m_name, device.name, FILENAME_MAX_LENGTH - 1);
    node->m_parent = m_root;
    node->m_inode_number = m_next_inode_number++;
    node->m_device = device;
    node->m_ops = device.ops;
    node->m_device_self = device.self;
    node->m_memory_base = memory_base;
    node->m_memory_length = memory_length;

    if (auto result = m_root->m_children.append(node); result.is_error()) {
        node->unref();
        return result.error();
    }

    klog(LOG_DEBUG, "devfs", "registered /dev/%s", device.name);
    return {};
}

void DevfsFileSystem::unregister_device(char const* name)
{
    for (usize i = 0; i < m_root->m_children.size(); ++i) {
        auto* child = m_root->m_children[i];
        if (strcmp(child->m_name, name) != 0)
            continue;
        // A module unloading must not free a node a process still has open;
        // the reference count keeps it alive until the last close. What it
        // must not keep is the driver's ops table and instance pointer --
        // those belong to the module and are about to be unmapped.
        m_root->m_children.remove_at(i);
        child->mark_unlinked();
        child->m_ops = nullptr;
        child->m_device_self = nullptr;
        child->unref();
        klog(LOG_DEBUG, "devfs", "unregistered /dev/%s", name);
        return;
    }
}

usize DevfsFileSystem::device_count() const
{
    return m_root->m_children.size();
}

} // namespace kernel::fs
