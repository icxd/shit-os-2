/* SPDX-License-Identifier: GPL-3.0-or-later */
/* shit os 2 -- waitpid(2) status encoding. */

#pragma once

/*
 * The historical encoding, kept because every ported program already knows it:
 *
 *   exited      (exit code << 8)
 *   signalled   signal number in the low seven bits
 *   stopped     (signal << 8) | 0x7f
 *   continued   0xffff
 *
 * 0x7f is what separates "stopped" from "signalled": no real signal number
 * reaches it, so the low byte can carry both meanings without ambiguity.
 */
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WTERMSIG(s) ((s) & 0x7f)
#define WSTOPSIG(s) WEXITSTATUS(s)

#define WIFSTOPPED(s) (((s) & 0xff) == 0x7f)
#define WIFCONTINUED(s) ((s) == 0xffff)
#define WIFEXITED(s) (!WIFSTOPPED(s) && !WIFCONTINUED(s) && WTERMSIG(s) == 0)
#define WIFSIGNALED(s) (!WIFSTOPPED(s) && !WIFCONTINUED(s) && WTERMSIG(s) != 0)

/* How the kernel builds them. Shared so the two sides cannot drift. */
#define W_EXITED(code) (((code) & 0xff) << 8)
#define W_SIGNALLED(signal) ((signal) & 0x7f)
#define W_STOPPED(signal) ((((signal) & 0xff) << 8) | 0x7f)
#define W_CONTINUED 0xffff

#define WNOHANG 1
#define WUNTRACED 2
#define WCONTINUED 8
