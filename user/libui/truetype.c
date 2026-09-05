/* SPDX-License-Identifier: GPL-3.0-or-later */
/* shit os 2 -- a TrueType rasteriser, written from the specification. */

#include "truetype.h"

#include <stdlib.h>
#include <string.h>

/* --- reading the file ------------------------------------------------------
 *
 * Everything in a TrueType file is big-endian and unaligned, so every read
 * goes through these rather than through a cast. A cast would be undefined
 * behaviour on the alignment and wrong on the byte order, and it would be
 * wrong silently on the machine this is developed on.
 */

static unsigned u8_at(const unsigned char* p)
{
    return p[0];
}

static unsigned u16_at(const unsigned char* p)
{
    return ((unsigned)p[0] << 8) | p[1];
}

static int i16_at(const unsigned char* p)
{
    return (int)(short)u16_at(p);
}

static unsigned u32_at(const unsigned char* p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}

static unsigned find_table(const unsigned char* data, size_t length, const char* tag)
{
    if (length < 12)
        return 0;

    unsigned const count = u16_at(data + 4);
    for (unsigned i = 0; i < count; ++i) {
        const unsigned char* record = data + 12 + i * 16;
        if ((size_t)(record - data) + 16 > length)
            return 0;
        if (memcmp(record, tag, 4) == 0)
            return u32_at(record + 8);
    }
    return 0;
}

int tt_open(TtFont* font, const void* data, size_t length)
{
    memset(font, 0, sizeof(*font));
    font->data = (const unsigned char*)data;
    font->length = length;

    if (length < 12)
        return -1;

    unsigned const version = u32_at(font->data);
    /* 0x00010000 is TrueType outlines; 'true' is the same thing as Apple
     * wrote it. An OpenType file with CFF outlines says 'OTTO' and has no
     * glyf table -- those are cubic Béziers in a different container and are
     * not supported, so it is rejected here rather than half-parsed. */
    if (version != 0x00010000u && memcmp(font->data, "true", 4) != 0)
        return -1;

    unsigned const head = find_table(font->data, length, "head");
    unsigned const maxp = find_table(font->data, length, "maxp");
    unsigned const hhea = find_table(font->data, length, "hhea");

    font->glyf = find_table(font->data, length, "glyf");
    font->loca = find_table(font->data, length, "loca");
    font->cmap = find_table(font->data, length, "cmap");
    font->hmtx = find_table(font->data, length, "hmtx");
    font->kern = find_table(font->data, length, "kern");

    if (head == 0 || maxp == 0 || hhea == 0 || font->glyf == 0 || font->loca == 0 || font->cmap == 0
        || font->hmtx == 0)
        return -1;
    if (head + 54 > length || maxp + 6 > length || hhea + 36 > length)
        return -1;

    font->units_per_em = (int)u16_at(font->data + head + 18);
    font->long_loca = i16_at(font->data + head + 50) != 0;
    font->glyph_count = (int)u16_at(font->data + maxp + 4);

    font->ascent = i16_at(font->data + hhea + 4);
    font->descent = i16_at(font->data + hhea + 6);
    font->line_gap = i16_at(font->data + hhea + 8);
    font->metric_count = (int)u16_at(font->data + hhea + 34);

    if (font->units_per_em == 0 || font->glyph_count == 0)
        return -1;

    return 0;
}

double tt_scale_for_pixel_size(const TtFont* font, double pixels)
{
    return pixels / (double)font->units_per_em;
}

/* --- cmap ------------------------------------------------------------------
 *
 * Only formats 4 and 12 are read. Format 4 is the Basic Multilingual Plane and
 * is in every font that exists; format 12 covers everything above it. The
 * older byte-oriented formats describe encodings nothing here uses.
 */

