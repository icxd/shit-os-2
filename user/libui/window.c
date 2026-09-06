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

    UiFonts fonts;

    int needs_layout;
    int needs_paint;
    int running;

    /*
     * What has to be painted again, in window coordinates. One rectangle
     * rather than a list: two changes in opposite corners then repaint the
     * space between them, which is worse than tracking both and far simpler
     * than tracking both. A terminal printing a line damages one row of cells,
     * which is the case that matters.
     */
    int damage_x0, damage_y0, damage_x1, damage_y1;

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

    /*
     * Both O_RDWR and both before announcing, so no open can block and no
     * ordering between client and server can deadlock.
     *
     * Close-on-exec, and it is not a tidiness measure. The server learns that
     * a client is gone by reading end of file on this channel, and end of file
     * only arrives when the *last* writer closes. A client that forks and
     * execs -- which the terminal does, that being its entire job -- would
     * otherwise hand a copy of the write end to the child, and the window
     * would outlive the process that owned it for as long as the child lived.
     */
    window->to_server = open(to_server, O_RDWR | O_CLOEXEC);
    window->to_client = open(to_client, O_RDWR | O_CLOEXEC);
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

        /* A layout pass can move anything anywhere, so nothing on the surface
         * can be trusted to still be right. */
        ui_window_invalidate(window);
    }

    if (window->damage_x0 >= window->damage_x1) {
        window->needs_paint = 0;
        return;
    }

    UiRect const region = { window->damage_x0, window->damage_y0,
        window->damage_x1 - window->damage_x0, window->damage_y1 - window->damage_y0 };

    /*
     * Clipped to the damage, so the whole tree can be walked while only the
     * part that changed is touched. A widget outside it gets an empty clip
     * from ui_painter_for and returns before drawing anything.
     */
    UiPainter painter = {
        .pixels = window->pixels,
        .width = window->width,
        .height = window->height,
        .origin_x = 0,
        .origin_y = 0,
        .clip = region,
        .font = window->fonts.body,
        .fonts = &window->fonts,
    };

    ui_fill_rect(&painter, region, ui_theme()->background);

    paint_widget(window->root, &painter);

    struct WsysMessage message;
    memset(&message, 0, sizeof(message));
    message.type = WSYS_DAMAGE;
    message.window = window->id;
    message.damage.x = region.x;
    message.damage.y = region.y;
    message.damage.width = region.width;
    message.damage.height = region.height;
    (void)send_message(window, &message);

    window->damage_x0 = window->damage_x1 = 0;
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

    if (ui_fonts_open(&window->fonts, "/usr/share/fonts") != 0)
        fprintf(stderr, "libui: no fonts in /usr/share/fonts; text will not draw\n");

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
    ui_fonts_close(&window->fonts);
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

const UiFonts* ui_window_fonts(UiWindow* window)
{
    return window != NULL ? &window->fonts : NULL;
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

void ui_window_damage(UiWindow* window, int x, int y, int width, int height)
{
    if (window == NULL)
        return;

    int x0 = x;
    int y0 = y;
    int x1 = x + width;
    int y1 = y + height;

    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > window->width)
        x1 = window->width;
    if (y1 > window->height)
        y1 = window->height;
    if (x0 >= x1 || y0 >= y1)
        return;

    if (window->damage_x0 >= window->damage_x1) {
        window->damage_x0 = x0;
        window->damage_y0 = y0;
        window->damage_x1 = x1;
        window->damage_y1 = y1;
    } else {
        if (x0 < window->damage_x0)
            window->damage_x0 = x0;
        if (y0 < window->damage_y0)
            window->damage_y0 = y0;
        if (x1 > window->damage_x1)
            window->damage_x1 = x1;
        if (y1 > window->damage_y1)
            window->damage_y1 = y1;
    }

    window->needs_paint = 1;
}

void ui_window_invalidate(UiWindow* window)
{
    if (window != NULL)
        ui_window_damage(window, 0, 0, window->width, window->height);
}

void ui_window_close(UiWindow* window)
{
    if (window != NULL)
        window->running = 0;
}

/*
 * Drain everything that has arrived before painting once. A mouse dragged
 * quickly delivers a burst of movement, and repainting per event would spend
 * the whole frame on positions nobody ever saw.
 */
static void drain_messages(UiWindow* window)
{
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
}

/*
 * One turn of the loop, for an application that has a loop of its own. Waits
 * up to `timeout_ms` for something to arrive -- 0 to only handle what is
 * already there -- handles it, and repaints if anything changed. Returns zero
 * once the window has been closed.
 */
int ui_window_step(UiWindow* window, int timeout_ms)
{
    if (window == NULL || !window->running)
        return 0;

    struct pollfd waiting = { .fd = window->to_client, .events = POLLIN, .revents = 0 };
    if (poll(&waiting, 1, timeout_ms) > 0 && (waiting.revents & POLLIN) != 0)
        drain_messages(window);

    if (window->needs_paint || window->needs_layout)
        repaint(window);

    return window->running;
}

int ui_window_pump(UiWindow* window, int extra, UiWindowReady on_ready, void* user)
{
    if (window == NULL)
        return 1;

    repaint(window);

    while (window->running) {
        struct pollfd waiting[2];
        int count = 0;

        waiting[count].fd = window->to_client;
        waiting[count].events = POLLIN;
        waiting[count++].revents = 0;

        int const extra_slot = extra >= 0 ? count : -1;
        if (extra >= 0) {
            waiting[count].fd = extra;
            waiting[count].events = POLLIN;
            waiting[count++].revents = 0;
        }

        if (poll(waiting, (unsigned)count, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (extra_slot >= 0 && (waiting[extra_slot].revents & (POLLIN | POLLHUP)) != 0) {
            if (on_ready != NULL && on_ready(extra, user) != 0)
                window->running = 0;
        }

        if ((waiting[0].revents & POLLIN) != 0)
            drain_messages(window);

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

int ui_window_run(UiWindow* window)
{
    return ui_window_pump(window, -1, NULL, NULL);
}
