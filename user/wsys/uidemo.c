/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- an application, written against the toolkit.
 *
 * Worth comparing with wsysdemo.c, which does the same job against the raw
 * protocol: two FIFOs, a hello, a mapped buffer, damage messages, and its own
 * idea of what a button is. Here there is a tree and some callbacks, and every
 * pixel of that is somebody else's problem.
 */

#include "ui.h"

#include <stdio.h>
#include <string.h>

static UiWidget* g_status;
static UiWidget* g_field;
static UiWidget* g_counter;
static int g_count;

static void on_greet(UiWidget* button, void* user)
{
    (void)button;
    (void)user;

    char message[128];
    const char* name = ui_textfield_text(g_field);
    snprintf(
        message, sizeof(message), "Hello, %s.", name[0] != '\0' ? name : "nobody in particular");
    ui_label_set_text(g_status, message);
}

static void on_count(UiWidget* button, void* user)
{
    (void)button;
    (void)user;

    ++g_count;

    char message[64];
    snprintf(message, sizeof(message), "clicked %d time%s", g_count, g_count == 1 ? "" : "s");
    ui_label_set_text(g_counter, message);
}

static void on_quit(UiWidget* button, void* user)
{
    (void)user;
    ui_window_close(button->window);
}

int main(void)
{
    UiWindow* window = ui_window_create("widgets", 420, 300);
    if (window == NULL) {
        fprintf(stderr, "uidemo: no window server\n");
        return 1;
    }

    UiWidget* root = ui_box_create(UI_VERTICAL);
    ui_box_set_padding(root, 16);
    ui_box_set_spacing(root, 12);

    UiWidget* heading = ui_label_create("Widgets");
    ui_label_set_style(heading, UI_TEXT_HEADING);
    ui_widget_add(root, heading);

    /* A row: a field and the button that reads it. */
    UiWidget* panel = ui_panel_create(UI_VERTICAL);
    panel->expand_x = 1;
    panel->expand_y = 1;
    ui_widget_add(root, panel);

    UiWidget* row = ui_box_create(UI_HORIZONTAL);
    ui_box_set_padding(row, 0);
    g_field = ui_textfield_create("world");
    g_field->expand_x = 1;
    ui_widget_add(row, g_field);
    ui_widget_add(row, ui_button_create("Greet", on_greet, NULL));
    ui_widget_add(panel, row);

    g_status = ui_label_create("Type a name and press Greet.");
    ui_label_set_colour(g_status, ui_theme()->text_dim);
    ui_widget_add(panel, g_status);

    ui_widget_add(panel, ui_checkbox_create("A checkbox, which does nothing", 1));
    ui_widget_add(panel, ui_checkbox_create("And another, which also does nothing", 0));

    g_counter = ui_label_create("clicked 0 times");
    ui_label_set_colour(g_counter, ui_theme()->text_dim);
    ui_widget_add(panel, g_counter);

    /* A spacer that takes all the leftover height, which is how the buttons
     * below end up pinned to the bottom. */
    UiWidget* spacer = ui_box_create(UI_VERTICAL);
    spacer->expand_y = 1;
    ui_widget_add(panel, spacer);

    UiWidget* buttons = ui_box_create(UI_HORIZONTAL);
    ui_box_set_padding(buttons, 0);
    ui_widget_add(buttons, ui_button_create("Count", on_count, NULL));

    UiWidget* gap = ui_box_create(UI_HORIZONTAL);
    gap->expand_x = 1;
    ui_widget_add(buttons, gap);

    UiWidget* quit = ui_button_create("Quit", on_quit, NULL);
    ui_button_set_default(quit, 1);
    ui_widget_add(buttons, quit);
    ui_widget_add(root, buttons);

    ui_window_set_root(window, root);
    ui_window_focus(window, g_field);

    int const result = ui_window_run(window);
    ui_window_destroy(window);
    return result;
}
