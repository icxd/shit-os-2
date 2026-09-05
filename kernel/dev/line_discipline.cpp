// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- canonical mode, job control, and the rest of it.

#include <kernel/dev/line_discipline.h>
#include <kernel/sched/process.h>

#include <shitos/abi/errno.h>
#include <shitos/abi/signal.h>

namespace kernel::dev {

void line_discipline_default_termios(struct termios& out)
{
    out.c_iflag = ICRNL;
    out.c_oflag = OPOST | ONLCR;
    out.c_cflag = 0;
    out.c_lflag = ISIG | ICANON | ECHO;

    out.c_cc[VINTR] = 3; /* ^C */
    out.c_cc[VQUIT] = 28; /* ^\ */
    out.c_cc[VERASE] = 8; /* ^H */
    out.c_cc[VKILL] = 21; /* ^U */
    out.c_cc[VEOF] = 4; /* ^D */
    out.c_cc[VSUSP] = 26; /* ^Z */
    out.c_cc[VMIN] = 1;
    out.c_cc[VTIME] = 0;
}

void LineDiscipline::configure(void* owner, EchoFunction echo)
{
    m_owner = owner;
    m_echo = echo;
    line_discipline_default_termios(m_termios);

    /* A terminal with no size is one that ports refuse to draw in. 80x24 is
     * what everything assumes when it cannot find out. */
    m_window.ws_col = 80;
    m_window.ws_row = 24;
}

void LineDiscipline::echo(char c)
{
    if ((m_termios.c_lflag & ECHO) == 0 || m_echo == nullptr)
        return;

    if (c == '\n') {
        m_echo(m_owner, '\n');
    } else if (c == '\b') {
        // Erase visually as well as logically: back up, overwrite, back up.
        m_echo(m_owner, '\b');
        m_echo(m_owner, ' ');
        m_echo(m_owner, '\b');
    } else if (c >= 32 && c < 127) {
        m_echo(m_owner, c);
    } else if (c < 32) {
        // Control characters echo as ^X, the way every terminal does.
        m_echo(m_owner, '^');
        m_echo(m_owner, static_cast<char>(c + '@'));
    }
}

void LineDiscipline::push_ready(char c)
{
    usize const next = (m_ready_head + 1) % READY_BUFFER_SIZE;
    if (next == m_ready_tail)
        return; // full: drop, rather than overwrite what has not been read
    m_ready[m_ready_head] = c;
    m_ready_head = next;
}

void LineDiscipline::commit_line()
{
    for (usize i = 0; i < m_line_length; ++i)
        push_ready(m_line[i]);
    m_line_length = 0;
    m_readers.wake_all();
}

void LineDiscipline::signal_foreground_group(int signal)
{
    if (m_foreground_group == 0)
        return;
    Process::for_each_in_group(
        m_foreground_group,
        [](Process& process, void* context) { process.raise_signal(*static_cast<int*>(context)); },
        &signal);
}

void LineDiscipline::set_hung_up(bool hung_up)
{
    m_hung_up = hung_up;
    m_readers.wake_all();
}

void LineDiscipline::feed(char c)
{
    if ((m_termios.c_iflag & ICRNL) != 0 && c == '\r')
        c = '\n';

    if ((m_termios.c_lflag & ISIG) != 0) {
        int generated = 0;
        if (c == static_cast<char>(m_termios.c_cc[VINTR]))
            generated = SIGINT;
        else if (c == static_cast<char>(m_termios.c_cc[VQUIT]))
            generated = SIGQUIT;
        else if (c == static_cast<char>(m_termios.c_cc[VSUSP]))
            generated = SIGTSTP;

        if (generated != 0) {
            echo(c);
            echo('\n');
            // Whatever was half typed is discarded: the line the user was
            // building is not what they meant to send any more.
            m_line_length = 0;
            signal_foreground_group(generated);
            m_readers.wake_all();
            return;
        }
    }

    if ((m_termios.c_lflag & ICANON) == 0) {
        // Raw mode: every byte is available immediately.
        push_ready(c);
        echo(c);
        m_readers.wake_all();
        return;
    }

    if (c == static_cast<char>(m_termios.c_cc[VERASE]) || c == 127) {
        if (m_line_length > 0) {
            --m_line_length;
            echo('\b');
        }
        return;
    }

    if (c == static_cast<char>(m_termios.c_cc[VKILL])) {
        while (m_line_length > 0) {
            --m_line_length;
            echo('\b');
        }
        return;
    }

    if (c == static_cast<char>(m_termios.c_cc[VEOF])) {
        // ^D ends the line early. On an empty line that is end of input.
        if (m_line_length == 0)
            m_saw_eof = true;
        commit_line();
        return;
    }

    if (c == '\n') {
        echo('\n');
        if (m_line_length < LINE_BUFFER_SIZE)
            m_line[m_line_length++] = '\n';
        commit_line();
        return;
    }

    if (m_line_length + 1 < LINE_BUFFER_SIZE) {
        m_line[m_line_length++] = c;
        echo(c);
    }
}

bool LineDiscipline::has_input() const
{
    return m_ready_head != m_ready_tail || m_saw_eof || m_hung_up;
}

isize LineDiscipline::read(void* buffer, usize length)
{
    auto* out = static_cast<char*>(buffer);
    if (length == 0)
        return 0;

    auto* reader = Process::current();

    // A terminal whose owning group has gone belongs to nobody, and leaving it
    // that way would stop every later reader with SIGTTIN -- a wedged terminal
    // with no way back. The next reader takes it instead.
    if (m_foreground_group != 0 && !Process::group_exists(m_foreground_group))
        m_foreground_group = 0;

    // Same rule before anyone has claimed it with TIOCSPGRP -- but only where
    // that is wanted. See set_adopts_readers: it is right for the console and
    // actively harmful for a pty.
    if (m_adopts_readers && m_foreground_group == 0 && reader != nullptr)
        m_foreground_group = reader->pgid();

    // A background job reading the terminal is stopped rather than allowed to
    // steal input the foreground job is waiting for. SIGTTIN is the mechanism
    // and `fg` is the cure.
    if (reader != nullptr && m_foreground_group != 0 && reader->pgid() != m_foreground_group) {
        reader->raise_signal(SIGTTIN);
        return -EINTR;
    }

    while (m_ready_head == m_ready_tail) {
        if (m_saw_eof) {
            m_saw_eof = false;
            return 0;
        }
        if (m_hung_up)
            return 0;

        m_readers.wait();

        // A signal arriving while blocked has to break the read, or ^C could
        // never interrupt a program sitting at a prompt.
        if (auto* process = Process::current();
            process != nullptr && process->has_pending_signals())
            return -EINTR;
    }

    usize written = 0;
    while (written < length && m_ready_head != m_ready_tail) {
        out[written++] = m_ready[m_ready_tail];
        m_ready_tail = (m_ready_tail + 1) % READY_BUFFER_SIZE;
    }
    return static_cast<isize>(written);
}

} // namespace kernel::dev
