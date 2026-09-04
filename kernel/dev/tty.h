// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- the terminal.
//
// Sits between the keyboard driver and the console and does the thing that
// makes a shell feel like a shell: canonical mode. Input is buffered a line at
// a time, backspace erases, ^C raises SIGINT and ^D ends the line early.
//
// The TTY reads through /dev/kbd0 rather than talking to the keyboard driver,
// so replacing the PS/2 module with a USB one changes nothing here.

#pragma once

#include <kernel/fs/vfs.h>
#include <kernel/lib/error.h>
#include <kernel/sched/waitqueue.h>

#include <shitos/abi/termios.h>
#include <shitos/module/api.h>

namespace kernel::dev {

inline constexpr usize TTY_LINE_BUFFER_SIZE = 1024;

class Tty {
public:
    static ErrorOr<void> initialize();
    static Tty& the();

    isize read(void* buffer, usize length);
    isize write(void const* buffer, usize length);
    int ioctl(u32 request, void* argument);
    bool has_line_ready() const;

    // Which process *group* owns the terminal. A shell sets it with TIOCSPGRP
    // as it starts and stops jobs; everything else about job control follows
    // from it. ^C goes to this group, and a read from any other group stops
    // the reader with SIGTTIN rather than stealing input from the foreground.
    void set_foreground_group(i32 pgid) { m_foreground_group = pgid; }
    i32 foreground_group() const { return m_foreground_group; }

private:
    friend void tty_input_thread(void*);
    friend void tty_serial_input_thread(void*);

    void process_input_character(char c);
    void echo(char c);

    struct termios m_termios { };

    char m_line[TTY_LINE_BUFFER_SIZE] {};
    usize m_line_length { 0 };

    // Completed lines waiting to be read, as a flat byte queue: a reader takes
    // bytes, not lines, and may ask for fewer than a whole line at a time.
    char m_ready[TTY_LINE_BUFFER_SIZE * 4] {};
    usize m_ready_head { 0 };
    usize m_ready_tail { 0 };

    // Sends `signal` to every process in the foreground group, which is what
    // makes ^C reach a whole pipeline rather than one member of it.
    void signal_foreground_group(int signal);

    fs::Inode* m_keyboard { nullptr };
    WaitQueue m_readers;
    i32 m_foreground_group { 0 };
    bool m_saw_eof { false };
};

// Pumps the keyboard into the line discipline. Runs as its own kernel thread
// because reading the keyboard blocks.
void tty_input_thread(void*);

// The same, for the serial port. Polled rather than interrupt driven, which is
// cheap at 10 ms and means a headless machine is usable over COM1 -- both for
// a human with no display and for the boot test in tools/run-qemu.sh.
void tty_serial_input_thread(void*);

} // namespace kernel::dev
