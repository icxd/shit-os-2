// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- pseudo-terminals.

#include <kernel/dev/pty.h>
#include <kernel/lib/new.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>
#include <kernel/sched/process.h>

#include <shitos/abi/errno.h>
#include <shitos/abi/termios.h>

namespace kernel::dev {

namespace {

/*
 * The two ends, as inodes. Neither has a name: a pty is reached through the
 * descriptors openpty hands back, so nothing needs to find it by path and
 * there is no /dev/pts to keep in step with reality.
 */
class PtySlaveInode final : public fs::Inode {
public:
    explicit PtySlaveInode(Pty& pty)
        : Inode(nullptr, fs::InodeType::CharacterDevice, 0620)
        , m_pty(pty)
    {
        drop_initial_link_reference();
    }

    ErrorOr<usize> read(u64, void* buffer, usize length) override
    {
        isize const got = m_pty.discipline().read(buffer, length);
        if (got < 0)
            return Error::from_errno(static_cast<int>(-got));
        return static_cast<usize>(got);
    }

    ErrorOr<usize> write(u64, void const* buffer, usize length) override
    {
        auto const* bytes = static_cast<char const*>(buffer);
        for (usize i = 0; i < length; ++i)
            m_pty.write_from_slave(bytes[i]);
        return length;
    }

    ErrorOr<int> ioctl(u32 request, void* argument) override;

    bool can_read_without_blocking() const override { return m_pty.discipline().has_input(); }
    bool is_hung_up() const override { return m_pty.master_closed(); }

    void on_description_closed(int) override;

private:
    Pty& m_pty;
};

class PtyMasterInode final : public fs::Inode {
public:
    explicit PtyMasterInode(Pty& pty)
        : Inode(nullptr, fs::InodeType::CharacterDevice, 0600)
        , m_pty(pty)
    {
        drop_initial_link_reference();
    }

    ErrorOr<usize> read(u64, void* buffer, usize length) override
    {
        isize const got = m_pty.read_from_master(buffer, length);
        if (got < 0)
            return Error::from_errno(static_cast<int>(-got));
        return static_cast<usize>(got);
    }

    /* Writing to the master is typing at the terminal. */
    ErrorOr<usize> write(u64, void const* buffer, usize length) override
    {
        auto const* bytes = static_cast<char const*>(buffer);
        for (usize i = 0; i < length; ++i)
            m_pty.discipline().feed(bytes[i]);
        return length;
    }

    /*
     * The master answers the terminal ioctls too, and they act on the same
     * state the slave sees. That is what lets an emulator set the window size
     * from its own side, which is the only side that knows it.
     */
    ErrorOr<int> ioctl(u32 request, void* argument) override;

    bool can_read_without_blocking() const override
    {
        return m_pty.master_has_output() || m_pty.slave_closed();
    }
    bool is_hung_up() const override { return m_pty.slave_closed(); }

