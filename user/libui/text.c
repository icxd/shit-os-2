/* SPDX-License-Identifier: GPL-3.0-or-later */
/* shit os 2 -- laid-out, cached, drawable text. */

#include "text.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Only the printable ASCII range is cached. Everything else still draws, just
 * without the cache -- which is the right trade while nothing here speaks a
 * language that needs more, and leaves the door open when something does.
 */
#define CACHE_FIRST 32
#define CACHE_LAST 126
#define CACHE_COUNT (CACHE_LAST - CACHE_FIRST + 1)

struct UiFont {
    TtFont face;

    /* The file, mapped rather than read: a font is most of a megabyte and
     * every process that draws text would otherwise keep its own copy. Shared
     * mappings of the same file are the same physical pages. */
    unsigned char* mapping;
    size_t mapping_length;

    double scale;
    int ascent, descent, line_gap;

    TtBitmap cache[CACHE_COUNT];
    int cached[CACHE_COUNT];
    int advance[CACHE_COUNT];
    int glyph[CACHE_COUNT];
};

UiFont* ui_font_open(const char* path, double pixel_size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;

    struct stat status;
    if (fstat(fd, &status) < 0 || status.st_size <= 0) {
        close(fd);
        return NULL;
    }

    UiFont* font = calloc(1, sizeof(UiFont));
    if (font == NULL) {
        close(fd);
        return NULL;
    }

    font->mapping_length = (size_t)status.st_size;
    font->mapping = mmap(NULL, font->mapping_length, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (font->mapping == MAP_FAILED) {
        free(font);
        return NULL;
    }

    if (tt_open(&font->face, font->mapping, font->mapping_length) != 0) {
        munmap(font->mapping, font->mapping_length);
        free(font);
        return NULL;
    }

    font->scale = tt_scale_for_pixel_size(&font->face, pixel_size);
    font->ascent = (int)(font->face.ascent * font->scale + 0.5);
    font->descent = (int)(-font->face.descent * font->scale + 0.5);
    font->line_gap = (int)(font->face.line_gap * font->scale + 0.5);

    return font;
}

void ui_font_close(UiFont* font)
{
    if (font == NULL)
        return;

    for (int i = 0; i < CACHE_COUNT; ++i)
        free(font->cache[i].pixels);

    munmap(font->mapping, font->mapping_length);
    free(font);
}

int ui_font_ascent(const UiFont* font)
{
    return font->ascent;
}

int ui_font_line_height(const UiFont* font)
{
    return font->ascent + font->descent + font->line_gap;
}

/*
 * Everything below goes through here, so a glyph is rasterised at most once
 * per font. Returns NULL only when the character is outside the cached range
 * and could not be rendered; a blank glyph caches as a zero-size bitmap,
 * which is a hit rather than a repeated miss.
 */
static const TtBitmap* glyph_for(UiFont* font, unsigned char c, int* advance_out, int* glyph_out)
{
    if (c < CACHE_FIRST || c > CACHE_LAST) {
        *advance_out = 0;
        *glyph_out = 0;
        return NULL;
    }

    int const slot = c - CACHE_FIRST;
    if (!font->cached[slot]) {
        int const glyph = tt_glyph_for_codepoint(&font->face, c);
        if (tt_render_glyph(&font->face, glyph, font->scale, &font->cache[slot]) != 0)
            memset(&font->cache[slot], 0, sizeof(TtBitmap));

        font->advance[slot] = (int)(tt_advance(&font->face, glyph) * font->scale + 0.5);
        font->glyph[slot] = glyph;
        font->cached[slot] = 1;
    }

    *advance_out = font->advance[slot];
    *glyph_out = font->glyph[slot];
    return &font->cache[slot];
}

int ui_text_width(UiFont* font, const char* text)
{
    int x = 0;
    int previous = 0;

    for (const unsigned char* p = (const unsigned char*)text; *p != '\0'; ++p) {
        int advance = 0;
        int glyph = 0;
        if (glyph_for(font, *p, &advance, &glyph) == NULL)
            continue;

        if (previous != 0)
            x += (int)(tt_kern(&font->face, previous, glyph) * font->scale + 0.5);

        x += advance;
        previous = glyph;
    }
    return x;
}

/* Coverage over whatever is already there. Doing it in 8-bit channels rather
 * than in floats keeps this cheap enough to run per pixel of every glyph of
 * every redraw. */
static unsigned blend(unsigned background, unsigned colour, unsigned coverage)
{
    if (coverage == 0)
        return background;
    if (coverage == 255)
        return colour;

    unsigned result = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        unsigned const b = (background >> shift) & 0xff;
        unsigned const f = (colour >> shift) & 0xff;
        result |= ((b * (255 - coverage) + f * coverage) / 255) << shift;
    }
    return result;
}

int ui_text_draw_clipped(UiFont* font, unsigned* surface, int surface_width, int surface_height,
    int x, int baseline, const char* text, unsigned colour, int clip_x0, int clip_y0, int clip_x1,
    int clip_y1)
{
    if (clip_x0 < 0)
        clip_x0 = 0;
    if (clip_y0 < 0)
        clip_y0 = 0;
    if (clip_x1 > surface_width)
        clip_x1 = surface_width;
    if (clip_y1 > surface_height)
        clip_y1 = surface_height;

    int previous = 0;

    for (const unsigned char* p = (const unsigned char*)text; *p != '\0'; ++p) {
        int advance = 0;
        int glyph = 0;
        const TtBitmap* bitmap = glyph_for(font, *p, &advance, &glyph);
        if (bitmap == NULL)
            continue;

        if (previous != 0)
            x += (int)(tt_kern(&font->face, previous, glyph) * font->scale + 0.5);

        if (bitmap->pixels != NULL) {
            int const origin_x = x + bitmap->left;
            int const origin_y = baseline - bitmap->top;

            for (int row = 0; row < bitmap->height; ++row) {
                int const py = origin_y + row;
                if (py < clip_y0 || py >= clip_y1)
                    continue;

                const unsigned char* source = bitmap->pixels + (size_t)row * bitmap->width;
                unsigned* destination = surface + (size_t)py * surface_width;

                for (int column = 0; column < bitmap->width; ++column) {
                    int const px = origin_x + column;
                    if (px < clip_x0 || px >= clip_x1)
                        continue;
                    unsigned char const coverage = source[column];
                    if (coverage != 0)
                        destination[px] = blend(destination[px], colour, coverage);
                }
            }
        }

        x += advance;
        previous = glyph;
    }

    return x;
}

int ui_text_draw(UiFont* font, unsigned* surface, int surface_width, int surface_height, int x,
    int baseline, const char* text, unsigned colour)
{
    return ui_text_draw_clipped(font, surface, surface_width, surface_height, x, baseline, text,
        colour, 0, 0, surface_width, surface_height);
}
