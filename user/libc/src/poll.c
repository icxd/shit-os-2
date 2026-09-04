/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- poll and select.
 *
 * poll is the syscall. select is translated onto it here rather than
 * implemented twice: three bitmaps and a modifiable timeout are a worse
 * interface to the same question, and the only reason it exists is that
 * ported software asks for it.
 */

#include "internal.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>

int poll(struct pollfd* entries, nfds_t count, int timeout)
{
    return (int)__syscall_return(__syscall3(SYS_poll, (long)entries, (long)count, (long)timeout));
}

int select(
    int nfds, fd_set* readable, fd_set* writable, fd_set* exceptional, struct timeval* timeout)
{
    if (nfds < 0 || nfds > FD_SETSIZE) {
        errno = EINVAL;
        return -1;
    }

    struct pollfd entries[FD_SETSIZE];
    int count = 0;

    /* One entry per descriptor mentioned in any set, with the interests
     * merged: poll asks about a descriptor once, select asks three times. */
    for (int fd = 0; fd < nfds; ++fd) {
        short events = 0;
        if (readable && FD_ISSET(fd, readable))
            events |= POLLIN;
        if (writable && FD_ISSET(fd, writable))
            events |= POLLOUT;
        /* No out-of-band data exists here, so an exceptional set can only ever
         * come back empty. Asking about the descriptor anyway keeps an invalid
         * one reported as such. */
        if (exceptional && FD_ISSET(fd, exceptional))
            events |= POLLPRI;
        if (events == 0)
            continue;

        entries[count].fd = fd;
        entries[count].events = (short)events;
        entries[count].revents = 0;
        ++count;
    }

    int milliseconds = -1;
    if (timeout)
        milliseconds = (int)(timeout->tv_sec * 1000 + (timeout->tv_usec + 999) / 1000);

    int const polled = poll(entries, (nfds_t)count, milliseconds);
    if (polled < 0)
        return -1;

    /* select reports back through the sets it was given, so they have to be
     * rebuilt from scratch rather than filtered. */
    fd_set readable_out;
    fd_set writable_out;
    fd_set exceptional_out;
    FD_ZERO(&readable_out);
    FD_ZERO(&writable_out);
    FD_ZERO(&exceptional_out);

    int ready = 0;
    for (int i = 0; i < count; ++i) {
        int counted = 0;
        /* A hangup or an error makes a descriptor readable as far as select is
         * concerned: the read is what reports which. */
        if (readable && (entries[i].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            FD_SET(entries[i].fd, &readable_out);
            counted = 1;
        }
        if (writable && (entries[i].revents & (POLLOUT | POLLERR)) != 0) {
            FD_SET(entries[i].fd, &writable_out);
            ++ready;
        }
        if (exceptional && (entries[i].revents & POLLPRI) != 0) {
            FD_SET(entries[i].fd, &exceptional_out);
            ++ready;
        }
        ready += counted;
    }

    if (readable)
        *readable = readable_out;
    if (writable)
        *writable = writable_out;
    if (exceptional)
        *exceptional = exceptional_out;

    /* select counts each set membership separately, so a descriptor ready both
     * ways counts twice. */
    if (timeout) {
        /* Linux writes back the time left; nothing here tracks it that
         * precisely, so report none left rather than a made-up number. */
        timeout->tv_sec = 0;
        timeout->tv_usec = 0;
    }

    return ready;
}
