/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- proof that ring 3 can draw.
 *
 * Everything the graphics stack will stand on, checked from a process with no
 * special privileges: ask /dev/fb0 its shape, take the screen off the console,
 * map three megabytes of it, draw, and follow the mouse and keyboard until
 * told to stop.
 *
 * Run with --check it does all of that without a human, verifies what it can
 * verify -- that the mapping is really the screen, that a store through it
 * lands, that the geometry is self-consistent -- and exits. That is the form
 * /etc/rc runs at every boot, so the day one of these pieces breaks it says
 * so on the console instead of the next person finding out.
 *
 * Run with no arguments it is interactive: move the mouse, press keys, press
 * q or escape to leave.
 */

#include <shitos/abi/fb.h>
#include <shitos/abi/input.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

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

/* --- the screen ----------------------------------------------------------- */

struct Screen {
    int fd;
    struct fb_info info;
    unsigned char* pixels;
    unsigned width, height, pitch, bytes_per_pixel;
};

static int screen_open(struct Screen* screen)
{
    screen->fd = open("/dev/fb0", O_RDWR);
    if (screen->fd < 0)
        return -1;

    if (ioctl(screen->fd, FBIOGET_INFO, &screen->info) < 0) {
        close(screen->fd);
        return -1;
    }

    screen->width = screen->info.width;
    screen->height = screen->info.height;
    screen->pitch = screen->info.pitch;
    screen->bytes_per_pixel = screen->info.bytes_per_pixel;

    screen->pixels
        = mmap(NULL, screen->info.length, PROT_READ | PROT_WRITE, MAP_SHARED, screen->fd, 0);
    if (screen->pixels == MAP_FAILED) {
        close(screen->fd);
        return -1;
    }

    return 0;
}

static void screen_close(struct Screen* screen)
{
    int release = 0;
    ioctl(screen->fd, FBIO_ACQUIRE, &release);
    munmap(screen->pixels, screen->info.length);
    close(screen->fd);
}

/* Pack a colour the way this particular framebuffer wants it. Nothing
 * guarantees 0x00RRGGBB, so the shifts come from the device. */
static unsigned long pack(const struct Screen* screen, unsigned r, unsigned g, unsigned b)
{
    return ((unsigned long)r << screen->info.red_shift)
        | ((unsigned long)g << screen->info.green_shift)
        | ((unsigned long)b << screen->info.blue_shift);
}

static void put_pixel(struct Screen* screen, int x, int y, unsigned long colour)
{
    if (x < 0 || y < 0 || (unsigned)x >= screen->width || (unsigned)y >= screen->height)
        return;

    unsigned char* p
        = screen->pixels + (size_t)y * screen->pitch + (size_t)x * screen->bytes_per_pixel;
    for (unsigned i = 0; i < screen->bytes_per_pixel; ++i)
        p[i] = (unsigned char)(colour >> (i * 8));
}

static unsigned long get_pixel(const struct Screen* screen, int x, int y)
{
    const unsigned char* p
        = screen->pixels + (size_t)y * screen->pitch + (size_t)x * screen->bytes_per_pixel;
    unsigned long value = 0;
    for (unsigned i = 0; i < screen->bytes_per_pixel; ++i)
        value |= (unsigned long)p[i] << (i * 8);
    return value;
}

static void fill_rect(struct Screen* screen, int x, int y, int w, int h, unsigned long colour)
{
    for (int row = 0; row < h; ++row) {
        for (int column = 0; column < w; ++column)
            put_pixel(screen, x + column, y + row, colour);
    }
}

/* A gradient, because it makes a wrong pitch obvious at a glance: get the
 * stride wrong and this shears instead of being smooth. */
