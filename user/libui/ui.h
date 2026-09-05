/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- a retained-mode widget toolkit.
 *
 * Retained rather than immediate: widgets exist between frames, they are laid
 * out once when something changes rather than every frame, and only the parts
 * that changed are repainted. That costs a tree and a layout pass, and buys
 * the thing an immediate-mode library can never quite have -- a window that
 * sits there costing nothing, and a text field that remembers where the caret
 * is without the application being asked.
 *
 * The shape is deliberately familiar. A widget has a class with a handful of
 * virtual functions, a parent, children, and a rectangle it was given by its
 * parent's layout. Anyone who has used Qt, GTK or LibGUI knows this already,
 * and a toolkit is not the place to be original.
 *
 * Everything here is C, because the libc is C and a toolkit that needed C++
 * would need an ABI, a runtime, and exceptions this kernel does not have.
 */

#pragma once

#include "text.h"

#include <stddef.h>

typedef struct UiWindow UiWindow;
typedef struct UiWidget UiWidget;

/* --- geometry -------------------------------------------------------------- */

typedef struct UiRect {
    int x, y, width, height;
} UiRect;

static inline int ui_rect_contains(UiRect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}

/* --- the theme -------------------------------------------------------------
 *
 * One struct, so that restyling the desktop is editing one place rather than
 * hunting for hex constants. Colours are 0xRRGGBB; the surface knows how to
 * pack them for the screen it is on.
 */

typedef struct UiTheme {
    unsigned window_background;
    unsigned surface;
    unsigned surface_raised;

    unsigned text;
    unsigned text_dim;
    unsigned text_on_accent;

    unsigned accent;
    unsigned accent_hover;
    unsigned accent_pressed;

    unsigned border;
    unsigned border_focus;

    unsigned danger;

    int corner_radius;
    int padding;
    int spacing;
} UiTheme;

const UiTheme* ui_theme(void);
void ui_theme_set(const UiTheme* theme);

/* --- painting --------------------------------------------------------------
 *
 * A painter is a surface plus a clip rectangle plus an origin. Widgets draw in
 * their own coordinates and the painter puts them where they belong, so a
 * widget never needs to know where on the screen it is.
 */

typedef struct UiPainter {
    unsigned* pixels;
    int width, height; /* of the whole surface */

    int origin_x, origin_y; /* added to every coordinate */
    UiRect clip; /* in surface coordinates */

    UiFont* font;
    UiFont* bold;
} UiPainter;

void ui_fill_rect(UiPainter*, UiRect, unsigned colour);
void ui_fill_rounded(UiPainter*, UiRect, int radius, unsigned colour);
void ui_stroke_rect(UiPainter*, UiRect, unsigned colour);
void ui_stroke_rounded(UiPainter*, UiRect, int radius, unsigned colour);

typedef enum UiAlign {
    UI_ALIGN_LEFT,
    UI_ALIGN_CENTRE,
    UI_ALIGN_RIGHT,
} UiAlign;

/* Draws vertically centred in `bounds`, horizontally per `align`. */
void ui_draw_text(UiPainter*, UiRect bounds, const char* text, unsigned colour, UiAlign align);

/* A painter clipped to a child's rectangle, with the origin moved to it. */
UiPainter ui_painter_for(const UiPainter*, UiRect child);

/* --- events ---------------------------------------------------------------- */

typedef struct UiMouseEvent {
    int x, y; /* in the receiving widget's coordinates */
    unsigned buttons;
    int wheel;
    int entered, left; /* synthesised by the window from movement */
} UiMouseEvent;

typedef struct UiKeyEvent {
    unsigned keycode;
    unsigned codepoint;
    unsigned modifiers;
    int pressed;
} UiKeyEvent;

/* --- widgets ---------------------------------------------------------------- */

