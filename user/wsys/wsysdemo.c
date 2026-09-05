/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- a window server client, written the hard way on purpose.
 *
 * There is no toolkit yet, so this talks the protocol directly: make two
 * FIFOs, say hello, ask for a window, map the buffer it is given, draw into
 * it, and say what changed. Every client will eventually do this through a
 * library, and the library will do exactly this.
 *
 * It draws a checkerboard that follows the mouse and changes colour when a key
 * is pressed, because that is the smallest thing that proves input arrives in
 * the right coordinates and in the right window.
 */

#include <shitos/wsys/protocol.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_to_server = -1;
static int g_to_client = -1;

static unsigned* g_pixels;
static size_t g_pixels_length;
static int g_width, g_height;
static unsigned g_window;

static int g_pointer_x = -1;
static int g_pointer_y = -1;
static unsigned g_tint = 0x5a8fd6;

static int connect_to_server(void)
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
     * Both O_RDWR, and both before announcing. That is what makes the
     * handshake work in any order: neither open can block for a peer, and
     * neither channel reports end of file while the other side is between
     * opens.
     */
    g_to_server = open(to_server, O_RDWR);
    g_to_client = open(to_client, O_RDWR);
    if (g_to_server < 0 || g_to_client < 0)
        return -1;

    int connect_fd = open(WSYS_CONNECT_FIFO, O_WRONLY);
    if (connect_fd < 0)
        return -1;

    struct WsysHello hello = { .pid = pid };
    ssize_t written = write(connect_fd, &hello, sizeof(hello));
    close(connect_fd);

    return written == (ssize_t)sizeof(hello) ? 0 : -1;
}

static int send_message(const struct WsysMessage* message)
{
    return write(g_to_server, message, sizeof(*message)) == (ssize_t)sizeof(*message) ? 0 : -1;
}

static void redraw(void)
{
    if (g_pixels == NULL)
        return;

    for (int y = 0; y < g_height; ++y) {
        for (int x = 0; x < g_width; ++x) {
            int const cell = ((x / 24) + (y / 24)) & 1;
            unsigned colour = cell ? 0x1b1d26 : 0x232634;

            /* A crosshair wherever the pointer is, which is the whole proof
             * that coordinates arrive in window space rather than screen
             * space. */
            if (g_pointer_x >= 0 && (x == g_pointer_x || y == g_pointer_y))
                colour = g_tint;

            g_pixels[(size_t)y * g_width + x] = colour;
        }
    }

    struct WsysMessage message;
    memset(&message, 0, sizeof(message));
    message.type = WSYS_DAMAGE;
    message.window = g_window;
    message.damage.x = 0;
    message.damage.y = 0;
    message.damage.width = g_width;
    message.damage.height = g_height;
    (void)send_message(&message);
}

int main(int argc, char** argv)
{
    const char* title = argc > 1 ? argv[1] : "hello";
    int const width = argc > 3 ? atoi(argv[2]) : 360;
    int const height = argc > 3 ? atoi(argv[3]) : 260;

    if (connect_to_server() < 0) {
        fprintf(stderr, "wsysdemo: cannot reach the window server: %s\n", strerror(errno));
        return 1;
    }

    struct WsysMessage message;
    memset(&message, 0, sizeof(message));
    message.type = WSYS_CREATE_WINDOW;
    message.create.width = width;
    message.create.height = height;
    message.create.x = -1;
    message.create.y = -1;
    if (send_message(&message) < 0) {
        fprintf(stderr, "wsysdemo: the server hung up\n");
        return 1;
    }

    for (;;) {
        struct WsysMessage event;
        ssize_t got = read(g_to_client, &event, sizeof(event));
        if (got != (ssize_t)sizeof(event))
            break;

        switch (event.type) {
        case WSYS_WINDOW_CREATED: {
            g_window = event.window;
            g_width = event.created.width;
            g_height = event.created.height;
            g_pixels_length = (size_t)g_width * g_height * 4;

            int fd = open(event.created.buffer_path, O_RDWR);
            if (fd < 0) {
                fprintf(stderr, "wsysdemo: cannot open the window buffer\n");
                return 1;
            }
            g_pixels = mmap(NULL, g_pixels_length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            close(fd);
            if (g_pixels == MAP_FAILED) {
                fprintf(stderr, "wsysdemo: cannot map the window buffer\n");
                return 1;
            }

            memset(&message, 0, sizeof(message));
            message.type = WSYS_SET_TITLE;
            message.window = g_window;
            strncpy(message.title.text, title, sizeof(message.title.text) - 1);
            (void)send_message(&message);

            redraw();
            break;
        }

        case WSYS_MOUSE:
            g_pointer_x = event.mouse.x;
            g_pointer_y = event.mouse.y;
            redraw();
            break;

        case WSYS_KEY:
            if (event.key.pressed) {
                if (event.key.codepoint == 'q')
                    goto done;
                /* Any other key restyles the crosshair, so that a keystroke
                 * reaching the focused window is visible. */
                g_tint = (g_tint * 1103515245u + event.key.codepoint * 12345u) | 0x404040;
                redraw();
            }
            break;

        case WSYS_CLOSE_REQUEST: goto done;

        default: break;
        }
    }

done:
    if (g_pixels != NULL)
        munmap(g_pixels, g_pixels_length);

    memset(&message, 0, sizeof(message));
    message.type = WSYS_DESTROY_WINDOW;
    message.window = g_window;
    (void)send_message(&message);

    close(g_to_server);
    close(g_to_client);
    return 0;
}
