/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- the widget toolkit, laid out and painted on the host.
 *
 * The painter, the widgets and the text layer touch nothing but memory, so
 * they build and run here exactly as they do on the target. That matters more
 * than it sounds: every visual change used to cost a two-minute round trip
 * through QEMU, and an interface nobody can iterate on is an interface that
 * stays plain.
 *
 * It renders a gallery of every control in every state to a PNG, and checks
 * the things that are facts rather than judgements -- that layout puts widgets
 * inside their parents, that nothing is painted outside the surface, that
 * hit-testing finds what is under a point.
 */

#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The widgets reach for three things on a window, and none of them needs a
 * window server: whether to repaint, and who has focus. UiWindow is opaque in
 * ui.h and window.c is not linked here, so the harness supplies its own -- the
 * whole of what a widget can actually see.
 */
struct UiWindow {
    UiWidget* focused;
    int needs_paint;
    UiFonts fonts;
};

const UiFonts* ui_window_fonts(UiWindow* window)
{
    return window != NULL ? &window->fonts : NULL;
}

void ui_window_invalidate(UiWindow* window)
{
    if (window != NULL)
        window->needs_paint = 1;
}

void ui_window_focus(UiWindow* window, UiWidget* widget)
{
    if (window != NULL)
        window->focused = widget;
}

UiWidget* ui_window_focused(UiWindow* window)
{
    return window != NULL ? window->focused : NULL;
}

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

/* --- a surface, standing in for a window's shared buffer ------------------- */

typedef struct Surface {
    unsigned* pixels;
    int width, height;
} Surface;

static void surface_write_ppm(const Surface* surface, const char* path)
{
    FILE* out = fopen(path, "wb");
    if (out == NULL)
        return;

    fprintf(out, "P6\n%d %d\n255\n", surface->width, surface->height);
    for (int i = 0; i < surface->width * surface->height; ++i) {
        unsigned const pixel = surface->pixels[i];
        unsigned char const rgb[3]
            = { (unsigned char)(pixel >> 16), (unsigned char)(pixel >> 8), (unsigned char)pixel };
        fwrite(rgb, 1, 3, out);
    }
    fclose(out);
}

/* --- laying a tree out without a window ------------------------------------- */

static void layout_tree(UiWidget* root, int width, int height)
{
    root->rect.x = 0;
    root->rect.y = 0;
    root->rect.width = width;
    root->rect.height = height;
    if (root->klass->layout != NULL)
        root->klass->layout(root);
}

static void paint_tree(UiWidget* widget, UiPainter* painter)
{
    if (widget == NULL || !widget->visible)
        return;
    if (widget->rect.width <= 0 || widget->rect.height <= 0)
        return;

    UiPainter local = ui_painter_for(painter, widget->rect);
    if (local.clip.width <= 0 || local.clip.height <= 0)
        return;

    if (widget->klass->paint != NULL)
        widget->klass->paint(widget, &local);

    for (int i = 0; i < widget->child_count; ++i)
        paint_tree(widget->children[i], &local);
}

/* Every widget must end up inside the parent that placed it. A layout bug
 * shows up here as a rectangle hanging off the edge of its box. */
static void check_containment(UiWidget* widget, const char* path)
{
    for (int i = 0; i < widget->child_count; ++i) {
        UiWidget* child = widget->children[i];
        if (!child->visible)
            continue;

        char here[256];
        snprintf(here, sizeof(here), "%s > %s", path, child->klass->name);

        if (child->rect.x < 0 || child->rect.y < 0
            || child->rect.x + child->rect.width > widget->rect.width
            || child->rect.y + child->rect.height > widget->rect.height) {
            ++s_failures;
            ++s_checks;
            printf("  FAIL %s escapes its parent: %d,%d %dx%d in %dx%d\n", here, child->rect.x,
                child->rect.y, child->rect.width, child->rect.height, widget->rect.width,
                widget->rect.height);
        } else {
            ++s_checks;
        }

        check_containment(child, here);
    }
}

static void noop(UiWidget* widget, void* user)
{
    (void)widget;
    (void)user;
}

/* --- the gallery -------------------------------------------------------------- */

static UiWidget* section(UiWidget* parent, const char* title)
{
    UiWidget* heading = ui_label_create(title);
    ui_label_set_style(heading, UI_TEXT_SMALL);
    ui_label_set_colour(heading, ui_theme()->text_dim);
    ui_widget_add(parent, heading);

    UiWidget* row = ui_box_create(UI_HORIZONTAL);
    ui_box_set_padding(row, 0);
    ui_widget_add(parent, row);
    return row;
}