static int lookup_format4(const unsigned char* table, unsigned codepoint)
{
    if (codepoint > 0xFFFF)
        return 0;

    unsigned const segment_count = u16_at(table + 6) / 2;
    const unsigned char* ends = table + 14;
    const unsigned char* starts = ends + segment_count * 2 + 2;
    const unsigned char* deltas = starts + segment_count * 2;
    const unsigned char* ranges = deltas + segment_count * 2;

    for (unsigned i = 0; i < segment_count; ++i) {
        if (codepoint > u16_at(ends + i * 2))
            continue;
        unsigned const start = u16_at(starts + i * 2);
        if (codepoint < start)
            return 0; /* falls in the gap before this segment */

        unsigned const range_offset = u16_at(ranges + i * 2);
        if (range_offset == 0)
            return (int)((codepoint + u16_at(deltas + i * 2)) & 0xFFFF);

        /* The offset is from the position of the entry itself, which is the
         * one genuinely strange thing in this format. */
        const unsigned char* at = ranges + i * 2 + range_offset + (codepoint - start) * 2;
        unsigned const glyph = u16_at(at);
        if (glyph == 0)
            return 0;
        return (int)((glyph + u16_at(deltas + i * 2)) & 0xFFFF);
    }
    return 0;
}

static int lookup_format12(const unsigned char* table, unsigned codepoint)
{
    unsigned const groups = u32_at(table + 12);
    for (unsigned i = 0; i < groups; ++i) {
        const unsigned char* group = table + 16 + i * 12;
        unsigned const first = u32_at(group);
        unsigned const last = u32_at(group + 4);
        if (codepoint < first)
            return 0; /* groups are sorted, so we have gone past it */
        if (codepoint <= last)
            return (int)(u32_at(group + 8) + (codepoint - first));
    }
    return 0;
}

int tt_glyph_for_codepoint(const TtFont* font, unsigned codepoint)
{
    const unsigned char* cmap = font->data + font->cmap;
    unsigned const count = u16_at(cmap + 2);

    unsigned best = 0;
    int best_score = -1;

    for (unsigned i = 0; i < count; ++i) {
        const unsigned char* record = cmap + 4 + i * 8;
        unsigned const platform = u16_at(record);
        unsigned const encoding = u16_at(record + 2);
        unsigned const offset = u32_at(record + 4);

        /* Prefer full Unicode over the BMP, and Windows over anything else,
         * because that is the subtable every font actually maintains. */
        int score = -1;
        if (platform == 3 && encoding == 10)
            score = 4;
        else if (platform == 0 && encoding >= 4)
            score = 3;
        else if (platform == 3 && encoding == 1)
            score = 2;
        else if (platform == 0)
            score = 1;

        if (score > best_score) {
            best_score = score;
            best = offset;
        }
    }

    if (best_score < 0)
        return 0;

    const unsigned char* table = cmap + best;
    switch (u16_at(table)) {
    case 4: return lookup_format4(table, codepoint);
    case 12: return lookup_format12(table, codepoint);
    default: return 0;
    }
}

/* --- metrics --------------------------------------------------------------- */

int tt_advance(const TtFont* font, int glyph)
{
    if (font->metric_count == 0)
        return 0;

    /* Past the last full entry every glyph shares the last advance; only the
     * bearings continue. Monospace fonts store exactly one entry. */
    int const index = glyph < font->metric_count ? glyph : font->metric_count - 1;
    return (int)u16_at(font->data + font->hmtx + index * 4);
}

int tt_kern(const TtFont* font, int left, int right)
{
    if (font->kern == 0)
        return 0;

    const unsigned char* table = font->data + font->kern;
    unsigned const tables = u16_at(table + 2);
    const unsigned char* subtable = table + 4;

    for (unsigned i = 0; i < tables; ++i) {
        unsigned const length = u16_at(subtable + 2);
        unsigned const coverage = u16_at(subtable + 4);

        /* Format 0, horizontal, not minimum-values: the only kind worth
         * reading, and the only kind fonts ship. */
        if ((coverage & 0xFF00) == 0 && (coverage & 0x0001) != 0) {
            unsigned const pairs = u16_at(subtable + 6);
            unsigned const want = ((unsigned)left << 16) | (unsigned)right;

            /* The pairs are sorted, so this is a binary search. */
            unsigned low = 0;
            unsigned high = pairs;
            while (low < high) {
                unsigned const middle = (low + high) / 2;
                const unsigned char* entry = subtable + 14 + middle * 6;
                unsigned const key = u32_at(entry);
                if (key < want)
                    low = middle + 1;
                else if (key > want)
                    high = middle;
                else
                    return i16_at(entry + 4);
            }
        }

        subtable += length;
    }
    return 0;
}

