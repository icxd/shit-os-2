/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- a terminal emulator.
 *
 * The application that makes the desktop worth using rather than worth looking
 * at, and it is almost nothing: open a pseudo-terminal, fork a shell onto the
 * far end of it, and pump bytes between the master and a terminal widget.
 *
 * Everything hard is somewhere else. The pty is the kernel's, the escape
 * parsing and the cell grid are the toolkit's, and the window is the window
 * server's. What is left here is the wiring, which is how it should be.
 */

#include "terminal.h"

#include "ui.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static int g_master = -1;
static pid_t g_shell = -1;
static UiWidget* g_terminal;

/* The widget typed something: it goes to the shell. */
static void on_input(const char* bytes, int length, void* user)
{
    (void)user;
    if (g_master >= 0)
        (void)write(g_master, bytes, (size_t)length);
}

/*
 * The widget changed shape. Telling the pty is what makes the shell wrap at
 * the right column -- without it a resized window keeps folding lines where
 * the old edge used to be, forever.
 */
static void on_resize(int columns, int rows, void* user)
{
    (void)user;
    if (g_master < 0)
        return;

    struct winsize size;
    memset(&size, 0, sizeof(size));
    size.ws_col = (unsigned short)columns;
    size.ws_row = (unsigned short)rows;
    (void)ioctl(g_master, TIOCSWINSZ, &size);
}

/*
 * The shell wrote something. Returning non-zero closes the window, which is
 * what happens when the shell exits: the master reads end of file, and a
 * terminal whose shell has gone has nothing left to show.
 */
static int on_shell_output(int fd, void* user)
{
    (void)user;

    char buffer[4096];
    ssize_t const got = read(fd, buffer, sizeof(buffer));
    if (got <= 0)
        return 1;

    ui_terminal_feed(g_terminal, buffer, (int)got);
    return 0;
}

static int start_shell(void)
{
    struct winsize size;
    memset(&size, 0, sizeof(size));
    size.ws_col = 80;
    size.ws_row = 24;

    pid_t const child = forkpty(&g_master, NULL, NULL, &size);
    if (child < 0)
        return -1;

    if (child == 0) {
        /* dash if it is here, our own shell if it is not: ports can be
         * switched off, and a terminal with no shell in it is useless in a way
         * that is worth avoiding. */
        setenv("TERM", "vt100", 1);

        execl("/bin/dash", "dash", "-i", NULL);
        execl("/bin/sh", "sh", NULL);

        /* Neither: say so on the terminal rather than exiting silently, so
         * that the window shows why it is empty. */
        const char* message = "terminal: no shell to run\r\n";
        (void)write(1, message, strlen(message));
        _exit(127);
    }

    g_shell = child;
    return 0;
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    /* A shell that dies must not take this process with it when we write to a
     * pty nobody is reading. */
    signal(SIGPIPE, SIG_IGN);

    UiWindow* window = ui_window_create("Terminal", 720, 440);
    if (window == NULL) {
        fprintf(stderr, "terminal: no window server\n");
        return 1;
    }

    /*
     * No padding and no panel. A terminal is the one window where the content
     * really does go edge to edge -- a border of window background around a
     * grid of cells looks like a mistake.
     */
    UiWidget* root = ui_box_create(UI_VERTICAL);
    ui_box_set_padding(root, 0);
    ui_box_set_spacing(root, 0);

    g_terminal = ui_terminal_create(on_input, NULL);
    ui_terminal_set_resize_handler(g_terminal, on_resize, NULL);
    ui_widget_add(root, g_terminal);

    ui_window_set_root(window, root);
    ui_window_focus(window, g_terminal);

    if (start_shell() < 0) {
        fprintf(stderr, "terminal: cannot start a shell: %s\n", strerror(errno));
        ui_window_destroy(window);
        return 1;
    }

    /* The size the widget actually came out at, now that it has been laid out
     * -- the 80x24 the pty was made with is only a starting guess. */
    on_resize(ui_terminal_columns(g_terminal), ui_terminal_rows(g_terminal), NULL);

    /*
     * Two things to wait on: the window server and the shell. ui_window_run
     * only knows about the first, so the loop is here instead -- which is the
     * one place a toolkit this small shows its edges.
     */
    ui_window_pump(window, g_master, on_shell_output, NULL);

    if (g_shell > 0) {
        kill(g_shell, SIGHUP);
        int status = 0;
        waitpid(g_shell, &status, 0);
    }

    if (g_master >= 0)
        close(g_master);

    ui_window_destroy(window);
    return 0;
}
