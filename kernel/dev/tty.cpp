// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- the terminal.

#include <kernel/arch/x86_64/serial.h>
#include <kernel/dev/console.h>
#include <kernel/dev/framebuffer.h>
#include <kernel/dev/tty.h>
#include <kernel/fs/devfs.h>
#include <kernel/lib/new.h>
#include <kernel/lib/spinlock.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>
#include <kernel/sched/process.h>
#include <kernel/sched/scheduler.h>

#include <shitos/abi/signal.h>

namespace kernel::dev {

namespace {

Tty s_tty;

isize tty_device_read(void* self, void* buffer, usize length, u64)
{
    return static_cast<Tty*>(self)->read(buffer, length);
}

isize tty_device_write(void* self, void const* buffer, usize length, u64)
{
    return static_cast<Tty*>(self)->write(buffer, length);
}

int tty_device_ioctl(void* self, u32 request, void* argument)
{
    return static_cast<Tty*>(self)->ioctl(request, argument);
}

bool tty_device_poll(void* self)
{
    return static_cast<Tty*>(self)->has_line_ready();
}

constexpr DeviceOps TTY_OPS {
    tty_device_read,
    tty_device_write,
    tty_device_ioctl,
    tty_device_poll,
};

} // namespace

Tty& Tty::the()
{
    return s_tty;
}

ErrorOr<void> Tty::initialize()
{
    auto& tty = s_tty;

    tty.m_termios.c_iflag = ICRNL;
    tty.m_termios.c_oflag = OPOST | ONLCR;
    tty.m_termios.c_lflag = ISIG | ICANON | ECHO;
    tty.m_termios.c_cc[VINTR] = 3; // ^C
    tty.m_termios.c_cc[VQUIT] = 28; // ctrl-backslash
    tty.m_termios.c_cc[VERASE] = 8; // backspace
    tty.m_termios.c_cc[VKILL] = 21; // ^U
    tty.m_termios.c_cc[VEOF] = 4; // ^D
    tty.m_termios.c_cc[VSUSP] = 26; // ^Z

    // The keyboard is whatever registered /dev/kbd0. If no keyboard module
    // loaded, the terminal is output-only rather than broken.
    auto keyboard = fs::resolve("/dev/kbd0");
    if (!keyboard.is_error()) {
        // Held for the lifetime of the system, so it takes a reference like
        // any other long-lived holder: unloading the keyboard module must
        // not free the node out from under a blocked read.
        tty.m_keyboard = keyboard.value();
        tty.m_keyboard->ref();
    } else
        klog(LOG_WARN, "tty", "no /dev/kbd0; the terminal will be output only");

    auto* devfs = fs::DevfsFileSystem::the();
    if (devfs == nullptr)
        return Error::from_errno(ENODEV);

    TRY(devfs->register_device({ "tty0", DEVICE_TYPE_CHAR, &tty, &TTY_OPS }));
    TRY(devfs->register_device({ "console", DEVICE_TYPE_CHAR, &tty, &TTY_OPS }));

    klog(LOG_INFO, "tty", "/dev/tty0 ready (canonical mode, echo on)");
    return {};
}

void Tty::signal_foreground_group(int signal)
{
    if (m_foreground_group == 0)
        return;
    Process::for_each_in_group(
        m_foreground_group,
        [](Process& process, void* context) { process.raise_signal(*static_cast<int*>(context)); },
        &signal);
}

void Tty::echo(char c)
{
    if ((m_termios.c_lflag & ECHO) == 0)
        return;

    if (c == '\n') {
        kputchar('\n');
    } else if (c == '\b') {
        // Erase visually as well as logically: back up, overwrite, back up.
        kputchar('\b');
        kputchar(' ');
        kputchar('\b');
    } else if (c >= 32 && c < 127) {
        kputchar(c);
    } else if (c < 32) {
        // Control characters echo as ^X, the way every terminal does.
        kputchar('^');
        kputchar(static_cast<char>(c + '@'));
    }
}

void Tty::process_input_character(char c)
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
            kputchar('\n');
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
        usize const next = (m_ready_head + 1) % sizeof(m_ready);
        if (next != m_ready_tail) {
            m_ready[m_ready_head] = c;
            m_ready_head = next;
        }
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
        for (usize i = 0; i < m_line_length; ++i) {
            usize const next = (m_ready_head + 1) % sizeof(m_ready);
            if (next == m_ready_tail)
                break;
            m_ready[m_ready_head] = m_line[i];
            m_ready_head = next;
        }
        m_line_length = 0;
        m_readers.wake_all();
        return;
    }

    if (c == '\n') {
        echo('\n');
        if (m_line_length < TTY_LINE_BUFFER_SIZE)
            m_line[m_line_length++] = '\n';
        for (usize i = 0; i < m_line_length; ++i) {
            usize const next = (m_ready_head + 1) % sizeof(m_ready);
            if (next == m_ready_tail)
                break;
            m_ready[m_ready_head] = m_line[i];
            m_ready_head = next;
        }
        m_line_length = 0;
        m_readers.wake_all();
        return;
    }

    if (m_line_length + 1 < TTY_LINE_BUFFER_SIZE) {
        m_line[m_line_length++] = c;
        echo(c);
    }
}

