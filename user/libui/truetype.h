/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- a TrueType rasteriser, written from the specification.
 *
 * This is the difference between a user interface and a DOS program. The 8x16
 * console font is one size, monospace, and 128 glyphs; everything drawn with
 * it looks like 1985 because it is 1985. Proportional outlines rendered with
 * anti-aliasing at any size look like software people use.
 *
 * The tables that matter and nothing else: head for the design grid, maxp for
 * the glyph count, hhea and hmtx for advances, loca and glyf for the outlines,
 * cmap to get from a character to a glyph, and kern where a font has one.
 * Hinting is not implemented and never will be -- it exists to make stems
 * align to a pixel grid at sizes where anti-aliasing alone looks muddy, and
 * the patent-era complexity of it buys very little at the sizes a screen this
 * size uses.
 *
 * Fonts are not in this tree. The port fetches one and verifies it, the same
 * as Lua and dash and sbase.
 */

#pragma once

#include <stddef.h>

typedef struct TtFont {
    const unsigned char* data;
    size_t length;

    /* Table offsets, zero when the font does not have one. */
    unsigned glyf, loca, cmap, hmtx, kern;

    int units_per_em; /* the design grid every coordinate is expressed in */
    int long_loca; /* loca entries are 32-bit rather than 16-bit halves */
    int glyph_count;
    int metric_count; /* hmtx has this many full entries, then only bearings */

    int ascent, descent, line_gap; /* in design units */
} TtFont;

/*
 * Coverage, not colour: one byte per pixel saying how much of it the glyph
 * covers. The caller blends that with whatever it is drawing over, which is
 * the only way text can sit correctly on a background it does not own.
 *
 * `left` and `top` place the bitmap relative to the pen: `top` is positive
 * upward, so a glyph is drawn at (pen_x + left, baseline - top).
 */
typedef struct TtBitmap {
    unsigned char* pixels;
    int width, height;
    int left, top;
} TtBitmap;

/* Returns 0 on success. The data is borrowed, not copied, and must outlive
 * the font -- mapping the file and never freeing it is the intended use. */
int tt_open(TtFont* font, const void* data, size_t length);

/* 0 when the font has no glyph for it, which is the .notdef box. */
int tt_glyph_for_codepoint(const TtFont* font, unsigned codepoint);

/* Design units to pixels, for a given em size in pixels. */
double tt_scale_for_pixel_size(const TtFont* font, double pixels);

/* Horizontal advance and left side bearing, in design units. */
int tt_advance(const TtFont* font, int glyph);

/* Kerning between two glyphs in design units; 0 when the font has no kern
 * table or no pair for these two. */
int tt_kern(const TtFont* font, int left, int right);

/*
 * Rasterises one glyph. The caller frees `out->pixels` with free().
 * Returns 0 on success, and also on a glyph with no outline -- a space has
 * an advance and no pixels -- in which case width and height are zero and
 * pixels is NULL.
 */
int tt_render_glyph(const TtFont* font, int glyph, double scale, TtBitmap* out);
