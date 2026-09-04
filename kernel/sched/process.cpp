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

#include <shitos/abi/wait.h>

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

    // A child starts in its parent's job and its parent's session. The first
    // process has neither to inherit, so it leads both -- which makes init a
    // session leader without any special case for it elsewhere.
    process->m_pgid = parent != nullptr ? parent->m_pgid : process->m_pid;
    process->m_sid = parent != nullptr ? parent->m_sid : process->m_pid;

    // The signal mask and the file creation mask are both inherited across
    // fork and survive exec, which is what makes "block SIGCHLD, then fork"
    // mean anything.
    if (parent != nullptr) {
        process->m_signal_mask = parent->m_signal_mask;
        process->m_umask = parent->m_umask;
    }

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

ErrorOr<pid_t> Process::reap_child(pid_t wanted, int& status_out, bool blocking, int options)
{
    for (;;) {
        bool any_children = false;
        Process* finished = nullptr;
        Process* changed = nullptr;

        {
            InterruptLockGuard guard(s_process_lock);
            for (Process* child : s_processes) {
                if (child->m_parent_pid != m_pid || child == this)
                    continue;
                // wanted < -1 waits on a process group, -1 on any child, 0 on
                // the caller's own group: what a shell needs to wait for a job
                // rather than a process.
                if (wanted > 0 && child->m_pid != wanted)
                    continue;
                if (wanted == 0 && child->m_pgid != m_pgid)
                    continue;
                if (wanted < -1 && child->m_pgid != -wanted)
                    continue;
                any_children = true;
                if (child->m_has_exited) {
                    finished = child;
                    break;
                }
                // A stop or a continue is reported without reaping: the child
                // is still alive and will be waited for again.
                if (changed == nullptr) {
                    if ((options & WUNTRACED) != 0 && child->m_stop_pending)
                        changed = child;
                    else if ((options & WCONTINUED) != 0 && child->m_continue_pending)
                        changed = child;
                }
            }
        }

        if (changed != nullptr) {
            if (changed->m_stop_pending) {
                changed->m_stop_pending = false;
                status_out = W_STOPPED(changed->m_stop_signal);
            } else {
                changed->m_continue_pending = false;
                status_out = W_CONTINUED;
            }
            return changed->m_pid;
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

        // A signal has to be able to break this, or ^C cannot interrupt a
        // shell that is waiting on a job. Callers that should not notice --
        // an ignored signal, or a stop and continue -- get the call restarted
        // for them on the way out of the kernel.
        if (has_pending_signals())
            return Error::from_errno(EINTR);
    }
}

// --- process groups and sessions ----------------------------------------

ErrorOr<void> Process::set_process_group(pid_t pid, pid_t pgid)
{
    auto* caller = Process::current();
    if (caller == nullptr)
        return Error::from_errno(ESRCH);

    if (pid < 0 || pgid < 0)
        return Error::from_errno(EINVAL);

    Process* target = pid == 0 ? caller : Process::by_pid(pid);
    if (target == nullptr)
        return Error::from_errno(ESRCH);

    // Only self or a child, and only within the caller's own session. Without
    // that a process could move an unrelated job into its own group and steal
    // the terminal from it.
    if (target != caller && target->m_parent_pid != caller->m_pid)
        return Error::from_errno(ESRCH);
    if (target->m_sid != caller->m_sid)
        return Error::from_errno(EPERM);

    // A session leader has no group to move to: its group is the session.
    if (target->m_pid == target->m_sid)
        return Error::from_errno(EPERM);

    pid_t const wanted = pgid == 0 ? target->m_pid : pgid;

    // The group must already exist in this session, or be created by the
    // process that will lead it. Anything else would put a process in a group
    // that nothing can name.
    if (wanted != target->m_pid) {
        InterruptLockGuard guard(s_process_lock);
        bool found = false;
        for (Process* process : s_processes) {
            if (process->m_pgid == wanted && process->m_sid == caller->m_sid) {
                found = true;
                break;
            }
        }
        if (!found)
            return Error::from_errno(EPERM);
    }

    target->m_pgid = wanted;
    return {};
}

ErrorOr<pid_t> Process::start_session()
{
    // A group leader cannot start a session: its group would end up split
    // across two sessions, which is the one thing the hierarchy forbids.
    if (m_pid == m_pgid)
        return Error::from_errno(EPERM);

    m_sid = m_pid;
    m_pgid = m_pid;
    // A new session has no controlling terminal. Nothing tracks one per
    // session yet, so this is where that would be dropped.
    return m_sid;
}

usize Process::for_each_in_group(pid_t pgid, void (*callback)(Process&, void*), void* context)
{
    // Collect first, then call: the callback may signal a process, and
    // signalling can stop or wake one, which must not happen under the lock.
    static constexpr usize MAX_GROUP = 64;
    Process* members[MAX_GROUP];
    usize count = 0;

    {
        InterruptLockGuard guard(s_process_lock);
        for (Process* process : s_processes) {
            if (process->m_pgid != pgid || process->m_has_exited)
                continue;
            if (count < MAX_GROUP)
                members[count++] = process;
        }
    }

    for (usize i = 0; i < count; ++i)
        callback(*members[i], context);
    return count;
}

bool Process::group_exists(pid_t pgid)
{
    InterruptLockGuard guard(s_process_lock);
    for (Process* process : s_processes) {
        if (process->m_pgid == pgid && !process->m_has_exited)
            return true;
    }
    return false;
}

void Process::stop(int signal)
{
    m_is_stopped = true;
    m_stop_signal = signal;
    m_stop_pending = true;
    m_continue_pending = false;

    // The parent hears about it the same way it hears about an exit, so a
    // shell blocked in waitpid wakes up rather than hanging until the job is
    // continued by something else.
    if (auto* parent = Process::by_pid(m_parent_pid); parent != nullptr) {
        parent->raise_signal(SIGCHLD);
        parent->m_child_exit_queue.wake_all();
    }

    // Does not return until resume() puts the thread back on the run queue.
    if (m_main_thread != nullptr && m_main_thread == Scheduler::current())
        Scheduler::block_current(ThreadState::Stopped);
}

void Process::resume()
{
    if (!m_is_stopped)
        return;

    m_is_stopped = false;
    m_stop_signal = 0;
    m_stop_pending = false;
    m_continue_pending = true;

    if (auto* parent = Process::by_pid(m_parent_pid); parent != nullptr) {
        parent->raise_signal(SIGCHLD);
        parent->m_child_exit_queue.wake_all();
    }

    if (m_main_thread != nullptr)
        Scheduler::unblock(m_main_thread);
}

// --- signals ------------------------------------------------------------

// The four signals whose default action is to suspend.
static constexpr u64 STOP_SIGNAL_MASK
    = (1ULL << SIGSTOP) | (1ULL << SIGTSTP) | (1ULL << SIGTTIN) | (1ULL << SIGTTOU);

void Process::raise_signal(int signal)
{
    if (signal <= 0 || signal >= NSIG)
        return;

    // Stopping and continuing cancel each other. A process that was sent
    // SIGTSTP and then SIGCONT before either was delivered must end up
    // running, not stopped and then confused about why.
    if (signal == SIGCONT)
        __atomic_and_fetch(&m_pending_signals, ~STOP_SIGNAL_MASK, __ATOMIC_RELEASE);
    else if ((STOP_SIGNAL_MASK & (1ULL << signal)) != 0)
        __atomic_and_fetch(&m_pending_signals, ~(1ULL << SIGCONT), __ATOMIC_RELEASE);

    __atomic_or_fetch(&m_pending_signals, 1ULL << signal, __ATOMIC_RELEASE);

    // Two signals cannot wait to be delivered, because a stopped process is
    // not running to deliver anything. Continuing has to happen here, and so
    // does the one signal that must reach a process no matter what state it
    // is in.
    //
    // Delivery still happens afterwards for SIGCONT: the bit stays set so a
    // caught handler runs once the process is going again.
    if (m_is_stopped && (signal == SIGCONT || signal == SIGKILL))
        resume();
}

// Neither can be blocked. A process that could block SIGKILL would be
// unkillable, and one that could block SIGSTOP could not be suspended.
static constexpr u64 UNBLOCKABLE_SIGNALS = (1ULL << SIGKILL) | (1ULL << SIGSTOP);

bool Process::has_pending_signals() const
{
    u64 const pending = __atomic_load_n(&m_pending_signals, __ATOMIC_ACQUIRE);
    u64 const blocked = __atomic_load_n(&m_signal_mask, __ATOMIC_ACQUIRE) & ~UNBLOCKABLE_SIGNALS;
    return (pending & ~blocked) != 0;
}

int Process::take_pending_signal()
{
    u64 const pending = __atomic_load_n(&m_pending_signals, __ATOMIC_ACQUIRE);
    u64 const blocked = __atomic_load_n(&m_signal_mask, __ATOMIC_ACQUIRE) & ~UNBLOCKABLE_SIGNALS;
    u64 const deliverable = pending & ~blocked;
    if (deliverable == 0)
        return 0;

    // Lowest-numbered deliverable signal first, which is close enough to the
    // ordering POSIX leaves unspecified. A blocked one stays pending.
    int const signal = __builtin_ctzll(deliverable);
    __atomic_and_fetch(&m_pending_signals, ~(1ULL << signal), __ATOMIC_RELEASE);
    return signal;
}

u64 Process::set_signal_mask(int how, u64 wanted)
{
    u64 const previous = __atomic_load_n(&m_signal_mask, __ATOMIC_ACQUIRE);
    u64 next = previous;

    switch (how) {
    case SIG_BLOCK: next = previous | wanted; break;
    case SIG_UNBLOCK: next = previous & ~wanted; break;
    case SIG_SETMASK: next = wanted; break;
    default: return previous;
    }

    __atomic_store_n(&m_signal_mask, next & ~UNBLOCKABLE_SIGNALS, __ATOMIC_RELEASE);
    return previous;
}

u32 Process::set_umask(u32 mask)
{
    u32 const previous = m_umask;
    m_umask = mask & 07777;
    return previous;
}

void Process::set_signal_action(int signal, void* handler, void* restorer, int flags)
{
    if (signal <= 0 || signal >= NSIG)
        return;
    // SIGKILL and SIGSTOP cannot be caught, and pretending otherwise would
    // give a process a way to become unkillable.
    if (signal == SIGKILL || signal == SIGSTOP)
        return;
    m_signal_handlers[signal] = handler;
    m_signal_restorers[signal] = restorer;
    m_signal_flags[signal] = flags;
}

int Process::signal_flags(int signal) const
{
    if (signal <= 0 || signal >= NSIG)
        return 0;
    return m_signal_flags[signal];
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
