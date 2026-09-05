/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- the TrueType rasteriser, checked on the host.
 *
 * Built with the address and undefined-behaviour sanitizers, which is most of
 * the value here: a rasteriser is nothing but array indexing driven by
 * untrusted file contents, and the interesting failures are one-past-the-end
 * writes that happen to land somewhere harmless in QEMU.
 */

#include "truetype.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_checks;
static int s_failures;

static void check(int condition, const char* what)
{
    ++s_checks;
    if (!condition) {
        ++s_failures;
        printf("  FAIL %s\n", what);
    }
}

static unsigned char* read_file(const char* path, size_t* length)
{
    FILE* handle = fopen(path, "rb");
    if (handle == NULL)
        return NULL;

    fseek(handle, 0, SEEK_END);
    long const size = ftell(handle);
    fseek(handle, 0, SEEK_SET);

    unsigned char* data = malloc((size_t)size);
    if (data == NULL || fread(data, 1, (size_t)size, handle) != (size_t)size) {
        free(data);
        fclose(handle);
        return NULL;
    }
    fclose(handle);
    *length = (size_t)size;
    return data;
}

/* --- a tiny text renderer, which is also the proof it can be used ---------- */

typedef struct Canvas {
    unsigned char* rgb;
    int width, height;
} Canvas;

static void blend(Canvas* canvas, int x, int y, unsigned char coverage)
{
    if (x < 0 || y < 0 || x >= canvas->width || y >= canvas->height)
        return;
    unsigned char* pixel = canvas->rgb + ((size_t)y * canvas->width + x) * 3;
    for (int i = 0; i < 3; ++i) {
        unsigned const background = pixel[i];
        pixel[i] = (unsigned char)((background * (255 - coverage) + 255 * coverage) / 255);
    }
}

