/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- the window server.
 *
 * An ordinary process. It has no privileges the shell does not have; it is the
 * window server only because it is the one holding /dev/fb0, and if it exits
 * the kernel hands the screen back to the console. That is the whole of the
 * arrangement, and it is why this is not in the kernel.
 *
 * What it owns: the screen, the mouse, the keyboard, and the decision about
 * who is on top. What it does not own: a single pixel of any window's
 * contents. Clients draw into shared memory the server maps too, so
 * compositing is a copy from one mapping to another and never a message.
 *
 * The loop is one poll() over the connect FIFO, the two input devices and
 * every client's channel, so an idle desktop costs nothing at all.
 */

#include <shitos/abi/fb.h>
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
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS 16
#define MAX_WINDOWS 32

/*
 * Styled after macOS. The chrome is almost all hairlines and very soft
 * shadows: a titlebar that is barely a different grey from the content, a
 * frame rounded enough to be obviously deliberate, and three coloured circles
 * that everyone on earth recognises.
 */
#define COLOUR_DESKTOP_TOP 0x3f5f8f
#define COLOUR_DESKTOP_BOTTOM 0x2a3f5f

#define COLOUR_CHROME 0xe8e8ea
#define COLOUR_CHROME_UNFOCUSED 0xf2f2f4
#define COLOUR_FRAME_BORDER 0xb4b4b8
#define COLOUR_CHROME_SEPARATOR 0xd0d0d4

#define COLOUR_TITLE 0x3a3a3e
#define COLOUR_TITLE_DIM 0xa0a0a6

/* The traffic lights, and what they go to when the window is not in front. */
#define COLOUR_CLOSE 0xff5f57
#define COLOUR_MINIMISE 0xfebc2e
#define COLOUR_ZOOM 0x28c840
#define COLOUR_LIGHT_INACTIVE 0xd2d2d6

#define COLOUR_CURSOR 0xffffff
#define COLOUR_CURSOR_EDGE 0x1a1a1e

#define WINDOW_CORNER_RADIUS 10

struct Window {
    int used;
    unsigned id;
    int client; /* index into g_clients */

    int x, y; /* top-left of the *frame*, chrome included */
    int width, height; /* of the content, chrome excluded */

    char title[WSYS_TITLE_MAX];

    unsigned* pixels; /* the client's buffer, mapped here too */
    size_t pixels_length;
    char buffer_path[WSYS_PATH_MAX];
};

struct Client {
    int used;
    int pid;
    int to_server; /* we read this */
    int to_client; /* we write this */
};

static struct Client g_clients[MAX_CLIENTS];
static struct Window g_windows[MAX_WINDOWS];

/* Bottom to top. The last entry is the focused window and the one on top. */
static unsigned g_stack[MAX_WINDOWS];
static int g_stack_depth;

static unsigned g_next_window_id = 1;

/* --- the screen ----------------------------------------------------------- */

static int g_fb;
static struct fb_info g_info;
static unsigned char* g_screen;

/*
 * Everything is drawn here first and copied out once. Compositing straight
 * into the framebuffer would show every intermediate stage -- the desktop, then
 * each window in turn -- which is what tearing looks like.
 */
static unsigned* g_back;
static int g_width, g_height;

static int g_cursor_x, g_cursor_y;
static unsigned g_buttons;

/* The rectangle that changed since the last blit. Copying the whole screen
 * every frame is three megabytes; copying what moved is usually a few kilobytes. */
static int g_dirty_x0, g_dirty_y0, g_dirty_x1, g_dirty_y1;

static void damage(int x0, int y0, int x1, int y1)
{
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > g_width)
        x1 = g_width;
    if (y1 > g_height)
        y1 = g_height;
    if (x0 >= x1 || y0 >= y1)
        return;

    if (g_dirty_x0 >= g_dirty_x1) {
        g_dirty_x0 = x0;
        g_dirty_y0 = y0;
        g_dirty_x1 = x1;
        g_dirty_y1 = y1;
        return;
    }
    if (x0 < g_dirty_x0)
        g_dirty_x0 = x0;
    if (y0 < g_dirty_y0)
        g_dirty_y0 = y0;
    if (x1 > g_dirty_x1)
        g_dirty_x1 = x1;
    if (y1 > g_dirty_y1)
        g_dirty_y1 = y1;
}

static void damage_all(void)
{
    damage(0, 0, g_width, g_height);
}

/*
 * The back buffer holds plain 0xRRGGBB, and packing into whatever the
 * framebuffer wants happens once, in the blit. That is what lets the toolkit's
 * painter draw the chrome directly into it -- there is one implementation of a
 * rounded rectangle in this system, and the window frames use it rather than
 * carrying a second copy that drifts.
 */
