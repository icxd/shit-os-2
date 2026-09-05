// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- pipes, named and not.
//
// The interesting behaviour of a pipe is not the ring buffer; it is what
// happens at the ends. A read with no writers left is end of file, a write
// with no readers left is EPIPE and a SIGPIPE, and a reader and a writer that
// arrive at different times have to find each other.
//
// All of that is the same whether the pipe has a name or not, so it lives in
// PipeBuffer and there is exactly one copy of it. An anonymous pipe is a
// PipeInode wrapped around one; a named FIFO is a tmpfs directory entry that
// owns one. The only real difference between them is how they are found.

#pragma once

#include <kernel/fs/vfs.h>
#include <kernel/sched/waitqueue.h>

namespace kernel::fs {

inline constexpr usize PIPE_CAPACITY = 4096;

class PipeBuffer {
public:
    ErrorOr<usize> read(void* buffer, usize length);
    ErrorOr<usize> write(void const* buffer, usize length);

    bool can_read_without_blocking() const;
    bool can_write_without_blocking() const;
    bool is_hung_up() const;

    void opened(int flags);
    void closed(int flags);

    /*
     * The rendezvous, for a FIFO that has a name. POSIX says opening one end
     * blocks until the other end is opened too, which is what makes a FIFO a
     * meeting point rather than a file: a client can start before the server
     * is listening and simply wait.
     *
     * O_RDWR is exempt and never blocks -- it is both ends at once, and it is
     * how a server holds a FIFO open so that reads do not see end of file
     * every time the last client disconnects.
     */
    ErrorOr<void> await_peer(int flags);

    usize used() const;

private:
    usize available() const;

    u8 m_buffer[PIPE_CAPACITY] {};
    usize m_head { 0 };
    usize m_tail { 0 };

    usize m_readers { 0 };
    usize m_writers { 0 };

    /*
     * Openers that are committed but have no description yet. The other side
     * must be able to see them -- both so that its own open() stops waiting,
     * and so that a write() does not decide there is no reader and raise
     * SIGPIPE at a reader that is a microsecond away from existing.
     */
    usize m_pending_readers { 0 };
    usize m_pending_writers { 0 };

    /*
     * How many ends have *ever* been opened. The rendezvous needs these
     * because arrival is an edge and presence is a level: a writer that opens
     * and closes while the reader is still waking up would otherwise leave
     * that reader waiting forever for something that already happened.
     */
    u64 m_reader_arrivals { 0 };
    u64 m_writer_arrivals { 0 };

    WaitQueue m_read_queue;
    WaitQueue m_write_queue;

    // Woken when an opener arrives, so that the other side's open() returns.
    WaitQueue m_open_queue;
};

class PipeInode final : public Inode {
public:
    // Creates the pipe and both descriptions. On success the caller owns them.
    static ErrorOr<void> create_pair(FileDescription*& read_end, FileDescription*& write_end);

    ErrorOr<usize> read(u64 offset, void* buffer, usize length) override;
    ErrorOr<usize> write(u64 offset, void const* buffer, usize length) override;
    bool can_read_without_blocking() const override;
    bool can_write_without_blocking() const override;
    bool is_hung_up() const override;

    void on_description_opened(int flags) override;
    void on_description_closed(int flags) override;

    u64 size() const override { return m_pipe.used(); }

private:
    PipeInode()
        : Inode(nullptr, InodeType::Fifo, 0600)
    {
        // A pipe has no name, so nothing holds the directory reference an
        // ordinary inode starts with. Its two descriptions are its whole
        // lifetime.
        drop_initial_link_reference();
    }

    PipeBuffer m_pipe;
};

} // namespace kernel::fs