static int draw_text(
    const TtFont* font, Canvas* canvas, int pen_x, int baseline, double size, const char* text)
{
    double const scale = tt_scale_for_pixel_size(font, size);
    int previous = 0;
    int drawn = 0;

    for (const char* p = text; *p != '\0'; ++p) {
        int const glyph = tt_glyph_for_codepoint(font, (unsigned char)*p);

        if (previous != 0)
            pen_x += (int)(tt_kern(font, previous, glyph) * scale + 0.5);

        TtBitmap bitmap;
        if (tt_render_glyph(font, glyph, scale, &bitmap) != 0)
            return -1;

        if (bitmap.pixels != NULL) {
            for (int row = 0; row < bitmap.height; ++row) {
                for (int column = 0; column < bitmap.width; ++column) {
                    unsigned char const coverage
                        = bitmap.pixels[(size_t)row * bitmap.width + column];
                    if (coverage != 0)
                        blend(canvas, pen_x + bitmap.left + column, baseline - bitmap.top + row,
                            coverage);
                }
            }
            ++drawn;
            free(bitmap.pixels);
        }

        pen_x += (int)(tt_advance(font, glyph) * scale + 0.5);
        previous = glyph;
    }
    return drawn;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: truetypecheck <font.ttf> <out.ppm>\n");
        return 2;
    }

    size_t length = 0;
    unsigned char* data = read_file(argv[1], &length);
    check(data != NULL, "reading the font file");
    if (data == NULL)
        return 1;

    TtFont font;
    check(tt_open(&font, data, length) == 0, "parsing the font");
    if (tt_open(&font, data, length) != 0)
        return 1;

    /* --- the tables say something sensible --- */
    check(font.units_per_em >= 16 && font.units_per_em <= 16384, "units per em is plausible");
    check(font.glyph_count > 100, "the font has a useful number of glyphs");
    check(font.ascent > 0, "the ascent is above the baseline");
    check(font.descent < 0, "the descent is below it");
    check(font.metric_count > 0, "there is at least one horizontal metric");

    /* --- cmap finds what it should --- */
    int const glyph_A = tt_glyph_for_codepoint(&font, 'A');
    int const glyph_a = tt_glyph_for_codepoint(&font, 'a');
    int const glyph_space = tt_glyph_for_codepoint(&font, ' ');

    check(glyph_A > 0, "'A' maps to a glyph");
    check(glyph_a > 0, "'a' maps to a glyph");
    check(glyph_A != glyph_a, "and they are different glyphs");
    check(glyph_space > 0, "a space maps to a glyph too");
    check(tt_glyph_for_codepoint(&font, 0x10FFFD) == 0,
        "an unassigned codepoint maps to .notdef rather than to nonsense");

    /* --- metrics --- */
    check(tt_advance(&font, glyph_A) > 0, "'A' advances the pen");
    check(tt_advance(&font, glyph_space) > 0, "so does a space");

    /* --- rasterising --- */
    double const scale = tt_scale_for_pixel_size(&font, 32.0);
    check(scale > 0.0, "a scale for 32 pixels");

    TtBitmap bitmap;
    check(tt_render_glyph(&font, glyph_A, scale, &bitmap) == 0, "rendering 'A'");
    check(bitmap.pixels != NULL, "'A' has an outline");

    if (bitmap.pixels != NULL) {
        check(bitmap.width > 4 && bitmap.width < 200, "'A' is a plausible width at 32px");
        check(bitmap.height > 4 && bitmap.height < 200, "and a plausible height");
        check(bitmap.top > 0, "'A' rises above the baseline");

        /* Coverage must be somewhere between nothing and everything, and it
         * must not be uniform -- a solid block would mean the winding rule
         * filled the bounding box instead of the letter. */
        int ink = 0;
        int blank = 0;
        int partial = 0;
        for (int i = 0; i < bitmap.width * bitmap.height; ++i) {
            unsigned char const value = bitmap.pixels[i];
            if (value == 0)
                ++blank;
            else if (value == 255)
                ++ink;
            else
                ++partial;
        }
        check(ink > 0, "'A' has solidly covered pixels");
        check(blank > 0, "and uncovered ones, so it is not a filled box");
        check(partial > 0, "and partially covered ones, so it is anti-aliased");

        /* The topmost row of an 'A' is its apex: narrow. The bottom row is its
         * two feet: also not full width, but wider than the apex. Getting the
         * winding direction backwards inverts this. */
        int top_ink = 0;
        int bottom_ink = 0;
        for (int x = 0; x < bitmap.width; ++x) {
            if (bitmap.pixels[x] != 0)
                ++top_ink;
            if (bitmap.pixels[(size_t)(bitmap.height - 1) * bitmap.width + x] != 0)
                ++bottom_ink;
        }
        check(top_ink < bottom_ink, "'A' is narrower at the top than at the bottom");

        free(bitmap.pixels);
    }

    /* A space is a real glyph with an advance and no ink at all. Returning an
     * error for it, or a one-pixel bitmap, would put a mark on the page. */
    check(tt_render_glyph(&font, glyph_space, scale, &bitmap) == 0, "rendering a space");
    check(bitmap.pixels == NULL, "a space has no pixels");
    check(bitmap.width == 0 && bitmap.height == 0, "and no size");

    /* Every glyph in the font, at a small size: this is the part the
     * sanitizers care about, because it is where a malformed or unusual
     * outline would walk off an array. */
    double const small = tt_scale_for_pixel_size(&font, 11.0);
    int rendered = 0;
    int failed = 0;
    for (int glyph = 0; glyph < font.glyph_count; ++glyph) {
        TtBitmap each;
        if (tt_render_glyph(&font, glyph, small, &each) != 0) {
            ++failed;
            continue;
        }
        if (each.pixels != NULL) {
            ++rendered;
            free(each.pixels);
        }
    }
    check(failed == 0, "every glyph in the font rasterises without error");
    check(rendered > font.glyph_count / 4, "and most of them have outlines");

    /* --- a page a human can look at --- */
    Canvas canvas = { .width = 760, .height = 260 };
    canvas.rgb = malloc((size_t)canvas.width * canvas.height * 3);
    memset(canvas.rgb, 0x14, (size_t)canvas.width * canvas.height * 3);

    int drawn = 0;
    drawn += draw_text(&font, &canvas, 20, 60, 42.0, "Hamburgefonstiv");
    drawn += draw_text(&font, &canvas, 20, 110, 24.0, "The quick brown fox jumps over it.");
    drawn += draw_text(&font, &canvas, 20, 148, 16.0,
        "abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789");
    drawn += draw_text(&font, &canvas, 20, 180, 12.0,
        "Small text is where anti-aliasing earns its keep: 12 pixels, no hinting.");
    drawn += draw_text(&font, &canvas, 20, 220, 28.0, "shit os 2 -- AVAWA To Ty");

    check(drawn > 100, "the sample page drew a lot of glyphs");

    FILE* out = fopen(argv[2], "wb");
    if (out != NULL) {
        fprintf(out, "P6\n%d %d\n255\n", canvas.width, canvas.height);
        fwrite(canvas.rgb, 1, (size_t)canvas.width * canvas.height * 3, out);
        fclose(out);
    }
    free(canvas.rgb);
    free(data);

    printf("%d checks, %d failed\n", s_checks, s_failures);
    return s_failures == 0 ? 0 : 1;
}