/* --- outlines --------------------------------------------------------------
 *
 * A contour is a closed loop of points, each on or off the curve. Two
 * consecutive off-curve points imply an on-curve point exactly between them,
 * which is the compression that makes TrueType outlines small and the parsing
 * fiddly.
 */

typedef struct Point {
    double x, y;
} Point;

typedef struct Edge {
    double x0, y0, x1, y1;
    int direction; /* +1 or -1, for the non-zero winding rule */
} Edge;

typedef struct EdgeList {
    Edge* items;
    int count;
    int capacity;
} EdgeList;

static int edges_push(EdgeList* list, double x0, double y0, double x1, double y1)
{
    /* A horizontal edge crosses no scanline and contributes no winding. */
    if (y0 == y1)
        return 0;

    if (list->count == list->capacity) {
        int const capacity = list->capacity == 0 ? 64 : list->capacity * 2;
        Edge* grown = realloc(list->items, (size_t)capacity * sizeof(Edge));
        if (grown == NULL)
            return -1;
        list->items = grown;
        list->capacity = capacity;
    }

    Edge* edge = &list->items[list->count++];
    if (y0 < y1) {
        edge->x0 = x0;
        edge->y0 = y0;
        edge->x1 = x1;
        edge->y1 = y1;
        edge->direction = 1;
    } else {
        edge->x0 = x1;
        edge->y0 = y1;
        edge->x1 = x0;
        edge->y1 = y0;
        edge->direction = -1;
    }
    return 0;
}

/*
 * Flatten a quadratic into line segments. The step count comes from how far
 * the control point is from the chord: a nearly straight curve gets two
 * segments and a tight one gets many, so the cost follows the curvature
 * rather than being the same everywhere.
 */
static int emit_quadratic(EdgeList* edges, Point a, Point control, Point b)
{
    double const dx = (a.x + b.x) * 0.5 - control.x;
    double const dy = (a.y + b.y) * 0.5 - control.y;
    double const deviation = dx * dx + dy * dy;

    int steps = 2;
    while (steps < 32 && deviation > (double)(steps * steps) * 0.05)
        ++steps;

    Point previous = a;
    for (int i = 1; i <= steps; ++i) {
        double const t = (double)i / steps;
        double const s = 1.0 - t;
        Point next;
        next.x = s * s * a.x + 2.0 * s * t * control.x + t * t * b.x;
        next.y = s * s * a.y + 2.0 * s * t * control.y + t * t * b.y;
        if (edges_push(edges, previous.x, previous.y, next.x, next.y) < 0)
            return -1;
        previous = next;
    }
    return 0;
}

/* Forward declaration: a composite glyph is made of other glyphs. */
static int collect_glyph(
    const TtFont* font, int glyph, double scale, double dx, double dy, EdgeList* edges, int depth);

static unsigned glyph_offset(const TtFont* font, int glyph, unsigned* end)
{
    if (glyph < 0 || glyph >= font->glyph_count)
        return 0;

    if (font->long_loca) {
        unsigned const start = u32_at(font->data + font->loca + (unsigned)glyph * 4);
        *end = u32_at(font->data + font->loca + (unsigned)glyph * 4 + 4);
        return start;
    }
    unsigned const start = u16_at(font->data + font->loca + (unsigned)glyph * 2) * 2u;
    *end = u16_at(font->data + font->loca + (unsigned)glyph * 2 + 2) * 2u;
    return start;
}

#define FLAG_ON_CURVE 0x01
#define FLAG_X_SHORT 0x02
#define FLAG_Y_SHORT 0x04
#define FLAG_REPEAT 0x08
#define FLAG_X_SAME_OR_POSITIVE 0x10
#define FLAG_Y_SAME_OR_POSITIVE 0x20

