/* SPDX-License-Identifier: GPL-3.0-or-later */
/* shit os 2 -- labels, buttons, checkboxes, text fields. */

#include <shitos/abi/input.h>

#include "ui.h"

#include <stdlib.h>
#include <string.h>

#define TEXT_MAX 256

/* --- label ------------------------------------------------------------------ */

typedef struct UiLabel {
    UiWidget base;
    char text[TEXT_MAX];
    UiAlign align;
    unsigned colour;
    int has_colour;
} UiLabel;

static void label_measure(UiWidget* widget, int* width, int* height)
{
    UiLabel* label = (UiLabel*)widget;

    /*
     * Measured without a font, because a widget is measured before it is
     * painted and the font lives on the painter. Eight pixels per character
     * is close enough for a layout that then gets the real width when it
     * draws -- and every label here is in a box that gives it the space
     * anyway.
     */
    int const length = (int)strlen(label->text);
    *width = length * 8;
    *height = 20;
}

static void label_paint(UiWidget* widget, UiPainter* painter)
{
    UiLabel* label = (UiLabel*)widget;
    UiRect const bounds = { 0, 0, widget->rect.width, widget->rect.height };

    unsigned const colour = label->has_colour
        ? label->colour
        : (widget->enabled ? ui_theme()->text : ui_theme()->text_dim);

    ui_draw_text(painter, bounds, label->text, colour, label->align);
}

static const UiWidgetClass LABEL_CLASS = {
    .name = "label",
    .size = sizeof(UiLabel),
    .paint = label_paint,
    .measure = label_measure,
};

UiWidget* ui_label_create(const char* text)
{
    UiWidget* widget = ui_widget_create(&LABEL_CLASS);
    if (widget == NULL)
        return NULL;
    ui_label_set_text(widget, text);
    return widget;
}

void ui_label_set_text(UiWidget* widget, const char* text)
{
    UiLabel* label = (UiLabel*)widget;
    strncpy(label->text, text != NULL ? text : "", TEXT_MAX - 1);
    label->text[TEXT_MAX - 1] = '\0';
    ui_widget_invalidate(widget);
}

void ui_label_set_align(UiWidget* widget, UiAlign align)
{
    ((UiLabel*)widget)->align = align;
    ui_widget_invalidate(widget);
}

void ui_label_set_colour(UiWidget* widget, unsigned colour)
{
    UiLabel* label = (UiLabel*)widget;
    label->colour = colour;
    label->has_colour = 1;
    ui_widget_invalidate(widget);
}

/* --- button ------------------------------------------------------------------ */

typedef struct UiButton {
    UiWidget base;
    char text[TEXT_MAX];
    UiAction on_click;
    void* user;
    int pressed;
} UiButton;

static void button_measure(UiWidget* widget, int* width, int* height)
{
    UiButton* button = (UiButton*)widget;
    *width = (int)strlen(button->text) * 8 + ui_theme()->padding * 3;
    *height = 28;
}

static void button_paint(UiWidget* widget, UiPainter* painter)
{
    UiButton* button = (UiButton*)widget;
    const UiTheme* theme = ui_theme();
    UiRect const bounds = { 0, 0, widget->rect.width, widget->rect.height };

    unsigned background = theme->accent;
    if (!widget->enabled)
        background = theme->surface_raised;
    else if (button->pressed)
        background = theme->accent_pressed;
    else if (widget->hovered)
        background = theme->accent_hover;

    ui_fill_rounded(painter, bounds, theme->corner_radius, background);

    /* A pressed button gets no border and an enabled one does, which is most
     * of what makes it read as going in and coming out again. */
    if (!button->pressed)
        ui_stroke_rounded(painter, bounds, theme->corner_radius, theme->border);

    unsigned const ink = widget->enabled ? theme->text_on_accent : theme->text_dim;
    ui_draw_text(painter, bounds, button->text, ink, UI_ALIGN_CENTRE);
}

static int button_on_mouse(UiWidget* widget, const UiMouseEvent* event)
{
    UiButton* button = (UiButton*)widget;
    if (!widget->enabled)
        return 1;

    int const down = (event->buttons & MOUSE_BUTTON_LEFT) != 0;

    if (down && !button->pressed) {
        button->pressed = 1;
        ui_widget_invalidate(widget);
    } else if (!down && button->pressed) {
        button->pressed = 0;
        ui_widget_invalidate(widget);

        /* Only if the release happened over the button. Dragging off a button
         * and letting go is how everyone cancels a click they regret. */
        UiRect const bounds = { 0, 0, widget->rect.width, widget->rect.height };
        if (ui_rect_contains(bounds, event->x, event->y) && button->on_click != NULL)
            button->on_click(widget, button->user);
    }

    return 1;
}

