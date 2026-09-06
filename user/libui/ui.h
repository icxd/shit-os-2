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
    /*
     * Surfaces, from furthest back to nearest front. Each step is a deliberate
     * lift rather than a hand-picked hex: a control has to read as sitting on
     * the panel it is in, and an input has to read as a hole in that panel.
     */
    unsigned background; /* the window itself */
    unsigned surface; /* a panel on it */
    unsigned surface_raised; /* a control on the panel */
    unsigned surface_sunken; /* somewhere text is typed into */

    /* Text, at three deliberate contrast steps against `surface`. */
    unsigned text; /* what you are meant to read */
    unsigned text_dim; /* labels, captions, things that support it */
    unsigned text_faint; /* disabled, and placeholders */
    unsigned text_on_accent;

    unsigned accent;
    unsigned accent_hover;
    unsigned accent_pressed;

    unsigned border; /* the edge of a surface */
    unsigned border_strong; /* the edge of something interactive */
    unsigned focus_ring; /* around whatever the keyboard is aimed at */

    unsigned danger;
    unsigned success;

    int corner_radius;
    int corner_radius_small;
    int padding;

    /*
     * A scale, not a number. Space is what tells a reader which things belong
     * together, and it can only do that if the distance between a label and
     * its own control is visibly smaller than the distance to the next group.
     * One `spacing` used everywhere -- which is what this was -- makes a
     * screen where everything is equally related to everything else, and that
     * is most of what "unpolished" means.
     */
    int spacing_tight; /* a label and the control it names */
    int spacing; /* neighbours inside one group */
    int spacing_section; /* one group and the next */

    /* How far a control and a panel sit above what is behind them. */
    int elevation_control;
    int elevation_panel;
} UiTheme;

const UiTheme* ui_theme(void);
void ui_theme_set(const UiTheme* theme);

/* --- painting --------------------------------------------------------------
 *
 * A painter is a surface plus a clip rectangle plus an origin. Widgets draw in
 * their own coordinates and the painter puts them where they belong, so a
 * widget never needs to know where on the screen it is.
 */

/*
 * The faces an interface needs, opened once and shared. A style is a role
 * rather than a size: "this is a heading" survives a change to the scale,
 * "this is 18 pixels" does not.
 */
typedef struct UiFonts {
    UiFont* body;
    UiFont* strong; /* body weight-for-weight, bold */
    UiFont* small;
    UiFont* heading;
    UiFont* mono;
} UiFonts;

typedef enum UiTextStyle {
    UI_TEXT_BODY,
    UI_TEXT_STRONG,
    UI_TEXT_SMALL,
    UI_TEXT_HEADING,
    UI_TEXT_MONO,
} UiTextStyle;

/* Opens every face in the set from `directory`. Any that will not open is left
 * null and falls back to the body face, so a missing bold is a flat-looking
 * interface rather than a crash. */
int ui_fonts_open(UiFonts*, const char* directory);
void ui_fonts_close(UiFonts*);

UiFont* ui_font_for(const UiFonts*, UiTextStyle);

typedef struct UiPainter {
    unsigned* pixels;
    int width, height; /* of the whole surface */

    int origin_x, origin_y; /* added to every coordinate */
    UiRect clip; /* in surface coordinates */

    /*
     * The face this widget draws with, and the whole set it may pick from. One
     * size and one weight throughout is most of what makes an interface look
     * like a test harness, so a widget that wants a heading takes it from here
     * rather than being handed a single font and making do.
     */
    UiFont* font;
    const UiFonts* fonts;
} UiPainter;

void ui_fill_rect(UiPainter*, UiRect, unsigned colour);
void ui_fill_rect_alpha(UiPainter*, UiRect, unsigned colour, unsigned alpha);
void ui_fill_rounded(UiPainter*, UiRect, int radius, unsigned colour);
void ui_fill_rounded_alpha(UiPainter*, UiRect, int radius, unsigned colour, unsigned alpha);
void ui_stroke_rect(UiPainter*, UiRect, unsigned colour);
void ui_stroke_rounded(UiPainter*, UiRect, int radius, unsigned colour);

/* The same at partial coverage. A focus ring wants to read as a halo rather
 * than as a second border, and the difference is entirely the alpha. */
void ui_stroke_rounded_alpha(UiPainter*, UiRect, int radius, unsigned colour, unsigned alpha);

/*
 * A soft shadow under a surface, drawn before the surface itself. `elevation`
 * is how far the thing is meant to sit above what is behind it, in pixels; two
 * or three for a control, six or so for a panel.
 */
void ui_drop_shadow(UiPainter*, UiRect, int radius, int elevation);

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

/* The same, for one part of a widget, in the widget's own coordinates. */
void ui_widget_invalidate_rect(UiWidget*, UiRect);

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

/* A box that draws itself: a white card with a hairline and a soft shadow.
 * Content on white sitting on a barely-grey window is most of the look. */
UiWidget* ui_panel_create(UiOrientation);

/* --- controls ---------------------------------------------------------------- */

UiWidget* ui_label_create(const char* text);
void ui_label_set_text(UiWidget*, const char* text);
void ui_label_set_align(UiWidget*, UiAlign);
void ui_label_set_colour(UiWidget*, unsigned colour);
void ui_label_set_style(UiWidget*, UiTextStyle);

typedef void (*UiAction)(UiWidget*, void* user);

UiWidget* ui_button_create(const char* text, UiAction on_click, void* user);
void ui_button_set_text(UiWidget*, const char* text);

/* The one the window would do if you pressed return: filled with the accent
 * rather than white. At most one per window, or it stops meaning anything. */
void ui_button_set_default(UiWidget*, int is_default);

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

/*
 * The faces the window draws with. A widget needs these when it is *measured*,
 * not only when it is painted -- guessing a width from the character count
 * gives a box that the real text then does not fit in, which is exactly how
 * "Disabled and checked" first came out as "Disabled and check".
 */
const UiFonts* ui_window_fonts(UiWindow*);

void ui_window_set_title(UiWindow*, const char* title);

/* Keyboard focus. A window with no focused widget sends keys nowhere. */
void ui_window_focus(UiWindow*, UiWidget*);
UiWidget* ui_window_focused(UiWindow*);

void ui_window_invalidate(UiWindow*);

/* Just this rectangle of it, in window coordinates. */
void ui_window_damage(UiWindow*, int x, int y, int width, int height);

/* Runs until the window is closed. Returns 0 on a clean exit. */
int ui_window_run(UiWindow*);

/*
 * The same, watching one more descriptor alongside the window's own. An
 * application whose work arrives on a pipe, a socket or a pseudo-terminal
 * cannot use ui_window_run: it would have to choose between blocking on the
 * window and blocking on its own input.
 *
 * `on_ready` is called whenever `extra` has something to read. Returning
 * non-zero from it closes the window, which is how a terminal notices that its
 * shell has exited.
 */
typedef int (*UiWindowReady)(int fd, void* user);
/* One turn of the event loop, for an application that has a loop of its own.
 * Returns zero once the window has been closed. */
int ui_window_step(UiWindow*, int timeout_ms);

int ui_window_pump(UiWindow*, int extra, UiWindowReady on_ready, void* user);

/* Asks the loop to stop, from inside a callback. */
void ui_window_close(UiWindow*);
