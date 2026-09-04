/* SPDX-License-Identifier: MIT */
#ifndef _POLL_H
#define _POLL_H

#include <shitos/abi/poll.h>

typedef unsigned long nfds_t;

/*
 * timeout is in milliseconds: 0 polls and returns, negative waits forever.
 * Returns the number of entries with a non-zero revents.
 */
int poll(struct pollfd* entries, nfds_t count, int timeout);

#endif /* _POLL_H */
