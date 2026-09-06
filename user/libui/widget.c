/* SPDX-License-Identifier: GPL-3.0-or-later */
/* shit os 2 -- the widget base, and the one container everything nests in. */

#include "ui.h"

#include <stdlib.h>
#include <string.h>

/* --- the base --------------------------------------------------------------- */

UiWidget* ui_widget_create(const UiWidgetClass* klass)
{
    size_t const size = klass->size > sizeof(UiWidget) ? klass->size : sizeof(UiWidget);
    UiWidget* widget = calloc(1, size);
    if (widget == NULL)
        return NULL;

    widget->klass = klass;
    widget->visible = 1;
    widget->enabled = 1;
    return widget;
}

void ui_widget_destroy(UiWidget* widget)
{
    if (widget == NULL)
        return;

    /* Depth first: a child's destructor may still want its own children. */
    for (int i = 0; i < widget->child_count; ++i)
        ui_widget_destroy(widget->children[i]);
    free(widget->children);

    if (widget->klass->destroy != NULL)
        widget->klass->destroy(widget);

    free(widget);
}

int ui_widget_add(UiWidget* parent, UiWidget* child)
{
    if (parent == NULL || child == NULL)
        return -1;

    if (parent->child_count == parent->child_capacity) {
        int const capacity = parent->child_capacity == 0 ? 4 : parent->child_capacity * 2;
        UiWidget** grown = realloc(parent->children, (size_t)capacity * sizeof(UiWidget*));
        if (grown == NULL)
            return -1;
        parent->children = grown;
        parent->child_capacity = capacity;
    }

    parent->children[parent->child_count++] = child;
    child->parent = parent;
    child->window = parent->window;

    /* A child added after the window was set has to inherit it, and so does
     * everything already hanging off that child. */
    for (int i = 0; i < child->child_count; ++i)
        child->children[i]->window = child->window;

    return 0;
}

/* Where a widget sits on the window's surface. Rects are relative to the
 * parent, so this is the walk up. */
static void absolute_origin(const UiWidget* widget, int* x, int* y)
{
    *x = 0;
    *y = 0;
    for (const UiWidget* w = widget; w != NULL; w = w->parent) {
        *x += w->rect.x;
        *y += w->rect.y;
    }
}

void ui_widget_invalidate(UiWidget* widget)
{
    if (widget == NULL)
        return;

    int x = 0;
    int y = 0;
    absolute_origin(widget, &x, &y);
    ui_window_damage(widget->window, x, y, widget->rect.width, widget->rect.height);
}

/*
 * Part of a widget, in its own coordinates. A terminal that has printed one
 * line has changed one row of cells out of twenty-four, and repainting the
 * other twenty-three is most of what a terminal spends its time on.
 */
void ui_widget_invalidate_rect(UiWidget* widget, UiRect rect)
{
    if (widget == NULL)
        return;

    int x = 0;
    int y = 0;
    absolute_origin(widget, &x, &y);
    ui_window_damage(widget->window, x + rect.x, y + rect.y, rect.width, rect.height);
}

void ui_widget_measure(UiWidget* widget, int* width, int* height)
{
    int w = 0;
    int h = 0;

    if (widget->klass->measure != NULL)
        widget->klass->measure(widget, &w, &h);

    if (widget->fixed_width > 0)
        w = widget->fixed_width;
    if (widget->fixed_height > 0)
        h = widget->fixed_height;

    *width = w;
    *height = h;
}

UiWidget* ui_widget_at(UiWidget* root, int x, int y)
{
    if (root == NULL || !root->visible || !ui_rect_contains(root->rect, x, y))
        return NULL;

    /* Later children are drawn on top, so they are hit first. */
    for (int i = root->child_count - 1; i >= 0; --i) {
        UiWidget* hit = ui_widget_at(root->children[i], x - root->rect.x, y - root->rect.y);
        if (hit != NULL)
            return hit;
    }
    return root;
}

/* --- the box ---------------------------------------------------------------- */

typedef struct UiBox {
    UiWidget base;
    UiOrientation orientation;
    int spacing;
    int padding;
} UiBox;

static void box_measure(UiWidget* widget, int* width, int* height)
{
    UiBox* box = (UiBox*)widget;

    int along = 0;
    int across = 0;
    int visible = 0;

    for (int i = 0; i < widget->child_count; ++i) {
        UiWidget* child = widget->children[i];
        if (!child->visible)
            continue;

        int w = 0;
        int h = 0;
        ui_widget_measure(child, &w, &h);

        int const child_along = box->orientation == UI_HORIZONTAL ? w : h;
        int const child_across = box->orientation == UI_HORIZONTAL ? h : w;

        along += child_along;
        if (child_across > across)
            across = child_across;
        ++visible;
    }

    if (visible > 1)
        along += box->spacing * (visible - 1);

    along += box->padding * 2;
    across += box->padding * 2;

    *width = box->orientation == UI_HORIZONTAL ? along : across;
    *height = box->orientation == UI_HORIZONTAL ? across : along;
}