/* The framebuffer's own packing, which is not guaranteed to be 0x00RRGGBB.
 * Called once per pixel in the blit, and only when it is not the identity. */
static unsigned pack(unsigned rgb)
{
    unsigned const r = (rgb >> 16) & 0xff;
    unsigned const g = (rgb >> 8) & 0xff;
    unsigned const b = rgb & 0xff;
    return (r << g_info.red_shift) | (g << g_info.green_shift) | (b << g_info.blue_shift);
}

/*
 * What a frame actually costs. "It feels laggy" is not something you can fix
 * without a number, and the number that matters here is not time -- which
 * varies with the host -- but how much work a frame does that it did not need
 * to. `considered` is the area the damage rectangle asked for; `drawn` is what
 * the drawing code actually touched. They should be close.
 */
static struct {
    unsigned long frames;
    unsigned long considered;
    unsigned long drawn;
    unsigned long windows;
    unsigned long micros;
} g_cost;

static UiPainter g_painter;
static UiFonts g_fonts;

/*
 * Clipped to the damage rectangle, not to the screen. The compositor used to
 * redraw the whole desktop and every window on it and then blit the twenty
 * pixels that had moved, which is what a cursor crossing an empty desktop
 * cost. Setting the clip here is most of the fix: the toolkit's drawing
 * already honours it, so every shape gets the reduction for free.
 */
static void painter_reset(void)
{
    g_painter.pixels = g_back;
    g_painter.width = g_width;
    g_painter.height = g_height;
    g_painter.origin_x = 0;
    g_painter.origin_y = 0;
    g_painter.clip.x = g_dirty_x0;
    g_painter.clip.y = g_dirty_y0;
    g_painter.clip.width = g_dirty_x1 - g_dirty_x0;
    g_painter.clip.height = g_dirty_y1 - g_dirty_y0;
    g_painter.font = g_fonts.body;
    g_painter.fonts = &g_fonts;
}

/* Does this rectangle have any pixel inside the damaged region? */
static int intersects_damage(int x0, int y0, int x1, int y1)
{
    return x0 < g_dirty_x1 && x1 > g_dirty_x0 && y0 < g_dirty_y1 && y1 > g_dirty_y0;
}

/* --- windows -------------------------------------------------------------- */

static int frame_height(const struct Window* window)
{
    return window->height + WSYS_TITLEBAR_HEIGHT + WSYS_BORDER_WIDTH;
}

static int frame_width(const struct Window* window)
{
    return window->width + 2 * WSYS_BORDER_WIDTH;
}

static int content_origin_x(const struct Window* window)
{
    return window->x + WSYS_BORDER_WIDTH;
}

static int content_origin_y(const struct Window* window)
{
    return window->y + WSYS_TITLEBAR_HEIGHT;
}

/*
 * The frame *and* its shadow. The shadow is drawn well outside the window, so
 * damaging only the frame leaves a smear of stale shadow behind every time one
 * is dragged -- the back buffer is right and the screen never hears about it.
 */
#define WINDOW_SHADOW_MARGIN 24

static void damage_window(const struct Window* window)
{
    damage(window->x - WINDOW_SHADOW_MARGIN, window->y - WINDOW_SHADOW_MARGIN,
        window->x + frame_width(window) + WINDOW_SHADOW_MARGIN,
        window->y + frame_height(window) + WINDOW_SHADOW_MARGIN);
}

static struct Window* window_by_id(unsigned id)
{
    for (int i = 0; i < MAX_WINDOWS; ++i) {
        if (g_windows[i].used && g_windows[i].id == id)
            return &g_windows[i];
    }
    return NULL;
}

static unsigned focused_window_id(void)
{
    return g_stack_depth > 0 ? g_stack[g_stack_depth - 1] : 0;
}

static void raise_window(unsigned id)
{
    int at = -1;
    for (int i = 0; i < g_stack_depth; ++i) {
        if (g_stack[i] == id)
            at = i;
    }
    if (at < 0 || at == g_stack_depth - 1)
        return;

    for (int i = at; i + 1 < g_stack_depth; ++i)
        g_stack[i] = g_stack[i + 1];
    g_stack[g_stack_depth - 1] = id;
}

static void unstack_window(unsigned id)
{
    int out = 0;
    for (int i = 0; i < g_stack_depth; ++i) {
        if (g_stack[i] != id)
            g_stack[out++] = g_stack[i];
    }
    g_stack_depth = out;
}

