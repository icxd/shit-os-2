/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- pseudo-terminals.
 *
 * The BSD interface rather than the POSIX one. posix_openpt hands back a
 * master and a *name*, and turning that name back into the slave needs a
 * /dev/pts filesystem whose contents track the ptys that exist; openpty needs
 * none of that, and openpty is what programs actually call.
 */

#pragma once

#include <sys/types.h>

struct termios;
struct winsize;

/*
 * Opens both ends. `name` is accepted and ignored -- there are no names here,
 * so there is nothing honest to write into it -- and the two struct pointers
 * are applied to the new terminal when they are not null.
 *
 * Returns 0, or -1 with errno set.
 */
int openpty(int* master, int* slave, char* name, const struct termios* settings,
    const struct winsize* window);

/*
 * openpty, then fork, with the child made a session leader and given the slave
 * as its standard input, output and error. Returns 0 in the child, the child's
 * pid in the parent, and -1 on failure.
 */
pid_t forkpty(
    int* master, char* name, const struct termios* settings, const struct winsize* window);
