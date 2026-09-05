/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- the window: a connection, a surface, and a tree.
 *
 * Everything wsysdemo does by hand happens here once -- the two FIFOs, the
 * hello, mapping the buffer, the damage messages -- so that an application is
 * widgets and callbacks and nothing else.
 */

#include <shitos/abi/input.h>
#include <shitos/wsys/protocol.h>

#include "ui.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct UiWindow {
    int to_server;
    int to_client;
    unsigned id;

    unsigned* pixels;
    size_t pixels_length;
    int width, height;

    UiWidget* root;
    UiWidget* focused;
    UiWidget* hovered;
    UiWidget* mouse_grab; /* whoever saw the press keeps the drag */

    UiFont* font;

    int needs_layout;
    int needs_paint;
    int running;

    char pending_title[WSYS_TITLE_MAX];
};

/* --- the connection ----------------------------------------------------------- */

static int send_message(UiWindow* window, const struct WsysMessage* message)
{
    return write(window->to_server, message, sizeof(*message)) == (ssize_t)sizeof(*message) ? 0
                                                                                            : -1;
}

static int connect_to_server(UiWindow* window)
{
    int const pid = (int)getpid();

    char to_server[WSYS_PATH_MAX];
    char to_client[WSYS_PATH_MAX];
    snprintf(to_server, sizeof(to_server), WSYS_ROOT "/%d.to-server", pid);
    snprintf(to_client, sizeof(to_client), WSYS_ROOT "/%d.to-client", pid);

    unlink(to_server);
    unlink(to_client);
    if (mkfifo(to_server, 0666) < 0 || mkfifo(to_client, 0666) < 0)
        return -1;

    /* Both O_RDWR and both before announcing, so no open can block and no
     * ordering between client and server can deadlock. */
    window->to_server = open(to_server, O_RDWR);
    window->to_client = open(to_client, O_RDWR);
    if (window->to_server < 0 || window->to_client < 0)
        return -1;

    int connect_fd = open(WSYS_CONNECT_FIFO, O_WRONLY);
    if (connect_fd < 0)
        return -1;

    struct WsysHello hello = { .pid = pid };
    ssize_t const written = write(connect_fd, &hello, sizeof(hello));
    close(connect_fd);
    return written == (ssize_t)sizeof(hello) ? 0 : -1;
}

/* --- painting the tree ---------------------------------------------------------- */

static void paint_widget(UiWidget* widget, UiPainter* painter)
{
    if (widget == NULL || !widget->visible)
        return;
    if (widget->rect.width <= 0 || widget->rect.height <= 0)
        return;

    UiPainter local = ui_painter_for(painter, widget->rect);
    if (local.clip.width <= 0 || local.clip.height <= 0)
        return; /* entirely outside its parent; nothing to do */

    if (widget->klass->paint != NULL)
        widget->klass->paint(widget, &local);

    for (int i = 0; i < widget->child_count; ++i)
        paint_widget(widget->children[i], &local);
}

static void repaint(UiWindow* window)
{
    if (window->pixels == NULL || window->root == NULL)
        return;

    if (window->needs_layout) {
        window->root->rect.x = 0;
        window->root->rect.y = 0;
        window->root->rect.width = window->width;
        window->root->rect.height = window->height;
        if (window->root->klass->layout != NULL)
            window->root->klass->layout(window->root);
        window->needs_layout = 0;
    }

    UiPainter painter = {
        .pixels = window->pixels,
        .width = window->width,
        .height = window->height,
        .origin_x = 0,
        .origin_y = 0,
        .clip = { 0, 0, window->width, window->height },
        .font = window->font,
        .bold = window->font,
    };

    UiRect const whole = { 0, 0, window->width, window->height };
    ui_fill_rect(&painter, whole, ui_theme()->window_background);

    paint_widget(window->root, &painter);

    struct WsysMessage message;
    memset(&message, 0, sizeof(message));
    message.type = WSYS_DAMAGE;
    message.window = window->id;
    message.damage.x = 0;
    message.damage.y = 0;
    message.damage.width = window->width;
    message.damage.height = window->height;
    (void)send_message(window, &message);

    window->needs_paint = 0;
}