/* The red one, on the left, where macOS puts it. */
static void close_button_rect(const struct Window* window, int* x, int* y, int* size)
{
    *size = 12;
    *x = window->x + 12;
    *y = window->y + (WSYS_TITLEBAR_HEIGHT - *size) / 2;
}

static void draw_window(struct Window* window)
{
    int const focused = window->id == focused_window_id();
    UiRect const frame = { window->x, window->y, frame_width(window), frame_height(window) };

    /* Nothing of this window, shadow included, is inside the damage: drawing
     * it would be pure waste, and with a stack of windows it is most of the
     * frame. */
    if (!intersects_damage(window->x - WINDOW_SHADOW_MARGIN, window->y - WINDOW_SHADOW_MARGIN,
            window->x + frame.width + WINDOW_SHADOW_MARGIN,
            window->y + frame.height + WINDOW_SHADOW_MARGIN))
        return;

    ++g_cost.windows;

    /*
     * The shadow is what makes a window look like it is above the desktop
     * rather than painted on it, and on macOS it is enormous and very faint.
     * The focused window gets more of it, which is a large part of how you can
     * tell at a glance which one is in front.
     */
    ui_drop_shadow(&g_painter, frame, WINDOW_CORNER_RADIUS, focused ? 14 : 7);

    ui_fill_rounded(
        &g_painter, frame, WINDOW_CORNER_RADIUS, focused ? COLOUR_CHROME : COLOUR_CHROME_UNFOCUSED);

    /* --- the traffic lights --- */
    int const light_size = 12;
    int const light_y = window->y + (WSYS_TITLEBAR_HEIGHT - light_size) / 2;
    unsigned const lights[3] = { COLOUR_CLOSE, COLOUR_MINIMISE, COLOUR_ZOOM };

    for (int i = 0; i < 3; ++i) {
        UiRect const light
            = { window->x + 12 + i * (light_size + 8), light_y, light_size, light_size };
        ui_fill_rounded(
            &g_painter, light, light_size / 2, focused ? lights[i] : COLOUR_LIGHT_INACTIVE);
    }

    /*
     * The title is centred on the window, not on the space left over beside
     * the lights. That is what macOS does, and it is why a title looks
     * off-centre there when the window is narrow -- it is centred on the frame
     * and the lights simply overlap where it would have been.
     */
    if (g_fonts.strong != NULL) {
        UiPainter titlebar = g_painter;
        titlebar.font = g_fonts.strong;

        int const lights_end = 12 + 3 * (light_size + 8);
        UiRect const bounds = { window->x + lights_end, window->y, frame.width - lights_end * 2,
            WSYS_TITLEBAR_HEIGHT };

        ui_draw_text(&titlebar, bounds, window->title, focused ? COLOUR_TITLE : COLOUR_TITLE_DIM,
            UI_ALIGN_CENTRE);
    }

    /* A hairline under the titlebar, and one around the whole frame. */
    UiRect const separator = { window->x, window->y + WSYS_TITLEBAR_HEIGHT - 1, frame.width, 1 };
    ui_fill_rect(&g_painter, separator, COLOUR_CHROME_SEPARATOR);
    ui_stroke_rounded(&g_painter, frame, WINDOW_CORNER_RADIUS, COLOUR_FRAME_BORDER);

    /*
     * Content, straight out of the client's buffer. This copy is the only
     * thing that ever reads it, and the client may be writing as we read -- the
     * worst case is one frame that mixes two, which is better than the
     * lock-step a lock would impose.
     */
    if (window->pixels == NULL)
        return;

    int const ox = content_origin_x(window);
    int const oy = content_origin_y(window);

    /*
     * Only the rows and columns inside the damage. Copying a whole 720x440
     * window because its cursor blinked is a third of a megapixel to show
     * eight by sixteen of them.
     */
    int const from_y = oy > g_dirty_y0 ? oy : g_dirty_y0;
    int const to_y = oy + window->height < g_dirty_y1 ? oy + window->height : g_dirty_y1;
    int const from_x = ox > g_dirty_x0 ? ox : g_dirty_x0;
    int const to_x = ox + window->width < g_dirty_x1 ? ox + window->width : g_dirty_x1;

    if (from_x >= to_x)
        return;

    for (int py = from_y; py < to_y; ++py) {
        if (py < 0 || py >= g_height)
            continue;

        const unsigned* source = window->pixels + (size_t)(py - oy) * window->width;
        unsigned* destination = g_back + (size_t)py * g_width;

        for (int px = from_x; px < to_x; ++px) {
            if (px < 0 || px >= g_width)
                continue;
            destination[px] = source[px - ox];
        }
    }

    g_cost.drawn += (unsigned long)(to_x - from_x) * (unsigned long)(to_y - from_y);
}

