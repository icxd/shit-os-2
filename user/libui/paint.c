/* SPDX-License-Identifier: GPL-3.0-or-later */
/* shit os 2 -- painting, and the theme it paints with. */

#include "ui.h"

#include <string.h>

/*
 * Dark, because a framebuffer console is dark and a desktop that flashes white
 * over it is unpleasant. Blue for the accent, matching the ANSI palette the
 * kernel console uses, so the two halves of the system look related.
 */
static UiTheme s_theme = {
    .window_background = 0x1b1d26,
    .surface = 0x232634,
    .surface_raised = 0x2d3142,

    .text = 0xd8dae4,
    .text_dim = 0x8a8fa0,
    .text_on_accent = 0xffffff,

    .accent = 0x3f5b86,
    .accent_hover = 0x4a6b9e,
    .accent_pressed = 0x33496b,

    .border = 0x3a3f52,
    .border_focus = 0x5a8fd6,

    .danger = 0xd3544f,

    .corner_radius = 4,
    .padding = 8,
    .spacing = 6,
};

const UiTheme* ui_theme(void)
{
    return &s_theme;
}

void ui_theme_set(const UiTheme* theme)
{
    s_theme = *theme;
}

/* --- the painter ----------------------------------------------------------- */

UiPainter ui_painter_for(const UiPainter* parent, UiRect child)
{
    UiPainter painter = *parent;

    painter.origin_x = parent->origin_x + child.x;
    painter.origin_y = parent->origin_y + child.y;

    /* The clip is the intersection, so a child can never draw outside the
     * parent that placed it however wrong its own arithmetic is. */
    int const x0 = painter.origin_x;
    int const y0 = painter.origin_y;
    int const x1 = x0 + child.width;
    int const y1 = y0 + child.height;

    painter.clip.x = x0 > parent->clip.x ? x0 : parent->clip.x;
    painter.clip.y = y0 > parent->clip.y ? y0 : parent->clip.y;

    int const right
        = x1 < parent->clip.x + parent->clip.width ? x1 : parent->clip.x + parent->clip.width;
    int const bottom
        = y1 < parent->clip.y + parent->clip.height ? y1 : parent->clip.y + parent->clip.height;

    painter.clip.width = right > painter.clip.x ? right - painter.clip.x : 0;
    painter.clip.height = bottom > painter.clip.y ? bottom - painter.clip.y : 0;

    return painter;
}

static void plot(UiPainter* painter, int x, int y, unsigned colour)
{
    if (x < painter->clip.x || y < painter->clip.y)
        return;
    if (x >= painter->clip.x + painter->clip.width || y >= painter->clip.y + painter->clip.height)
        return;
    painter->pixels[(size_t)y * painter->width + x] = colour;
}

void ui_fill_rect(UiPainter* painter, UiRect rect, unsigned colour)
{
    int const x0 = painter->origin_x + rect.x;
    int const y0 = painter->origin_y + rect.y;

    for (int y = 0; y < rect.height; ++y) {
        for (int x = 0; x < rect.width; ++x)
            plot(painter, x0 + x, y0 + y, colour);
    }
}

void ui_stroke_rect(UiPainter* painter, UiRect rect, unsigned colour)
{
    int const x0 = painter->origin_x + rect.x;
    int const y0 = painter->origin_y + rect.y;

    for (int x = 0; x < rect.width; ++x) {
        plot(painter, x0 + x, y0, colour);
        plot(painter, x0 + x, y0 + rect.height - 1, colour);
    }
    for (int y = 0; y < rect.height; ++y) {
        plot(painter, x0, y0 + y, colour);
        plot(painter, x0 + rect.width - 1, y0 + y, colour);
    }
}

/*
 * Rounded corners, without anti-aliasing on the curve.
 *
 * A four-pixel radius covers so few pixels that the smoothing is barely
 * visible, and the alternative -- coverage per corner pixel -- means either a
 * per-radius table or a square root per pixel on a machine with no hardware
 * for either. The corner is cut on the exact circle, which is the shape it
 * should be, just hard-edged.
 */
