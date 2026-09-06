/* SPDX-License-Identifier: GPL-3.0-or-later */
/* shit os 2 -- a terminal, as a widget. */

#include "terminal.h"

#include <shitos/abi/input.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_COLUMNS 400
#define MAX_ROWS 200
#define SCROLLBACK_ROWS 2000
#define MAX_PARAMETERS 16

/*
 * The macOS Terminal palette, near enough. The point of matching a real one
 * rather than picking sixteen colours is that programs choose from these
 * assuming a particular relationship between them -- that "bright black" is a
 * usable grey, that red and green are distinguishable at a glance.
 */
static const UiTerminalTheme s_theme = {
    .background = 0x1e1e22,
    .foreground = 0xdcdcdc,
    .cursor = 0xc8c8c8,
    .selection = 0x3a4a63,
    .palette = {
        0x2a2a2e, /* black, lifted so it is visible on this background */
        0xd9534f, /* red */
        0x5cb85c, /* green */
        0xd4a72c, /* yellow */
        0x5b8dd9, /* blue */
        0xa77bd4, /* magenta */
        0x4fb3bf, /* cyan */
        0xcfcfcf, /* white -- the default foreground */
        0x6a6a70, /* bright black, which is what dim text uses */
        0xef7a76,
        0x7ed17e,
        0xe8c65a,
        0x82aee8,
        0xc39ce8,
        0x76cdd6,
        0xffffff,
    },
};

const UiTerminalTheme* ui_terminal_theme(void)
{
    return &s_theme;
}

/* --- the grid --------------------------------------------------------------- */

#define ATTR_BOLD 0x01
#define ATTR_INVERSE 0x02
#define ATTR_DIM 0x04

typedef struct Cell {
    unsigned char ch;
    unsigned char attributes;
    signed char foreground; /* an index into the palette, or -1 for the default */
    signed char background;
} Cell;

typedef enum ParserState {
    PARSE_GROUND,
    PARSE_ESCAPE, /* saw ESC */
    PARSE_CSI, /* inside ESC[ */
    PARSE_OSC, /* inside ESC], swallowed to its terminator */
} ParserState;

typedef struct UiTerminal {
    UiWidget base;

    UiTerminalInput on_input;
    void* input_user;
    UiTerminalResize on_resize;
    void* resize_user;

    int columns, rows;
    int cell_width, cell_height, baseline;

    /*
     * One ring of rows covering scrollback and the screen together. The screen
     * is the last `rows` of it, so scrolling is moving an index rather than
     * copying a screenful of cells.
     */
    Cell* storage;
    int storage_rows;
    int top; /* index in the ring of the first scrollback row */
    int used; /* how many rows of the ring hold anything */
    int view; /* how far back the user has scrolled, in rows */

    int cursor_x, cursor_y;
    int saved_x, saved_y;
    int cursor_visible;

    unsigned char attributes;
    signed char foreground, background;

    /* Which rows of the visible screen have changed since the last paint.
     * A shell printing a line touches one of twenty-four. */
    int dirty_top, dirty_bottom;

    ParserState state;
    int parameters[MAX_PARAMETERS];
    int parameter_count;
    int private_marker;
} UiTerminal;

/* The row `index` rows down from the top of scrollback. */
static Cell* row_at(UiTerminal* terminal, int index)
{
    int const wrapped = (terminal->top + index) % terminal->storage_rows;
    return terminal->storage + (size_t)wrapped * terminal->columns;
}

/* The row `y` rows down the visible screen. */
static Cell* screen_row(UiTerminal* terminal, int y)
{
    return row_at(terminal, terminal->used - terminal->rows + y);
}

static void touch_rows(UiTerminal* terminal, int from, int to)
{
    if (from < 0)
        from = 0;
    if (to > terminal->rows)
        to = terminal->rows;
    if (from >= to)
        return;

    if (terminal->dirty_top >= terminal->dirty_bottom) {
        terminal->dirty_top = from;
        terminal->dirty_bottom = to;
        return;
    }
    if (from < terminal->dirty_top)
        terminal->dirty_top = from;
    if (to > terminal->dirty_bottom)
        terminal->dirty_bottom = to;
}