static void draw_cursor(void)
{
    /* An arrow with a dark outline, so it stays visible over anything. */
    static const char* SHAPE[] = {
        "X         ",
        "XX        ",
        "X.X       ",
        "X..X      ",
        "X...X     ",
        "X....X    ",
        "X.....X   ",
        "X......X  ",
        "X.......X ",
        "X....XXXXX",
        "X..X.X    ",
        "X.X  X.X  ",
        "XX    X.X ",
        "X      X.X",
        "        XX",
    };

    for (int row = 0; row < (int)(sizeof(SHAPE) / sizeof(SHAPE[0])); ++row) {
        for (int column = 0; SHAPE[row][column] != '\0'; ++column) {
            char const pixel = SHAPE[row][column];
            if (pixel == ' ')
                continue;
            int const px = g_cursor_x + column;
            int const py = g_cursor_y + row;
            if (px < 0 || py < 0 || px >= g_width || py >= g_height)
                continue;
            g_back[(size_t)py * g_width + px] = pixel == 'X' ? COLOUR_CURSOR_EDGE : COLOUR_CURSOR;
        }
    }
}

static void draw_desktop(void)
{
    /*
     * A plain vertical gradient. Not a photograph, but the point of a desktop
     * background is to be something windows are obviously on top of.
     *
     * Signed, deliberately. The first version interpolated in unsigned and the
     * top colour happened to be lighter than the bottom, so every channel
     * difference wrapped to about four billion and the sky came out in bands of
     * green.
     */
    int const top = COLOUR_DESKTOP_TOP;
    int const bottom = COLOUR_DESKTOP_BOTTOM;

    /* The gradient is a function of y alone, so a damaged strip needs only its
     * own rows -- and only the columns that changed within them. */
    for (int y = g_dirty_y0; y < g_dirty_y1; ++y) {
        unsigned colour = 0;
        for (int shift = 0; shift <= 16; shift += 8) {
            int const from = (top >> shift) & 0xff;
            int const to = (bottom >> shift) & 0xff;
            int const value = from + (to - from) * y / g_height;
            colour |= (unsigned)value << shift;
        }

        unsigned* line = g_back + (size_t)y * g_width;
        for (int x = g_dirty_x0; x < g_dirty_x1; ++x)
            line[x] = colour;
    }

    g_cost.drawn
        += (unsigned long)(g_dirty_x1 - g_dirty_x0) * (unsigned long)(g_dirty_y1 - g_dirty_y0);
}

/*
 * Focus is not a property of one window: gaining it changes how this titlebar
 * looks and losing it changes how another one does. Every path that can move
 * focus -- a click, a new window, a window closing -- would otherwise have to
 * remember to damage both, and the first one to forget produced a desktop with
 * two focused-looking windows and no way to tell which would get the keys.
 *
 * So it is checked here instead, once, where it cannot be forgotten.
 */
static unsigned g_last_focus;

static void reconcile_focus(void)
{
    unsigned const now = focused_window_id();
    if (now == g_last_focus)
        return;

    struct Window* window = window_by_id(g_last_focus);
    if (window != NULL)
        damage_window(window);

    window = window_by_id(now);
    if (window != NULL)
        damage_window(window);

    g_last_focus = now;
}

static unsigned long now_micros(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0;
    return (unsigned long)now.tv_sec * 1000000UL + (unsigned long)(now.tv_nsec / 1000);
}

static void composite(void)
{
    reconcile_focus();

    if (g_dirty_x0 >= g_dirty_x1)
        return;

    unsigned long const started = now_micros();
    ++g_cost.frames;
    g_cost.considered
        += (unsigned long)(g_dirty_x1 - g_dirty_x0) * (unsigned long)(g_dirty_y1 - g_dirty_y0);

    painter_reset();

    draw_desktop();
    for (int i = 0; i < g_stack_depth; ++i) {
        struct Window* window = window_by_id(g_stack[i]);
        if (window != NULL)
            draw_window(window);
    }
    draw_cursor();

    /*
     * Out to the real screen, only the part that changed, packing into whatever
     * the framebuffer wants on the way. QEMU's is 0x00RRGGBB, which is what the
     * back buffer already holds, so the common case stays a memcpy -- but the
     * shifts come from the device and a machine that lays its pixels out
     * differently still gets the right colours rather than blue faces.
     */
    int const direct = g_info.bytes_per_pixel == 4 && g_info.red_shift == 16
        && g_info.green_shift == 8 && g_info.blue_shift == 0;

    for (int y = g_dirty_y0; y < g_dirty_y1; ++y) {
        const unsigned* source = g_back + (size_t)y * g_width + g_dirty_x0;
        unsigned char* destination
            = g_screen + (size_t)y * g_info.pitch + (size_t)g_dirty_x0 * g_info.bytes_per_pixel;

        if (direct) {
            memcpy(destination, source, (size_t)(g_dirty_x1 - g_dirty_x0) * 4);
            continue;
        }

        for (int x = 0; x < g_dirty_x1 - g_dirty_x0; ++x) {
            unsigned const value = pack(source[x]);
            for (unsigned i = 0; i < g_info.bytes_per_pixel; ++i)
                destination[(size_t)x * g_info.bytes_per_pixel + i]
                    = (unsigned char)(value >> (i * 8));
        }
    }

    g_dirty_x0 = g_dirty_x1 = 0;
    g_cost.micros += now_micros() - started;
}

