// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- the thing that makes a terminal feel like a terminal.
//
// Canonical mode: input buffered a line at a time, backspace erases, ^C raises
// SIGINT, ^D ends the line early and on an empty line ends the input. Raw mode
// when a program asks for it. Job control, so a background reader is stopped
// rather than allowed to steal what the foreground job is waiting for.
//
// It lives on its own because there are two terminals now. The console reads a
// keyboard and echoes to a screen; a pseudo-terminal reads whatever its master
// writes and echoes back to that master. Everything between those two ends is
// identical, and a second copy of it would be a second copy of the job-control
// rules -- which are the part nobody gets right twice.
//
// So the two ends are callbacks and the middle is here.

#pragma once

#include <kernel/sched/waitqueue.h>

#include <shitos/abi/termios.h>
#include <shitos/types.h>

namespace kernel::dev {

inline constexpr usize LINE_BUFFER_SIZE = 1024;
inline constexpr usize READY_BUFFER_SIZE = LINE_BUFFER_SIZE * 4;

class LineDiscipline {
public:
    // Where an echoed character goes. The console writes it to the screen; a
    // pseudo-terminal writes it back to whatever is driving the master.
    using EchoFunction = void (*)(void* owner, char c);

    void configure(void* owner, EchoFunction echo);

    // One byte in from the far side -- a key press, or a write to the master.
    void feed(char c);

    // Bytes out to whoever is reading the terminal. Blocks until a line is
    // ready in canonical mode, or until any byte arrives in raw mode.
    // Returns 0 at end of input and a negative errno on failure.
    isize read(void* buffer, usize length);

    // True when a read would not block, which is the whole of what poll needs.
    bool has_input() const;

    struct termios& termios() { return m_termios; }
    struct termios const& termios() const { return m_termios; }

    struct winsize& window() { return m_window; }
    struct winsize const& window() const { return m_window; }

    // Which process *group* owns the terminal. Everything about job control
    // comes back to this: ^C goes to this group, and a read from any other
    // group stops the reader with SIGTTIN.
    void set_foreground_group(i32 pgid) { m_foreground_group = pgid; }
    i32 foreground_group() const { return m_foreground_group; }

    /*
     * Whether an unowned terminal is claimed by whoever reads it first.
     *
     * The console does this, and has to: nothing calls TIOCSPGRP before the
     * first shell starts, and without it every early reader would be a
     * background job of a terminal nobody owns and would stop itself with
     * SIGTTIN.
     *
     * A pseudo-terminal must not. Its master is held by a program in a
     * different session, and letting a stray read make that program the
     * foreground group means closing the master sends SIGHUP to *itself* --
     * which is exactly what happened, and took the whole boot script with it.
     * A pty gets a foreground group when something says so and not before.
     */
    void set_adopts_readers(bool adopts) { m_adopts_readers = adopts; }

    // Set when the far end has gone for good -- the master closed, or there is
    // no keyboard. A read then returns end of file instead of waiting.
    void set_hung_up(bool hung_up);
    bool is_hung_up() const { return m_hung_up; }

    void wake_readers() { m_readers.wake_all(); }

private:
    void echo(char c);
    void commit_line();
    void push_ready(char c);
    void signal_foreground_group(int signal);

    void* m_owner { nullptr };
    EchoFunction m_echo { nullptr };

    struct termios m_termios { };
    struct winsize m_window { };

    char m_line[LINE_BUFFER_SIZE] {};
    usize m_line_length { 0 };

    // Completed lines waiting to be read, as a flat byte queue: a reader takes
    // bytes, not lines, and may ask for fewer than a whole line at a time.
    char m_ready[READY_BUFFER_SIZE] {};
    usize m_ready_head { 0 };
    usize m_ready_tail { 0 };

    WaitQueue m_readers;
    i32 m_foreground_group { 0 };
    bool m_saw_eof { false };
    bool m_hung_up { false };
    bool m_adopts_readers { false };
};

// The termios a terminal starts in: canonical, echoing, ^C and friends live,
// CR translated to NL coming in and NL to CRNL going out.
void line_discipline_default_termios(struct termios& out);

} // namespace kernel::dev
