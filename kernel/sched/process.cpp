// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- processes.

#include <kernel/arch/x86_64/percpu.h>
#include <kernel/dev/console.h>
#include <kernel/lib/new.h>
#include <kernel/lib/spinlock.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>
#include <kernel/panic.h>
#include <kernel/sched/process.h>
#include <kernel/sched/scheduler.h>

namespace kernel {

namespace {

IntrusiveList<Process, &Process::list_node> s_processes;
InterruptSpinLock s_process_lock;
pid_t s_next_pid = 1;

// Kernel threads need a Process to point at so that `ps` and the scheduler do
// not have to special-case them everywhere.
Process* s_kernel_process = nullptr;

} // namespace

void Process::initialize()
{
    auto* process = static_cast<Process*>(kzalloc(sizeof(Process)));
    if (process == nullptr)
        panic("could not allocate the kernel process");
    new (process) Process();

    process->m_pid = 0;
    process->m_parent_pid = 0;
    strncpy(process->m_name, "kernel", PROCESS_NAME_MAX - 1);
    process->m_address_space = &mm::AddressSpace::kernel_space();

    s_kernel_process = process;

    InterruptLockGuard guard(s_process_lock);
    s_processes.append(process);
}

Process& Process::kernel_process()
{
    VERIFY(s_kernel_process != nullptr);
    return *s_kernel_process;
}

Process* Process::current()
{
    auto* thread = Scheduler::current();
    if (thread == nullptr)
        return s_kernel_process;
    auto* process = thread->process();
    return process != nullptr ? process : s_kernel_process;
}

Process* Process::by_pid(pid_t pid)
{
    InterruptLockGuard guard(s_process_lock);
    for (Process* process : s_processes) {
        if (process->m_pid == pid)
            return process;
    }
    return nullptr;
}

ErrorOr<Process*> Process::create(char const* name, Process* parent)
{
    auto* process = static_cast<Process*>(kzalloc(sizeof(Process)));
    if (process == nullptr)
        return Error::from_errno(ENOMEM);
    new (process) Process();

    {
        InterruptLockGuard guard(s_process_lock);
        process->m_pid = s_next_pid++;
    }
    process->m_parent_pid = parent != nullptr ? parent->m_pid : 0;
    strncpy(process->m_name, name, PROCESS_NAME_MAX - 1);

    // Inherit the parent's working directory, or start at the root. Holding
    // a pointer to it means holding a reference: a directory can be removed
    // while a process is sitting in it.
    process->m_working_directory
        = parent != nullptr ? parent->m_working_directory : fs::root_inode();
    if (process->m_working_directory != nullptr)
        process->m_working_directory->ref();

    {
        InterruptLockGuard guard(s_process_lock);
        s_processes.append(process);
    }

    return process;
}

Process::~Process()
{
    close_all_descriptors();

    if (m_working_directory != nullptr) {
        m_working_directory->unref();
        m_working_directory = nullptr;
    }

    if (m_address_space != nullptr && m_address_space != &mm::AddressSpace::kernel_space()) {
        m_address_space->destroy_user_mappings();
        kfree(m_address_space);
    }
}

void Process::set_working_directory(fs::Inode* inode)
{
    // Reference the new one before releasing the old, in case they are the
    // same inode and the release would otherwise be the last one.
    if (inode != nullptr)
        inode->ref();
    if (m_working_directory != nullptr)
        m_working_directory->unref();
    m_working_directory = inode;
}

void Process::set_name(char const* name)
{
    strncpy(m_name, name, PROCESS_NAME_MAX - 1);
    m_name[PROCESS_NAME_MAX - 1] = '\0';
}

void Process::add_thread(Thread* thread)
{
    thread->set_process(this);
    if (m_main_thread == nullptr)
        m_main_thread = thread;
    ++m_thread_count;
}

// --- file descriptors ---------------------------------------------------

ErrorOr<int> Process::allocate_descriptor(fs::FileDescription* description, int lowest)
{
    if (lowest < 0)
        return Error::from_errno(EINVAL);

    // POSIX requires the lowest free descriptor at or above `lowest`, which
    // is what makes the shell's `exec 2>&1` dance work.
    for (int fd = lowest; fd < static_cast<int>(MAX_FILE_DESCRIPTORS); ++fd) {
        if (m_descriptors[fd].description == nullptr) {
            m_descriptors[fd] = { description, false };
            return fd;
        }
    }
    return Error::from_errno(EMFILE);
}

ErrorOr<fs::FileDescription*> Process::description_for(int fd) const
{
    if (fd < 0 || fd >= static_cast<int>(MAX_FILE_DESCRIPTORS))
        return Error::from_errno(EBADF);
    auto* description = m_descriptors[fd].description;
    if (description == nullptr)
        return Error::from_errno(EBADF);
    return description;
}

ErrorOr<void> Process::close_descriptor(int fd)
{
    auto* description = TRY(description_for(fd));
    m_descriptors[fd] = {};

    // Descriptions are shared by dup() and across fork(), so only the last
    // reference actually closes the file -- and only that close lets go of
    // the inode.
    fs::release_description(description);
    return {};
}

ErrorOr<int> Process::duplicate_descriptor(int fd, int to)
{
    auto* description = TRY(description_for(fd));

    if (to < 0) {
        // dup(): lowest free descriptor, and the description gains a reference
        // only once we know there was room for it.
        int const allocated = TRY(allocate_descriptor(description, 0));
        description->ref();
        return allocated;
    }

    if (to >= static_cast<int>(MAX_FILE_DESCRIPTORS))
        return Error::from_errno(EBADF);

    if (to == fd) {
        // dup2(fd, fd) is a no-op that must not close anything.
        return to;
    }

    if (m_descriptors[to].description != nullptr)
        (void)close_descriptor(to);

    description->ref();
    m_descriptors[to] = { description, false };
    return to;
}

ErrorOr<bool> Process::descriptor_close_on_exec(int fd) const
{
    TRY(description_for(fd));
    return m_descriptors[fd].close_on_exec;
}

ErrorOr<void> Process::set_descriptor_close_on_exec(int fd, bool close_on_exec)
{
    TRY(description_for(fd));
    m_descriptors[fd].close_on_exec = close_on_exec;
    return {};
}

void Process::close_all_descriptors()
{
    for (int fd = 0; fd < static_cast<int>(MAX_FILE_DESCRIPTORS); ++fd) {
        if (m_descriptors[fd].description != nullptr)
            (void)close_descriptor(fd);
    }
}

void Process::close_on_exec_descriptors()
{
    for (int fd = 0; fd < static_cast<int>(MAX_FILE_DESCRIPTORS); ++fd) {
        if (m_descriptors[fd].description != nullptr && m_descriptors[fd].close_on_exec)
            (void)close_descriptor(fd);
    }
}

usize Process::open_descriptor_count() const
{
    usize count = 0;
    for (auto const& entry : m_descriptors) {
        if (entry.description != nullptr)
            ++count;
    }
    return count;
}

// --- the break ----------------------------------------------------------

ErrorOr<u64> Process::set_brk(u64 address)
{
    // brk(0) is the conventional way to ask where the break currently is.
    if (address == 0)
        return m_brk_current;

    if (address < m_brk_start)
        return Error::from_errno(EINVAL);
    if (address >= USER_STACK_TOP - USER_STACK_SIZE)
        return Error::from_errno(ENOMEM);

    u64 const old_end = align_up<u64>(m_brk_current, PAGE_SIZE);
    u64 const new_end = align_up<u64>(address, PAGE_SIZE);

    if (new_end > old_end) {
        auto const flags = mm::PageFlags::Present | mm::PageFlags::Writable | mm::PageFlags::User
            | mm::PageFlags::NoExecute;
        TRY(m_address_space->map_anonymous(virt(old_end), new_end - old_end, flags));
    } else if (new_end < old_end) {
        m_address_space->unmap_range(virt(new_end), old_end - new_end);
    }

    m_brk_current = address;
    return m_brk_current;
}

// --- lifetime -----------------------------------------------------------

void Process::mark_exited(int status)
{
    m_has_exited = true;
    m_wait_status = status;

    // Children of a dead process are handed to init, so nothing is left
    // waiting on a parent that will never call wait().
    {
        InterruptLockGuard guard(s_process_lock);
        for (Process* other : s_processes) {
            if (other->m_parent_pid == m_pid)
                other->m_parent_pid = 1;
        }
    }

    close_all_descriptors();

    if (auto* parent = by_pid(m_parent_pid); parent != nullptr) {
        parent->raise_signal(SIGCHLD);
        parent->m_child_exit_queue.wake_all();
    }
}

ErrorOr<pid_t> Process::reap_child(pid_t wanted, int& status_out, bool blocking)
{
    for (;;) {
        bool any_children = false;
        Process* finished = nullptr;

        {
            InterruptLockGuard guard(s_process_lock);
            for (Process* child : s_processes) {
                if (child->m_parent_pid != m_pid || child == this)
                    continue;
                if (wanted > 0 && child->m_pid != wanted)
                    continue;
                any_children = true;
                if (child->m_has_exited) {
                    finished = child;
                    break;
                }
            }
        }

        if (finished != nullptr) {
            pid_t const pid = finished->m_pid;
            status_out = finished->m_wait_status;

            {
                InterruptLockGuard guard(s_process_lock);
                s_processes.remove(finished);
            }
            finished->~Process();
            kfree(finished);
            return pid;
        }

        if (!any_children)
            return Error::from_errno(ECHILD);
        if (!blocking)
            return Error::from_errno(EAGAIN);

        m_child_exit_queue.wait();
    }
}

// --- signals ------------------------------------------------------------

void Process::raise_signal(int signal)
{
    if (signal <= 0 || signal >= NSIG)
        return;
    __atomic_or_fetch(&m_pending_signals, 1ULL << signal, __ATOMIC_RELEASE);
}

int Process::take_pending_signal()
{
    u64 pending = __atomic_load_n(&m_pending_signals, __ATOMIC_ACQUIRE);
    if (pending == 0)
        return 0;

    // Lowest-numbered pending signal first, which is close enough to the
    // ordering POSIX leaves unspecified.
    int const signal = __builtin_ctzll(pending);
    __atomic_and_fetch(&m_pending_signals, ~(1ULL << signal), __ATOMIC_RELEASE);
    return signal;
}

void Process::set_signal_action(int signal, void* handler, void* restorer)
{
    if (signal <= 0 || signal >= NSIG)
        return;
    // SIGKILL and SIGSTOP cannot be caught, and pretending otherwise would
    // give a process a way to become unkillable.
    if (signal == SIGKILL || signal == SIGSTOP)
        return;
    m_signal_handlers[signal] = handler;
    m_signal_restorers[signal] = restorer;
}

void* Process::signal_disposition(int signal) const
{
    if (signal <= 0 || signal >= NSIG)
        return nullptr;
    return m_signal_handlers[signal];
}

void* Process::signal_restorer(int signal) const
{
    if (signal <= 0 || signal >= NSIG)
        return nullptr;
    return m_signal_restorers[signal];
}

void Process::reset_signal_handlers()
{
    // execve keeps ignored signals ignored but resets everything that had a
    // handler, because the handler's code no longer exists.
    for (int signal = 0; signal < NSIG; ++signal) {
        if (m_signal_handlers[signal] != SIG_IGN)
            m_signal_handlers[signal] = SIG_DFL;
        m_signal_restorers[signal] = nullptr;
    }
}

u64 Process::resident_bytes() const
{
    if (m_address_space == nullptr || m_address_space == &mm::AddressSpace::kernel_space())
        return 0;
    return m_address_space->resident_bytes();
}

void Process::for_each(void (*callback)(Process&, void*), void* context)
{
    InterruptLockGuard guard(s_process_lock);
    for (Process* process : s_processes)
        callback(*process, context);
}

usize Process::count()
{
    InterruptLockGuard guard(s_process_lock);
    return s_processes.size();
}

} // namespace kernel