static void touch_all(UiTerminal* terminal)
{
    touch_rows(terminal, 0, terminal->rows);
}

/*
 * An erased cell takes the *current* background rather than the default one,
 * which is what makes `clear` inside a program that set a background colour
 * clear to that colour. Everything else about it goes back to plain: leaving
 * the old attributes behind means an erased inverse-video cell stays inverse
 * and the erase is visible as a stripe.
 */
static void erase_cells(UiTerminal* terminal, Cell* row, int from, int to)
{
    if (from < 0)
        from = 0;
    if (to > terminal->columns)
        to = terminal->columns;

    for (int x = from; x < to; ++x) {
        row[x].ch = ' ';
        row[x].attributes = 0;
        row[x].foreground = -1;
        row[x].background = terminal->background;
    }
}

static void clear_row(UiTerminal* terminal, Cell* row)
{
    erase_cells(terminal, row, 0, terminal->columns);
}

static void scroll_up(UiTerminal* terminal)
{
    if (terminal->used < terminal->storage_rows) {
        ++terminal->used;
    } else {
        /* The ring is full: the oldest scrollback row becomes the new blank
         * bottom line, which is why nothing is ever copied here. */
        terminal->top = (terminal->top + 1) % terminal->storage_rows;
    }
    clear_row(terminal, screen_row(terminal, terminal->rows - 1));
    touch_all(terminal);

    /* Scrolling pins the view to the bottom: new output is what the reader
     * wants to see, and staying where they were would be a screen that never
     * updates. */
    terminal->view = 0;
}

static void reset_grid(UiTerminal* terminal)
{
    terminal->used = terminal->rows;
    terminal->top = 0;
    terminal->view = 0;
    terminal->cursor_x = 0;
    terminal->cursor_y = 0;

    for (int i = 0; i < terminal->storage_rows; ++i)
        clear_row(terminal, terminal->storage + (size_t)i * terminal->columns);

    touch_all(terminal);
}

/* --- the parser --------------------------------------------------------------
 *
 * A small state machine rather than a switch over bytes, because escape
 * sequences arrive split across reads: a shell can write the ESC in one packet
 * and the rest in the next, and a parser that cannot be interrupted mid
 * sequence turns that into garbage on the screen every time it happens.
 */

static int parameter(UiTerminal* terminal, int index, int fallback)
{
    if (index >= terminal->parameter_count)
        return fallback;
    return terminal->parameters[index] > 0 ? terminal->parameters[index] : fallback;
}

static void apply_sgr(UiTerminal* terminal)
{
    if (terminal->parameter_count == 0) {
        terminal->attributes = 0;
        terminal->foreground = -1;
        terminal->background = -1;
        return;
    }

    for (int i = 0; i < terminal->parameter_count; ++i) {
        int const code = terminal->parameters[i];

        if (code == 0) {
            terminal->attributes = 0;
            terminal->foreground = -1;
            terminal->background = -1;
        } else if (code == 1) {
            terminal->attributes |= ATTR_BOLD;
        } else if (code == 2) {
            terminal->attributes |= ATTR_DIM;
        } else if (code == 7) {
            terminal->attributes |= ATTR_INVERSE;
        } else if (code == 22) {
            terminal->attributes &= (unsigned char)~(ATTR_BOLD | ATTR_DIM);
        } else if (code == 27) {
            terminal->attributes &= (unsigned char)~ATTR_INVERSE;
        } else if (code >= 30 && code <= 37) {
            terminal->foreground = (signed char)(code - 30);
        } else if (code == 39) {
            terminal->foreground = -1;
        } else if (code >= 40 && code <= 47) {
            terminal->background = (signed char)(code - 40);
        } else if (code == 49) {
            terminal->background = -1;
        } else if (code >= 90 && code <= 97) {
            terminal->foreground = (signed char)(code - 90 + 8);
        } else if (code >= 100 && code <= 107) {
            terminal->background = (signed char)(code - 100 + 8);
        }
        /* Anything else -- 256-colour, true colour, underline, blink -- is
         * ignored rather than guessed at. Ignoring an attribute shows the text
         * plainly; guessing shows it wrongly. */
    }
}