/* --- clients -------------------------------------------------------------- */

/*
 * Returns -1 when the client is gone. SIGPIPE is ignored, so a write to a
 * channel whose reader has died comes back as EPIPE rather than killing the
 * server -- which is the other half of noticing a dead client, for the case
 * where we speak before it does.
 */
static int send_to_client(struct Client* client, const struct WsysMessage* message)
{
    ssize_t written = write(client->to_client, message, sizeof(*message));
    return written == (ssize_t)sizeof(*message) ? 0 : -1;
}

static void destroy_window(struct Window* window)
{
    if (!window->used)
        return;

    damage_window(window);

    if (window->pixels != NULL)
        munmap(window->pixels, window->pixels_length);
    if (window->buffer_path[0] != '\0')
        unlink(window->buffer_path);

    unstack_window(window->id);
    memset(window, 0, sizeof(*window));
}

static void drop_client(int index)
{
    struct Client* client = &g_clients[index];
    if (!client->used)
        return;

    for (int i = 0; i < MAX_WINDOWS; ++i) {
        if (g_windows[i].used && g_windows[i].client == index)
            destroy_window(&g_windows[i]);
    }

    close(client->to_server);
    close(client->to_client);

    char path[WSYS_PATH_MAX];
    snprintf(path, sizeof(path), WSYS_ROOT "/%d.to-server", client->pid);
    unlink(path);
    snprintf(path, sizeof(path), WSYS_ROOT "/%d.to-client", client->pid);
    unlink(path);

    memset(client, 0, sizeof(*client));
}

