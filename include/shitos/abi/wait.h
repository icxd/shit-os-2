/* SPDX-License-Identifier: GPL-3.0-or-later */
/* shit os 2 -- waitpid(2) status encoding. */

#pragma once

/* Low byte carries the terminating signal, next byte the exit status. */
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WTERMSIG(s) ((s) & 0x7f)
#define WIFEXITED(s) (WTERMSIG(s) == 0)
#define WIFSIGNALED(s) (WTERMSIG(s) != 0)

#define WNOHANG 1
#define WUNTRACED 2