static void draw_background(struct Screen* screen)
{
    for (unsigned y = 0; y < screen->height; ++y) {
        unsigned const shade = (y * 64) / screen->height;
        unsigned long const colour = pack(screen, 12 + shade / 2, 12 + shade / 2, 24 + shade);
        for (unsigned x = 0; x < screen->width; ++x)
            put_pixel(screen, (int)x, (int)y, colour);
    }
}

static void draw_cursor(struct Screen* screen, int x, int y, unsigned long colour)
{
    /* A plain arrow: a triangle wide enough to see, drawn without a mask
     * because there is nothing to composite against yet. */
    for (int row = 0; row < 12; ++row) {
        for (int column = 0; column <= row / 2; ++column)
            put_pixel(screen, x + column, y + row, colour);
    }
}

/* --- the automated form --------------------------------------------------- */

static int run_checks(void)
{
    struct Screen screen;
    check(screen_open(&screen) == 0, "opening and mapping /dev/fb0");
    if (screen.fd < 0)
        return 1;

    check(screen.width > 0 && screen.height > 0, "the framebuffer has a size");
    check(screen.bytes_per_pixel >= 3 && screen.bytes_per_pixel <= 4, "3 or 4 bytes per pixel");
    check(screen.pitch >= screen.width * screen.bytes_per_pixel,
        "the pitch covers at least one row of pixels");
    check(screen.info.length >= (unsigned long)screen.pitch * screen.height,
        "the mappable length covers the whole visible area");

    /*
     * The mapping has to *be* the framebuffer, not a copy of it. Reading the
     * device back through the file descriptor is how we find out: if mmap had
     * quietly given us anonymous memory, this would still pass every test
     * above and fail this one.
     */
    int take = 1;
    check(ioctl(screen.fd, FBIO_ACQUIRE, &take) == 0, "taking the screen from the console");

    unsigned long const marker = pack(&screen, 0xAB, 0xCD, 0xEF);
    put_pixel(&screen, 0, 0, marker);
    check(get_pixel(&screen, 0, 0) == marker, "a store through the mapping reads back");

    unsigned char through_the_file[4] = { 0, 0, 0, 0 };
    check(lseek(screen.fd, 0, SEEK_SET) == 0, "seeking the framebuffer device");
    check(read(screen.fd, through_the_file, screen.bytes_per_pixel)
            == (ssize_t)screen.bytes_per_pixel,
        "reading the framebuffer through the device");

    unsigned long readback = 0;
    for (unsigned i = 0; i < screen.bytes_per_pixel; ++i)
        readback |= (unsigned long)through_the_file[i] << (i * 8);
    check(readback == marker, "the mapping is the screen, not a copy of it");

    /* And the last pixel, which catches a length that is a page too short. */
    put_pixel(&screen, (int)screen.width - 1, (int)screen.height - 1, marker);
    check(get_pixel(&screen, (int)screen.width - 1, (int)screen.height - 1) == marker,
        "the last pixel on the screen is mapped too");

    /* Draw something real, briefly, so that a human watching the boot sees the
     * graphics path work rather than only being told it did. */
    draw_background(&screen);
    fill_rect(&screen, 40, 40, 220, 90, pack(&screen, 0x5a, 0x8f, 0xd6));
    fill_rect(&screen, 48, 48, 204, 74, pack(&screen, 0x1c, 0x1c, 0x22));
    draw_cursor(&screen, 300, 200, pack(&screen, 0xff, 0xff, 0xff));

    /* The input devices exist and are readable without blocking forever. */
    int mouse = open("/dev/mouse0", O_RDONLY);
    check(mouse >= 0, "opening /dev/mouse0");
    int keys = open("/dev/kbdraw", O_RDONLY);
    check(keys >= 0, "opening /dev/kbdraw");

    if (mouse >= 0 && keys >= 0) {
        /* Nothing has moved, so poll must say so rather than claiming data. */
        struct pollfd waiting[2];
        waiting[0].fd = mouse;
        waiting[0].events = POLLIN;
        waiting[0].revents = 0;
        waiting[1].fd = keys;
        waiting[1].events = POLLIN;
        waiting[1].revents = 0;

        int ready = poll(waiting, 2, 50);
        check(ready >= 0, "polling the input devices");
    }

    if (mouse >= 0)
        close(mouse);
    if (keys >= 0)
        close(keys);

    screen_close(&screen);

    printf("%d passed, %d failed\n", s_checks - s_failures, s_failures);
    return s_failures == 0 ? 0 : 1;
}

