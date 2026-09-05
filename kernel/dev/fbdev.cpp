// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- /dev/fb0, the screen as a file.
//
// The framebuffer is not a driver: GRUB set the mode before we were running
// and there is nothing to talk to. So this is registered in-kernel rather than
// coming from a module, and what it exposes is a fixed physical range plus the
// geometry needed to make sense of it.
//
// Two ways in. mmap is the real one -- a compositor maps the whole thing once
// and never syscalls again. read and write are there so that a shell can prove
// the device works without writing a program, and because `cat /dev/fb0 > x`
// being a screenshot is worth the twenty lines.

#include <kernel/dev/console.h>
#include <kernel/dev/fbdev.h>
#include <kernel/dev/framebuffer.h>
#include <kernel/fs/devfs.h>
#include <kernel/lib/kstd.h>
#include <kernel/lib/string.h>

#include <shitos/abi/errno.h>
#include <shitos/abi/fb.h>

namespace kernel::dev {

namespace {

// Whether the screen currently belongs to userland. A flag rather than a
// count: "the console is drawing" and "it is not" are the only two states
// there are, and devfs already knows when the last descriptor has gone.
bool s_acquired = false;

u8* framebuffer_bytes()
{
    auto const& info = framebuffer_console().info();
    return static_cast<u8*>(phys_to_virt(phys(info.phys_address)));
}

u64 framebuffer_length()
{
    auto const& info = framebuffer_console().info();
    return static_cast<u64>(info.pitch) * info.height;
}

isize fb_read(void*, void* buffer, usize length, u64 offset)
{
    u64 const total = framebuffer_length();
    if (offset >= total)
        return 0;

    usize const to_copy = min<usize>(length, static_cast<usize>(total - offset));
    memcpy(buffer, framebuffer_bytes() + offset, to_copy);
    return static_cast<isize>(to_copy);
}

isize fb_write(void*, void const* buffer, usize length, u64 offset)
{
    u64 const total = framebuffer_length();
    if (offset >= total)
        return -ENOSPC;

    usize const to_copy = min<usize>(length, static_cast<usize>(total - offset));
    memcpy(framebuffer_bytes() + offset, buffer, to_copy);
    return static_cast<isize>(to_copy);
}

int fb_ioctl(void*, u32 request, void* argument)
{
    auto& console = framebuffer_console();
    auto const& source = console.info();

    switch (request) {
    case FBIOGET_INFO: {
        auto* out = static_cast<struct fb_info*>(argument);
        memset(out, 0, sizeof(*out));
        out->width = source.width;
        out->height = source.height;
        out->pitch = source.pitch;
        out->bytes_per_pixel = source.bits_per_pixel / 8;
        out->length = framebuffer_length();
        out->red_shift = source.red_shift;
        out->red_bits = source.red_bits;
        out->green_shift = source.green_shift;
        out->green_bits = source.green_bits;
        out->blue_shift = source.blue_shift;
        out->blue_bits = source.blue_bits;
        return 0;
    }

    case FBIO_ACQUIRE: {
        int const wanted = *static_cast<int*>(argument);
        if (wanted != 0) {
            s_acquired = true;
            console.suspend();
        } else if (s_acquired) {
            s_acquired = false;
            console.resume();
        }
        return 0;
    }

    default: return -ENOTTY;
    }
}

constexpr DeviceOps FB_OPS = {
    .read = fb_read,
    .write = fb_write,
    .ioctl = fb_ioctl,
    .poll_readable = nullptr, // the screen is always ready
};

} // namespace

void framebuffer_device_release()
{
    if (!s_acquired)
        return;
    s_acquired = false;
    framebuffer_console().resume();
}

ErrorOr<void> framebuffer_device_initialize()
{
    auto& console = framebuffer_console();
    if (!console.is_usable())
        return {}; // no framebuffer, no /dev/fb0; not an error

    auto const& info = console.info();
    if (info.format != boot::FramebufferFormat::Rgb) {
        // An EGA text buffer is not something a compositor can use, and
        // pretending otherwise would hand out a mapping of character cells.
        klog(LOG_WARN, "fbdev", "text mode framebuffer; /dev/fb0 not registered");
        return {};
    }

    auto* devfs = fs::DevfsFileSystem::the();
    if (devfs == nullptr)
        return Error::from_errno(ENODEV);

    u64 const length = framebuffer_length();
    TRY(devfs->register_memory_device(
        { "fb0", DEVICE_TYPE_CHAR, nullptr, &FB_OPS }, phys(info.phys_address), length));

    klog(LOG_INFO, "fbdev", "/dev/fb0 ready: %ux%u, %llu KiB mappable", info.width, info.height,
        static_cast<unsigned long long>(length / 1024));
    return {};
}

} // namespace kernel::dev
