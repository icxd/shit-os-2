// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- the console as a terminal.
//
// What is left here after the line discipline moved out: the keyboard on one
// side, the screen on the other, and the window size, which is the one thing
// the console knows and a pseudo-terminal does not -- it is however many
// characters actually fit on the framebuffer.
//
// The TTY reads through /dev/kbd0 rather than talking to the keyboard driver,
// so replacing the PS/2 module with a USB one changes nothing here.

#pragma once

#include <kernel/dev/line_discipline.h>
#include <kernel/fs/vfs.h>
#include <kernel/lib/error.h>

#include <shitos/abi/termios.h>
#include <shitos/module/api.h>

namespace kernel::dev {

class Tty {
public:
    static ErrorOr<void> initialize();
    static Tty& the();

    isize read(void* buffer, usize length);
    isize write(void const* buffer, usize length);
    int ioctl(u32 request, void* argument);
    bool has_line_ready() const { return m_discipline.has_input(); }

    LineDiscipline& discipline() { return m_discipline; }

    void set_foreground_group(i32 pgid) { m_discipline.set_foreground_group(pgid); }
    i32 foreground_group() const { return m_discipline.foreground_group(); }

private:
    friend void tty_input_thread(void*);
    friend void tty_serial_input_thread(void*);

    // Where the discipline's echo goes: the screen and the serial port, which
    // is what kputchar already means.
    static void echo_to_console(void* owner, char c);

    LineDiscipline m_discipline;
    fs::Inode* m_keyboard { nullptr };

    // The console's own /dev/tty0 node. Held so that TIOCSCTTY has something
    // to hand the process as its controlling terminal -- a Tty is not an
    // Inode, and a session has to point at something /dev/tty can forward to.
    fs::Inode* m_node { nullptr };
};

// Pumps the keyboard into the line discipline. Runs as its own kernel thread
// because reading the keyboard blocks.
void tty_input_thread(void*);

// The same, for the serial port. Polled rather than interrupt driven, which is
// cheap at 10 ms and means a headless machine is usable over COM1 -- both for
// a human with no display and for the boot test in tools/run-qemu.sh.
void tty_serial_input_thread(void*);

} // namespace kernel::dev