static void erase_in_display(UiTerminal* terminal, int mode)
{
    switch (mode) {
    case 0: /* cursor to end of screen */
        erase_cells(terminal, screen_row(terminal, terminal->cursor_y), terminal->cursor_x,
            terminal->columns);
        for (int y = terminal->cursor_y + 1; y < terminal->rows; ++y)
            clear_row(terminal, screen_row(terminal, y));
        break;
    case 1: /* start of screen to cursor */
        for (int y = 0; y < terminal->cursor_y; ++y)
            clear_row(terminal, screen_row(terminal, y));
        erase_cells(terminal, screen_row(terminal, terminal->cursor_y), 0, terminal->cursor_x + 1);
        break;
    default: /* the whole screen */
        for (int y = 0; y < terminal->rows; ++y)
            clear_row(terminal, screen_row(terminal, y));
        break;
    }
}

static void erase_in_line(UiTerminal* terminal, int mode)
{
    Cell* row = screen_row(terminal, terminal->cursor_y);
    int from = 0;
    int to = terminal->columns;

    if (mode == 0)
        from = terminal->cursor_x;
    else if (mode == 1)
        to = terminal->cursor_x + 1;

    erase_cells(terminal, row, from, to);
    touch_rows(terminal, terminal->cursor_y, terminal->cursor_y + 1);
}

static void execute_csi(UiTerminal* terminal, char final)
{
    switch (final) {
    case 'A': /* up */ terminal->cursor_y -= parameter(terminal, 0, 1); break;
    case 'B': /* down */ terminal->cursor_y += parameter(terminal, 0, 1); break;
    case 'C': /* right */ terminal->cursor_x += parameter(terminal, 0, 1); break;
    case 'D': /* left */ terminal->cursor_x -= parameter(terminal, 0, 1); break;
    case 'G': /* to column */ terminal->cursor_x = parameter(terminal, 0, 1) - 1; break;
    case 'd': /* to row */ terminal->cursor_y = parameter(terminal, 0, 1) - 1; break;
    case 'H':
    case 'f': /* to row and column */
        terminal->cursor_y = parameter(terminal, 0, 1) - 1;
        terminal->cursor_x = parameter(terminal, 1, 1) - 1;
        break;
    case 'J': erase_in_display(terminal, parameter(terminal, 0, 0)); break;
    case 'K': erase_in_line(terminal, parameter(terminal, 0, 0)); break;
    case 'm': apply_sgr(terminal); break;
    case 'h':
    case 'l':
        /* Only the one mode anything here sets: the cursor. */
        if (terminal->private_marker && parameter(terminal, 0, 0) == 25)
            terminal->cursor_visible = final == 'h';
        break;
    case 's':
        terminal->saved_x = terminal->cursor_x;
        terminal->saved_y = terminal->cursor_y;
        break;
    case 'u':
        terminal->cursor_x = terminal->saved_x;
        terminal->cursor_y = terminal->saved_y;
        break;
    default: break;
    }

    if (terminal->cursor_x < 0)
        terminal->cursor_x = 0;
    if (terminal->cursor_x >= terminal->columns)
        terminal->cursor_x = terminal->columns - 1;
    if (terminal->cursor_y < 0)
        terminal->cursor_y = 0;
    if (terminal->cursor_y >= terminal->rows)
        terminal->cursor_y = terminal->rows - 1;
}