    void on_description_closed(int) override;

private:
    Pty& m_pty;
};

ErrorOr<int> terminal_ioctl(Pty& pty, u32 request, void* argument)
{
    auto& discipline = pty.discipline();

    switch (request) {
    case TCGETS:
        if (argument == nullptr)
            return Error::from_errno(EINVAL);
        memcpy(argument, &discipline.termios(), sizeof(struct termios));
        return 0;

    case TCSETS:
        if (argument == nullptr)
            return Error::from_errno(EINVAL);
        memcpy(&discipline.termios(), argument, sizeof(struct termios));
        return 0;

    case TIOCGWINSZ:
        if (argument == nullptr)
            return Error::from_errno(EINVAL);
        memcpy(argument, &discipline.window(), sizeof(struct winsize));
        return 0;

    case TIOCSWINSZ: {
        if (argument == nullptr)
            return Error::from_errno(EINVAL);
        memcpy(&discipline.window(), argument, sizeof(struct winsize));

        /*
         * POSIX says the foreground group is told when the size changes, and
         * it matters: a shell that is not told keeps wrapping its prompt at
         * the old width forever.
         */
        if (i32 const group = discipline.foreground_group(); group != 0) {
            int signal = SIGWINCH;
            Process::for_each_in_group(
                group,
                [](Process& process, void* context) {
                    process.raise_signal(*static_cast<int*>(context));
                },
                &signal);
        }
        return 0;
    }

    case TIOCGPGRP:
        if (argument == nullptr)
            return Error::from_errno(EINVAL);
        *static_cast<i32*>(argument) = discipline.foreground_group();
        return 0;

    case TIOCSPGRP: {
        if (argument == nullptr)
            return Error::from_errno(EINVAL);
        i32 const wanted = *static_cast<i32 const*>(argument);
        if (wanted <= 0)
            return Error::from_errno(EINVAL);
        if (!Process::group_exists(wanted))
            return Error::from_errno(EPERM);
        discipline.set_foreground_group(wanted);
        return 0;
    }

    default: return Error::from_errno(ENOTTY);
    }
}

ErrorOr<int> PtySlaveInode::ioctl(u32 request, void* argument)
{
    /*
     * Claiming the pty as the session's terminal, which only the slave can be
     * -- the master is the emulator's end, and an emulator attaching to its
     * own pty would be answering its own questions.
     *
     * This is the only way a pty ever becomes a controlling terminal. The
     * console can be claimed implicitly by being opened by name; a pty has no
     * name to open, which is exactly why TIOCSCTTY exists.
     */
    if (request == TIOCSCTTY) {
        auto* process = Process::current();
        if (process == nullptr)
            return Error::from_errno(ENXIO);
        if (process->pid() != process->sid())
            return Error::from_errno(EPERM);
        process->set_controlling_terminal(this);
        return 0;
    }

    return terminal_ioctl(m_pty, request, argument);
}

ErrorOr<int> PtyMasterInode::ioctl(u32 request, void* argument)
{
    return terminal_ioctl(m_pty, request, argument);
}

void PtySlaveInode::on_description_closed(int)
{
    m_pty.note_slave_closed();
}

void PtyMasterInode::on_description_closed(int)
{
    m_pty.note_master_closed();
}

} // namespace

void Pty::echo_to_master(void* owner, char c)
{
    static_cast<Pty*>(owner)->write_from_slave(c);
}

void Pty::write_from_slave(char c)
{
    if (m_master_closed)
        return;

    auto push = [this](char value) {
        usize const next = (m_head + 1) % PTY_OUTPUT_CAPACITY;
        if (next == m_tail)
            return; // full: drop, rather than overwrite unread output
        m_output[m_head] = value;
        m_head = next;
    };

    /*
     * Output processing. A shell writes a bare newline and expects the
     * terminal to turn it into a carriage return as well; leaving that to the
     * emulator would mean every emulator had to know it.
     */
    if (c == '\n' && (m_discipline.termios().c_oflag & (OPOST | ONLCR)) == (OPOST | ONLCR))
        push('\r');

    push(c);
    m_master_readers.wake_all();
}

isize Pty::read_from_master(void* buffer, usize length)
{
    auto* out = static_cast<char*>(buffer);
    if (length == 0)
        return 0;

    while (m_head == m_tail) {
        // Nothing more can ever arrive once the slave has gone.
        if (m_slave_closed)
            return 0;

        m_master_readers.wait();

        if (auto* process = Process::current();
            process != nullptr && process->has_pending_signals())
            return -EINTR;
    }

    usize written = 0;
    while (written < length && m_head != m_tail) {
        out[written++] = m_output[m_tail];
        m_tail = (m_tail + 1) % PTY_OUTPUT_CAPACITY;
    }
    return static_cast<isize>(written);
}

void Pty::note_master_closed()
{
    m_master_closed = true;

    /*
     * The terminal has been unplugged. POSIX sends SIGHUP to the foreground
     * group, which is what makes closing a terminal window take the shell
     * inside it with them rather than leaving an orphan reading forever.
     */
    if (i32 const group = m_discipline.foreground_group(); group != 0) {
        int signal = SIGHUP;
        Process::for_each_in_group(
            group,
            [](Process& process, void* context) {
                process.raise_signal(*static_cast<int*>(context));
            },
            &signal);
    }

    m_discipline.set_hung_up(true);
}

void Pty::note_slave_closed()
{
    m_slave_closed = true;
    m_master_readers.wake_all();
}

ErrorOr<void> Pty::create(fs::FileDescription*& master_out, fs::FileDescription*& slave_out)
{
    auto* pty = static_cast<Pty*>(kzalloc(sizeof(Pty)));
    if (pty == nullptr)
        return Error::from_errno(ENOMEM);
    new (pty) Pty();

    pty->m_discipline.configure(pty, &Pty::echo_to_master);

    /*
     * Deliberately *not* adopting its first reader, unlike the console. The
     * master is held by a program in another session; letting a read make that
     * program the foreground group means closing the master sends SIGHUP to
     * itself. A pty gets a foreground group when something says so.
     */
    pty->m_discipline.set_adopts_readers(false);

    auto* master_inode = static_cast<PtyMasterInode*>(kzalloc(sizeof(PtyMasterInode)));
    auto* slave_inode = static_cast<PtySlaveInode*>(kzalloc(sizeof(PtySlaveInode)));
    auto* master = static_cast<fs::FileDescription*>(kmalloc(sizeof(fs::FileDescription)));
    auto* slave = static_cast<fs::FileDescription*>(kmalloc(sizeof(fs::FileDescription)));

    if (master_inode == nullptr || slave_inode == nullptr || master == nullptr
        || slave == nullptr) {
        kfree(master_inode);
        kfree(slave_inode);
        kfree(master);
        kfree(slave);
        kfree(pty);
        return Error::from_errno(ENOMEM);
    }

    new (master_inode) PtyMasterInode(*pty);
    new (slave_inode) PtySlaveInode(*pty);
    new (master) fs::FileDescription(*master_inode, O_RDWR);
    new (slave) fs::FileDescription(*slave_inode, O_RDWR);

    master_out = master;
    slave_out = slave;
    return {};
}

} // namespace kernel::dev
