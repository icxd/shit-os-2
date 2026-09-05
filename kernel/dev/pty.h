// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- pseudo-terminals.
//
// A pty is two ends of the same terminal. The *slave* behaves exactly like the
// console: canonical mode, echo, ^C, job control, a window size. The *master*
// is where a program sits pretending to be the hardware -- what it writes is
// what the slave's user appears to have typed, and what the slave writes comes
// back out of it.
//
// That is the whole of what a terminal emulator needs, and there is nothing
// else in the system that lets one exist: a shell wants a terminal, and until
// now the only terminal was the screen.
//
// Both ends share one LineDiscipline, so a shell under a pty gets the same
// behaviour as a shell on the console rather than a second implementation of
// it that is subtly different.

#pragma once

#include <kernel/dev/line_discipline.h>
#include <kernel/fs/vfs.h>
#include <kernel/lib/error.h>
#include <kernel/sched/waitqueue.h>

namespace kernel::dev {

inline constexpr usize PTY_OUTPUT_CAPACITY = 8192;

class Pty {
public:
    // Creates a pair and its two descriptions. On success the caller owns both.
    static ErrorOr<void> create(fs::FileDescription*& master, fs::FileDescription*& slave);

    LineDiscipline& discipline() { return m_discipline; }

    // slave -> master. Output processing happens here, which is why a shell's
    // bare newlines arrive at the emulator as CRLF.
    void write_from_slave(char c);

    isize read_from_master(void* buffer, usize length);
    bool master_has_output() const { return m_head != m_tail; }

    void note_master_closed();
    void note_slave_closed();

    bool master_closed() const { return m_master_closed; }
    bool slave_closed() const { return m_slave_closed; }

    // Both ends gone: nothing can reach it again, so it can be freed.
    bool is_abandoned() const { return m_master_closed && m_slave_closed; }

private:
    Pty() = default;

    // The echo path. A pty echoes back towards its master rather than to a
    // screen -- what the person typing sees is whatever the emulator draws.
    static void echo_to_master(void* owner, char c);

    LineDiscipline m_discipline;

    char m_output[PTY_OUTPUT_CAPACITY] {};
    usize m_head { 0 };
    usize m_tail { 0 };
    WaitQueue m_master_readers;

    bool m_master_closed { false };
    bool m_slave_closed { false };
};

} // namespace kernel::dev
