/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- taking a device number apart.
 *
 * Devices are named, not numbered, in devfs -- st_rdev is zero for everything.
 * These exist so that software which prints a major:minor pair compiles, and
 * the pair it prints is 0:0, which is true.
 */

#ifndef _SYS_SYSMACROS_H
#define _SYS_SYSMACROS_H

#define major(device) ((unsigned)(((device) >> 8) & 0xfff))
#define minor(device) ((unsigned)((device) & 0xff))
#define makedev(major, minor) ((dev_t)((((unsigned)(major)) << 8) | ((unsigned)(minor) & 0xff)))

#endif /* _SYS_SYSMACROS_H */
