// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- bringing up the filesystem tree at boot.

#include <kernel/dev/console.h>
#include <kernel/fs/boot_mounts.h>
#include <kernel/fs/devfs.h>
#include <kernel/fs/tmpfs.h>
#include <kernel/fs/ustar.h>
#include <kernel/fs/vfs.h>
#include <kernel/lib/string.h>

namespace kernel::fs {

ErrorOr<void> mount_boot_filesystems(boot::BootInfo const& info)
{
    initialize();

    // The initrd is whichever module GRUB loaded; there is only ever one, and
    // grub.cfg names it "initrd" so a future second module is distinguishable.
    boot::BootModule const* initrd = nullptr;
    for (usize i = 0; i < info.module_count; ++i) {
        if (info.modules[i].name == nullptr || strstr(info.modules[i].name, "initrd") != nullptr) {
            initrd = &info.modules[i];
            break;
        }
    }
    if (initrd == nullptr && info.module_count > 0)
        initrd = &info.modules[0];

    if (initrd == nullptr) {
        klog(LOG_ERROR, "vfs", "no initrd module; there is nothing to mount as root");
        return Error::from_errno(ENODEV);
    }

    // The module's pages were marked used by the physical allocator at boot,
    // so pointing straight into them through the direct map is safe and means
    // file contents are never copied.
    auto const* data = static_cast<u8 const*>(phys_to_virt(phys(initrd->phys_start)));
    usize const length = static_cast<usize>(initrd->phys_end - initrd->phys_start);

    auto* initrd_fs = TRY(UstarFileSystem::create(data, length));
    TRY(mount_root(initrd_fs));

    auto* tmp_fs = TRY(TmpfsFileSystem::create());
    TRY(mount("/tmp", tmp_fs));

    auto* dev_fs = TRY(DevfsFileSystem::create());
    TRY(mount("/dev", dev_fs));

    return {};
}

} // namespace kernel::fs
