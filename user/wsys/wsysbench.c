/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- how long a frame takes, end to end.
 *
 * "The interface feels laggy" is not something anyone can fix without a
 * number, and the number that matters is the whole round trip: the client
 * repaints into shared memory, tells the server which rectangle moved, the
 * server composites it and blits. Timing any one of those in isolation misses
 * the two that are slow.
 *
 * So this is a real client doing the most demanding thing the desktop does --
 * a terminal printing -- with nothing in it that a human has to watch.
 *
 *   wsysbench [frames]
 */

#include "terminal.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned long now_micros(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0;
    return (unsigned long)now.tv_sec * 1000000UL + (unsigned long)(now.tv_nsec / 1000);
}

int main(int argc, char** argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 100;
    if (frames < 1)
        frames = 1;

    UiWindow* window = ui_window_create("Benchmark", 720, 440);
    if (window == NULL) {
        fprintf(stderr, "wsysbench: no window server\n");
        return 1;
    }

    UiWidget* root = ui_box_create(UI_VERTICAL);
    ui_box_set_padding(root, 0);
    ui_box_set_spacing(root, 0);

    UiWidget* terminal = ui_terminal_create(NULL, NULL);
    ui_widget_add(root, terminal);
    ui_window_set_root(window, root);

    /* One turn to get the window laid out and on screen, so that the timed
     * part measures steady state rather than startup. */
    ui_window_step(window, 0);

    unsigned long const started = now_micros();

    for (int i = 0; i < frames; ++i) {
        char line[128];
        int const length = snprintf(line, sizeof(line),
            "\x1b[32m%6d\x1b[0m the quick brown fox jumps over the lazy dog\r\n", i);
        ui_terminal_feed(terminal, line, length);

        /*
         * Zero timeout: do not wait for anything, just repaint and tell the
         * server. The server's own cost still lands in this measurement --
         * once the channel fills, the write blocks until it has caught up,
         * which is exactly the backpressure a real client feels.
         */
        if (!ui_window_step(window, 0))
            break;
    }

    unsigned long const elapsed = now_micros() - started;

    printf("wsysbench: %d frames in %lu us, %lu us/frame\n", frames, elapsed,
        elapsed / (unsigned long)frames);

    ui_window_destroy(window);
    return 0;
}