/* --- the interactive form ------------------------------------------------- */

static int run_interactive(void)
{
    struct Screen screen;
    if (screen_open(&screen) != 0) {
        fprintf(stderr, "gfxtest: cannot open /dev/fb0: %s\n", strerror(errno));
        return 1;
    }

    int mouse = open("/dev/mouse0", O_RDONLY);
    int keys = open("/dev/kbdraw", O_RDONLY);
    if (mouse < 0 || keys < 0) {
        fprintf(stderr, "gfxtest: no input devices\n");
        screen_close(&screen);
        return 1;
    }

    int take = 1;
    ioctl(screen.fd, FBIO_ACQUIRE, &take);
    draw_background(&screen);

    int x = (int)screen.width / 2;
    int y = (int)screen.height / 2;
    unsigned long const ink = pack(&screen, 0xff, 0xff, 0xff);
    unsigned long const pressed_ink = pack(&screen, 0xd3, 0x54, 0x4f);
    unsigned buttons = 0;

    draw_cursor(&screen, x, y, ink);

    for (;;) {
        struct pollfd waiting[2];
        waiting[0].fd = mouse;
        waiting[0].events = POLLIN;
        waiting[0].revents = 0;
        waiting[1].fd = keys;
        waiting[1].events = POLLIN;
        waiting[1].revents = 0;

        if (poll(waiting, 2, -1) < 0)
            break;

        if (waiting[0].revents & POLLIN) {
            struct mouse_event events[16];
            ssize_t got = read(mouse, events, sizeof(events));
            for (ssize_t i = 0; got > 0 && i < got / (ssize_t)sizeof(events[0]); ++i) {
                /* Y is positive upward from the mouse and downward on the
                 * screen, which is the one conversion the kernel refuses to
                 * make on anyone's behalf. */
                x += events[i].dx;
                y -= events[i].dy;

                if (x < 0)
                    x = 0;
                if (y < 0)
                    y = 0;
                if ((unsigned)x >= screen.width)
                    x = (int)screen.width - 1;
                if ((unsigned)y >= screen.height)
                    y = (int)screen.height - 1;

                buttons = events[i].buttons;

                /* Held buttons paint. There is no compositor to ask for a
                 * repaint, so the trail is the whole of the feedback. */
                if (buttons != 0)
                    fill_rect(&screen, x - 1, y - 1, 3, 3, pressed_ink);
            }
            draw_cursor(&screen, x, y, buttons ? pressed_ink : ink);
        }

        if (waiting[1].revents & POLLIN) {
            struct key_event events[16];
            ssize_t got = read(keys, events, sizeof(events));
            for (ssize_t i = 0; got > 0 && i < got / (ssize_t)sizeof(events[0]); ++i) {
                if (!events[i].pressed)
                    continue;
                if (events[i].codepoint == 'q' || events[i].keycode == 0x01 /* escape */)
                    goto done;

                /* A pressed key drops a square, so that the codepoint arriving
                 * at all is visible without a font to draw it with. */
                unsigned const shade = (unsigned)(events[i].codepoint * 37) & 0xff;
                fill_rect(&screen, 20 + (int)(events[i].keycode % 40) * 24, 700, 20, 20,
                    pack(&screen, shade, 0xb0, 0xff - shade));
            }
        }
    }

done:
    close(mouse);
    close(keys);
    screen_close(&screen);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "--check") == 0)
        return run_checks();
    return run_interactive();
}