static int collect_simple(const unsigned char* glyph, int contour_count, double scale, double dx,
    double dy, EdgeList* edges)
{
    const unsigned char* ends = glyph + 10;
    int const point_count = (int)u16_at(ends + (contour_count - 1) * 2) + 1;
    if (point_count <= 0)
        return 0;

    /* Skip the hinting bytecode, which is not interpreted. */
    const unsigned char* p = ends + contour_count * 2;
    unsigned const instruction_length = u16_at(p);
    p += 2 + instruction_length;

    unsigned char* flags = malloc((size_t)point_count);
    Point* points = malloc((size_t)point_count * sizeof(Point));
    if (flags == NULL || points == NULL) {
        free(flags);
        free(points);
        return -1;
    }

    /* Flags are run-length encoded: a flag with REPEAT set is followed by a
     * count of how many more points share it. */
    for (int i = 0; i < point_count;) {
        unsigned char const flag = *p++;
        flags[i++] = flag;
        if ((flag & FLAG_REPEAT) != 0) {
            unsigned repeats = u8_at(p++);
            while (repeats-- > 0 && i < point_count)
                flags[i++] = flag;
        }
    }

    /* Coordinates are deltas, and each axis is stored in full before the
     * next begins. */
    int value = 0;
    for (int i = 0; i < point_count; ++i) {
        unsigned char const flag = flags[i];
        if ((flag & FLAG_X_SHORT) != 0) {
            int const delta = (int)u8_at(p++);
            value += (flag & FLAG_X_SAME_OR_POSITIVE) != 0 ? delta : -delta;
        } else if ((flag & FLAG_X_SAME_OR_POSITIVE) == 0) {
            value += i16_at(p);
            p += 2;
        }
        points[i].x = (double)value * scale + dx;
    }

    value = 0;
    for (int i = 0; i < point_count; ++i) {
        unsigned char const flag = flags[i];
        if ((flag & FLAG_Y_SHORT) != 0) {
            int const delta = (int)u8_at(p++);
            value += (flag & FLAG_Y_SAME_OR_POSITIVE) != 0 ? delta : -delta;
        } else if ((flag & FLAG_Y_SAME_OR_POSITIVE) == 0) {
            value += i16_at(p);
            p += 2;
        }
        points[i].y = (double)value * scale + dy;
    }

    int result = 0;
    int first = 0;
    for (int contour = 0; contour < contour_count && result == 0; ++contour) {
        int const last = (int)u16_at(ends + contour * 2);
        int const count = last - first + 1;
        if (count <= 0) {
            first = last + 1;
            continue;
        }

        /*
         * The loop needs to start on the curve. If the first point is not,
         * either the last one is and we start there, or neither is and the
         * implied midpoint between them is.
         */
        Point start;
        int index = first;
        if ((flags[first] & FLAG_ON_CURVE) != 0) {
            start = points[first];
            index = first + 1;
        } else if ((flags[last] & FLAG_ON_CURVE) != 0) {
            start = points[last];
        } else {
            start.x = (points[first].x + points[last].x) * 0.5;
            start.y = (points[first].y + points[last].y) * 0.5;
        }

        Point cursor = start;
        Point control;
        int have_control = 0;

        for (int step = 0; step < count; ++step) {
            int const at = first + ((index - first) + step) % count;
            Point const point = points[at];
            int const on_curve = (flags[at] & FLAG_ON_CURVE) != 0;

            if (on_curve) {
                if (have_control) {
                    result = emit_quadratic(edges, cursor, control, point);
                    have_control = 0;
                } else {
                    result = edges_push(edges, cursor.x, cursor.y, point.x, point.y);
                }
                cursor = point;
            } else if (have_control) {
                /* Two off-curve points in a row: the on-curve point between
                 * them is implied rather than stored. */
                Point implied;
                implied.x = (control.x + point.x) * 0.5;
                implied.y = (control.y + point.y) * 0.5;
                result = emit_quadratic(edges, cursor, control, implied);
                cursor = implied;
                control = point;
            } else {
                control = point;
                have_control = 1;
            }

            if (result != 0)
                break;
        }

        if (result == 0) {
            /* Close the contour back to where it started. */
            if (have_control)
                result = emit_quadratic(edges, cursor, control, start);
            else
                result = edges_push(edges, cursor.x, cursor.y, start.x, start.y);
        }

        first = last + 1;
    }

    free(flags);
    free(points);
    return result;
}