/* --- events ----------------------------------------------------------------------- */

/* Walks up from the deepest widget until something consumes the event, which
 * is what lets a label inside a button still press the button. */
static void deliver_mouse(UiWindow* window, int x, int y, unsigned buttons, int wheel)
{
    UiWidget* target
        = window->mouse_grab != NULL ? window->mouse_grab : ui_widget_at(window->root, x, y);

    /* Hover, which is entirely the window's business: a widget only ever finds
     * out that it is or is not hovered. */
    UiWidget* under = ui_widget_at(window->root, x, y);
    if (under != window->hovered) {
        if (window->hovered != NULL) {
            window->hovered->hovered = 0;
            ui_widget_invalidate(window->hovered);
        }
        window->hovered = under;
        if (under != NULL) {
            under->hovered = 1;
            ui_widget_invalidate(under);
        }
    }

    if (target == NULL)
        return;

    /* A press grabs, so that dragging outside the widget still reaches it and
     * the release is seen by whoever saw the press. */
    if (buttons != 0 && window->mouse_grab == NULL)
        window->mouse_grab = target;
    else if (buttons == 0)
        window->mouse_grab = NULL;

    for (UiWidget* widget = target; widget != NULL; widget = widget->parent) {
        if (widget->klass->on_mouse == NULL)
            continue;

        /* A widget's rect is in its parent's coordinates, so where it actually
         * sits in the window is the sum of the chain above it. */
        int origin_x = 0;
        int origin_y = 0;
        for (UiWidget* above = widget; above != NULL; above = above->parent) {
            origin_x += above->rect.x;
            origin_y += above->rect.y;
        }

        UiMouseEvent const event = {
            .x = x - origin_x,
            .y = y - origin_y,
            .buttons = buttons,
            .wheel = wheel,
        };

        if (widget->klass->on_mouse(widget, &event))
            return;
    }
}

static void deliver_key(UiWindow* window, const struct WsysMessage* message)
{
    UiKeyEvent event = {
        .keycode = message->key.keycode,
        .codepoint = message->key.codepoint,
        .modifiers = message->key.modifiers,
        .pressed = message->key.pressed,
    };

    for (UiWidget* widget = window->focused; widget != NULL; widget = widget->parent) {
        if (widget->klass->on_key != NULL && widget->klass->on_key(widget, &event))
            return;
    }
}

/* --- the public surface -------------------------------------------------------------- */

UiWindow* ui_window_create(const char* title, int width, int height)
{
    UiWindow* window = calloc(1, sizeof(UiWindow));
    if (window == NULL)
        return NULL;

    window->to_server = -1;
    window->to_client = -1;
    window->running = 1;
    strncpy(window->pending_title, title != NULL ? title : "untitled",
        sizeof(window->pending_title) - 1);

    /* A client that dies mid-message must not take this process with it. */
    signal(SIGPIPE, SIG_IGN);

    if (connect_to_server(window) < 0) {
        free(window);
        return NULL;
    }

    window->font = ui_font_open("/usr/share/fonts/sans.ttf", 14.0);

    struct WsysMessage message;
    memset(&message, 0, sizeof(message));
    message.type = WSYS_CREATE_WINDOW;
    message.create.width = width;
    message.create.height = height;
    message.create.x = -1;
    message.create.y = -1;
    if (send_message(window, &message) < 0) {
        ui_window_destroy(window);
        return NULL;
    }

    /* Wait for the server to hand back a buffer. Nothing can be drawn until
     * it does, so there is nothing else to be doing. */
    for (;;) {
        struct WsysMessage reply;
        if (read(window->to_client, &reply, sizeof(reply)) != (ssize_t)sizeof(reply)) {
            ui_window_destroy(window);
            return NULL;
        }
        if (reply.type != WSYS_WINDOW_CREATED)
            continue;

        window->id = reply.window;
        window->width = reply.created.width;
        window->height = reply.created.height;
        window->pixels_length = (size_t)window->width * window->height * 4;

        int fd = open(reply.created.buffer_path, O_RDWR);
        if (fd < 0) {
            ui_window_destroy(window);
            return NULL;
        }
        window->pixels
            = mmap(NULL, window->pixels_length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);

        if (window->pixels == MAP_FAILED) {
            window->pixels = NULL;
            ui_window_destroy(window);
            return NULL;
        }
        break;
    }

    ui_window_set_title(window, window->pending_title);
    window->needs_layout = 1;
    window->needs_paint = 1;
    return window;
}