int main(int argc, char** argv)
{
    const char* output = argc > 1 ? argv[1] : "ui.ppm";

    Surface surface = { .width = 720, .height = 560 };
    surface.pixels = calloc((size_t)surface.width * surface.height, sizeof(unsigned));
    check(surface.pixels != NULL, "allocating the surface");
    if (surface.pixels == NULL)
        return 1;

    UiFonts fonts;
    const char* directory = getenv("SHITOS_UI_FONTS");
    check(ui_fonts_open(&fonts, directory != NULL ? directory : "/usr/share/fonts") == 0,
        "opening the font set");
    check(fonts.body != NULL, "the body face opened");
    check(fonts.strong != NULL, "the bold face opened");
    check(fonts.mono != NULL, "the monospace face opened");

    UiWidget* root = ui_box_create(UI_VERTICAL);
    ui_box_set_padding(root, 22);
    ui_box_set_spacing(root, 14);

    UiWidget* title = ui_label_create("Every control, every state");
    ui_label_set_style(title, UI_TEXT_HEADING);
    ui_widget_add(root, title);

    UiWidget* panel = ui_panel_create(UI_VERTICAL);
    panel->expand_x = 1;
    ui_widget_add(root, panel);

    /* Buttons, in each of the states a pointer can put them in. */
    UiWidget* buttons = section(panel, "Buttons");
    UiWidget* def = ui_button_create("Default", noop, NULL);
    ui_button_set_default(def, 1);
    UiWidget* normal = ui_button_create("Normal", noop, NULL);
    UiWidget* hovered = ui_button_create("Hovered", noop, NULL);
    UiWidget* disabled = ui_button_create("Disabled", noop, NULL);
    hovered->hovered = 1;
    disabled->enabled = 0;
    ui_widget_add(buttons, def);
    ui_widget_add(buttons, normal);
    ui_widget_add(buttons, hovered);
    ui_widget_add(buttons, disabled);

    /* Checkboxes. */
    UiWidget* checks = ui_box_create(UI_VERTICAL);
    ui_box_set_padding(checks, 0);
    ui_box_set_spacing(checks, 2);
    ui_widget_add(checks, ui_checkbox_create("Unchecked", 0));
    ui_widget_add(checks, ui_checkbox_create("Checked", 1));
    UiWidget* disabled_check = ui_checkbox_create("Disabled and checked", 1);
    disabled_check->enabled = 0;
    ui_widget_add(checks, disabled_check);
    UiWidget* checks_row = section(panel, "Checkboxes");
    ui_widget_add(checks_row, checks);

    /* Text fields. */
    UiWidget* fields = section(panel, "Text fields");
    UiWidget* field = ui_textfield_create("Typed something");
    field->expand_x = 1;
    ui_widget_add(fields, field);
    ui_widget_add(fields, ui_textfield_create(""));

    /* The type scale, which is the thing that stops it looking like a test
     * harness. */
    UiWidget* labels = ui_box_create(UI_VERTICAL);
    ui_box_set_padding(labels, 0);
    ui_box_set_spacing(labels, 2);

    UiWidget* strong = ui_label_create("Strong text, for emphasis");
    ui_label_set_style(strong, UI_TEXT_STRONG);
    UiWidget* bright = ui_label_create("Body text, which should be easy to read");
    UiWidget* dim = ui_label_create("Secondary text, which should be clearly quieter");
    ui_label_set_colour(dim, ui_theme()->text_dim);
    UiWidget* small = ui_label_create("Small text, for captions and asides");
    ui_label_set_style(small, UI_TEXT_SMALL);
    ui_label_set_colour(small, ui_theme()->text_dim);
    UiWidget* mono = ui_label_create("Monospace 0O1lI, for the terminal");
    ui_label_set_style(mono, UI_TEXT_MONO);

    ui_widget_add(labels, strong);
    ui_widget_add(labels, bright);
    ui_widget_add(labels, dim);
    ui_widget_add(labels, small);
    ui_widget_add(labels, mono);
    UiWidget* labels_row = section(panel, "Type");
    ui_widget_add(labels_row, labels);

    /* Give the tree a window so that focus is a thing that exists. */
    static UiWindow window;
    UiWidget* stack[128];
    int depth = 0;
    stack[depth++] = root;
    while (depth > 0) {
        UiWidget* widget = stack[--depth];
        widget->window = &window;
        for (int i = 0; i < widget->child_count && depth < 128; ++i)
            stack[depth++] = widget->children[i];
    }
    window.fonts = fonts;
    ui_window_focus(&window, field);

    layout_tree(root, surface.width, surface.height);
    check_containment(root, "root");

    /* Hit testing has to find the deepest thing under a point. */
    UiWidget* hit
        = ui_widget_at(root, root->rect.x + panel->rect.x + buttons->rect.x + normal->rect.x + 5,
            root->rect.y + panel->rect.y + buttons->rect.y + normal->rect.y + 5);
    check(hit == normal, "hit testing finds the button under the pointer");
    check(ui_widget_at(root, -5, -5) == NULL, "and nothing outside the tree");

    UiPainter painter = {
        .pixels = surface.pixels,
        .width = surface.width,
        .height = surface.height,
        .clip = { 0, 0, surface.width, surface.height },
        .font = fonts.body,
        .fonts = &fonts,
    };

    UiRect const whole = { 0, 0, surface.width, surface.height };
    ui_fill_rect(&painter, whole, ui_theme()->background);
    paint_tree(root, &painter);

    /* Something was drawn, and it was not all one colour. */
    unsigned const first = surface.pixels[0];
    int different = 0;
    for (int i = 0; i < surface.width * surface.height; ++i) {
        if (surface.pixels[i] != first)
            ++different;
    }
    check(different > surface.width, "the gallery painted something");

    surface_write_ppm(&surface, output);

    ui_widget_destroy(root);
    ui_fonts_close(&fonts);
    free(surface.pixels);

    printf("%d checks, %d failed\n", s_checks, s_failures);
    return s_failures == 0 ? 0 : 1;
}