static void put_character(UiTerminal* terminal, char c)
{
    if (terminal->cursor_x >= terminal->columns) {
        terminal->cursor_x = 0;
        ++terminal->cursor_y;
    }
    while (terminal->cursor_y >= terminal->rows) {
        scroll_up(terminal);
        --terminal->cursor_y;
    }

    Cell* cell = &screen_row(terminal, terminal->cursor_y)[terminal->cursor_x];
    cell->ch = (unsigned char)c;
    cell->attributes = terminal->attributes;
    cell->foreground = terminal->foreground;
    cell->background = terminal->background;
    touch_rows(terminal, terminal->cursor_y, terminal->cursor_y + 1);
    ++terminal->cursor_x;
}

static void feed_byte(UiTerminal* terminal, char c)
{
    switch (terminal->state) {
    case PARSE_ESCAPE:
        if (c == '[') {
            terminal->state = PARSE_CSI;
            terminal->parameter_count = 0;
            terminal->private_marker = 0;
            terminal->parameters[0] = 0;
        } else if (c == ']') {
            terminal->state = PARSE_OSC;
        } else {
            /* Everything else is a two-byte sequence we do not implement.
             * Swallowing it is right: printing the letter would put a stray
             * character on the screen every time. */
            terminal->state = PARSE_GROUND;
        }
        return;

    case PARSE_CSI:
        if (c == '?') {
            terminal->private_marker = 1;
            return;
        }
        if (c >= '0' && c <= '9') {
            if (terminal->parameter_count == 0)
                terminal->parameter_count = 1;
            if (terminal->parameter_count <= MAX_PARAMETERS) {
                int* value = &terminal->parameters[terminal->parameter_count - 1];
                *value = *value * 10 + (c - '0');
            }
            return;
        }
        if (c == ';') {
            /* An omitted parameter still counts. `ESC[;5m` is two of them, and
             * treating it as one puts the 5 in the wrong position -- which is
             * the difference between a colour and an attribute. */
            if (terminal->parameter_count == 0)
                terminal->parameter_count = 1;
            if (terminal->parameter_count < MAX_PARAMETERS)
                terminal->parameters[terminal->parameter_count++] = 0;
            return;
        }
        execute_csi(terminal, c);
        terminal->state = PARSE_GROUND;
        return;

    case PARSE_OSC:
        /* A window title, usually. Ends at BEL or at ESC backslash; there is
         * nothing to do with it, but it has to be consumed or the text lands
         * on the screen. (The single-byte 0x9c terminator is not tested for:
         * char is signed here, so it can never hold it, and nothing that talks
         * to a vt100 sends it.) */
        if (c == '\a')
            terminal->state = PARSE_GROUND;
        else if (c == 0x1b)
            terminal->state = PARSE_ESCAPE;
        return;

    case PARSE_GROUND: break;
    }

    switch (c) {
    case 0x1b: terminal->state = PARSE_ESCAPE; return;
    case '\r': terminal->cursor_x = 0; return;
    case '\n':
        ++terminal->cursor_y;
        while (terminal->cursor_y >= terminal->rows) {
            scroll_up(terminal);
            --terminal->cursor_y;
        }
        return;
    case '\b':
        if (terminal->cursor_x > 0)
            --terminal->cursor_x;
        return;
    case '\t':
        /* Eight-column stops, which is what everything assumes. */
        terminal->cursor_x = (terminal->cursor_x + 8) & ~7;
        if (terminal->cursor_x >= terminal->columns)
            terminal->cursor_x = terminal->columns - 1;
        return;
    case '\a': return; /* a bell nobody can hear */
    default: break;
    }

    if ((unsigned char)c < 32)
        return; /* an unhandled control character prints as nothing, not as junk */

    put_character(terminal, c);
}

void ui_terminal_feed(UiWidget* widget, const char* bytes, int length)
{
    UiTerminal* terminal = (UiTerminal*)widget;
    if (terminal->storage == NULL)
        return;

    /* The cursor is a filled block, so wherever it was has to be repainted
     * too -- otherwise it leaves a copy of itself behind on the row it left. */
    int const was = terminal->cursor_y;

    for (int i = 0; i < length; ++i)
        feed_byte(terminal, bytes[i]);

    touch_rows(terminal, was, was + 1);
    touch_rows(terminal, terminal->cursor_y, terminal->cursor_y + 1);

    if (terminal->dirty_top >= terminal->dirty_bottom)
        return;

    UiRect const changed = { 0, terminal->dirty_top * terminal->cell_height, widget->rect.width,
        (terminal->dirty_bottom - terminal->dirty_top) * terminal->cell_height };
    ui_widget_invalidate_rect(widget, changed);
}