void ui_window_destroy(UiWindow* window)
{
    if (window == NULL)
        return;

    if (window->root != NULL)
        ui_widget_destroy(window->root);
    if (window->font != NULL)
        ui_font_close(window->font);
    if (window->pixels != NULL)
        munmap(window->pixels, window->pixels_length);

    if (window->to_server >= 0)
        close(window->to_server);
    if (window->to_client >= 0)
        close(window->to_client);

    char path[WSYS_PATH_MAX];
    snprintf(path, sizeof(path), WSYS_ROOT "/%d.to-server", (int)getpid());
    unlink(path);
    snprintf(path, sizeof(path), WSYS_ROOT "/%d.to-client", (int)getpid());
    unlink(path);

    free(window);
}

void ui_window_set_root(UiWindow* window, UiWidget* root)
{
    window->root = root;
    if (root != NULL) {
        root->window = window;
        /* Everything below inherits the window, however it was assembled. */
        UiWidget* stack[64];
        int depth = 0;
        stack[depth++] = root;
        while (depth > 0) {
            UiWidget* widget = stack[--depth];
            widget->window = window;
            for (int i = 0; i < widget->child_count && depth < 64; ++i)
                stack[depth++] = widget->children[i];
        }
    }
    window->needs_layout = 1;
    window->needs_paint = 1;
}

UiWidget* ui_window_root(UiWindow* window)
{
    return window->root;
}

void ui_window_set_title(UiWindow* window, const char* title)
{
    struct WsysMessage message;
    memset(&message, 0, sizeof(message));
    message.type = WSYS_SET_TITLE;
    message.window = window->id;
    strncpy(message.title.text, title, sizeof(message.title.text) - 1);
    (void)send_message(window, &message);
}

void ui_window_focus(UiWindow* window, UiWidget* widget)
{
    if (window->focused == widget)
        return;

    UiWidget* previous = window->focused;
    window->focused = widget;

    if (previous != NULL)
        ui_widget_invalidate(previous);
    if (widget != NULL)
        ui_widget_invalidate(widget);
}

UiWidget* ui_window_focused(UiWindow* window)
{
    return window != NULL ? window->focused : NULL;
}

void ui_window_invalidate(UiWindow* window)
{
    if (window != NULL)
        window->needs_paint = 1;
}

void ui_window_close(UiWindow* window)
{
    if (window != NULL)
        window->running = 0;
}

int ui_window_run(UiWindow* window)
{
    if (window == NULL)
        return 1;

    repaint(window);

    while (window->running) {
        struct pollfd waiting = { .fd = window->to_client, .events = POLLIN, .revents = 0 };

        if (poll(&waiting, 1, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        /*
         * Drain everything that has arrived before painting once. A mouse
         * dragged quickly delivers a burst of movement, and repainting per
         * event would spend the whole frame on positions nobody ever saw.
         */
        for (;;) {
            struct WsysMessage message;
            ssize_t const got = read(window->to_client, &message, sizeof(message));
            if (got != (ssize_t)sizeof(message))
                break;

            switch (message.type) {
            case WSYS_MOUSE:
                deliver_mouse(window, message.mouse.x, message.mouse.y, message.mouse.buttons,
                    message.mouse.wheel);
                break;
            case WSYS_KEY: deliver_key(window, &message); break;
            case WSYS_CLOSE_REQUEST: window->running = 0; break;
            default: break;
            }

            struct pollfd more = { .fd = window->to_client, .events = POLLIN, .revents = 0 };
            if (poll(&more, 1, 0) <= 0)
                break;
        }

        if (window->needs_paint || window->needs_layout)
            repaint(window);
    }

    struct WsysMessage message;
    memset(&message, 0, sizeof(message));
    message.type = WSYS_DESTROY_WINDOW;
    message.window = window->id;
    (void)send_message(window, &message);

    return 0;
}
