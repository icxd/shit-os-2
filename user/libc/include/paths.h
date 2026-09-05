/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- the handful of paths that are effectively ABI.
 *
 * A program that spawns a shell has to name one, and a program that wants a
 * sink has to name that. Hardcoding the strings at every call site is how they
 * end up inconsistent.
 */

#ifndef _PATHS_H
#define _PATHS_H

#define _PATH_BSHELL "/bin/sh"
#define _PATH_DEVNULL "/dev/null"
/* The *controlling* terminal, not the console. A shell asks this who owns the
 * foreground, and pointing it at /dev/tty0 means every shell under a
 * pseudo-terminal asks the wrong terminal and stops itself waiting for a turn
 * that never comes. */
#define _PATH_TTY "/dev/tty"
#define _PATH_CONSOLE "/dev/console"
#define _PATH_DEFPATH "/bin"
#define _PATH_STDPATH "/bin"
#define _PATH_TMP "/tmp/"

#endif /* _PATHS_H */