#define COMPONENT_ARGS_ARE_WORDS 0x0001
#define COMPONENT_ARGS_ARE_XY 0x0002
#define COMPONENT_HAVE_SCALE 0x0008
#define COMPONENT_MORE 0x0020
#define COMPONENT_XY_SCALE 0x0040
#define COMPONENT_TWO_BY_TWO 0x0080

static int collect_composite(const TtFont* font, const unsigned char* glyph, double scale,
    double dx, double dy, EdgeList* edges, int depth)
{
    const unsigned char* p = glyph + 10;

    for (;;) {
        unsigned const flags = u16_at(p);
        int const component = (int)u16_at(p + 2);
        p += 4;

        double offset_x = 0;
        double offset_y = 0;

        if ((flags & COMPONENT_ARGS_ARE_WORDS) != 0) {
            offset_x = i16_at(p);
            offset_y = i16_at(p + 2);
            p += 4;
        } else {
            offset_x = (double)(signed char)p[0];
            offset_y = (double)(signed char)p[1];
            p += 2;
        }

        /*
         * Only the offset form is honoured. The other form matches a point in
         * this glyph to a point in the component, which needs the component's
         * points before it has been placed -- a two-pass arrangement that no
         * font in practice uses for anything a reader would notice.
         */
        if ((flags & COMPONENT_ARGS_ARE_XY) == 0) {
            offset_x = 0;
            offset_y = 0;
        }

        /*
         * Scales are skipped past but not applied. Composites in practice are
         * accents placed on letters at their natural size; the scaled and
         * two-by-two forms are for effects that would need the whole affine
         * transform threaded through the outline collector, which is a lot of
         * machinery for a case this rasteriser has never yet been handed.
         */
        if ((flags & COMPONENT_HAVE_SCALE) != 0)
            p += 2;
        else if ((flags & COMPONENT_XY_SCALE) != 0)
            p += 4;
        else if ((flags & COMPONENT_TWO_BY_TWO) != 0)
            p += 8;

        if (collect_glyph(font, component, scale, dx + offset_x * scale, dy + offset_y * scale,
                edges, depth + 1)
            < 0)
            return -1;

        if ((flags & COMPONENT_MORE) == 0)
            break;
    }
    return 0;
}

static int collect_glyph(
    const TtFont* font, int glyph, double scale, double dx, double dy, EdgeList* edges, int depth)
{
    /* A composite that refers to itself would recurse forever. Fonts are
     * data, and data from a file is not to be trusted with the stack. */
    if (depth > 8)
        return 0;

    unsigned end = 0;
    unsigned const start = glyph_offset(font, glyph, &end);
    if (end <= start)
        return 0; /* an empty glyph: a space, legitimately */
    if (font->glyf + end > font->length)
        return -1;

    const unsigned char* data = font->data + font->glyf + start;
    int const contour_count = i16_at(data);

    if (contour_count >= 0)
        return collect_simple(data, contour_count, scale, dx, dy, edges);
    return collect_composite(font, data, scale, dx, dy, edges, depth);
}

/* --- rasterising -----------------------------------------------------------
 *
 * Scanline fill with the non-zero winding rule, five sub-scanlines per pixel
 * row and exact fractional coverage horizontally. That combination is what
 * makes a diagonal stem look smooth without the cost of full supersampling:
 * vertical detail is where a glyph has the most of it, and horizontally the
 * exact span endpoints are cheap to compute.
 */

#define SUBSAMPLES 5

typedef struct Crossing {
    double x;
    int direction;
} Crossing;

