/* SPDX-License-Identifier: GPL-3.0-or-later */
/* shit os 2 -- poll(2), as seen by both kernel and libc. */

#pragma once

#include <shitos/types.h>

/* What a caller can ask about. */
#define POLLIN 0x0001 /* reading would not block */
#define POLLPRI 0x0002 /* urgent data; nothing generates it here */
#define POLLOUT 0x0004 /* writing would not block */

/*
 * What the kernel can report without being asked. A caller does not set these
 * in `events` and must always be ready to see them in `revents`, which is the
 * part everyone forgets: a closed pipe reports POLLHUP whether or not the
 * caller asked about anything at all.
 */
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020

struct pollfd {
    int fd; /* negative means "skip me", and revents is zeroed */
    i16 events;
    i16 revents;
};