static const UiWidgetClass BUTTON_CLASS = {
    .name = "button",
    .size = sizeof(UiButton),
    .paint = button_paint,
    .on_mouse = button_on_mouse,
    .measure = button_measure,
};

UiWidget* ui_button_create(const char* text, UiAction on_click, void* user)
{
    UiWidget* widget = ui_widget_create(&BUTTON_CLASS);
    if (widget == NULL)
        return NULL;

    UiButton* button = (UiButton*)widget;
    button->on_click = on_click;
    button->user = user;
    ui_button_set_text(widget, text);
    return widget;
}

void ui_button_set_text(UiWidget* widget, const char* text)
{
    UiButton* button = (UiButton*)widget;
    strncpy(button->text, text != NULL ? text : "", TEXT_MAX - 1);
    button->text[TEXT_MAX - 1] = '\0';
    ui_widget_invalidate(widget);
}

/* --- checkbox ----------------------------------------------------------------- */

typedef struct UiCheckbox {
    UiWidget base;
    char text[TEXT_MAX];
    int checked;
    int pressed;
} UiCheckbox;

#define CHECKBOX_SIZE 16

static void checkbox_measure(UiWidget* widget, int* width, int* height)
{
    UiCheckbox* box = (UiCheckbox*)widget;
    *width = CHECKBOX_SIZE + ui_theme()->spacing + (int)strlen(box->text) * 8;
    *height = 24;
}

static void checkbox_paint(UiWidget* widget, UiPainter* painter)
{
    UiCheckbox* box = (UiCheckbox*)widget;
    const UiTheme* theme = ui_theme();

    UiRect const mark
        = { 0, (widget->rect.height - CHECKBOX_SIZE) / 2, CHECKBOX_SIZE, CHECKBOX_SIZE };

    ui_fill_rounded(painter, mark, 3, box->checked ? theme->accent : theme->surface);
    ui_stroke_rounded(painter, mark, 3, widget->hovered ? theme->border_focus : theme->border);

    if (box->checked) {
        /* A tick, drawn as two strokes. Small enough that the exact shape
         * matters less than it being obviously not a filled square. */
        for (int i = 0; i < 4; ++i) {
            UiRect const p = { mark.x + 4 + i, mark.y + 7 + i, 2, 2 };
            ui_fill_rect(painter, p, theme->text_on_accent);
        }
        for (int i = 0; i < 5; ++i) {
            UiRect const p = { mark.x + 7 + i, mark.y + 10 - i, 2, 2 };
            ui_fill_rect(painter, p, theme->text_on_accent);
        }
    }

    UiRect const label = { CHECKBOX_SIZE + theme->spacing, 0,
        widget->rect.width - CHECKBOX_SIZE - theme->spacing, widget->rect.height };
    ui_draw_text(
        painter, label, box->text, widget->enabled ? theme->text : theme->text_dim, UI_ALIGN_LEFT);
}

static int checkbox_on_mouse(UiWidget* widget, const UiMouseEvent* event)
{
    UiCheckbox* box = (UiCheckbox*)widget;
    if (!widget->enabled)
        return 1;

    int const down = (event->buttons & MOUSE_BUTTON_LEFT) != 0;

    if (down && !box->pressed) {
        box->pressed = 1;
    } else if (!down && box->pressed) {
        box->pressed = 0;
        UiRect const bounds = { 0, 0, widget->rect.width, widget->rect.height };
        if (ui_rect_contains(bounds, event->x, event->y)) {
            box->checked = !box->checked;
            ui_widget_invalidate(widget);
        }
    }
    return 1;
}

static const UiWidgetClass CHECKBOX_CLASS = {
    .name = "checkbox",
    .size = sizeof(UiCheckbox),
    .paint = checkbox_paint,
    .on_mouse = checkbox_on_mouse,
    .measure = checkbox_measure,
};

UiWidget* ui_checkbox_create(const char* text, int checked)
{
    UiWidget* widget = ui_widget_create(&CHECKBOX_CLASS);
    if (widget == NULL)
        return NULL;

    UiCheckbox* box = (UiCheckbox*)widget;
    box->checked = checked != 0;
    strncpy(box->text, text != NULL ? text : "", TEXT_MAX - 1);
    return widget;
}

int ui_checkbox_is_checked(const UiWidget* widget)
{
    return ((const UiCheckbox*)widget)->checked;
}

void ui_checkbox_set_checked(UiWidget* widget, int checked)
{
    ((UiCheckbox*)widget)->checked = checked != 0;
    ui_widget_invalidate(widget);
}

