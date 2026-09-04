// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- bringing up the filesystem tree at boot.

#pragma once

#include <kernel/boot/boot_info.h>
#include <kernel/lib/error.h>

namespace kernel::fs {

// Mounts the initrd read-only at /, then tmpfs on /tmp and devfs on /dev.
// The mount points have to already exist in the initrd, which is why
// tools/mkinitrd.sh creates them.
ErrorOr<void> mount_boot_filesystems(boot::BootInfo const& info);

} // namespace kernel::fs