static int compare_crossings(const void* a, const void* b)
{
    double const x = ((const Crossing*)a)->x;
    double const y = ((const Crossing*)b)->x;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static void add_span(double* coverage, int width, double x0, double x1, double weight)
{
    if (x1 <= x0)
        return;
    if (x0 < 0)
        x0 = 0;
    if (x1 > width)
        x1 = width;
    if (x1 <= x0)
        return;

    int const first = (int)x0;
    int const last = (int)(x1 - 1e-9);

    if (first == last) {
        coverage[first] += (x1 - x0) * weight;
        return;
    }

    coverage[first] += ((double)(first + 1) - x0) * weight;
    for (int x = first + 1; x < last; ++x)
        coverage[x] += weight;
    if (last < width)
        coverage[last] += (x1 - (double)last) * weight;
}

int tt_render_glyph(const TtFont* font, int glyph, double scale, TtBitmap* out)
{
    memset(out, 0, sizeof(*out));

    EdgeList edges = { 0 };
    if (collect_glyph(font, glyph, scale, 0.0, 0.0, &edges, 0) < 0) {
        free(edges.items);
        return -1;
    }
    if (edges.count == 0) {
        free(edges.items);
        return 0; /* no outline: a space */
    }

    /* The bounding box comes from the flattened edges rather than from the
     * glyph header, because the header's box is in design units and rounding
     * it can be a pixel short of what actually gets drawn. */
    double min_x = edges.items[0].x0;
    double max_x = min_x;
    double min_y = edges.items[0].y0;
    double max_y = min_y;

    for (int i = 0; i < edges.count; ++i) {
        Edge const* edge = &edges.items[i];
        double const xs[2] = { edge->x0, edge->x1 };
        double const ys[2] = { edge->y0, edge->y1 };
        for (int k = 0; k < 2; ++k) {
            if (xs[k] < min_x)
                min_x = xs[k];
            if (xs[k] > max_x)
                max_x = xs[k];
            if (ys[k] < min_y)
                min_y = ys[k];
            if (ys[k] > max_y)
                max_y = ys[k];
        }
    }

    int const left = (int)__builtin_floor(min_x);
    int const bottom = (int)__builtin_floor(min_y);
    int const right = (int)__builtin_ceil(max_x);
    int const top = (int)__builtin_ceil(max_y);

    int const width = right - left;
    int const height = top - bottom;
    if (width <= 0 || height <= 0) {
        free(edges.items);
        return 0;
    }

    unsigned char* pixels = calloc((size_t)width * height, 1);
    double* coverage = calloc((size_t)width, sizeof(double));
    Crossing* crossings = malloc((size_t)edges.count * sizeof(Crossing));
    if (pixels == NULL || coverage == NULL || crossings == NULL) {
        free(pixels);
        free(coverage);
        free(crossings);
        free(edges.items);
        return -1;
    }

    double const weight = 1.0 / SUBSAMPLES;

    for (int row = 0; row < height; ++row) {
        memset(coverage, 0, (size_t)width * sizeof(double));

        /* Row 0 of the bitmap is the top of the glyph, and y grows upward in
         * the font, so the rows are walked in the opposite order. */
        double const row_top = (double)(top - row);

        for (int sub = 0; sub < SUBSAMPLES; ++sub) {
            double const y = row_top - ((double)sub + 0.5) / SUBSAMPLES;

            int found = 0;
            for (int i = 0; i < edges.count; ++i) {
                Edge const* edge = &edges.items[i];
                /* Half-open in y, so a vertex shared by two edges is counted
                 * exactly once and does not leave a pinhole. */
                if (y < edge->y0 || y >= edge->y1)
                    continue;
                double const t = (y - edge->y0) / (edge->y1 - edge->y0);
                crossings[found].x = edge->x0 + t * (edge->x1 - edge->x0) - (double)left;
                crossings[found].direction = edge->direction;
                ++found;
            }

            if (found < 2)
                continue;

            qsort(crossings, (size_t)found, sizeof(Crossing), compare_crossings);

            /* Non-zero winding: inside wherever the running total is not
             * zero. Even-odd would hollow out the inner ring of an 'o' the
             * wrong way round in fonts that wind both contours alike. */
            int winding = 0;
            for (int i = 0; i + 1 <= found - 1; ++i) {
                winding += crossings[i].direction;
                if (winding != 0)
                    add_span(coverage, width, crossings[i].x, crossings[i + 1].x, weight);
            }
        }

        unsigned char* line = pixels + (size_t)row * width;
        for (int x = 0; x < width; ++x) {
            double value = coverage[x];
            if (value <= 0.0)
                continue;
            if (value > 1.0)
                value = 1.0;
            line[x] = (unsigned char)(value * 255.0 + 0.5);
        }
    }

    free(coverage);
    free(crossings);
    free(edges.items);

    out->pixels = pixels;
    out->width = width;
    out->height = height;
    out->left = left;
    out->top = top;
    return 0;
}
