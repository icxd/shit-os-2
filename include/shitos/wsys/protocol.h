/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- what a window server and its clients say to each other.
 *
 * Two channels and a pile of shared memory.
 *
 * Messages go through named FIFOs and are fixed-size tagged structs, never
 * parsed. A FIFO write of less than PIPE_BUF is atomic, so a message never
 * arrives in halves and neither side needs a reassembly buffer or a length
 * prefix. The cost is that every message is as big as the largest one, which
 * at 64 bytes is not a cost.
 *
 * Pixels do not go through the channel at all. Each window is a tmpfs file
 * that both sides map MAP_SHARED, so a client draws directly into memory the
 * compositor will read, and "I have finished drawing" is a 64-byte message
 * rather than a megabyte of copying.
 *
 * Connecting deliberately involves no blocking open. A client makes its own
 * two FIFOs, opens both O_RDWR so neither can block or see end of file, and
 * only then announces itself on the well-known one. The server does the same.
 * Every ordering works, including a client that starts first.
 */

#pragma once

#include <shitos/types.h>

#define WSYS_ROOT "/tmp/wsys"
#define WSYS_CONNECT_FIFO WSYS_ROOT "/connect"

#define WSYS_TITLE_MAX 64
#define WSYS_PATH_MAX 96

/* Chrome, in pixels. The client never draws any of it and never sees it in
 * its own coordinates: a window's buffer is its content and nothing else. */
#define WSYS_TITLEBAR_HEIGHT 26
#define WSYS_BORDER_WIDTH 1

enum WsysMessageType {
    /* client -> server */
    WSYS_CREATE_WINDOW = 1,
    WSYS_DESTROY_WINDOW,
    WSYS_SET_TITLE,
    WSYS_DAMAGE, /* I have drawn; the buffer is worth reading again */

    /* server -> client */
    WSYS_WINDOW_CREATED = 100,
    WSYS_KEY,
    WSYS_MOUSE,
    WSYS_CLOSE_REQUEST, /* the titlebar button, or a keyboard shortcut */
    WSYS_FOCUS,
};

struct WsysHello {
    i32 pid; /* names the two FIFOs the client has already made */
};

struct WsysMessage {
    u32 type;
    u32 window;

    union {
        struct {
            i32 width;
            i32 height;
            i32 x; /* -1 for "put it wherever" */
            i32 y;
        } create;

        struct {
            char text[WSYS_TITLE_MAX];
        } title;

        /* The rectangle that changed, in window coordinates. A client that
         * cannot be bothered may send the whole window; the compositor will
         * believe it and do more work. */
        struct {
            i32 x, y, width, height;
        } damage;

        struct {
            i32 width;
            i32 height;
            char buffer_path[WSYS_PATH_MAX]; /* mmap this, MAP_SHARED */
        } created;

        struct {
            u16 keycode;
            u8 pressed;
            u8 modifiers;
            u32 codepoint;
        } key;

        struct {
            i32 x, y; /* in window coordinates; may be negative on the chrome */
            u8 buttons;
            i8 wheel;
            u8 _reserved[2];
        } mouse;

        struct {
            u32 focused;
        } focus;

        u8 _size[WSYS_PATH_MAX + 16];
    };
};