/* --- painting ------------------------------------------------------------------ */

static unsigned colour_for(
    const UiTerminal* terminal, signed char index, unsigned fallback, int bold)
{
    if (index < 0)
        return fallback;

    int slot = index;
    /* Bold has meant "use the bright half of the palette" for forty years, and
     * a terminal that renders it as a heavier weight instead surprises every
     * program that uses it for colour. */
    if (bold && slot < 8)
        slot += 8;

    (void)terminal;
    return s_theme.palette[slot];
}

static void terminal_paint(UiWidget* widget, UiPainter* painter)
{
    UiTerminal* terminal = (UiTerminal*)widget;

    UiRect const bounds = { 0, 0, widget->rect.width, widget->rect.height };
    ui_fill_rect(painter, bounds, s_theme.background);

    UiFont* font = ui_font_for(painter->fonts, UI_TEXT_MONO);
    if (font == NULL)
        return;

    UiPainter cells = *painter;
    cells.font = font;

    /* Where the top of the visible area sits in the ring. Scrolling back moves
     * this and nothing else. */
    int const first = terminal->used - terminal->rows - terminal->view;

    /*
     * Only the rows and columns the clip can show. Walking the whole grid and
     * letting each cell be clipped away still costs two calls per cell, and
     * there are nineteen hundred of them.
     */
    int const clip_top = cells.clip.y - cells.origin_y;
    int const clip_bottom = clip_top + cells.clip.height;
    int const clip_left = cells.clip.x - cells.origin_x;
    int const clip_right = clip_left + cells.clip.width;

    int first_row = clip_top / terminal->cell_height;
    int last_row = (clip_bottom + terminal->cell_height - 1) / terminal->cell_height;
    int first_column = clip_left / terminal->cell_width;
    int last_column = (clip_right + terminal->cell_width - 1) / terminal->cell_width;

    if (first_row < 0)
        first_row = 0;
    if (last_row > terminal->rows)
        last_row = terminal->rows;
    if (first_column < 0)
        first_column = 0;
    if (last_column > terminal->columns)
        last_column = terminal->columns;

    for (int y = first_row; y < last_row; ++y) {
        int const index = first + y;
        if (index < 0 || index >= terminal->used)
            continue;

        Cell const* row = row_at(terminal, index);
        int const py = y * terminal->cell_height;

        for (int x = first_column; x < last_column; ++x) {
            Cell const cell = row[x];

            unsigned foreground = colour_for(
                terminal, cell.foreground, s_theme.foreground, (cell.attributes & ATTR_BOLD) != 0);
            unsigned background = colour_for(terminal, cell.background, s_theme.background, 0);

            if ((cell.attributes & ATTR_INVERSE) != 0) {
                unsigned const swap = foreground;
                foreground = background;
                background = swap;
            }

            int const px = x * terminal->cell_width;

            if (background != s_theme.background) {
                UiRect const box = { px, py, terminal->cell_width, terminal->cell_height };
                ui_fill_rect(&cells, box, background);
            }

            if (cell.ch == ' ' || cell.ch == 0)
                continue;

            char text[2] = { (char)cell.ch, '\0' };
            ui_text_draw_clipped(font, cells.pixels, cells.width, cells.height, cells.origin_x + px,
                cells.origin_y + py + terminal->baseline, text, foreground, cells.clip.x,
                cells.clip.y, cells.clip.x + cells.clip.width, cells.clip.y + cells.clip.height);
        }
    }

    /* The cursor, only where the live screen is: scrolled back into history
     * there is nothing for it to mark. */
    terminal->dirty_top = terminal->dirty_bottom = 0;

    if (terminal->cursor_visible && terminal->view == 0) {
        UiRect const caret = { terminal->cursor_x * terminal->cell_width,
            terminal->cursor_y * terminal->cell_height, terminal->cell_width,
            terminal->cell_height };
        ui_fill_rect_alpha(&cells, caret, s_theme.cursor, 190);
    }
}