/* --- text field ---------------------------------------------------------------- */

typedef struct UiTextField {
    UiWidget base;
    char text[TEXT_MAX];
    int length;
    int caret;
} UiTextField;

static void textfield_measure(UiWidget* widget, int* width, int* height)
{
    (void)widget;
    *width = 160;
    *height = 28;
}

static void textfield_paint(UiWidget* widget, UiPainter* painter)
{
    UiTextField* field = (UiTextField*)widget;
    const UiTheme* theme = ui_theme();
    UiRect const bounds = { 0, 0, widget->rect.width, widget->rect.height };

    int const focused = ui_window_focused(widget->window) == widget;

    ui_fill_rounded(painter, bounds, theme->corner_radius, theme->window_background);
    ui_stroke_rounded(
        painter, bounds, theme->corner_radius, focused ? theme->border_focus : theme->border);

    UiRect const inner
        = { theme->padding, 0, widget->rect.width - theme->padding * 2, widget->rect.height };
    ui_draw_text(painter, inner, field->text, theme->text, UI_ALIGN_LEFT);

    if (focused && painter->font != NULL) {
        /* The caret sits after the text up to the insertion point, so it has
         * to be measured rather than counted -- proportional text has no
         * character width to multiply. */
        char before[TEXT_MAX];
        int const count = field->caret < TEXT_MAX - 1 ? field->caret : TEXT_MAX - 1;
        memcpy(before, field->text, (size_t)count);
        before[count] = '\0';

        int const offset = ui_text_width(painter->font, before);
        int const height = ui_font_line_height(painter->font);
        UiRect const caret
            = { inner.x + offset, (widget->rect.height - height) / 2 + 2, 1, height - 2 };
        ui_fill_rect(painter, caret, theme->text);
    }
}

static int textfield_on_mouse(UiWidget* widget, const UiMouseEvent* event)
{
    if ((event->buttons & MOUSE_BUTTON_LEFT) != 0) {
        ui_window_focus(widget->window, widget);
        ui_widget_invalidate(widget);
    }
    return 1;
}

static int textfield_on_key(UiWidget* widget, const UiKeyEvent* event)
{
    UiTextField* field = (UiTextField*)widget;
    if (!event->pressed)
        return 1;

    switch (event->keycode) {
    case KEY_LEFT:
        if (field->caret > 0)
            --field->caret;
        ui_widget_invalidate(widget);
        return 1;
    case KEY_RIGHT:
        if (field->caret < field->length)
            ++field->caret;
        ui_widget_invalidate(widget);
        return 1;
    case KEY_HOME:
        field->caret = 0;
        ui_widget_invalidate(widget);
        return 1;
    case KEY_END:
        field->caret = field->length;
        ui_widget_invalidate(widget);
        return 1;
    default: break;
    }

    if (event->codepoint == '\b') {
        if (field->caret > 0) {
            memmove(&field->text[field->caret - 1], &field->text[field->caret],
                (size_t)(field->length - field->caret + 1));
            --field->caret;
            --field->length;
            ui_widget_invalidate(widget);
        }
        return 1;
    }

    /* Printable ASCII only. Anything else -- a control character, a function
     * key -- is not text and is left for the window to do something with. */
    if (event->codepoint < 32 || event->codepoint > 126)
        return 0;

    if (field->length + 1 >= TEXT_MAX)
        return 1;

    memmove(&field->text[field->caret + 1], &field->text[field->caret],
        (size_t)(field->length - field->caret + 1));
    field->text[field->caret] = (char)event->codepoint;
    ++field->caret;
    ++field->length;
    ui_widget_invalidate(widget);
    return 1;
}

static const UiWidgetClass TEXTFIELD_CLASS = {
    .name = "textfield",
    .size = sizeof(UiTextField),
    .paint = textfield_paint,
    .on_mouse = textfield_on_mouse,
    .on_key = textfield_on_key,
    .measure = textfield_measure,
};

UiWidget* ui_textfield_create(const char* text)
{
    UiWidget* widget = ui_widget_create(&TEXTFIELD_CLASS);
    if (widget == NULL)
        return NULL;
    ui_textfield_set_text(widget, text);
    return widget;
}

const char* ui_textfield_text(const UiWidget* widget)
{
    return ((const UiTextField*)widget)->text;
}

void ui_textfield_set_text(UiWidget* widget, const char* text)
{
    UiTextField* field = (UiTextField*)widget;
    strncpy(field->text, text != NULL ? text : "", TEXT_MAX - 1);
    field->text[TEXT_MAX - 1] = '\0';
    field->length = (int)strlen(field->text);
    field->caret = field->length;
    ui_widget_invalidate(widget);
}