static void handle_create_window(int index, const struct WsysMessage* request)
{
    struct Client* client = &g_clients[index];

    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS; ++i) {
        if (!g_windows[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0 || g_stack_depth >= MAX_WINDOWS)
        return;

    struct Window* window = &g_windows[slot];
    memset(window, 0, sizeof(*window));

    window->width = request->create.width > 0 ? request->create.width : 320;
    window->height = request->create.height > 0 ? request->create.height : 240;
    if (window->width > g_width)
        window->width = g_width;
    if (window->height > g_height - WSYS_TITLEBAR_HEIGHT)
        window->height = g_height - WSYS_TITLEBAR_HEIGHT;

    window->id = g_next_window_id++;
    window->client = index;

    if (request->create.x >= 0 && request->create.y >= 0) {
        window->x = request->create.x;
        window->y = request->create.y;
    } else {
        /* Cascade, so two windows opened at once are both visible. */
        window->x = 60 + (int)(window->id % 8) * 28;
        window->y = 50 + (int)(window->id % 8) * 24;
    }

    strncpy(window->title, "untitled", sizeof(window->title) - 1);

    /*
     * The buffer. A tmpfs file, sized and then mapped by both sides -- that is
     * all POSIX shared memory has ever been, and it means the client's drawing
     * needs no syscall at all.
     */
    snprintf(window->buffer_path, sizeof(window->buffer_path), WSYS_ROOT "/win%u.px", window->id);
    int fd = open(window->buffer_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        memset(window, 0, sizeof(*window));
        return;
    }

    window->pixels_length = (size_t)window->width * window->height * 4;
    if (ftruncate(fd, (off_t)window->pixels_length) < 0) {
        close(fd);
        unlink(window->buffer_path);
        memset(window, 0, sizeof(*window));
        return;
    }

    window->pixels = mmap(NULL, window->pixels_length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (window->pixels == MAP_FAILED) {
        window->pixels = NULL;
        unlink(window->buffer_path);
        memset(window, 0, sizeof(*window));
        return;
    }

    window->used = 1;
    g_stack[g_stack_depth++] = window->id;

    struct WsysMessage reply;
    memset(&reply, 0, sizeof(reply));
    reply.type = WSYS_WINDOW_CREATED;
    reply.window = window->id;
    reply.created.width = window->width;
    reply.created.height = window->height;
    strncpy(reply.created.buffer_path, window->buffer_path, sizeof(reply.created.buffer_path) - 1);

    if (send_to_client(client, &reply) < 0) {
        destroy_window(window);
        return;
    }

    damage_window(window);
}

static void handle_client_message(int index, const struct WsysMessage* message)
{
    struct Window* window = window_by_id(message->window);

    switch (message->type) {
    case WSYS_CREATE_WINDOW: handle_create_window(index, message); break;

    case WSYS_DESTROY_WINDOW:
        if (window != NULL && window->client == index)
            destroy_window(window);
        break;

    case WSYS_SET_TITLE:
        if (window != NULL && window->client == index) {
            memcpy(window->title, message->title.text, sizeof(window->title));
            window->title[sizeof(window->title) - 1] = '\0';
            damage(window->x, window->y, window->x + frame_width(window),
                window->y + WSYS_TITLEBAR_HEIGHT);
        }
        break;

    case WSYS_DAMAGE:
        if (window != NULL && window->client == index) {
            int const ox = content_origin_x(window);
            int const oy = content_origin_y(window);
            damage(ox + message->damage.x, oy + message->damage.y,
                ox + message->damage.x + message->damage.width,
                oy + message->damage.y + message->damage.height);
        }
        break;

    default:
        /* A client speaking a dialect we do not know is ignored rather than
         * disconnected: it may simply be newer. */
        break;
    }
}

static void accept_connection(int connect_fd)
{
    struct WsysHello hello;
    ssize_t got = read(connect_fd, &hello, sizeof(hello));
    if (got != (ssize_t)sizeof(hello))
        return;

    int slot = -1;
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (!g_clients[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return;

    char to_server[WSYS_PATH_MAX];
    char to_client[WSYS_PATH_MAX];
    snprintf(to_server, sizeof(to_server), WSYS_ROOT "/%d.to-server", hello.pid);
    snprintf(to_client, sizeof(to_client), WSYS_ROOT "/%d.to-client", hello.pid);

    /*
     * Not O_RDWR, and that matters. O_RDWR is the trick that stops a FIFO
     * reporting end of file, and end of file is exactly what is wanted here:
     * it is the only way to learn that a client has died rather than gone
     * quiet. Holding a write end of the channel we read would make us our own
     * writer, so the read would block forever on a process that no longer
     * exists, and its windows would stay on screen for good.
     *
     * The client opens both ends O_RDWR before it announces itself, so there
     * is a peer on both by the time we get here and neither of these can
     * block -- O_NONBLOCK is belt and braces, and costs nothing because every
     * read is already gated on poll.
     */
    int in = open(to_server, O_RDONLY | O_NONBLOCK);
    int out = open(to_client, O_WRONLY | O_NONBLOCK);
    if (in < 0 || out < 0) {
        if (in >= 0)
            close(in);
        if (out >= 0)
            close(out);
        return;
    }

    g_clients[slot].used = 1;
    g_clients[slot].pid = hello.pid;
    g_clients[slot].to_server = in;
    g_clients[slot].to_client = out;
}

/* --- input ---------------------------------------------------------------- */

static struct Window* window_at(int x, int y)
{
    for (int i = g_stack_depth - 1; i >= 0; --i) {
        struct Window* window = window_by_id(g_stack[i]);
        if (window == NULL)
            continue;
        if (x >= window->x && x < window->x + frame_width(window) && y >= window->y
            && y < window->y + frame_height(window))
            return window;
    }
    return NULL;
}

static int g_dragging; /* window id, or 0 */
static int g_drag_offset_x, g_drag_offset_y;

static void send_close_request(struct Window* window)
{
    struct WsysMessage message;
    memset(&message, 0, sizeof(message));
    message.type = WSYS_CLOSE_REQUEST;
    message.window = window->id;
    (void)send_to_client(&g_clients[window->client], &message);
}

static void deliver_mouse(struct Window* window, int x, int y, unsigned buttons, int wheel)
{
    struct WsysMessage message;
    memset(&message, 0, sizeof(message));
    message.type = WSYS_MOUSE;
    message.window = window->id;
    message.mouse.x = x - content_origin_x(window);
    message.mouse.y = y - content_origin_y(window);
    message.mouse.buttons = (unsigned char)buttons;
    message.mouse.wheel = (signed char)wheel;
    (void)send_to_client(&g_clients[window->client], &message);
}

static void handle_mouse(const struct mouse_event* event)
{
    int const old_x = g_cursor_x;
    int const old_y = g_cursor_y;

    g_cursor_x += event->dx;
    /* The mouse reports Y upward; the screen counts it downward. This is the
     * one conversion the kernel deliberately refuses to make for anyone. */
    g_cursor_y -= event->dy;

    if (g_cursor_x < 0)
        g_cursor_x = 0;
    if (g_cursor_y < 0)
        g_cursor_y = 0;
    if (g_cursor_x >= g_width)
        g_cursor_x = g_width - 1;
    if (g_cursor_y >= g_height)
        g_cursor_y = g_height - 1;

    if (g_cursor_x != old_x || g_cursor_y != old_y) {
        damage(old_x - 1, old_y - 1, old_x + 18, old_y + 18);
        damage(g_cursor_x - 1, g_cursor_y - 1, g_cursor_x + 18, g_cursor_y + 18);
    }

    unsigned const was = g_buttons;
    g_buttons = event->buttons;
    int const pressed = (g_buttons & ~was) & MOUSE_BUTTON_LEFT;
    int const released = (was & ~g_buttons) & MOUSE_BUTTON_LEFT;

    if (g_dragging != 0) {
        struct Window* window = window_by_id((unsigned)g_dragging);
        if (window == NULL || released) {
            g_dragging = 0;
        } else {
            damage_window(window);
            window->x = g_cursor_x - g_drag_offset_x;
            window->y = g_cursor_y - g_drag_offset_y;
            damage_window(window);
        }
        return;
    }

    struct Window* window = window_at(g_cursor_x, g_cursor_y);

    if (pressed && window != NULL) {
        if (window->id != focused_window_id()) {
            raise_window(window->id);
            damage_all();
        }

        int close_x, close_y, close_size;
        close_button_rect(window, &close_x, &close_y, &close_size);

        if (g_cursor_x >= close_x && g_cursor_x < close_x + close_size && g_cursor_y >= close_y
            && g_cursor_y < close_y + close_size) {
            send_close_request(window);
            return;
        }

        if (g_cursor_y < window->y + WSYS_TITLEBAR_HEIGHT) {
            g_dragging = (int)window->id;
            g_drag_offset_x = g_cursor_x - window->x;
            g_drag_offset_y = g_cursor_y - window->y;
            return;
        }
    }

    /* Anything not claimed by the chrome belongs to the window under the
     * pointer, in its own coordinates. */
    if (window != NULL && g_cursor_y >= content_origin_y(window))
        deliver_mouse(window, g_cursor_x, g_cursor_y, g_buttons, event->dz);
}

static void handle_key(const struct key_event* event)
{
    struct Window* window = window_by_id(focused_window_id());
    if (window == NULL)
        return;

    struct WsysMessage message;
    memset(&message, 0, sizeof(message));
    message.type = WSYS_KEY;
    message.window = window->id;
    message.key.keycode = event->keycode;
    message.key.pressed = event->pressed;
    message.key.modifiers = event->modifiers;
    message.key.codepoint = event->codepoint;
    (void)send_to_client(&g_clients[window->client], &message);
}

/* --- setup and the loop --------------------------------------------------- */

static volatile int g_running = 1;

static void on_terminate(int signal)
{
    (void)signal;
    g_running = 0;
}

static int make_root(void)
{
    /* mkdir over an existing directory is fine; anything else is not. */
    if (mkdir(WSYS_ROOT, 0777) < 0 && errno != EEXIST)
        return -1;

    unlink(WSYS_CONNECT_FIFO);
    if (mkfifo(WSYS_CONNECT_FIFO, 0666) < 0)
        return -1;
    return 0;
}

static int open_screen(void)
{
    g_fb = open("/dev/fb0", O_RDWR);
    if (g_fb < 0)
        return -1;
    if (ioctl(g_fb, FBIOGET_INFO, &g_info) < 0)
        return -1;

    g_screen = mmap(NULL, g_info.length, PROT_READ | PROT_WRITE, MAP_SHARED, g_fb, 0);
    if (g_screen == MAP_FAILED)
        return -1;

    g_width = (int)g_info.width;
    g_height = (int)g_info.height;

    g_back = malloc((size_t)g_width * g_height * sizeof(unsigned));
    if (g_back == NULL)
        return -1;

    int take = 1;
    ioctl(g_fb, FBIO_ACQUIRE, &take);
    return 0;
}

static void open_fonts(void)
{
    if (ui_fonts_open(&g_fonts, "/usr/share/fonts") != 0)
        fprintf(stderr, "wsys: no fonts in /usr/share/fonts; titles will be blank\n");
}

int main(int argc, char** argv)
{
    int run_once = argc > 1 && strcmp(argv[1], "--check") == 0;

    signal(SIGTERM, on_terminate);
    signal(SIGINT, on_terminate);
    /* A client that dies mid-message must not take the server with it. */
    signal(SIGPIPE, SIG_IGN);

    if (make_root() < 0) {
        fprintf(stderr, "wsys: cannot set up " WSYS_ROOT ": %s\n", strerror(errno));
        return 1;
    }

    if (open_screen() < 0) {
        fprintf(stderr, "wsys: cannot open the screen: %s\n", strerror(errno));
        return 1;
    }

    int connect_fd = open(WSYS_CONNECT_FIFO, O_RDWR);
    if (connect_fd < 0) {
        fprintf(stderr, "wsys: cannot open the connect fifo: %s\n", strerror(errno));
        return 1;
    }

    int mouse = open("/dev/mouse0", O_RDONLY);
    int keys = open("/dev/kbdraw", O_RDONLY);

    open_fonts();

    g_cursor_x = g_width / 2;
    g_cursor_y = g_height / 2;

    damage_all();
    composite();

    if (run_once) {
        /* Enough to prove the server comes up, owns the screen and draws. The
         * interactive part needs a human, so it is not attempted here. */
        printf("wsys: %dx%d, ready\n", g_width, g_height);
        int release = 0;
        ioctl(g_fb, FBIO_ACQUIRE, &release);
        return 0;
    }

    while (g_running) {
        struct pollfd waiting[3 + MAX_CLIENTS];
        int count = 0;

        waiting[count].fd = connect_fd;
        waiting[count].events = POLLIN;
        waiting[count++].revents = 0;

        int const mouse_slot = mouse >= 0 ? count : -1;
        if (mouse >= 0) {
            waiting[count].fd = mouse;
            waiting[count].events = POLLIN;
            waiting[count++].revents = 0;
        }

        int const keys_slot = keys >= 0 ? count : -1;
        if (keys >= 0) {
            waiting[count].fd = keys;
            waiting[count].events = POLLIN;
            waiting[count++].revents = 0;
        }

        int client_slot[MAX_CLIENTS];
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            client_slot[i] = -1;
            if (!g_clients[i].used)
                continue;
            client_slot[i] = count;
            waiting[count].fd = g_clients[i].to_server;
            waiting[count].events = POLLIN;
            waiting[count++].revents = 0;
        }

        if (poll(waiting, (unsigned)count, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (waiting[0].revents & POLLIN)
            accept_connection(connect_fd);

        if (mouse_slot >= 0 && (waiting[mouse_slot].revents & POLLIN)) {
            struct mouse_event events[32];
            ssize_t got = read(mouse, events, sizeof(events));
            for (ssize_t i = 0; got > 0 && i < got / (ssize_t)sizeof(events[0]); ++i)
                handle_mouse(&events[i]);
        }

        if (keys_slot >= 0 && (waiting[keys_slot].revents & POLLIN)) {
            struct key_event events[32];
            ssize_t got = read(keys, events, sizeof(events));
            for (ssize_t i = 0; got > 0 && i < got / (ssize_t)sizeof(events[0]); ++i)
                handle_key(&events[i]);
        }

        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (client_slot[i] < 0 || !(waiting[client_slot[i]].revents & POLLIN))
                continue;

            struct WsysMessage message;
            ssize_t got = read(g_clients[i].to_server, &message, sizeof(message));
            if (got == (ssize_t)sizeof(message))
                handle_client_message(i, &message);
            else if (got == 0)
                drop_client(i);
        }

        composite();
    }

    int release = 0;
    ioctl(g_fb, FBIO_ACQUIRE, &release);

    /*
     * To serial, where it does not disturb the screen. `drawn/considered` is
     * the number that matters: 1.0 means the compositor did exactly the work
     * the damage asked for, and anything much above it is work thrown away.
     */
    if (g_cost.frames > 0) {
        fprintf(stderr,
            "wsys: %lu frames, %lu us (%lu us/frame), %lu windows, "
            "%lu kpx considered, %lu kpx drawn, waste %lux\n",
            g_cost.frames, g_cost.micros, g_cost.micros / g_cost.frames, g_cost.windows,
            g_cost.considered / 1000, g_cost.drawn / 1000,
            g_cost.considered > 0 ? g_cost.drawn / g_cost.considered : 0);
    }
    return 0;
}
