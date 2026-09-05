/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- a terminal, as a widget.
 *
 * A grid of cells, an escape-sequence parser, and scrollback. It knows nothing
 * about pseudo-terminals: bytes are fed in, keystrokes come out, and whoever
 * owns it decides where those go. That separation is what lets the same widget
 * show a log file or a serial port later without being told about either.
 *
 * What it understands is what a shell and the coreutils actually emit, which
 * is a small and well-worn corner of the VT100 that everything after it kept:
 * SGR colour and bold, cursor movement, erase, and scrolling. Nothing here
 * implements a character set switch or a double-height line, because nothing
 * has ever sent one.
 */

#pragma once

#include "ui.h"

/* The sixteen ANSI colours, which is what SGR 30-37 and 90-97 select. */
#define TERMINAL_PALETTE_SIZE 16

typedef struct UiTerminalTheme {
    unsigned background;
    unsigned foreground;
    unsigned cursor;
    unsigned selection;
    unsigned palette[TERMINAL_PALETTE_SIZE];
} UiTerminalTheme;

const UiTerminalTheme* ui_terminal_theme(void);

/*
 * `on_input` is handed bytes the user typed, to be written to whatever is on
 * the other end. `on_resize` is told when the grid changes shape, so the pty
 * can be told its new size and the shell can wrap correctly.
 */
typedef void (*UiTerminalInput)(const char* bytes, int length, void* user);
typedef void (*UiTerminalResize)(int columns, int rows, void* user);

UiWidget* ui_terminal_create(UiTerminalInput on_input, void* user);
void ui_terminal_set_resize_handler(UiWidget*, UiTerminalResize, void* user);

/* Bytes from the far end. Everything the terminal does happens in here. */
void ui_terminal_feed(UiWidget*, const char* bytes, int length);

int ui_terminal_columns(const UiWidget*);
int ui_terminal_rows(const UiWidget*);

/*
 * What is on the visible screen, read back. Selection and copy will need this
 * -- there is nowhere else the text lives -- and until then it is what makes
 * the parser testable without a screenshot. Outside the grid, `\0`.
 */
int ui_terminal_character_at(const UiWidget*, int x, int y);
int ui_terminal_cursor_x(const UiWidget*);
int ui_terminal_cursor_y(const UiWidget*);
