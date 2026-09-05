/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- input events, as a GUI needs them.
 *
 * The TTY wants characters: it asks /dev/kbd0 and gets 'a', or 3 for ^C. That
 * is the right answer for a terminal and useless for anything else. A window
 * manager needs to know that a key went *down* and later came *up*, which key
 * physically it was, and what modifiers were held -- none of which survives
 * translation to a character.
 *
 * So there are two streams off the same hardware. /dev/kbd0 stays exactly as
 * it was, and /dev/kbdraw carries these.
 */

#pragma once

#include <shitos/types.h>

/* --- mouse --------------------------------------------------------------- */

#define MOUSE_BUTTON_LEFT 0x01
#define MOUSE_BUTTON_RIGHT 0x02
#define MOUSE_BUTTON_MIDDLE 0x04

/*
 * Deltas, not positions: a PS/2 mouse has no idea where the pointer is, and
 * neither does the kernel. Whoever owns the screen decides what the cursor
 * does with these, and is the only thing that knows how big the screen is.
 *
 * Y is positive upward, the way the mouse reports it. A compositor drawing
 * into a framebuffer whose Y grows downward has to negate it, and doing that
 * here would be inventing a convention the hardware does not have.
 */
struct mouse_event {
    i16 dx;
    i16 dy;
    i8 dz; /* scroll wheel; always 0 unless the mouse admitted to having one */
    u8 buttons; /* a mask of the three above -- state, not a change */
    u16 _reserved;
};

/* --- keyboard ------------------------------------------------------------ */

#define KEY_MODIFIER_SHIFT 0x01
#define KEY_MODIFIER_CONTROL 0x02
#define KEY_MODIFIER_ALT 0x04
#define KEY_MODIFIER_CAPS_LOCK 0x08

/*
 * Keycodes are scancode set 1 make codes, with the E0-prefixed keys moved up
 * by 0x100 so that the arrow keys and right control get numbers of their own
 * rather than colliding with the keypad.
 */
#define KEY_EXTENDED_BASE 0x100

#define KEY_UP (KEY_EXTENDED_BASE + 0x48)
#define KEY_DOWN (KEY_EXTENDED_BASE + 0x50)
#define KEY_LEFT (KEY_EXTENDED_BASE + 0x4B)
#define KEY_RIGHT (KEY_EXTENDED_BASE + 0x4D)
#define KEY_HOME (KEY_EXTENDED_BASE + 0x47)
#define KEY_END (KEY_EXTENDED_BASE + 0x4F)
#define KEY_PAGE_UP (KEY_EXTENDED_BASE + 0x49)
#define KEY_PAGE_DOWN (KEY_EXTENDED_BASE + 0x51)
#define KEY_DELETE (KEY_EXTENDED_BASE + 0x53)

struct key_event {
    u16 keycode;
    u8 pressed; /* 1 on the way down, 0 on the way up */
    u8 modifiers; /* held at the moment of the event */

    /*
     * The character this keystroke produces, or 0 for one that produces none
     * -- shift, F1, an arrow. Provided because every client would otherwise
     * carry its own copy of the same layout table, and they would disagree.
     */
    u32 codepoint;
};
