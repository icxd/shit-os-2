/* SPDX-License-Identifier: GPL-3.0-or-later */
/* shit os 2 -- terminal ioctls and the line-discipline flags we honour. */

#pragma once

#include <shitos/types.h>

#define TCGETS 0x5401
#define TCSETS 0x5402
#define TIOCGWINSZ 0x5413

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
#define VMIN 6
#define VTIME 5