static int inside_corner(int dx, int dy, int radius)
{
    return dx * dx + dy * dy <= radius * radius;
}

void ui_fill_rounded(UiPainter* painter, UiRect rect, int radius, unsigned colour)
{
    if (radius <= 0) {
        ui_fill_rect(painter, rect, colour);
        return;
    }
    if (radius * 2 > rect.width)
        radius = rect.width / 2;
    if (radius * 2 > rect.height)
        radius = rect.height / 2;

    int const x0 = painter->origin_x + rect.x;
    int const y0 = painter->origin_y + rect.y;

    for (int y = 0; y < rect.height; ++y) {
        for (int x = 0; x < rect.width; ++x) {
            /* Only the four corner squares need testing; everything between
             * them is unconditionally inside. */
            int const left = x < radius;
            int const right = x >= rect.width - radius;
            int const top = y < radius;
            int const bottom = y >= rect.height - radius;

            if ((left || right) && (top || bottom)) {
                int const dx = left ? radius - 1 - x : x - (rect.width - radius);
                int const dy = top ? radius - 1 - y : y - (rect.height - radius);
                if (!inside_corner(dx, dy, radius))
                    continue;
            }
            plot(painter, x0 + x, y0 + y, colour);
        }
    }
}

void ui_stroke_rounded(UiPainter* painter, UiRect rect, int radius, unsigned colour)
{
    if (radius <= 0) {
        ui_stroke_rect(painter, rect, colour);
        return;
    }
    if (radius * 2 > rect.width)
        radius = rect.width / 2;
    if (radius * 2 > rect.height)
        radius = rect.height / 2;

    int const x0 = painter->origin_x + rect.x;
    int const y0 = painter->origin_y + rect.y;

    /* The straight runs between the corners. */
    for (int x = radius; x < rect.width - radius; ++x) {
        plot(painter, x0 + x, y0, colour);
        plot(painter, x0 + x, y0 + rect.height - 1, colour);
    }
    for (int y = radius; y < rect.height - radius; ++y) {
        plot(painter, x0, y0 + y, colour);
        plot(painter, x0 + rect.width - 1, y0 + y, colour);
    }

    /* The corners: the outermost pixel of the disc on each row. */
    for (int i = 0; i < radius; ++i) {
        int span = 0;
        while (span < radius && inside_corner(radius - 1 - span, radius - 1 - i, radius))
            ++span;
        if (span == 0)
            continue;

        int const edge = radius - span;
        plot(painter, x0 + edge, y0 + i, colour);
        plot(painter, x0 + rect.width - 1 - edge, y0 + i, colour);
        plot(painter, x0 + edge, y0 + rect.height - 1 - i, colour);
        plot(painter, x0 + rect.width - 1 - edge, y0 + rect.height - 1 - i, colour);
    }
}

void ui_draw_text(
    UiPainter* painter, UiRect bounds, const char* text, unsigned colour, UiAlign align)
{
    if (painter->font == NULL || text == NULL)
        return;

    int const width = ui_text_width(painter->font, text);

    int x = painter->origin_x + bounds.x;
    switch (align) {
    case UI_ALIGN_CENTRE: x += (bounds.width - width) / 2; break;
    case UI_ALIGN_RIGHT: x += bounds.width - width; break;
    case UI_ALIGN_LEFT: break;
    }

    /* Optically centred: the baseline sits so that the ascent and descent are
     * balanced in the box, which is not the same as centring the em. */
    int const ascent = ui_font_ascent(painter->font);
    int const line = ui_font_line_height(painter->font);
    int const baseline = painter->origin_y + bounds.y + (bounds.height - line) / 2 + ascent;

    ui_text_draw_clipped(painter->font, painter->pixels, painter->width, painter->height, x,
        baseline, text, colour, painter->clip.x, painter->clip.y,
        painter->clip.x + painter->clip.width, painter->clip.y + painter->clip.height);
}
