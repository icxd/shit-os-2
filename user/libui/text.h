/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- laid-out, cached, drawable text.
 *
 * The rasteriser turns one glyph into coverage. This turns a string into
 * pixels on something, which needs three more things: a font loaded from disk,
 * a cache so that the letter 'e' is rasterised once rather than once per
 * appearance, and the arithmetic that walks a pen along a baseline applying
 * advances and kerning.
 *
 * The cache is the part that matters for it being usable at all. Rasterising a
 * glyph is hundreds of times more expensive than blitting one, and a window
 * server redraws a titlebar on every mouse move.
 */

#pragma once

#include "truetype.h"

typedef struct UiFont UiFont;

/*
 * Loads a font and fixes it at one pixel size. Sizes are separate fonts here
 * rather than a parameter on every call, because the cache is per size and a
 * caller almost always draws a run of text at one size.
 *
 * Returns NULL if the file cannot be read or is not a TrueType font.
 */
UiFont* ui_font_open(const char* path, double pixel_size);
void ui_font_close(UiFont* font);

/* Distance from the top of a line to the baseline, and the whole line height,
 * both in pixels. Use these rather than the pixel size: a font's own idea of
 * how much room it needs is in its metrics, and it is never exactly the em. */
int ui_font_ascent(const UiFont* font);
int ui_font_line_height(const UiFont* font);

/* How wide the string will be, without drawing it. */
int ui_text_width(UiFont* font, const char* text);

/*
 * Draws into a 32-bit surface, blending each glyph's coverage against what is
 * already there -- so text over a gradient looks right rather than sitting in
 * a box of its own background colour.
 *
 * `x` is the left of the first glyph and `baseline` is the baseline, not the
 * top. Returns the pen position after the last glyph, which is what a caller
 * laying out a line of mixed styles needs.
 */
int ui_text_draw(UiFont* font, unsigned* surface, int surface_width, int surface_height, int x,
    int baseline, const char* text, unsigned colour);

/* The same, clipped to a rectangle. Everything with a scrollbar needs this. */
int ui_text_draw_clipped(UiFont* font, unsigned* surface, int surface_width, int surface_height,
    int x, int baseline, const char* text, unsigned colour, int clip_x0, int clip_y0, int clip_x1,
    int clip_y1);
