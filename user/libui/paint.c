/* SPDX-License-Identifier: GPL-3.0-or-later */
/* shit os 2 -- painting, and the theme it paints with. */

#include "ui.h"

#include <string.h>

/*
 * Styled after macOS, which means light. The whole look rests on three things
 * and not on any particular hex value: content sits on white, white sits on a
 * grey that is barely grey, and everything is separated by hairlines and very
 * soft shadows rather than by borders with weight.
 *
 * The accent is Apple's system blue. The greys are theirs too -- they are not
 * neutral, they carry a trace of blue, which is what stops a light interface
 * looking like unprinted paper.
 */
static UiTheme s_theme = {
    .background = 0xececec, /* the window behind the content */
    .surface = 0xffffff, /* content sits on white */
    .surface_raised = 0xfbfbfd, /* a control on that white */
    .surface_sunken = 0xffffff, /* a field is white too, and outlined instead */

    .text = 0x1d1d1f,
    .text_dim = 0x6e6e73,
    .text_faint = 0xaeaeb2,
    .text_on_accent = 0xffffff,

    .accent = 0x007aff, /* system blue */
    .accent_hover = 0x1a86ff,
    .accent_pressed = 0x0062cc,

    .border = 0xd2d2d7,
    .border_strong = 0xb8b8bd,
    .focus_ring = 0x007aff,

    .danger = 0xff3b30,
    .success = 0x34c759,

    /*
     * Generous. Small radii read as Windows; macOS rounds a button by nearly
     * half its height and a panel by ten pixels, and that alone does a
     * surprising amount of the work.
     */
    .corner_radius = 6,
    .corner_radius_small = 4,
    .padding = 16,
    .spacing_tight = 6,
    .spacing = 10,
    .spacing_section = 22,

    .elevation_control = 1,
    .elevation_panel = 8,
};

const UiTheme* ui_theme(void)
{
    return &s_theme;
}

void ui_theme_set(const UiTheme* theme)
{
    s_theme = *theme;
}

/* --- the faces ------------------------------------------------------------- */

/*
 * The scale. Four steps and one monospace, which is as many as an interface
 * this size can use without the differences stopping meaning anything.
 */
int ui_fonts_open(UiFonts* fonts, const char* directory)
{
    char path[256];

    struct {
        UiFont** slot;
        const char* file;
        double size;
    } const wanted[] = {
        { &fonts->body, "sans.ttf", 14.0 },
        { &fonts->strong, "bold.ttf", 14.0 },
        { &fonts->small, "sans.ttf", 12.0 },
        { &fonts->heading, "bold.ttf", 19.0 },
        { &fonts->mono, "mono.ttf", 14.0 },
    };

    for (unsigned i = 0; i < sizeof(wanted) / sizeof(wanted[0]); ++i) {
        int const length = (int)strlen(directory);
        if (length + 16 >= (int)sizeof(path))
            return -1;

        memcpy(path, directory, (size_t)length);
        path[length] = '/';
        strcpy(path + length + 1, wanted[i].file);

        *wanted[i].slot = ui_font_open(path, wanted[i].size);
    }

    /* Body is the one that has to work; everything else falls back to it. */
    return fonts->body != NULL ? 0 : -1;
}

void ui_fonts_close(UiFonts* fonts)
{
    /* The same face can be opened at two sizes, but never twice at one, so
     * every pointer here is distinct and each is closed exactly once. */
    ui_font_close(fonts->body);
    ui_font_close(fonts->strong);
    ui_font_close(fonts->small);
    ui_font_close(fonts->heading);
    ui_font_close(fonts->mono);
    memset(fonts, 0, sizeof(*fonts));
}

