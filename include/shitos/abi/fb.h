/* SPDX-License-Identifier: GPL-3.0-or-later */
/* shit os 2 -- the framebuffer, as seen from ring 3. */

#pragma once

#include <shitos/abi/ioctl.h>
#include <shitos/types.h>

/*
 * /dev/fb0 is the screen. Ask it what shape it is, mmap it, write pixels.
 *
 * There is exactly one mode and no way to change it: GRUB picks the mode at
 * boot and nothing here can talk to a real GPU. So there is no SET to pair
 * with the GET, and a program that wants a different resolution is out of
 * luck rather than being told a lie it can act on.
 */

struct fb_info {
    u32 width;
    u32 height;
    u32 pitch; /* bytes per scanline, which is not width * bytes_per_pixel */
    u32 bytes_per_pixel;
    u64 length; /* the whole mappable region, in bytes */

    /* Where each channel sits in a pixel. Nothing here is guaranteed to be
     * the 0x00RRGGBB everyone assumes, so a program that wants to be right on
     * hardware other than QEMU's default has to read these. */
    u8 red_shift, red_bits;
    u8 green_shift, green_bits;
    u8 blue_shift, blue_bits;
    u8 _reserved[2];
};

/* Geometry. The argument is a struct fb_info*. */
#define FBIOGET_INFO _IOR(0xFB01, struct fb_info)

/*
 * Take the screen away from the kernel console, or give it back. The argument
 * is an int: non-zero to acquire, zero to release.
 *
 * While acquired the console stops drawing, so a compositor's pixels are not
 * scribbled over by a stray klog. Serial keeps getting everything, so nothing
 * is actually lost, and the screen is released automatically when the last
 * descriptor onto /dev/fb0 closes -- a compositor that segfaults leaves a
 * usable console behind rather than a frozen picture of one.
 */
#define FBIO_ACQUIRE _IOW(0xFB02, int)
