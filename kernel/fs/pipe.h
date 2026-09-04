// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- pipes.
//
// A pipe is an inode with a ring buffer and no name. Two FileDescriptions
// point at it, one readable and one writable, and the interesting behaviour is
// entirely in what happens when one end goes away: a read with no writers left
// is end of file, and a write with no readers left is EPIPE.

#pragma once

#include <kernel/fs/vfs.h>
#include <kernel/sched/waitqueue.h>

namespace kernel::fs {

inline constexpr usize PIPE_CAPACITY = 4096;

class PipeInode final : public Inode {
public:
    // Creates the pipe and both descriptions. On success the caller owns them.
    static ErrorOr<void> create_pair(FileDescription*& read_end, FileDescription*& write_end);

    ErrorOr<usize> read(u64 offset, void* buffer, usize length) override;
    ErrorOr<usize> write(u64 offset, void const* buffer, usize length) override;
    bool can_read_without_blocking() const override;

    void on_description_opened(int flags) override;
    void on_description_closed(int flags) override;

    u64 size() const override { return used(); }

private:
    PipeInode()
        : Inode(nullptr, InodeType::Fifo, 0600)
    {
    }

    usize used() const;
    usize available() const;

    u8 m_buffer[PIPE_CAPACITY] {};
    usize m_head { 0 };
    usize m_tail { 0 };

    usize m_readers { 0 };
    usize m_writers { 0 };

    WaitQueue m_read_queue;
    WaitQueue m_write_queue;
};

} // namespace kernel::fs
