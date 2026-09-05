// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- pipes, named and not.

#include <kernel/fs/pipe.h>
#include <kernel/lib/new.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>
#include <kernel/sched/process.h>

#include <shitos/abi/signal.h>

namespace kernel::fs {

usize PipeBuffer::used() const
{
    return (m_head + PIPE_CAPACITY - m_tail) % PIPE_CAPACITY;
}

usize PipeBuffer::available() const
{
    // One slot is always left empty so that a full buffer is distinguishable
    // from an empty one without a separate count.
    return PIPE_CAPACITY - 1 - used();
}

void PipeBuffer::opened(int flags)
{
    int const access = flags & O_ACCMODE;
    if (access == O_RDONLY) {
        ++m_readers;
    } else if (access == O_WRONLY) {
        ++m_writers;
    } else {
        // O_RDWR is both ends held by one description.
        ++m_readers;
        ++m_writers;
    }
    m_open_queue.wake_all();
}

void PipeBuffer::closed(int flags)
{
    int const access = flags & O_ACCMODE;

    if (access != O_WRONLY && m_readers > 0)
        --m_readers;
    if (access != O_RDONLY && m_writers > 0)
        --m_writers;

    // A writer blocked for space will never be satisfied if the readers have
    // gone; a reader blocked for data has reached end of file if the writers
    // have. Waking both is cheaper than working out which.
    m_write_queue.wake_all();
    m_read_queue.wake_all();
}

ErrorOr<void> PipeBuffer::await_peer(int flags)
{
    int const access = flags & O_ACCMODE;

    // Both ends at once: there is nobody to wait for.
    if (access != O_RDONLY && access != O_WRONLY)
        return {};

    bool const as_reader = access == O_RDONLY;

    // Announce the arrival before waiting, and wake anyone already waiting for
    // it. Without this both sides block forever, each waiting for a
    // counterpart that is already here but has no description yet.
    if (as_reader) {
        ++m_pending_readers;
        ++m_reader_arrivals;
    } else {
        ++m_pending_writers;
        ++m_writer_arrivals;
    }
    m_open_queue.wake_all();

    /*
     * What we are waiting for is a peer *arriving*, which is an edge, but the
     * only thing we can test is whether one is here, which is a level. So the
     * arrival counter is snapshotted first: if it moves while we sleep, a peer
     * came -- and it does not matter that it may already have gone again.
     *
     * Getting this wrong is not a hypothetical. The first version tested
     * presence alone, and a writer that opened, wrote and exited before the
     * reader was scheduled left that reader asleep for good.
     */
    u64 const arrivals_at_entry = as_reader ? m_writer_arrivals : m_reader_arrivals;

    for (;;) {
        usize const present
            = as_reader ? m_writers + m_pending_writers : m_readers + m_pending_readers;
        u64 const arrivals = as_reader ? m_writer_arrivals : m_reader_arrivals;
        if (present > 0 || arrivals != arrivals_at_entry)
            break;

        if (auto* process = Process::current();
            process != nullptr && process->has_pending_signals()) {
            if (as_reader)
                --m_pending_readers;
            else
                --m_pending_writers;
            return Error::from_errno(EINTR);
        }

        m_open_queue.wait();
    }

    if (as_reader)
        --m_pending_readers;
    else
        --m_pending_writers;

    return {};
}

bool PipeBuffer::can_read_without_blocking() const
{
    return used() > 0 || m_writers == 0;
}

bool PipeBuffer::can_write_without_blocking() const
{
    // A pipe with no readers left never blocks: the write fails immediately
    // with EPIPE, which is a completed operation as far as poll is concerned.
    return available() > 0 || m_readers == 0;
}

bool PipeBuffer::is_hung_up() const
{
    // No writers and nothing buffered: end of file, for good.
    return m_writers == 0 && used() == 0;
}

ErrorOr<usize> PipeBuffer::read(void* buffer, usize length)
{
    if (length == 0)
        return static_cast<usize>(0);

    while (used() == 0) {
        // Every writer has gone: end of file, not a stall.
        if (m_writers == 0)
            return static_cast<usize>(0);

        if (auto* process = Process::current();
            process != nullptr && process->has_pending_signals())
            return Error::from_errno(EINTR);

        m_read_queue.wait();
    }

    auto* out = static_cast<u8*>(buffer);
    usize read_bytes = 0;
    while (read_bytes < length && used() > 0) {
        out[read_bytes++] = m_buffer[m_tail];
        m_tail = (m_tail + 1) % PIPE_CAPACITY;
    }

    m_write_queue.wake_all();
    return read_bytes;
}

ErrorOr<usize> PipeBuffer::write(void const* buffer, usize length)
{
    if (length == 0)
        return static_cast<usize>(0);

    auto const* bytes = static_cast<u8 const*>(buffer);
    usize written = 0;

    while (written < length) {
        /*
         * A pending reader counts. Its open() has already been allowed to
         * succeed on the strength of this writer existing, so the two are
         * committed to each other -- and on a FIFO the writer routinely gets
         * to its first write before the reader has been scheduled far enough
         * to be counted. Looking only at m_readers here killed that writer
         * with SIGPIPE.
         */
        if (m_readers + m_pending_readers == 0) {
            // Nobody will ever read this. POSIX says raise SIGPIPE as well as
            // returning EPIPE, so a program that ignores the error still dies
            // rather than looping forever.
            if (auto* process = Process::current(); process != nullptr)
                process->raise_signal(SIGPIPE);
            return Error::from_errno(EPIPE);
        }

        if (available() == 0) {
            if (auto* process = Process::current();
                process != nullptr && process->has_pending_signals())
                return written > 0 ? ErrorOr<usize>(written)
                                   : ErrorOr<usize>(Error::from_errno(EINTR));
            m_write_queue.wait();
            continue;
        }

        while (written < length && available() > 0) {
            m_buffer[m_head] = bytes[written++];
            m_head = (m_head + 1) % PIPE_CAPACITY;
        }
        m_read_queue.wake_all();
    }

    return written;
}

// --- the anonymous kind -----------------------------------------------------

ErrorOr<void> PipeInode::create_pair(FileDescription*& read_end, FileDescription*& write_end)
{
    auto* pipe = static_cast<PipeInode*>(kzalloc(sizeof(PipeInode)));
    if (pipe == nullptr)
        return Error::from_errno(ENOMEM);
    new (pipe) PipeInode();

    auto* reader = static_cast<FileDescription*>(kmalloc(sizeof(FileDescription)));
    auto* writer = static_cast<FileDescription*>(kmalloc(sizeof(FileDescription)));
    if (reader == nullptr || writer == nullptr) {
        kfree(reader);
        kfree(writer);
        kfree(pipe);
        return Error::from_errno(ENOMEM);
    }

    // Constructing a description takes a reference and reports the open, so
    // the pipe goes from zero to two here and back to zero when both ends
    // close.
    new (reader) FileDescription(*pipe, O_RDONLY);
    new (writer) FileDescription(*pipe, O_WRONLY);

    read_end = reader;
    write_end = writer;
    return {};
}

void PipeInode::on_description_opened(int flags)
{
    m_pipe.opened(flags);
}

void PipeInode::on_description_closed(int flags)
{
    m_pipe.closed(flags);

    // Destroying itself here would be a double free: release_description
    // unrefs the inode straight after this returns, and that is what decides
    // when the pipe goes away.
}

bool PipeInode::can_read_without_blocking() const
{
    return m_pipe.can_read_without_blocking();
}

bool PipeInode::can_write_without_blocking() const
{
    return m_pipe.can_write_without_blocking();
}

bool PipeInode::is_hung_up() const
{
    return m_pipe.is_hung_up();
}

ErrorOr<usize> PipeInode::read(u64, void* buffer, usize length)
{
    return m_pipe.read(buffer, length);
}

ErrorOr<usize> PipeInode::write(u64, void const* buffer, usize length)
{
    return m_pipe.write(buffer, length);
}

} // namespace kernel::fs
