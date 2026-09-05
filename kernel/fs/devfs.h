// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- /dev.
//
// Entirely synthetic: nothing here is backed by storage. A loadable module
// calling KernelApi::device_register gets a node in this tree, which is how a
// driver becomes something userspace can open. `cat /dev/kbd0` and
// `cat /etc/motd` differ only in which Inode subclass answers the read.

#pragma once

#include <kernel/fs/vfs.h>
#include <kernel/lib/vector.h>

#include <shitos/module/api.h>

namespace kernel::fs {

class DevfsInode final : public Inode {
public:
    DevfsInode(FileSystem* filesystem, InodeType type, u32 mode)
        : Inode(filesystem, type, mode)
    {
    }

    ErrorOr<usize> read(u64 offset, void* buffer, usize length) override;
    ErrorOr<usize> write(u64 offset, void const* buffer, usize length) override;
    ErrorOr<int> ioctl(u32 request, void* argument) override;
    bool can_read_without_blocking() const override;
    ErrorOr<PhysAddr> physical_page(u64 offset, bool for_write) override;
    void on_description_opened(int flags) override;
    void on_description_closed(int flags) override;

    ErrorOr<Inode*> lookup(char const* name) override;
    ErrorOr<bool> read_directory(usize index, DirectoryEntry& out) override;

    char const* device_name() const { return m_name; }

private:
    friend class DevfsFileSystem;

    char m_name[FILENAME_MAX_LENGTH] {};

    // A copy of what the module registered. The ops table and `self` belong to
    // the module and must outlive the node, which is what unregistering on
    // module unload is for.
    DeviceDescriptor m_device {};
    DeviceOps const* m_ops { nullptr };
    void* m_device_self { nullptr };

    // Devices whose memory a process may map -- the framebuffer today. Left
    // zero for everything else, which then reports ENODEV like any other file.
    // This is deliberately not part of DeviceOps: a loadable module cannot set
    // it, because no module yet needs to, and inventing that ABI before there
    // is a driver to shape it would be guessing.
    PhysAddr m_memory_base { PhysAddr(0) };
    u64 m_memory_length { 0 };

    // Live descriptions onto this node. Only /dev/fb0 uses it, to notice when
    // the last one goes away.
    usize m_open_descriptions { 0 };

    Vector<DevfsInode*> m_children;
};

class DevfsFileSystem final : public FileSystem {
public:
    static ErrorOr<DevfsFileSystem*> create();

    // The single instance, so KernelApi::device_register can find it without
    // every driver having to be handed a pointer.
    static DevfsFileSystem* the();

    char const* name() const override { return "devfs"; }
    Inode& root() override { return *m_root; }

    ErrorOr<void> register_device(DeviceDescriptor const& device);

    // As above, plus a physical range the device will let a process mmap.
    ErrorOr<void> register_memory_device(
        DeviceDescriptor const& device, PhysAddr memory_base, u64 memory_length);
    void unregister_device(char const* name);

    usize device_count() const;

private:
    DevfsFileSystem() = default;

    DevfsInode* m_root { nullptr };
    u64 m_next_inode_number { 1 };
};

} // namespace kernel::fs
