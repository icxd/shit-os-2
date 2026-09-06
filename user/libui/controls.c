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
    UiTextStyle style;
    unsigned colour;
    int has_colour;
} UiLabel;

/*
 * Measured with the same face that will draw it. A width guessed from the
 * character count is a box the text then does not fit in, and proportional
 * text has no character width to guess with in the first place.
 *
 * A widget with no window yet -- one built before it was added to a tree --
 * falls back to an estimate, because there is nothing better to be had and a
 * layout that happens again the moment it has a window costs nothing.
 */
static void measure_text(
    UiWidget* widget, UiTextStyle style, const char* text, int* width, int* height)
{
    const UiFonts* fonts = ui_window_fonts(widget->window);
    UiFont* font = ui_font_for(fonts, style);

    if (font == NULL) {
        *width = (int)strlen(text) * 8;
        *height = 20;
        return;
    }

    *width = ui_text_width(font, text);
    *height = ui_font_line_height(font);
}

static void label_measure(UiWidget* widget, int* width, int* height)
{
    UiLabel* label = (UiLabel*)widget;
    measure_text(widget, label->style, label->text, width, height);
}

static void label_paint(UiWidget* widget, UiPainter* painter)
{
    UiLabel* label = (UiLabel*)widget;
    UiRect const bounds = { 0, 0, widget->rect.width, widget->rect.height };

    unsigned const colour = label->has_colour
        ? label->colour
        : (widget->enabled ? ui_theme()->text : ui_theme()->text_faint);

    UiPainter styled = *painter;
    styled.font = ui_font_for(painter->fonts, label->style);
    ui_draw_text(&styled, bounds, label->text, colour, label->align);
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

void ui_label_set_style(UiWidget* widget, UiTextStyle style)
{
    ((UiLabel*)widget)->style = style;
    ui_widget_invalidate(widget);
}

/* --- button ------------------------------------------------------------------ */

typedef struct UiButton {
    UiWidget base;
    char text[TEXT_MAX];
    UiAction on_click;
    void* user;
    int pressed;
    int is_default; /* the blue one; there is at most one per window */
} UiButton;

static void button_measure(UiWidget* widget, int* width, int* height)
{
    UiButton* button = (UiButton*)widget;
    int text_width = 0;
    int text_height = 0;
    measure_text(widget, UI_TEXT_STRONG, button->text, &text_width, &text_height);

    *width = text_width + ui_theme()->padding * 3;
    *height = text_height + ui_theme()->padding + 2;
}

static void button_paint(UiWidget* widget, UiPainter* painter)
{
    UiButton* button = (UiButton*)widget;
    const UiTheme* theme = ui_theme();
    UiRect const bounds = { 0, 0, widget->rect.width, widget->rect.height };

    /*
     * Two kinds, as macOS has: the default action is filled with the accent,
     * and everything else is white with a hairline. A dialog full of blue
     * buttons tells you nothing about which one to press.
     */
    unsigned background;
    unsigned ink;

    if (button->is_default) {
        background = theme->accent;
        if (!widget->enabled)
            background = theme->border;
        else if (button->pressed)
            background = theme->accent_pressed;
        else if (widget->hovered)
            background = theme->accent_hover;
        ink = widget->enabled ? theme->text_on_accent : 0xffffff;
    } else {
        background = theme->surface;
        if (!widget->enabled)
            background = theme->surface;
        else if (button->pressed)
            background = 0xe8e8ed;
        else if (widget->hovered)
            background = 0xf7f7fa;
        ink = widget->enabled ? theme->text : theme->text_faint;
    }

    /* A white button needs the shadow to lift off the panel; a filled one has
     * enough contrast of its own and looks heavy with one. */
    if (widget->enabled && !button->pressed && !button->is_default)
        ui_drop_shadow(painter, bounds, theme->corner_radius, theme->elevation_control);

    ui_fill_rounded(painter, bounds, theme->corner_radius, background);

    if (!button->is_default)
        ui_stroke_rounded(painter, bounds, theme->corner_radius, theme->border);

    /*
     * A halo just inside the edge rather than a second hard line. Two solid
     * strokes side by side read as a thick border somebody drew by accident;
     * a fading one reads as a glow, which is what it is meant to be. It goes
     * inside because a widget is clipped to its own rectangle.
     */
    if (ui_window_focused(widget->window) == widget) {
        for (int i = 1; i <= 2; ++i) {
            UiRect const ring
                = { bounds.x + i, bounds.y + i, bounds.width - i * 2, bounds.height - i * 2 };
            ui_stroke_rounded_alpha(
                painter, ring, theme->corner_radius - i, theme->focus_ring, i == 1 ? 200u : 90u);
        }
    }

    UiPainter styled = *painter;
    styled.font = ui_font_for(painter->fonts, UI_TEXT_STRONG);
    ui_draw_text(&styled, bounds, button->text, ink, UI_ALIGN_CENTRE);
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

void ui_button_set_default(UiWidget* widget, int is_default)
{
    ((UiButton*)widget)->is_default = is_default != 0;
    ui_widget_invalidate(widget);
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
    int text_width = 0;
    int text_height = 0;
    measure_text(widget, UI_TEXT_BODY, box->text, &text_width, &text_height);

    *width = CHECKBOX_SIZE + ui_theme()->spacing + text_width;
    *height = text_height > CHECKBOX_SIZE ? text_height + 6 : CHECKBOX_SIZE + 6;
}

static void checkbox_paint(UiWidget* widget, UiPainter* painter)
{
    UiCheckbox* box = (UiCheckbox*)widget;
    const UiTheme* theme = ui_theme();

    UiRect const mark
        = { 0, (widget->rect.height - CHECKBOX_SIZE) / 2, CHECKBOX_SIZE, CHECKBOX_SIZE };

    unsigned fill = theme->surface;
    if (box->checked)
        fill = widget->enabled ? theme->accent : theme->border;

    if (!box->checked && widget->enabled)
        ui_drop_shadow(painter, mark, theme->corner_radius_small, 1);

    ui_fill_rounded(painter, mark, theme->corner_radius_small, fill);

    /* Only the empty one is outlined. A filled checkbox with a border round it
     * looks like two controls. */
    if (!box->checked)
        ui_stroke_rounded(painter, mark, theme->corner_radius_small,
            widget->hovered && widget->enabled ? theme->border_strong : theme->border);

    if (box->checked) {
        /*
         * The tick, as two strokes of a two-pixel pen. Drawn rather than
         * spelled with a glyph so that it lines up with the box at any size
         * and does not depend on the font having one.
         */
        unsigned const ink = widget->enabled ? theme->text_on_accent : theme->text_faint;

        for (int i = 0; i < 3; ++i) {
            UiRect const stroke = { mark.x + 4 + i, mark.y + 8 + i, 2, 2 };
            ui_fill_rounded(painter, stroke, 1, ink);
        }
        for (int i = 0; i < 6; ++i) {
            UiRect const stroke = { mark.x + 6 + i, mark.y + 11 - i, 2, 2 };
            ui_fill_rounded(painter, stroke, 1, ink);
        }
    }

    UiRect const label = { CHECKBOX_SIZE + theme->spacing, 0,
        widget->rect.width - CHECKBOX_SIZE - theme->spacing, widget->rect.height };
    ui_draw_text(painter, label, box->text, widget->enabled ? theme->text : theme->text_faint,
        UI_ALIGN_LEFT);
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
    *width = 180;
    *height = 32;
}

static void textfield_paint(UiWidget* widget, UiPainter* painter)
{
    UiTextField* field = (UiTextField*)widget;
    const UiTheme* theme = ui_theme();
    UiRect const bounds = { 0, 0, widget->rect.width, widget->rect.height };

    int const focused = ui_window_focused(widget->window) == widget;

    /* White, with a hairline, and the accent takes over the outline when it has
     * focus. On a light interface a field is defined by its edge rather than by
     * being darker than what it sits on. */
    ui_fill_rounded(painter, bounds, theme->corner_radius_small, theme->surface);
    ui_stroke_rounded(
        painter, bounds, theme->corner_radius_small, focused ? theme->accent : theme->border);

    if (focused) {
        for (int i = 1; i <= 2; ++i) {
            UiRect const ring
                = { bounds.x + i, bounds.y + i, bounds.width - i * 2, bounds.height - i * 2 };
            ui_stroke_rounded_alpha(painter, ring, theme->corner_radius_small - i,
                theme->focus_ring, i == 1 ? 200u : 90u);
        }
    }

    UiRect const inner
        = { theme->padding, 0, widget->rect.width - theme->padding * 2, widget->rect.height };

    if (field->length == 0 && !focused)
        ui_draw_text(painter, inner, "empty", theme->text_faint, UI_ALIGN_LEFT);
    else
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
            = { inner.x + offset, (widget->rect.height - height) / 2 + 3, 2, height - 4 };
        ui_fill_rounded(painter, caret, 1, theme->accent);
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
