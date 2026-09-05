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

/* --- /dev/tty ------------------------------------------------------------
 *
 * Not a device of its own: it is whichever terminal the calling process's
 * session is attached to, resolved on every call. That indirection is the
 * whole point. A program whose output is in a pipe still has a terminal, and
 * `/dev/tty` is the only way to reach it -- which is why every shell opens it
 * before deciding whether it can do job control, and why dash sat silently
 * waiting for a terminal that was somebody else's until this existed.
 */

fs::Inode* controlling_terminal()
{
    auto* process = Process::current();
    return process != nullptr ? process->controlling_terminal() : nullptr;
}

isize controlling_read(void*, void* buffer, usize length, u64 offset)
{
    auto* terminal = controlling_terminal();
    if (terminal == nullptr)
        return -ENXIO;

    auto result = terminal->read(offset, buffer, length);
    if (result.is_error())
        return -result.error().code();
    return static_cast<isize>(result.value());
}

isize controlling_write(void*, void const* buffer, usize length, u64 offset)
{
    auto* terminal = controlling_terminal();
    if (terminal == nullptr)
        return -ENXIO;

    auto result = terminal->write(offset, buffer, length);
    if (result.is_error())
        return -result.error().code();
    return static_cast<isize>(result.value());
}

int controlling_ioctl(void*, u32 request, void* argument)
{
    auto* terminal = controlling_terminal();
    if (terminal == nullptr)
        return -ENXIO;

    // Claiming through /dev/tty would make a session's terminal point at
    // itself, and every read after that would recurse until the stack ran
    // out. TIOCSCTTY belongs to the real device.
    if (request == TIOCSCTTY)
        return -ENOTTY;

    auto result = terminal->ioctl(request, argument);
    if (result.is_error())
        return -result.error().code();
    return result.value();
}

bool controlling_poll(void*)
{
    auto* terminal = controlling_terminal();
    return terminal != nullptr ? terminal->can_read_without_blocking() : true;
}

constexpr DeviceOps CONTROLLING_TTY_OPS {
    controlling_read,
    controlling_write,
    controlling_ioctl,
    controlling_poll,
};

} // namespace

Tty& Tty::the()
{
    return s_tty;
}

ErrorOr<void> Tty::initialize()
{
    auto& tty = s_tty;

    tty.m_discipline.configure(&tty, &Tty::echo_to_console);

    // The console, unlike a pty, is claimed by whoever reads it first. Nothing
    // calls TIOCSPGRP before the first shell exists, and without this every
    // early reader would be a background job of a terminal nobody owns.
    tty.m_discipline.set_adopts_readers(true);

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
    TRY(devfs->register_device({ "tty", DEVICE_TYPE_CHAR, nullptr, &CONTROLLING_TTY_OPS }));

    // The console needs to be findable as an inode, not just as a device with
    // a name: that is what a process attaches to when it claims the console
    // with TIOCSCTTY, and what /dev/tty then forwards to.
    if (auto node = fs::resolve("/dev/tty0"); !node.is_error()) {
        tty.m_node = node.value();
        tty.m_node->ref();
    }

    klog(LOG_INFO, "tty", "/dev/tty0 ready (canonical mode, echo on)");
    return {};
}

void Tty::echo_to_console(void*, char c)
{
    kputchar(c);
}

isize Tty::read(void* buffer, usize length)
{
    // No keyboard means nothing will ever arrive, so a read is end of file
    // rather than a wait nobody can end.
    if (m_keyboard == nullptr)
        return 0;
    return m_discipline.read(buffer, length);
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
    case TCGETS:
        if (argument == nullptr)
            return -EINVAL;
        memcpy(argument, &m_discipline.termios(), sizeof(struct termios));
        return 0;

    case TCSETS:
        if (argument == nullptr)
            return -EINVAL;
        memcpy(&m_discipline.termios(), argument, sizeof(struct termios));
        return 0;

    case TIOCGPGRP:
        if (argument == nullptr)
            return -EINVAL;
        *static_cast<i32*>(argument) = m_discipline.foreground_group();
        return 0;

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
        m_discipline.set_foreground_group(wanted);
        return 0;
    }

    case TIOCSCTTY: {
        if (m_node == nullptr)
            return -ENXIO;
        auto* process = Process::current();
        if (process == nullptr)
            return -ENXIO;
        // Only a session leader, because the terminal belongs to the session
        // rather than to the process that happened to ask.
        if (process->pid() != process->sid())
            return -EPERM;
        process->set_controlling_terminal(m_node);
        return 0;
    }

    case TIOCGWINSZ: {
        // The one thing the console knows that a pty does not: its size is
        // however many characters fit on the screen, and is not settable.
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
            tty.m_discipline.feed(scratch[i]);
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
            tty.m_discipline.feed(c);
            saw_input = true;
        }

        // Poll rather than spin. 10 ms is imperceptible when typing and costs
        // nothing when nobody is.
        if (!saw_input)
            Scheduler::sleep_ms(10);
    }
}

} // namespace kernel::dev