UiFont* ui_font_for(const UiFonts* fonts, UiTextStyle style)
{
    if (fonts == NULL)
        return NULL;

    UiFont* chosen = NULL;
    switch (style) {
    case UI_TEXT_BODY: chosen = fonts->body; break;
    case UI_TEXT_STRONG: chosen = fonts->strong; break;
    case UI_TEXT_SMALL: chosen = fonts->small; break;
    case UI_TEXT_HEADING: chosen = fonts->heading; break;
    case UI_TEXT_MONO: chosen = fonts->mono; break;
    }

    /* A face that would not open leaves the interface flat rather than blank. */
    return chosen != NULL ? chosen : fonts->body;
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

/*
 * The span of a rectangle that survives the clip, in surface coordinates.
 * Everything below works this out once and then runs a tight loop, rather
 * than testing four bounds per pixel: a full-screen fill is three quarters of
 * a million pixels, and the clip does not change between any two of them.
 */
typedef struct ClippedRect {
    int x0, y0, x1, y1;
} ClippedRect;

static ClippedRect clip_rect(const UiPainter* painter, int x, int y, int width, int height)
{
    ClippedRect out;
    out.x0 = x;
    out.y0 = y;
    out.x1 = x + width;
    out.y1 = y + height;

    if (out.x0 < painter->clip.x)
        out.x0 = painter->clip.x;
    if (out.y0 < painter->clip.y)
        out.y0 = painter->clip.y;
    if (out.x1 > painter->clip.x + painter->clip.width)
        out.x1 = painter->clip.x + painter->clip.width;
    if (out.y1 > painter->clip.y + painter->clip.height)
        out.y1 = painter->clip.y + painter->clip.height;

    return out;
}

static int clipped_is_empty(ClippedRect r)
{
    return r.x0 >= r.x1 || r.y0 >= r.y1;
}

static void plot(UiPainter* painter, int x, int y, unsigned colour)
{
    if (x < painter->clip.x || y < painter->clip.y)
        return;
    if (x >= painter->clip.x + painter->clip.width || y >= painter->clip.y + painter->clip.height)
        return;
    painter->pixels[(size_t)y * painter->width + x] = colour;
}

/*
 * Coverage over what is already there. Everything anti-aliased goes through
 * this, in 8-bit channels rather than floats -- it runs per pixel of every
 * corner of every control on every repaint, on a machine with no vector unit
 * anyone has taught us to use.
 */
static void blend_pixel(UiPainter* painter, int x, int y, unsigned colour, unsigned coverage)
{
    if (coverage == 0)
        return;
    if (coverage >= 255) {
        plot(painter, x, y, colour);
        return;
    }

    if (x < painter->clip.x || y < painter->clip.y)
        return;
    if (x >= painter->clip.x + painter->clip.width || y >= painter->clip.y + painter->clip.height)
        return;

    unsigned* pixel = &painter->pixels[(size_t)y * painter->width + x];
    unsigned const background = *pixel;

    unsigned result = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        unsigned const b = (background >> shift) & 0xff;
        unsigned const f = (colour >> shift) & 0xff;
        result |= ((b * (255 - coverage) + f * coverage) / 255) << shift;
    }
    *pixel = result;
}

/* The same blend, for a pixel already known to be inside the clip. */
static void blend_at(unsigned* pixel, unsigned colour, unsigned coverage)
{
    unsigned const background = *pixel;

    unsigned result = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        unsigned const b = (background >> shift) & 0xff;
        unsigned const f = (colour >> shift) & 0xff;
        result |= ((b * (255 - coverage) + f * coverage) / 255) << shift;
    }
    *pixel = result;
}

void ui_fill_rect(UiPainter* painter, UiRect rect, unsigned colour)
{
    ClippedRect const box = clip_rect(
        painter, painter->origin_x + rect.x, painter->origin_y + rect.y, rect.width, rect.height);
    if (clipped_is_empty(box))
        return;

    for (int y = box.y0; y < box.y1; ++y) {
        unsigned* row = painter->pixels + (size_t)y * painter->width;
        for (int x = box.x0; x < box.x1; ++x)
            row[x] = colour;
    }
}

