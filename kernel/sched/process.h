// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- processes.
//
// A process owns an address space, a file descriptor table and a working
// directory; a thread owns a stack and a register set. Today every process has
// exactly one thread, but the split is real so that adding threads later is
// not a rewrite.

#pragma once

#include <kernel/fs/vfs.h>
#include <kernel/lib/error.h>
#include <kernel/lib/intrusive_list.h>
#include <kernel/mm/address_space.h>
#include <kernel/sched/thread.h>
#include <kernel/sched/waitqueue.h>

#include <shitos/abi/signal.h>
#include <shitos/types.h>

namespace kernel {

using pid_t = i32;

inline constexpr usize MAX_FILE_DESCRIPTORS = 64;
inline constexpr usize PROCESS_NAME_MAX = 32;

// Where a user process is laid out. The heap starts after the executable and
// grows up; the stack starts near the top of the user half and grows down.
inline constexpr u64 USER_STACK_TOP = 0x00007FFFFFFFF000ULL;
inline constexpr usize USER_STACK_SIZE = 64 * 1024;
inline constexpr usize MAX_ARGUMENT_BYTES = 4096;
inline constexpr usize MAX_ARGUMENTS = 64;

struct FileDescriptorEntry {
    fs::FileDescription* description { nullptr };
    bool close_on_exec { false };
};

class Process {
public:
    static void initialize();

    // The kernel-side pseudo-process every kernel thread belongs to, so that
    // `ps` has something coherent to say about them.
    static Process& kernel_process();

    static ErrorOr<Process*> create(char const* name, Process* parent);
    static Process* current();
    static Process* by_pid(pid_t pid);

    ~Process();

    pid_t pid() const { return m_pid; }
    pid_t parent_pid() const { return m_parent_pid; }
    char const* name() const { return m_name; }
    void set_name(char const* name);

    mm::AddressSpace* address_space() const { return m_address_space; }
    void set_address_space(mm::AddressSpace* space) { m_address_space = space; }

    fs::Inode* working_directory() const { return m_working_directory; }
    void set_working_directory(fs::Inode* inode);

    // --- file descriptors ---
    ErrorOr<int> allocate_descriptor(fs::FileDescription* description, int lowest = 0);
    ErrorOr<fs::FileDescription*> description_for(int fd) const;
    ErrorOr<void> close_descriptor(int fd);
    ErrorOr<int> duplicate_descriptor(int fd, int to = -1);
    void close_all_descriptors();
    void close_on_exec_descriptors();
    usize open_descriptor_count() const;

    // FD_CLOEXEC lives on the descriptor, not the description: dup() gives you
    // a second descriptor onto the same open file, and only one of the two may
    // be marked to close.
    ErrorOr<bool> descriptor_close_on_exec(int fd) const;
    ErrorOr<void> set_descriptor_close_on_exec(int fd, bool close_on_exec);

    // --- the break, for brk(2) ---
    u64 brk() const { return m_brk_current; }
    void set_brk_start(u64 address) { m_brk_start = m_brk_current = address; }
    ErrorOr<u64> set_brk(u64 address);

    // --- lifetime ---
    bool has_exited() const { return m_has_exited; }

    // Already encoded the way waitpid(2) reports it: low byte the terminating
    // signal, next byte the exit status.
    int wait_status() const { return m_wait_status; }
    void mark_exited(int wait_status);

    // Reaps a finished child, or returns EAGAIN if one exists but is running,
    // or ECHILD if there are no children at all.
    ErrorOr<pid_t> reap_child(pid_t wanted, int& status_out, bool blocking);

    void add_thread(Thread* thread);
    Thread* main_thread() const { return m_main_thread; }
    usize thread_count() const { return m_thread_count; }

    // Signals. Only what a shell needs: a pending mask and default actions.
    void raise_signal(int signal);
    bool has_pending_signals() const { return m_pending_signals != 0; }
    int take_pending_signal();
    void set_signal_action(int signal, void* handler, void* restorer);
    void* signal_disposition(int signal) const;
    void* signal_restorer(int signal) const;
    void reset_signal_handlers();

    u64 resident_bytes() const;

    ListNode<Process> list_node;

    static void for_each(void (*callback)(Process&, void*), void* context);
    static usize count();

private:
    Process() = default;

    pid_t m_pid { 0 };
    pid_t m_parent_pid { 0 };
    char m_name[PROCESS_NAME_MAX] {};

    mm::AddressSpace* m_address_space { nullptr };
    fs::Inode* m_working_directory { nullptr };

    FileDescriptorEntry m_descriptors[MAX_FILE_DESCRIPTORS] {};

    u64 m_brk_start { 0 };
    u64 m_brk_current { 0 };

    Thread* m_main_thread { nullptr };
    usize m_thread_count { 0 };

    bool m_has_exited { false };
    int m_wait_status { 0 };

    // A parent blocks here; a child exiting wakes it.
    WaitQueue m_child_exit_queue;

    u64 m_pending_signals { 0 };
    void* m_signal_handlers[NSIG] {};
    void* m_signal_restorers[NSIG] {};

public:
    // Where the next anonymous mmap goes. A bump pointer: unmapping does not
    // reclaim address space, which is fine until something long-running maps
    // and unmaps in a loop.
    u64 mmap_next { 0x0000100000000000ULL };
};

} // namespace kernel
