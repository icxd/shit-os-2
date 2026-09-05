/* SPDX-License-Identifier: GPL-3.0-or-later */
/* shit os 2 -- terminal ioctls and the line-discipline flags we honour. */

#pragma once

#include <shitos/abi/ioctl.h>
#include <shitos/types.h>

/*
 * These keep their traditional numbers rather than being re-encoded with the
 * _IOR/_IOW macros, because ported software hardcodes them. They predate the
 * encoding and so carry no size; the kernel has a small table for exactly
 * these three. Anything new should use the macros and describe itself.
 */
#define TCGETS 0x5401
#define TCSETS 0x5402
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

/*
 * Which process group owns the terminal. Everything about job control comes
 * back to these two: the foreground group is the one that gets ^C, and the
 * one allowed to read without being stopped.
 */
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410

/*
 * Claim this terminal as the session's controlling terminal, which is what
 * makes `/dev/tty` mean it. A session leader does this after `setsid`; it is
 * the only way a pty ever becomes one, there being no path to open it by.
 */
#define TIOCSCTTY 0x540E

struct winsize {
    u16 ws_row;
    u16 ws_col;
    u16 ws_xpixel;
    u16 ws_ypixel;
};

#define NCCS 32

struct termios {
    u32 c_iflag;
    u32 c_oflag;
    u32 c_cflag;
    u32 c_lflag;
    u8 c_cc[NCCS];
};

/* c_lflag */
#define ISIG 0x0001 /* ^C and friends raise signals */
#define ICANON 0x0002 /* line-buffered with editing */
#define ECHO 0x0008 /* echo input back to the terminal */

/* c_iflag */
#define ICRNL 0x0100 /* translate CR to NL on input */

/* c_oflag */
#define OPOST 0x0001 /* enable output processing */
#define ONLCR 0x0004 /* translate NL to CRNL on output */

/* c_cc indices */
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6
/* ^Z. Linux numbers it 10, and matching that costs nothing and means a
 * ported program that hardcodes the index gets the right control character. */
#define VSUSP 10