void ui_fill_rect_alpha(UiPainter* painter, UiRect rect, unsigned colour, unsigned alpha)
{
    if (alpha == 0)
        return;
    if (alpha >= 255) {
        ui_fill_rect(painter, rect, colour);
        return;
    }

    ClippedRect const box = clip_rect(
        painter, painter->origin_x + rect.x, painter->origin_y + rect.y, rect.width, rect.height);
    if (clipped_is_empty(box))
        return;

    for (int y = box.y0; y < box.y1; ++y) {
        unsigned* row = painter->pixels + (size_t)y * painter->width;
        for (int x = box.x0; x < box.x1; ++x)
            blend_at(&row[x], colour, alpha);
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
 * --- rounded rectangles, anti-aliased ----------------------------------------
 *
 * Only the four corners need any thought. Between them the edges are
 * axis-aligned and land exactly on pixel boundaries, so they are solid fills
 * with nothing to smooth; a corner is a quarter disc, and there the coverage of
 * each pixel is worth computing properly.
 *
 * The estimate is the classic one: signed distance from the pixel centre to the
 * circle, mapped through half a pixel of falloff. It is not exact area coverage
 * -- that would need the integral of a circular segment per pixel -- but at the
 * radii an interface uses the difference is invisible, and this is one square
 * root per corner pixel rather than per pixel of the whole shape.
 */

static double square_root(double value)
{
    /* Newton's method, because the freestanding target has no sqrt worth
     * calling from here and the range is tiny and well behaved. */
    if (value <= 0.0)
        return 0.0;

    double guess = value > 1.0 ? value : 1.0;
    for (int i = 0; i < 12; ++i)
        guess = 0.5 * (guess + value / guess);
    return guess;
}

/* Coverage of the pixel at (x, y) by a disc of `radius` centred on
 * (centre_x, centre_y), as 0..255. */
static unsigned disc_coverage(double centre_x, double centre_y, double radius, int x, int y)
{
    double const dx = (double)x + 0.5 - centre_x;
    double const dy = (double)y + 0.5 - centre_y;
    double const squared = dx * dx + dy * dy;

    /*
     * Only the one-pixel band at the edge of the disc needs a distance at all;
     * everything inside is solid and everything outside is nothing. Comparing
     * squares first keeps the root off all but a handful of pixels per corner,
     * and the root here is twelve Newton iterations.
     */
    double const inner = radius - 0.5;
    double const outer = radius + 0.5;
    if (inner > 0.0 && squared <= inner * inner)
        return 255;
    if (squared >= outer * outer)
        return 0;

    double const distance = square_root(squared);
    double const coverage = radius - distance + 0.5;
    if (coverage <= 0.0)
        return 0;
    if (coverage >= 1.0)
        return 255;
    return (unsigned)(coverage * 255.0 + 0.5);
}

static int clamp_radius(UiRect rect, int radius)
{
    if (radius < 0)
        radius = 0;
    if (radius * 2 > rect.width)
        radius = rect.width / 2;
    if (radius * 2 > rect.height)
        radius = rect.height / 2;
    return radius;
}

/*
 * One horizontal run, clipped once. `hole` is a rectangle to leave alone,
 * which is what lets a shadow skip the part of itself that the opaque window
 * is about to cover -- by far the largest part, and the reason the desktop
 * used to spend a fifth of a second on a frame.
 */
static void span_alpha(UiPainter* painter, int y, int from, int to, unsigned colour, unsigned alpha,
    const UiRect* hole)
{
    if (alpha == 0 || from >= to)
        return;

    if (hole != NULL && y >= hole->y && y < hole->y + hole->height) {
        int const hole_x1 = hole->x + hole->width;
        if (from < hole->x)
            span_alpha(painter, y, from, to < hole->x ? to : hole->x, colour, alpha, NULL);
        if (to > hole_x1)
            span_alpha(painter, y, from > hole_x1 ? from : hole_x1, to, colour, alpha, NULL);
        return;
    }

    ClippedRect const box = clip_rect(painter, from, y, to - from, 1);
    if (clipped_is_empty(box))
        return;

    unsigned* row = painter->pixels + (size_t)y * painter->width;
    if (alpha >= 255) {
        for (int x = box.x0; x < box.x1; ++x)
            row[x] = colour;
        return;
    }
    for (int x = box.x0; x < box.x1; ++x)
        blend_at(&row[x], colour, alpha);
}

static void point_alpha(
    UiPainter* painter, int x, int y, unsigned colour, unsigned alpha, const UiRect* hole)
{
    if (hole != NULL && x >= hole->x && x < hole->x + hole->width && y >= hole->y
        && y < hole->y + hole->height)
        return;
    blend_pixel(painter, x, y, colour, alpha);
}

static void fill_rounded_hole(UiPainter* painter, UiRect rect, int radius, unsigned colour,
    unsigned alpha, const UiRect* hole)
{
    radius = clamp_radius(rect, radius);
    if (rect.width <= 0 || rect.height <= 0 || alpha == 0)
        return;

    int const x0 = painter->origin_x + rect.x;
    int const y0 = painter->origin_y + rect.y;

    if (radius == 0) {
        for (int y = 0; y < rect.height; ++y)
            span_alpha(painter, y0 + y, x0, x0 + rect.width, colour, alpha, hole);
        return;
    }

    /* The band between the corners: full width, nothing to smooth. */
    for (int y = radius; y < rect.height - radius; ++y)
        span_alpha(painter, y0 + y, x0, x0 + rect.width, colour, alpha, hole);

    /* The four corner blocks, and the solid span between each pair. */
    for (int y = 0; y < radius; ++y) {
        int const top = y0 + y;
        int const bottom = y0 + rect.height - 1 - y;

        for (int x = 0; x < radius; ++x) {
            unsigned const coverage = disc_coverage(radius, radius, radius, x, y);
            if (coverage == 0)
                continue;

            unsigned const value = alpha >= 255 ? coverage : (coverage * alpha) / 255;
            int const left = x0 + x;
            int const right = x0 + rect.width - 1 - x;

            point_alpha(painter, left, top, colour, value, hole);
            point_alpha(painter, right, top, colour, value, hole);
            point_alpha(painter, left, bottom, colour, value, hole);
            point_alpha(painter, right, bottom, colour, value, hole);
        }

        span_alpha(painter, top, x0 + radius, x0 + rect.width - radius, colour, alpha, hole);
        span_alpha(painter, bottom, x0 + radius, x0 + rect.width - radius, colour, alpha, hole);
    }
}

static void fill_rounded_alpha(
    UiPainter* painter, UiRect rect, int radius, unsigned colour, unsigned alpha)
{
    fill_rounded_hole(painter, rect, radius, colour, alpha, NULL);
}

void ui_fill_rounded(UiPainter* painter, UiRect rect, int radius, unsigned colour)
{
    fill_rounded_alpha(painter, rect, radius, colour, 255);
}

void ui_fill_rounded_alpha(
    UiPainter* painter, UiRect rect, int radius, unsigned colour, unsigned alpha)
{
    fill_rounded_alpha(painter, rect, radius, colour, alpha);
}

/*
 * A stroke is the difference between two discs: covered by the outer one and
 * not by the inner. Drawing it as a filled ring rather than as a line keeps the
 * corner anti-aliasing identical to the fill it sits on, which is what stops a
 * bordered control showing a pale seam where the two disagree.
 */
void ui_stroke_rounded_alpha(
    UiPainter* painter, UiRect rect, int radius, unsigned colour, unsigned alpha)
{
    radius = clamp_radius(rect, radius);
    if (rect.width <= 0 || rect.height <= 0 || alpha == 0)
        return;

    int const x0 = painter->origin_x + rect.x;
    int const y0 = painter->origin_y + rect.y;

    if (radius == 0) {
        span_alpha(painter, y0, x0, x0 + rect.width, colour, alpha, NULL);
        span_alpha(painter, y0 + rect.height - 1, x0, x0 + rect.width, colour, alpha, NULL);
        for (int y = 1; y < rect.height - 1; ++y) {
            blend_pixel(painter, x0, y0 + y, colour, alpha);
            blend_pixel(painter, x0 + rect.width - 1, y0 + y, colour, alpha);
        }
        return;
    }

    span_alpha(painter, y0, x0 + radius, x0 + rect.width - radius, colour, alpha, NULL);
    span_alpha(
        painter, y0 + rect.height - 1, x0 + radius, x0 + rect.width - radius, colour, alpha, NULL);

    for (int y = radius; y < rect.height - radius; ++y) {
        blend_pixel(painter, x0, y0 + y, colour, alpha);
        blend_pixel(painter, x0 + rect.width - 1, y0 + y, colour, alpha);
    }

    for (int y = 0; y < radius; ++y) {
        for (int x = 0; x < radius; ++x) {
            unsigned const outer = disc_coverage(radius, radius, radius, x, y);
            unsigned const inner = disc_coverage(radius, radius, radius - 1, x, y);
            if (outer <= inner)
                continue;

            unsigned value = outer - inner;
            if (alpha < 255)
                value = value * alpha / 255;
            int const left = x;
            int const right = rect.width - 1 - x;
            int const top = y;
            int const bottom = rect.height - 1 - y;

            blend_pixel(painter, x0 + left, y0 + top, colour, value);
            blend_pixel(painter, x0 + right, y0 + top, colour, value);
            blend_pixel(painter, x0 + left, y0 + bottom, colour, value);
            blend_pixel(painter, x0 + right, y0 + bottom, colour, value);
        }
    }
}

void ui_stroke_rounded(UiPainter* painter, UiRect rect, int radius, unsigned colour)
{
    ui_stroke_rounded_alpha(painter, rect, radius, colour, 255);
}

/*
 * --- elevation ----------------------------------------------------------------
 *
 * A soft shadow, built as a handful of progressively larger and fainter rounded
 * rectangles offset downwards. A real Gaussian blur would be a convolution over
 * the whole area for a difference nobody can see at these sizes; stacking four
 * layers costs four fills and reads as light coming from above, which is the
 * entire job.
 *
 * The shadow is drawn *before* the surface, so the layers that fall underneath
 * are covered up and only the fringe survives.
 */
void ui_drop_shadow(UiPainter* painter, UiRect rect, int radius, int elevation)
{
    if (elevation <= 0)
        return;

    /*
     * The window itself is opaque and is drawn straight over this, so every
     * layer's interior is thrown away the moment it is painted. Cutting it out
     * turns a stack of full-size rounded rectangles into a stack of rings, and
     * for a 720x440 window at elevation 14 that is five million alpha blends a
     * frame becoming about a quarter of a million. It was most of why the
     * desktop ran at four frames a second.
     */
    UiRect const covered = {
        .x = painter->origin_x + rect.x,
        .y = painter->origin_y + rect.y,
        .width = rect.width,
        .height = rect.height,
    };

    for (int layer = elevation; layer >= 1; --layer) {
        UiRect spread = {
            .x = rect.x - layer,
            .y = rect.y - layer + (elevation + 1) / 2,
            .width = rect.width + layer * 2,
            .height = rect.height + layer * 2,
        };

        /*
         * Very faint, and fainter the further out. A light interface shows a
         * shadow far more readily than a dark one -- what reads as a gentle
         * lift on white is a smear if it is given the weight that works on a
         * dark background.
         */
        unsigned const alpha = (unsigned)(14 / layer + 2);
        fill_rounded_hole(painter, spread, radius + layer, 0x000000, alpha, &covered);
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