typedef struct UiWidgetClass {
    const char* name;
    size_t size; /* so the base can allocate a subclass */

    void (*paint)(UiWidget*, UiPainter*);

    /* Non-zero when the event was consumed. An unconsumed event goes on to
     * the parent, which is what makes a click on a label inside a button
     * still press the button. */
    int (*on_mouse)(UiWidget*, const UiMouseEvent*);
    int (*on_key)(UiWidget*, const UiKeyEvent*);

    /* How big this widget wants to be, before layout gives it a size. */
    void (*measure)(UiWidget*, int* width, int* height);

    /* Position the children inside `self->rect`. Leaf widgets leave it null. */
    void (*layout)(UiWidget*);

    void (*destroy)(UiWidget*);
} UiWidgetClass;

struct UiWidget {
    const UiWidgetClass* klass;

    UiWindow* window;
    UiWidget* parent;
    UiWidget** children;
    int child_count, child_capacity;

    UiRect rect; /* in window coordinates, filled in by layout */

    int fixed_width, fixed_height; /* 0 means "ask measure" */
    int expand_x, expand_y; /* takes a share of the leftover space */

    int visible;
    int enabled;
    int hovered;

    void* user;
};

/* Allocates a widget of `klass->size`, zeroed, with the base fields set. */
UiWidget* ui_widget_create(const UiWidgetClass* klass);
void ui_widget_destroy(UiWidget*);

int ui_widget_add(UiWidget* parent, UiWidget* child);

/* Marks the widget as needing to be painted again. Cheap and idempotent:
 * everything that changes anything calls it, and the window coalesces. */
void ui_widget_invalidate(UiWidget*);

/* The deepest visible widget containing the point, in window coordinates. */
UiWidget* ui_widget_at(UiWidget* root, int x, int y);

void ui_widget_measure(UiWidget*, int* width, int* height);

/* --- containers ------------------------------------------------------------- */

typedef enum UiOrientation {
    UI_VERTICAL,
    UI_HORIZONTAL,
} UiOrientation;

/*
 * The only container that matters. Children are laid out along one axis at
 * their measured size, with any leftover space divided between those that
 * asked to expand -- which covers almost every layout anyone draws on a
 * whiteboard, and nests for the rest.
 */
UiWidget* ui_box_create(UiOrientation);
void ui_box_set_spacing(UiWidget*, int spacing);
void ui_box_set_padding(UiWidget*, int padding);

/* --- controls ---------------------------------------------------------------- */

UiWidget* ui_label_create(const char* text);
void ui_label_set_text(UiWidget*, const char* text);
void ui_label_set_align(UiWidget*, UiAlign);
void ui_label_set_colour(UiWidget*, unsigned colour);

typedef void (*UiAction)(UiWidget*, void* user);

UiWidget* ui_button_create(const char* text, UiAction on_click, void* user);
void ui_button_set_text(UiWidget*, const char* text);

UiWidget* ui_checkbox_create(const char* text, int checked);
int ui_checkbox_is_checked(const UiWidget*);
void ui_checkbox_set_checked(UiWidget*, int checked);

UiWidget* ui_textfield_create(const char* text);
const char* ui_textfield_text(const UiWidget*);
void ui_textfield_set_text(UiWidget*, const char* text);

/* --- windows ------------------------------------------------------------------
 *
 * A window owns the connection to the server, the shared buffer it draws into,
 * and the widget tree. Everything a client has to do by hand today -- FIFOs,
 * hello, mapping the buffer, damage messages -- happens in here once.
 */

UiWindow* ui_window_create(const char* title, int width, int height);
void ui_window_destroy(UiWindow*);

void ui_window_set_root(UiWindow*, UiWidget* root);
UiWidget* ui_window_root(UiWindow*);

void ui_window_set_title(UiWindow*, const char* title);

/* Keyboard focus. A window with no focused widget sends keys nowhere. */
void ui_window_focus(UiWindow*, UiWidget*);
UiWidget* ui_window_focused(UiWindow*);

void ui_window_invalidate(UiWindow*);

/* Runs until the window is closed. Returns 0 on a clean exit. */
int ui_window_run(UiWindow*);

/* Asks the loop to stop, from inside a callback. */
void ui_window_close(UiWindow*);
