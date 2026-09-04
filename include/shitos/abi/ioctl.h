/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- ioctl request encoding.
 *
 * A request number carries the direction and the size of its argument, so the
 * kernel knows how many bytes to move without a table entry per request. That
 * matters for the module ABI: a driver can define its own ioctls and the
 * kernel handles the userspace copies correctly without being taught about
 * them.
 *
 *   bits 31..30  direction (none / write / read / both)
 *   bits 29..16  argument size in bytes
 *   bits 15..0   request number
 *
 * "write" is userspace -> kernel and "read" is kernel -> userspace, which is
 * the direction the *caller* sees, matching every other system that does this.
 */

#pragma once

#define _IOC_NONE 0u
#define _IOC_WRITE 1u
#define _IOC_READ 2u

#define _IOC_SIZE_BITS 14
#define _IOC_SIZE_MAX ((1u << _IOC_SIZE_BITS) - 1)

#define _IOC(direction, number, size)                                                              \
    ((((unsigned)(direction)) << 30) | (((unsigned)(size)) << 16) | ((unsigned)(number) & 0xFFFFu))

#define _IO(number) _IOC(_IOC_NONE, (number), 0)
#define _IOW(number, type) _IOC(_IOC_WRITE, (number), sizeof(type))
#define _IOR(number, type) _IOC(_IOC_READ, (number), sizeof(type))
#define _IOWR(number, type) _IOC(_IOC_READ | _IOC_WRITE, (number), sizeof(type))

#define _IOC_DIRECTION(request) (((unsigned)(request)) >> 30)
#define _IOC_ARGUMENT_SIZE(request) ((((unsigned)(request)) >> 16) & _IOC_SIZE_MAX)
