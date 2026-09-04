// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- pipes.

#include <kernel/fs/pipe.h>
#include <kernel/lib/new.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>
#include <kernel/sched/process.h>

#include <shitos/abi/signal.h>

namespace kernel::fs {

usize PipeInode::used() const
{
    return (m_head + PIPE_CAPACITY - m_tail) % PIPE_CAPACITY;
}

usize PipeInode::available() const
{
    // One slot is always left empty so that a full buffer is distinguishable
    // from an empty one without a separate count.
    return PIPE_CAPACITY - 1 - used();
}

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

    new (reader) FileDescription(*pipe, O_RDONLY);
    new (writer) FileDescription(*pipe, O_WRONLY);
    pipe->on_description_opened(O_RDONLY);
    pipe->on_description_opened(O_WRONLY);

    read_end = reader;
    write_end = writer;
    return {};
}

void PipeInode::on_description_opened(int flags)
{
    if ((flags & O_ACCMODE) == O_RDONLY)
        ++m_readers;
    else
        ++m_writers;
}

void PipeInode::on_description_closed(int flags)
{
    if ((flags & O_ACCMODE) == O_RDONLY) {
        if (m_readers > 0)
            --m_readers;
        // A writer blocked for space will never be satisfied now.
        m_write_queue.wake_all();
    } else {
        if (m_writers > 0)
            --m_writers;
        // A reader blocked for data has just reached end of file.
        m_read_queue.wake_all();
    }

    // Nothing refers to this pipe any more.
    if (m_readers == 0 && m_writers == 0) {
        this->~PipeInode();
        kfree(this);
    }
}

bool PipeInode::can_read_without_blocking() const
{
    return used() > 0 || m_writers == 0;
}

ErrorOr<usize> PipeInode::read(u64, void* buffer, usize length)
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

ErrorOr<usize> PipeInode::write(u64, void const* buffer, usize length)
{
    if (length == 0)
        return static_cast<usize>(0);

    auto const* bytes = static_cast<u8 const*>(buffer);
    usize written = 0;

    while (written < length) {
        if (m_readers == 0) {
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

} // namespace kernel::fs