static void box_layout(UiWidget* widget)
{
    UiBox* box = (UiBox*)widget;

    int const inner_width = widget->rect.width - box->padding * 2;
    int const inner_height = widget->rect.height - box->padding * 2;
    int const available = box->orientation == UI_HORIZONTAL ? inner_width : inner_height;

    /* First pass: what everyone wants, and how many want more. */
    int wanted = 0;
    int visible = 0;
    int expanders = 0;

    for (int i = 0; i < widget->child_count; ++i) {
        UiWidget* child = widget->children[i];
        if (!child->visible)
            continue;

        int w = 0;
        int h = 0;
        ui_widget_measure(child, &w, &h);
        wanted += box->orientation == UI_HORIZONTAL ? w : h;
        ++visible;

        if (box->orientation == UI_HORIZONTAL ? child->expand_x : child->expand_y)
            ++expanders;
    }

    if (visible > 1)
        wanted += box->spacing * (visible - 1);

    /*
     * More children than room. A box that just lets them run off the end
     * produces the worst-looking thing an interface can do -- a button sliced
     * in half by the window edge -- so the shortfall is taken back from them
     * in proportion to what they asked for, which is what every layout that
     * survives a resize does.
     */
    int shortfall = wanted - available;
    if (shortfall < 0)
        shortfall = 0;

    int leftover = available - wanted;
    if (leftover < 0)
        leftover = 0;

    /* Divided evenly, with the remainder going to the first few rather than
     * being lost -- otherwise a row of three in a 100-pixel box comes out a
     * pixel short and the gap is visible. */
    int const share = expanders > 0 ? leftover / expanders : 0;
    int remainder = expanders > 0 ? leftover % expanders : 0;

    int cursor = box->padding;

    for (int i = 0; i < widget->child_count; ++i) {
        UiWidget* child = widget->children[i];
        if (!child->visible)
            continue;

        int w = 0;
        int h = 0;
        ui_widget_measure(child, &w, &h);

        int along = box->orientation == UI_HORIZONTAL ? w : h;
        int const across = box->orientation == UI_HORIZONTAL ? inner_height : inner_width;

        if (shortfall > 0 && wanted > 0) {
            int give = along * shortfall / wanted;
            if (give > along)
                give = along;
            along -= give;
        }

        if (box->orientation == UI_HORIZONTAL ? child->expand_x : child->expand_y) {
            along += share;
            if (remainder > 0) {
                ++along;
                --remainder;
            }
        }

        if (box->orientation == UI_HORIZONTAL) {
            child->rect.x = cursor;
            child->rect.y = box->padding;
            child->rect.width = along;
            child->rect.height = child->expand_y || child->fixed_height == 0 ? across : h;
        } else {
            child->rect.x = box->padding;
            child->rect.y = cursor;
            child->rect.width = child->expand_x || child->fixed_width == 0 ? across : w;
            child->rect.height = along;
        }

        if (child->klass->layout != NULL)
            child->klass->layout(child);

        cursor += along + box->spacing;
    }
}

static void box_paint(UiWidget* widget, UiPainter* painter)
{
    /* A box draws nothing of its own; it is scaffolding. Children are painted
     * by the window walking the tree. */
    (void)widget;
    (void)painter;
}

static const UiWidgetClass BOX_CLASS = {
    .name = "box",
    .size = sizeof(UiBox),
    .paint = box_paint,
    .measure = box_measure,
    .layout = box_layout,
};

/* --- the panel ----------------------------------------------------------------
 *
 * A box that draws itself: a white surface with a hairline and a soft shadow.
 * The whole macOS look is content on white sitting on a barely-grey window, and
 * this is the "on white" half of that.
 */

static void panel_paint(UiWidget* widget, UiPainter* painter)
{
    const UiTheme* theme = ui_theme();
    UiRect const bounds = { 0, 0, widget->rect.width, widget->rect.height };

    ui_drop_shadow(painter, bounds, theme->corner_radius, theme->elevation_panel);
    ui_fill_rounded(painter, bounds, theme->corner_radius, theme->surface);
    ui_stroke_rounded(painter, bounds, theme->corner_radius, theme->border);
}

static const UiWidgetClass PANEL_CLASS = {
    .name = "panel",
    .size = sizeof(UiBox),
    .paint = panel_paint,
    .measure = box_measure,
    .layout = box_layout,
};

UiWidget* ui_panel_create(UiOrientation orientation)
{
    UiWidget* widget = ui_widget_create(&PANEL_CLASS);
    if (widget == NULL)
        return NULL;

    UiBox* box = (UiBox*)widget;
    box->orientation = orientation;
    box->spacing = ui_theme()->spacing;
    box->padding = ui_theme()->padding + 4;
    return widget;
}

UiWidget* ui_box_create(UiOrientation orientation)
{
    UiWidget* widget = ui_widget_create(&BOX_CLASS);
    if (widget == NULL)
        return NULL;

    UiBox* box = (UiBox*)widget;
    box->orientation = orientation;
    box->spacing = ui_theme()->spacing;
    box->padding = ui_theme()->padding;
    return widget;
}

void ui_box_set_spacing(UiWidget* widget, int spacing)
{
    ((UiBox*)widget)->spacing = spacing;
}

void ui_box_set_padding(UiWidget* widget, int padding)
{
    ((UiBox*)widget)->padding = padding;
}