bool Tty::has_line_ready() const
{
    return m_ready_head != m_ready_tail || m_saw_eof;
}

isize Tty::read(void* buffer, usize length)
{
    auto* out = static_cast<char*>(buffer);
    if (length == 0)
        return 0;

    auto* reader = Process::current();

    // A terminal whose owning group has gone belongs to nobody, and leaving it
    // that way would stop every later reader with SIGTTIN -- a wedged console
    // with no way back. The next reader takes it instead.
    if (m_foreground_group != 0 && !Process::group_exists(m_foreground_group))
        m_foreground_group = 0;

    // Same rule before anyone has claimed it with TIOCSPGRP. Without it, the
    // shell init spawns would be in the background of a terminal nobody owns
    // and could never read at all.
    if (m_foreground_group == 0 && reader != nullptr)
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
        if (m_keyboard == nullptr)
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
        m_ready_tail = (m_ready_tail + 1) % sizeof(m_ready);
    }
    return static_cast<isize>(written);
}

isize Tty::write(void const* buffer, usize length)
{
    auto const* bytes = static_cast<char const*>(buffer);
    for (usize i = 0; i < length; ++i) {
        char const c = bytes[i];
        // The console already turns \n into CRLF for the serial port, so
        // ONLCR needs no extra work here.
        kputchar(c);
    }
    return static_cast<isize>(length);
}

int Tty::ioctl(u32 request, void* argument)
{
    switch (request) {
    case TCGETS: {
        if (argument == nullptr)
            return -EINVAL;
        memcpy(argument, &m_termios, sizeof(m_termios));
        return 0;
    }
    case TCSETS: {
        if (argument == nullptr)
            return -EINVAL;
        memcpy(&m_termios, argument, sizeof(m_termios));
        return 0;
    }
    case TIOCGPGRP: {
        if (argument == nullptr)
            return -EINVAL;
        *static_cast<i32*>(argument) = m_foreground_group;
        return 0;
    }
    case TIOCSPGRP: {
        if (argument == nullptr)
            return -EINVAL;
        i32 const wanted = *static_cast<i32 const*>(argument);
        if (wanted <= 0)
            return -EINVAL;
        // A group that does not exist would leave the terminal owned by
        // nothing, and every subsequent read would stop its caller.
        if (!Process::group_exists(wanted))
            return -EPERM;
        m_foreground_group = wanted;
        return 0;
    }
    case TIOCGWINSZ: {
        if (argument == nullptr)
            return -EINVAL;
        auto* size = static_cast<struct winsize*>(argument);
        auto& console = framebuffer_console();
        size->ws_col = console.is_usable() ? static_cast<u16>(console.columns()) : 80;
        size->ws_row = console.is_usable() ? static_cast<u16>(console.rows()) : 25;
        size->ws_xpixel = 0;
        size->ws_ypixel = 0;
        return 0;
    }
    default: return -ENOTTY;
    }
}

void tty_input_thread(void*)
{
    auto& tty = s_tty;
    if (tty.m_keyboard == nullptr)
        return;

    // Blocking in here is exactly why this is a thread: the keyboard read
    // sleeps until a key arrives, and nothing else has to wait for it.
    for (;;) {
        char scratch[32];
        auto read = tty.m_keyboard->read(0, scratch, sizeof(scratch));
        if (read.is_error()) {
            Scheduler::sleep_ms(50);
            continue;
        }
        for (usize i = 0; i < read.value(); ++i)
            tty.process_input_character(scratch[i]);
    }
}

void tty_serial_input_thread(void*)
{
    auto& tty = s_tty;
    auto& port = arch::serial_com1();

    for (;;) {
        bool saw_input = false;
        while (port.has_input()) {
            char c = port.read_char();
            // Terminals send CR for the return key; the line discipline wants
            // NL, and ICRNL already handles that. DEL is what most terminals
            // send for backspace.
            tty.process_input_character(c);
            saw_input = true;
        }

        // Poll rather than spin. 10 ms is imperceptible when typing and costs
        // nothing when nobody is.
        if (!saw_input)
            Scheduler::sleep_ms(10);
    }
}

} // namespace kernel::dev