/* --- geometry --------------------------------------------------------------- */

static void reshape(UiWidget* widget)
{
    UiTerminal* terminal = (UiTerminal*)widget;

    const UiFonts* fonts = ui_window_fonts(widget->window);
    UiFont* font = ui_font_for(fonts, UI_TEXT_MONO);
    if (font == NULL)
        return;

    /* Every glyph in a monospace face is the same width, so measuring one is
     * measuring all of them. */
    terminal->cell_width = ui_text_width(font, "M");
    terminal->cell_height = ui_font_line_height(font);
    terminal->baseline = ui_font_ascent(font);

    if (terminal->cell_width <= 0 || terminal->cell_height <= 0)
        return;

    int columns = widget->rect.width / terminal->cell_width;
    int rows = widget->rect.height / terminal->cell_height;

    if (columns < 1)
        columns = 1;
    if (rows < 1)
        rows = 1;
    if (columns > MAX_COLUMNS)
        columns = MAX_COLUMNS;
    if (rows > MAX_ROWS)
        rows = MAX_ROWS;

    if (columns == terminal->columns && rows == terminal->rows && terminal->storage != NULL)
        return;

    /*
     * Reflowing what is already on the screen into a new width is a genuinely
     * hard problem -- a wrapped line has to be found and rejoined -- and
     * getting it wrong looks far worse than starting clean. So a resize clears
     * the grid, and the shell redraws its prompt.
     */
    free(terminal->storage);
    terminal->columns = columns;
    terminal->rows = rows;
    terminal->storage_rows = SCROLLBACK_ROWS > rows ? SCROLLBACK_ROWS : rows;
    terminal->storage = calloc((size_t)terminal->storage_rows * (size_t)columns, sizeof(Cell));

    if (terminal->storage == NULL) {
        terminal->columns = 0;
        terminal->rows = 0;
        return;
    }

    reset_grid(terminal);

    if (terminal->on_resize != NULL)
        terminal->on_resize(columns, rows, terminal->resize_user);
}

static void terminal_layout(UiWidget* widget)
{
    reshape(widget);
}

static void terminal_measure(UiWidget* widget, int* width, int* height)
{
    /* A terminal has no natural size; it takes what it is given. Eighty by
     * twenty-four is only what it asks for when nothing else has an opinion. */
    const UiFonts* fonts = ui_window_fonts(widget->window);
    UiFont* font = ui_font_for(fonts, UI_TEXT_MONO);

    if (font == NULL) {
        *width = 640;
        *height = 384;
        return;
    }

    *width = ui_text_width(font, "M") * 80;
    *height = ui_font_line_height(font) * 24;
}

/* --- input ------------------------------------------------------------------- */

static void send(UiTerminal* terminal, const char* bytes, int length)
{
    if (terminal->on_input != NULL)
        terminal->on_input(bytes, length, terminal->input_user);
}

static int terminal_on_key(UiWidget* widget, const UiKeyEvent* event)
{
    UiTerminal* terminal = (UiTerminal*)widget;
    if (!event->pressed)
        return 1;

    /* Any keystroke returns to the live screen. Typing into history and
     * watching nothing happen is the single most confusing thing a terminal
     * with scrollback can do. */
    if (terminal->view != 0) {
        terminal->view = 0;
        touch_all(terminal);
        ui_widget_invalidate(widget);
    }

    /* The arrows and friends, as the escape sequences every program expects.
     * A shell with line editing reads exactly these. */
    switch (event->keycode) {
    case KEY_UP: send(terminal, "\x1b[A", 3); return 1;
    case KEY_DOWN: send(terminal, "\x1b[B", 3); return 1;
    case KEY_RIGHT: send(terminal, "\x1b[C", 3); return 1;
    case KEY_LEFT: send(terminal, "\x1b[D", 3); return 1;
    case KEY_HOME: send(terminal, "\x1b[H", 3); return 1;
    case KEY_END: send(terminal, "\x1b[F", 3); return 1;
    case KEY_DELETE: send(terminal, "\x1b[3~", 4); return 1;
    case KEY_PAGE_UP:
    case KEY_PAGE_DOWN: {
        /* Scrollback rather than something sent to the shell, which is what
         * everyone means by these in a terminal. */
        int const direction = event->keycode == KEY_PAGE_UP ? 1 : -1;
        terminal->view += direction * (terminal->rows - 1);

        int const furthest = terminal->used - terminal->rows;
        if (terminal->view > furthest)
            terminal->view = furthest;
        if (terminal->view < 0)
            terminal->view = 0;

        touch_all(terminal);
        ui_widget_invalidate(widget);
        return 1;
    }
    default: break;
    }

    if (event->codepoint == 0)
        return 1;

    char const byte = (char)event->codepoint;
    send(terminal, &byte, 1);
    return 1;
}

static int terminal_on_mouse(UiWidget* widget, const UiMouseEvent* event)
{
    UiTerminal* terminal = (UiTerminal*)widget;

    if (event->buttons != 0)
        ui_window_focus(widget->window, widget);

    if (event->wheel != 0) {
        terminal->view += event->wheel * 3;

        int const furthest = terminal->used - terminal->rows;
        if (terminal->view > furthest)
            terminal->view = furthest;
        if (terminal->view < 0)
            terminal->view = 0;

        touch_all(terminal);
        ui_widget_invalidate(widget);
    }
    return 1;
}

static void terminal_destroy(UiWidget* widget)
{
    free(((UiTerminal*)widget)->storage);
}

static const UiWidgetClass TERMINAL_CLASS = {
    .name = "terminal",
    .size = sizeof(UiTerminal),
    .paint = terminal_paint,
    .on_mouse = terminal_on_mouse,
    .on_key = terminal_on_key,
    .measure = terminal_measure,
    .layout = terminal_layout,
    .destroy = terminal_destroy,
};

UiWidget* ui_terminal_create(UiTerminalInput on_input, void* user)
{
    UiWidget* widget = ui_widget_create(&TERMINAL_CLASS);
    if (widget == NULL)
        return NULL;

    UiTerminal* terminal = (UiTerminal*)widget;
    terminal->on_input = on_input;
    terminal->input_user = user;
    terminal->foreground = -1;
    terminal->background = -1;
    terminal->cursor_visible = 1;

    widget->expand_x = 1;
    widget->expand_y = 1;
    return widget;
}

void ui_terminal_set_resize_handler(UiWidget* widget, UiTerminalResize on_resize, void* user)
{
    UiTerminal* terminal = (UiTerminal*)widget;
    terminal->on_resize = on_resize;
    terminal->resize_user = user;
}

int ui_terminal_columns(const UiWidget* widget)
{
    return ((const UiTerminal*)widget)->columns;
}

int ui_terminal_rows(const UiWidget* widget)
{
    return ((const UiTerminal*)widget)->rows;
}

int ui_terminal_character_at(const UiWidget* widget, int x, int y)
{
    UiTerminal* terminal = (UiTerminal*)(UiWidget*)widget;
    if (terminal->storage == NULL)
        return 0;
    if (x < 0 || x >= terminal->columns || y < 0 || y >= terminal->rows)
        return 0;
    return screen_row(terminal, y)[x].ch;
}

int ui_terminal_cursor_x(const UiWidget* widget)
{
    return ((const UiTerminal*)widget)->cursor_x;
}

int ui_terminal_cursor_y(const UiWidget* widget)
{
    return ((const UiTerminal*)widget)->cursor_y;
}
