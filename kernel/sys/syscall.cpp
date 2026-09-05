// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- system call dispatch.
//
// One function per call, a table to find it, and a hard rule: nothing here
// dereferences a userspace pointer directly. Every one goes through
// copy_from_user or copy_to_user, which check it against the calling process's
// own page tables first. A bad pointer from ring 3 must be EFAULT, never a
// kernel fault.

#include <kernel/arch/x86_64/cpu.h>
#include <kernel/arch/x86_64/gdt.h>
#include <kernel/arch/x86_64/io.h>
#include <kernel/arch/x86_64/percpu.h>
#include <kernel/dev/console.h>
#include <kernel/dev/pty.h>
#include <kernel/dev/tty.h>
#include <kernel/fs/pipe.h>
#include <kernel/fs/vfs.h>
#include <kernel/lib/new.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/physical.h>
#include <kernel/module/loader.h>
#include <kernel/panic.h>
#include <kernel/sched/process.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sys/clock.h>
#include <kernel/sys/elf_loader.h>
#include <kernel/sys/syscall.h>

#include <shitos/abi/fcntl.h>
#include <shitos/abi/ioctl.h>
#include <shitos/abi/mman.h>
#include <shitos/abi/poll.h>
#include <shitos/abi/signal.h>
#include <shitos/abi/syscall.h>
#include <shitos/abi/time.h>
#include <shitos/abi/utsname.h>
#include <shitos/abi/wait.h>

extern "C" void syscall_entry();

namespace kernel::sys {

namespace {

constexpr u32 MSR_EFER = 0xC0000080;
constexpr u32 MSR_STAR = 0xC0000081;
constexpr u32 MSR_LSTAR = 0xC0000082;
constexpr u32 MSR_SFMASK = 0xC0000084;

u64 s_syscall_count = 0;

// rflags bits cleared on entry to the kernel. Clearing IF means a syscall
// starts with interrupts off; clearing DF means string instructions in the
// kernel behave regardless of what userspace left set.
constexpr u64 SFMASK_BITS = (1 << 9) | (1 << 10) | (1 << 8) | (1 << 18);

struct SignalContext {
    InterruptFrame frame;
    u64 signal;
};

} // namespace

void syscall_initialize()
{
    if (!arch::cpu_features().syscall)
        panic("this CPU has no syscall instruction, which we are not prepared for");

    // STAR: kernel selectors in [47:32], the base sysret derives user
    // selectors from in [63:48]. The GDT layout in gdt.h exists to satisfy this.
    u64 const star = (static_cast<u64>(arch::STAR_USER_BASE) << 48)
        | (static_cast<u64>(arch::SELECTOR_KERNEL_CODE) << 32);
    arch::write_msr(MSR_STAR, star);
    arch::write_msr(MSR_LSTAR, reinterpret_cast<u64>(&syscall_entry));
    arch::write_msr(MSR_SFMASK, SFMASK_BITS);

    // EFER.SCE was set in cpu_initialize; without it `syscall` faults.
    arch::write_msr(MSR_EFER, arch::read_msr(MSR_EFER) | 1);

    klog(LOG_INFO, "syscall", "gate installed, %d POSIX calls + %d extensions", SYS_MAX_POSIX,
        SYS_MAX_EXT);
}

u64 syscall_count()
{
    return __atomic_load_n(&s_syscall_count, __ATOMIC_RELAXED);
}

// --- user memory --------------------------------------------------------

ErrorOr<void> copy_from_user(void* destination, u64 user_source, usize length)
{
    if (length == 0)
        return {};

    auto* process = Process::current();
    if (process == nullptr || process->address_space() == nullptr)
        return Error::from_errno(EFAULT);
    if (!process->address_space()->validate_user_range(virt(user_source), length, false))
        return Error::from_errno(EFAULT);

    // The process's address space is the active one, so a plain copy is fine
    // once the range has been checked.
    memcpy(destination, reinterpret_cast<void const*>(user_source), length);
    return {};
}

ErrorOr<void> copy_to_user(u64 user_destination, void const* source, usize length)
{
    if (length == 0)
        return {};

    auto* process = Process::current();
    if (process == nullptr || process->address_space() == nullptr)
        return Error::from_errno(EFAULT);
    if (!process->address_space()->validate_user_range(virt(user_destination), length, true))
        return Error::from_errno(EFAULT);

    memcpy(reinterpret_cast<void*>(user_destination), source, length);
    return {};
}

ErrorOr<usize> copy_string_from_user(char* destination, u64 user_source, usize capacity)
{
    if (capacity == 0)
        return Error::from_errno(EINVAL);

    auto* process = Process::current();
    if (process == nullptr || process->address_space() == nullptr)
        return Error::from_errno(EFAULT);

    // Validate a page at a time rather than assuming a length: the string's
    // end is not known until it is found, and it must not be looked for in
    // memory the process cannot read.
    usize length = 0;
    while (length + 1 < capacity) {
        u64 const address = user_source + length;
        if ((address & (PAGE_SIZE - 1)) == 0 || length == 0) {
            usize const to_page_end = PAGE_SIZE - (address & (PAGE_SIZE - 1));
            if (!process->address_space()->validate_user_range(virt(address), to_page_end, false))
                return Error::from_errno(EFAULT);
        }
        char const c = *reinterpret_cast<char const*>(address);
        destination[length] = c;
        if (c == '\0')
            return length;
        ++length;
    }

    destination[capacity - 1] = '\0';
    return Error::from_errno(ENAMETOOLONG);
}

namespace {

// --- helpers ------------------------------------------------------------

ErrorOr<fs::FileDescription*> description_for(int fd)
{
    auto* process = Process::current();
    if (process == nullptr)
        return Error::from_errno(ESRCH);
    return process->description_for(fd);
}

// Reads a NULL-terminated array of user string pointers into kernel memory.
// The caller owns the result and frees it with free_string_array.
ErrorOr<char**> copy_string_array_from_user(u64 user_array)
{
    if (user_array == 0) {
        auto** empty = static_cast<char**>(kzalloc(sizeof(char*)));
        if (empty == nullptr)
            return Error::from_errno(ENOMEM);
        return empty;
    }

    u64 pointers[MAX_ARGUMENTS + 1];
    usize count = 0;
    for (; count < MAX_ARGUMENTS; ++count) {
        TRY(copy_from_user(&pointers[count], user_array + count * sizeof(u64), sizeof(u64)));
        if (pointers[count] == 0)
            break;
    }
    if (count == MAX_ARGUMENTS)
        return Error::from_errno(E2BIG);

    auto** result = static_cast<char**>(kzalloc((count + 1) * sizeof(char*)));
    if (result == nullptr)
        return Error::from_errno(ENOMEM);

    for (usize i = 0; i < count; ++i) {
        auto* buffer = static_cast<char*>(kmalloc(fs::PATH_MAX_LENGTH));
        if (buffer == nullptr) {
            for (usize j = 0; j < i; ++j)
                kfree(result[j]);
            kfree(result);
            return Error::from_errno(ENOMEM);
        }
        auto copied = copy_string_from_user(buffer, pointers[i], fs::PATH_MAX_LENGTH);
        if (copied.is_error()) {
            kfree(buffer);
            for (usize j = 0; j < i; ++j)
                kfree(result[j]);
            kfree(result);
            return copied.error();
        }
        result[i] = buffer;
    }

    result[count] = nullptr;
    return result;
}

void free_string_array(char** array)
{
    if (array == nullptr)
        return;
    for (usize i = 0; array[i] != nullptr; ++i)
        kfree(array[i]);
    kfree(array);
}

// --- process lifetime ---------------------------------------------------

} // namespace

[[noreturn]] void do_exit(int wait_status)
{
    auto* process = Process::current();
    if (process != nullptr && process->pid() != 0)
        process->mark_exited(wait_status);
    Scheduler::exit_current();
}

namespace {

// --- the calls ----------------------------------------------------------

ErrorOr<u64> sys_exit(InterruptFrame&, u64 status, u64, u64, u64, u64, u64)
{
    do_exit(static_cast<int>((status & 0xFF) << 8));
}

ErrorOr<u64> sys_read(InterruptFrame&, u64 fd, u64 buffer, u64 length, u64, u64, u64)
{
    auto* description = TRY(description_for(static_cast<int>(fd)));

    usize const to_read = min<usize>(length, 64 * 1024);
    if (to_read == 0)
        return static_cast<u64>(0);

    auto* scratch = static_cast<u8*>(kmalloc(to_read));
    if (scratch == nullptr)
        return Error::from_errno(ENOMEM);

    auto read = description->read(scratch, to_read);
    if (read.is_error()) {
        kfree(scratch);
        return read.error();
    }

    auto copied = copy_to_user(buffer, scratch, read.value());
    usize const count = read.value();
    kfree(scratch);
    TRY(copied);

    return static_cast<u64>(count);
}

ErrorOr<u64> sys_write(InterruptFrame&, u64 fd, u64 buffer, u64 length, u64, u64, u64)
{
    auto* description = TRY(description_for(static_cast<int>(fd)));

    usize const to_write = min<usize>(length, 64 * 1024);
    if (to_write == 0)
        return static_cast<u64>(0);

    auto* scratch = static_cast<u8*>(kmalloc(to_write));
    if (scratch == nullptr)
        return Error::from_errno(ENOMEM);

    if (auto copied = copy_from_user(scratch, buffer, to_write); copied.is_error()) {
        kfree(scratch);
        return copied.error();
    }

    auto written = description->write(scratch, to_write);
    kfree(scratch);
    return static_cast<u64>(TRY(written));
}

ErrorOr<u64> sys_open(InterruptFrame&, u64 path_pointer, u64 flags, u64 mode, u64, u64, u64)
{
    char path[fs::PATH_MAX_LENGTH];
    TRY(copy_string_from_user(path, path_pointer, sizeof(path)));

    auto* process = Process::current();
    // The file creation mask clears the bits the caller asked for but the
    // process has said it does not want given away.
    auto const permissions = static_cast<u32>(mode) & ~process->umask();
    auto* description
        = TRY(fs::open(path, static_cast<int>(flags), permissions, process->working_directory()));

    auto fd = process->allocate_descriptor(description);
    if (fd.is_error()) {
        fs::release_description(description);
        return fd.error();
    }

    // O_CLOEXEC is the whole reason the flag exists: marking it afterwards
    // with fcntl leaves a window where a fork inherits the descriptor.
    if ((flags & O_CLOEXEC) != 0)
        (void)process->set_descriptor_close_on_exec(fd.value(), true);

    return static_cast<u64>(fd.value());
}

ErrorOr<u64> sys_close(InterruptFrame&, u64 fd, u64, u64, u64, u64, u64)
{
    TRY(Process::current()->close_descriptor(static_cast<int>(fd)));
    return static_cast<u64>(0);
}

ErrorOr<u64> sys_lseek(InterruptFrame&, u64 fd, u64 offset, u64 whence, u64, u64, u64)
{
    auto* description = TRY(description_for(static_cast<int>(fd)));
    return TRY(description->seek(static_cast<i64>(offset), static_cast<int>(whence)));
}

ErrorOr<u64> sys_fork(InterruptFrame& frame, u64, u64, u64, u64, u64, u64)
{
    auto* parent = Process::current();
    if (parent == nullptr || parent->address_space() == nullptr)
        return Error::from_errno(EINVAL);

    auto* child = TRY(Process::create(parent->name(), parent));

    auto space = parent->address_space()->clone_user_space();
    if (space.is_error())
        return space.error();
    child->set_address_space(space.value());
    child->set_working_directory(parent->working_directory());
    child->mmap_next = parent->mmap_next;

    // Descriptors are shared, not copied: the child sees the parent's file
    // offsets move and vice versa, which is what POSIX specifies.
    //
    // FD_CLOEXEC is the exception. It belongs to the descriptor rather than the
    // open file, and fork duplicates the descriptor -- so the flag comes with
    // it. Only exec clears it, which is the point: the usual pattern is to set
    // it, fork, and let exec do the closing.
    for (int fd = 0; fd < static_cast<int>(MAX_FILE_DESCRIPTORS); ++fd) {
        auto description = parent->description_for(fd);
        if (description.is_error())
            continue;
        description.value()->ref();
        (void)child->allocate_descriptor(description.value(), fd);
        if (auto inherited = parent->descriptor_close_on_exec(fd); !inherited.is_error())
            (void)child->set_descriptor_close_on_exec(fd, inherited.value());
    }

    auto thread = Thread::create_from_frame(parent->name(), frame);
    if (thread.is_error())
        return thread.error();

    child->add_thread(thread.value());
    child->set_brk_start(parent->brk());
    Scheduler::enqueue(thread.value());

    return static_cast<u64>(child->pid());
}

ErrorOr<u64> sys_execve(
    InterruptFrame& frame, u64 path_pointer, u64 argv_pointer, u64 envp_pointer, u64, u64, u64)
{
    char path[fs::PATH_MAX_LENGTH];
    TRY(copy_string_from_user(path, path_pointer, sizeof(path)));

    auto* process = Process::current();
    auto* inode = TRY(fs::resolve(path, process->working_directory()));
    if (inode->is_directory())
        return Error::from_errno(EACCES);

    // Copy the arguments out of the old address space before it is destroyed.
    char** argv = TRY(copy_string_array_from_user(argv_pointer));
    auto envp_or_error = copy_string_array_from_user(envp_pointer);
    if (envp_or_error.is_error()) {
        free_string_array(argv);
        return envp_or_error.error();
    }
    char** envp = envp_or_error.value();

    auto space_or_error = mm::AddressSpace::create_user_space();
    if (space_or_error.is_error()) {
        free_string_array(argv);
        free_string_array(envp);
        return space_or_error.error();
    }
    auto* space = space_or_error.value();

    auto executable = load_executable(*inode, *space);
    if (executable.is_error()) {
        space->destroy_user_mappings();
        kfree(space);
        free_string_array(argv);
        free_string_array(envp);
        return executable.error();
    }

    auto stack = setup_user_stack(*space, executable.value(), argv, envp);
    free_string_array(argv);
    free_string_array(envp);
    if (stack.is_error()) {
        space->destroy_user_mappings();
        kfree(space);
        return stack.error();
    }

    // Past this point the old image is gone and there is no going back, so
    // everything that could fail has already happened.
    auto* old_space = process->address_space();
    process->set_address_space(space);
    space->activate();

    if (old_space != nullptr && old_space != &mm::AddressSpace::kernel_space()) {
        old_space->destroy_user_mappings();
        kfree(old_space);
    }

    process->close_on_exec_descriptors();
    process->reset_signal_handlers();
    process->set_brk_start(executable.value().brk_start);

    char const* base_name = path;
    if (auto const* slash = strrchr(path, '/'); slash != nullptr)
        base_name = slash + 1;
    process->set_name(base_name);

    // Rewrite this thread's frame so the return from the syscall lands in the
    // new program instead of after the execve call that no longer exists.
    memset(&frame, 0, sizeof(InterruptFrame));
    frame.rip = executable.value().entry;
    frame.rsp = stack.value();
    frame.cs = arch::SELECTOR_USER_CODE;
    frame.ss = arch::SELECTOR_USER_DATA;
    frame.rflags = 0x202;
    frame.vector = 0x100;

    return static_cast<u64>(0);
}

ErrorOr<u64> sys_waitpid(InterruptFrame&, u64 pid, u64 status_pointer, u64 options, u64, u64, u64)
{
    auto* process = Process::current();
    int status = 0;
    auto reaped = process->reap_child(
        static_cast<pid_t>(pid), status, (options & WNOHANG) == 0, static_cast<int>(options));

    if (reaped.is_error()) {
        // WNOHANG with a live child is "nothing to report", not an error.
        if (reaped.error().code() == EAGAIN && (options & WNOHANG) != 0)
            return static_cast<u64>(0);
        return reaped.error();
    }

    if (status_pointer != 0)
        TRY(copy_to_user(status_pointer, &status, sizeof(status)));

    return static_cast<u64>(reaped.value());
}

ErrorOr<u64> sys_getpid(InterruptFrame&, u64, u64, u64, u64, u64, u64)
{
    return static_cast<u64>(Process::current()->pid());
}

ErrorOr<u64> sys_getppid(InterruptFrame&, u64, u64, u64, u64, u64, u64)
{
    return static_cast<u64>(Process::current()->parent_pid());
}

ErrorOr<u64> sys_brk(InterruptFrame&, u64 address, u64, u64, u64, u64, u64)
{
    return TRY(Process::current()->set_brk(address));
}

ErrorOr<u64> sys_mmap(
    InterruptFrame&, u64 address, u64 length, u64 protection, u64 flags, u64 fd, u64 offset)
{
    if (length == 0)
        return Error::from_errno(EINVAL);

    bool const anonymous = (flags & MAP_ANONYMOUS) != 0;
    bool const shared = (flags & MAP_SHARED) != 0;
    if (shared == ((flags & MAP_PRIVATE) != 0))
        return Error::from_errno(EINVAL); // exactly one of the two, says POSIX

    auto* process = Process::current();
    usize const rounded = align_up<usize>(length, PAGE_SIZE);

    u64 target = 0;
    if ((flags & MAP_FIXED) != 0) {
        if ((address & (PAGE_SIZE - 1)) != 0)
            return Error::from_errno(EINVAL);
        target = address;
    } else {
        target = process->mmap_next;
        process->mmap_next += rounded + PAGE_SIZE; // a guard page between mappings
    }

    auto page_flags = mm::PageFlags::Present | mm::PageFlags::User;
    if ((protection & PROT_WRITE) != 0)
        page_flags = page_flags | mm::PageFlags::Writable;
    if ((protection & PROT_EXEC) == 0)
        page_flags = page_flags | mm::PageFlags::NoExecute;

    if (anonymous) {
        TRY(process->address_space()->map_anonymous(virt(target), rounded, page_flags));
        return target;
    }

    // --- file-backed ------------------------------------------------------

    auto* description = TRY(description_for(static_cast<int>(fd)));
    if (!description->is_readable())
        return Error::from_errno(EACCES);
    if (shared && (protection & PROT_WRITE) != 0 && !description->is_writable())
        return Error::from_errno(EACCES);

    if ((offset & (PAGE_SIZE - 1)) != 0)
        return Error::from_errno(EINVAL);

    auto& inode = description->inode();

    /*
     * A private file mapping is a copy that happens to start out looking like
     * the file, so it is anonymous memory filled in by reading. Eager, like
     * fork is -- the fault handler has nowhere to record what a lazy page
     * would need to fetch.
     */
    if (!shared) {
        /*
         * Writable while it is being filled in, whatever the caller asked for.
         * A read-only mapping cannot be written to, and the contents have to
         * get in somehow -- mapping it PROT_READ and then copying into it
         * fails, which is exactly what happened the first time: a font opened
         * for reading mapped successfully and arrived full of zeroes.
         */
        auto fill_flags = page_flags | mm::PageFlags::Writable;
        TRY(process->address_space()->map_anonymous(virt(target), rounded, fill_flags));

        // One page-sized bounce buffer for the whole loop. On the stack it
        // would be a quarter of this thread's kernel stack.
        auto* bounce = static_cast<u8*>(kmalloc(PAGE_SIZE));
        if (bounce == nullptr) {
            process->address_space()->unmap_range(virt(target), rounded);
            return Error::from_errno(ENOMEM);
        }

        for (usize done = 0; done < rounded; done += PAGE_SIZE) {
            auto read = inode.read(offset + done, bounce, PAGE_SIZE);
            usize const got = read.is_error() ? 0 : read.value();
            if (got < PAGE_SIZE)
                memset(bounce + got, 0, PAGE_SIZE - got);

            if (auto copied = copy_to_user(target + done, bounce, PAGE_SIZE); copied.is_error()) {
                kfree(bounce);
                process->address_space()->unmap_range(virt(target), rounded);
                return copied.error();
            }
        }
        kfree(bounce);

        // Now down to what was actually asked for.
        if ((protection & PROT_WRITE) == 0)
            TRY(process->address_space()->protect(virt(target), rounded, page_flags));

        return target;
    }

    /*
     * A shared mapping points straight at the inode's own pages, so a write
     * through it is a write to the file and every other mapper sees it. Only
     * an inode that owns whole pages can answer -- tmpfs and the framebuffer
     * do, and everything else reports ENODEV.
     *
     * Foreign marks each entry so that neither process teardown nor fork
     * treats these frames as the process's own to free or to copy.
     */
    page_flags = page_flags | mm::PageFlags::Foreign;

    for (usize done = 0; done < rounded; done += PAGE_SIZE) {
        auto page = inode.physical_page(offset + done, (protection & PROT_WRITE) != 0);
        if (page.is_error()) {
            process->address_space()->unmap_range(virt(target), done);
            return page.error();
        }
        if (auto mapped
            = process->address_space()->map(virt(target + done), page.value(), page_flags);
            mapped.is_error()) {
            process->address_space()->unmap_range(virt(target), done);
            return mapped.error();
        }
    }

    return target;
}

ErrorOr<u64> sys_munmap(InterruptFrame&, u64 address, u64 length, u64, u64, u64, u64)
{
    if (length == 0 || (address & (PAGE_SIZE - 1)) != 0)
        return Error::from_errno(EINVAL);
    Process::current()->address_space()->unmap_range(
        virt(address), align_up<usize>(length, PAGE_SIZE));
    return static_cast<u64>(0);
}

ErrorOr<u64> sys_dup(InterruptFrame&, u64 fd, u64, u64, u64, u64, u64)
{
    return static_cast<u64>(TRY(Process::current()->duplicate_descriptor(static_cast<int>(fd))));
}

ErrorOr<u64> sys_dup2(InterruptFrame&, u64 fd, u64 to, u64, u64, u64, u64)
{
    return static_cast<u64>(
        TRY(Process::current()->duplicate_descriptor(static_cast<int>(fd), static_cast<int>(to))));
}

ErrorOr<u64> sys_pipe(InterruptFrame&, u64 fds_pointer, u64, u64, u64, u64, u64)
{
    fs::FileDescription* read_end = nullptr;
    fs::FileDescription* write_end = nullptr;
    TRY(fs::PipeInode::create_pair(read_end, write_end));

    auto* process = Process::current();
    auto read_fd = process->allocate_descriptor(read_end);
    if (read_fd.is_error()) {
        fs::release_description(read_end);
        fs::release_description(write_end);
        return read_fd.error();
    }
    auto write_fd = process->allocate_descriptor(write_end);
    if (write_fd.is_error()) {
        (void)process->close_descriptor(read_fd.value());
        fs::release_description(write_end);
        return write_fd.error();
    }

    // Handing the numbers back can still fail, and a process that never
    // learns its fds cannot close them. Undo the whole call rather than
    // leaking a pipe into its table.
    int const fds[2] = { read_fd.value(), write_fd.value() };
    if (auto copied = copy_to_user(fds_pointer, fds, sizeof(fds)); copied.is_error()) {
        (void)process->close_descriptor(read_fd.value());
        (void)process->close_descriptor(write_fd.value());
        return copied.error();
    }
    return static_cast<u64>(0);
}

ErrorOr<u64> sys_stat(InterruptFrame&, u64 path_pointer, u64 stat_pointer, u64, u64, u64, u64)
{
    char path[fs::PATH_MAX_LENGTH];
    TRY(copy_string_from_user(path, path_pointer, sizeof(path)));

    auto* inode = TRY(fs::resolve(path, Process::current()->working_directory()));
    struct stat status;
    TRY(inode->stat(status));
    TRY(copy_to_user(stat_pointer, &status, sizeof(status)));
    return static_cast<u64>(0);
}

ErrorOr<u64> sys_fstat(InterruptFrame&, u64 fd, u64 stat_pointer, u64, u64, u64, u64)
{
    auto* description = TRY(description_for(static_cast<int>(fd)));
    struct stat status;
    TRY(description->inode().stat(status));
    TRY(copy_to_user(stat_pointer, &status, sizeof(status)));
    return static_cast<u64>(0);
}

ErrorOr<u64> sys_getdents(InterruptFrame&, u64 fd, u64 buffer, u64 length, u64, u64, u64)
{
    auto* description = TRY(description_for(static_cast<int>(fd)));

    usize const to_read = min<usize>(length, 16 * 1024);
    auto* scratch = static_cast<u8*>(kmalloc(to_read));
    if (scratch == nullptr)
        return Error::from_errno(ENOMEM);

    auto written = description->get_directory_entries(scratch, to_read);
    if (written.is_error()) {
        kfree(scratch);
        return written.error();
    }

    auto copied = copy_to_user(buffer, scratch, written.value());
    usize const count = written.value();
    kfree(scratch);
    TRY(copied);

    return static_cast<u64>(count);
}

ErrorOr<u64> sys_mkdir(InterruptFrame&, u64 path_pointer, u64 mode, u64, u64, u64, u64)
{
    char path[fs::PATH_MAX_LENGTH];
    TRY(copy_string_from_user(path, path_pointer, sizeof(path)));

    auto* process = Process::current();

    // Existence wins over everything else: mkdir on something that is already
    // there is EEXIST even when it lives on a read-only mount, which is what
    // makes `mkdir -p` work across a mount point like /tmp.
    if (!fs::resolve(path, process->working_directory()).is_error())
        return Error::from_errno(EEXIST);

    char name[fs::FILENAME_MAX_LENGTH];
    auto* parent = TRY(fs::resolve_parent(path, process->working_directory(), name));
    if (parent->filesystem() != nullptr && parent->filesystem()->is_read_only())
        return Error::from_errno(EROFS);

    TRY(parent->create(name, fs::InodeType::Directory, static_cast<u32>(mode) & ~process->umask()));
    return static_cast<u64>(0);
}

ErrorOr<u64> sys_unlink_common(u64 path_pointer, bool must_be_directory)
{
    char path[fs::PATH_MAX_LENGTH];
    TRY(copy_string_from_user(path, path_pointer, sizeof(path)));

    auto* process = Process::current();
    auto* target = TRY(fs::resolve(path, process->working_directory()));
    if (must_be_directory && !target->is_directory())
        return Error::from_errno(ENOTDIR);
    if (!must_be_directory && target->is_directory())
        return Error::from_errno(EISDIR);

    char name[fs::FILENAME_MAX_LENGTH];
    auto* parent = TRY(fs::resolve_parent(path, process->working_directory(), name));
    if (parent->filesystem() != nullptr && parent->filesystem()->is_read_only())
        return Error::from_errno(EROFS);

    TRY(parent->unlink(name));
    return static_cast<u64>(0);
}

ErrorOr<u64> sys_rmdir(InterruptFrame&, u64 path_pointer, u64, u64, u64, u64, u64)
{
    return sys_unlink_common(path_pointer, true);
}

ErrorOr<u64> sys_unlink(InterruptFrame&, u64 path_pointer, u64, u64, u64, u64, u64)
{
    return sys_unlink_common(path_pointer, false);
}

ErrorOr<u64> sys_chdir(InterruptFrame&, u64 path_pointer, u64, u64, u64, u64, u64)
{
    char path[fs::PATH_MAX_LENGTH];
    TRY(copy_string_from_user(path, path_pointer, sizeof(path)));

    auto* process = Process::current();
    auto* inode = TRY(fs::resolve(path, process->working_directory()));
    if (!inode->is_directory())
        return Error::from_errno(ENOTDIR);

    process->set_working_directory(inode);
    return static_cast<u64>(0);
}

ErrorOr<u64> sys_getcwd(InterruptFrame&, u64 buffer, u64 capacity, u64, u64, u64, u64)
{
    auto* process = Process::current();
    auto* cwd = process->working_directory();
    if (cwd == nullptr)
        return Error::from_errno(ENOENT);

    char path[fs::PATH_MAX_LENGTH];
    usize const length = TRY(fs::absolute_path_of(*cwd, path, sizeof(path)));
    if (length + 1 > capacity)
        return Error::from_errno(ERANGE);

    TRY(copy_to_user(buffer, path, length + 1));
    return buffer;
}

// The requests whose numbers predate the size encoding. Anything defined with
// _IOR/_IOW/_IOWR describes itself and needs no entry here.
//
// Missing one is not harmless. Without an entry the request is treated as
// taking its argument by value, so the raw user pointer is handed to the
// device and dereferenced in kernel context -- which is what was happening to
// TIOCGPGRP and TIOCSPGRP, and worked only because nothing had yet passed a
// bad pointer.
struct LegacyIoctl {
    u32 request;
    u32 direction;
    usize size;
};

constexpr LegacyIoctl LEGACY_IOCTLS[] = {
    { TCGETS, _IOC_READ, sizeof(struct termios) },
    { TCSETS, _IOC_WRITE, sizeof(struct termios) },
    { TIOCGWINSZ, _IOC_READ, sizeof(struct winsize) },
    { TIOCSWINSZ, _IOC_WRITE, sizeof(struct winsize) },
    { TIOCGPGRP, _IOC_READ, sizeof(i32) },
    { TIOCSPGRP, _IOC_WRITE, sizeof(i32) },
};

bool describe_ioctl(u32 request, u32& direction, usize& size)
{
    for (auto const& legacy : LEGACY_IOCTLS) {
        if (legacy.request == request) {
            direction = legacy.direction;
            size = legacy.size;
            return true;
        }
    }

    direction = _IOC_DIRECTION(request);
    size = _IOC_ARGUMENT_SIZE(request);

    // A request with no encoded size takes its argument by value, not by
    // pointer, and nothing is copied in either direction.
    return size != 0;
}

ErrorOr<u64> sys_ioctl(InterruptFrame&, u64 fd, u64 request, u64 argument, u64, u64, u64)
{
    auto* description = TRY(description_for(static_cast<int>(fd)));
    auto const number = static_cast<u32>(request);

    u32 direction = _IOC_NONE;
    usize size = 0;

    // No pointer argument: hand the value straight to the device.
    if (!describe_ioctl(number, direction, size) || argument == 0)
        return static_cast<u64>(
            TRY(description->inode().ioctl(number, reinterpret_cast<void*>(argument))));

    if (size > PAGE_SIZE)
        return Error::from_errno(EINVAL);

    // Bounce through the kernel so the device never sees a user pointer, and
    // move exactly the number of bytes the request describes. Copying a fixed
    // size instead would write past the caller's struct.
    auto* scratch = static_cast<u8*>(kzalloc(size));
    if (scratch == nullptr)
        return Error::from_errno(ENOMEM);

    if ((direction & _IOC_WRITE) != 0) {
        if (auto copied = copy_from_user(scratch, argument, size); copied.is_error()) {
            kfree(scratch);
            return copied.error();
        }
    }

    auto result = description->inode().ioctl(number, scratch);
    if (result.is_error()) {
        kfree(scratch);
        return result.error();
    }

    if ((direction & _IOC_READ) != 0) {
        if (auto copied = copy_to_user(argument, scratch, size); copied.is_error()) {
            kfree(scratch);
            return copied.error();
        }
    }

    kfree(scratch);
    return static_cast<u64>(result.value());
}

void raise_on(Process& process, void* context)
{
    process.raise_signal(*static_cast<int*>(context));
}

ErrorOr<u64> sys_kill(InterruptFrame&, u64 pid_argument, u64 signal, u64, u64, u64, u64)
{
    auto const pid = static_cast<pid_t>(pid_argument);
    int number = static_cast<int>(signal);
    if (number < 0 || number >= NSIG)
        return Error::from_errno(EINVAL);

    auto* caller = Process::current();
    // A negative pid addresses a process group, which is what makes ^C reach a
    // pipeline rather than whichever member of it happened to be reading.
    if (pid < -1 || pid == 0) {
        pid_t const group = pid == 0 ? caller->pgid() : -pid;
        if (!Process::group_exists(group))
            return Error::from_errno(ESRCH);
        if (number == 0)
            return static_cast<u64>(0);
        Process::for_each_in_group(group, raise_on, &number);
        return static_cast<u64>(0);
    }

    if (pid == -1) {
        // Every process the caller may signal, which here is everything but
        // init and the caller itself -- killing pid 1 would end userland.
        if (number == 0)
            return static_cast<u64>(0);
        struct Broadcast {
            int signal;
            pid_t caller_pid;
        } broadcast { number, caller->pid() };

        Process::for_each(
            [](Process& process, void* context) {
                auto const& state = *static_cast<Broadcast*>(context);
                if (process.pid() <= 1 || process.pid() == state.caller_pid)
                    return;
                process.raise_signal(state.signal);
            },
            &broadcast);
        return static_cast<u64>(0);
    }

    auto* target = Process::by_pid(pid);
    if (target == nullptr)
        return Error::from_errno(ESRCH);
    if (number == 0)
        return static_cast<u64>(0); // the existence check form of kill(2)

    target->raise_signal(number);
    return static_cast<u64>(0);
}

ErrorOr<u64> sys_setpgid(InterruptFrame&, u64 pid, u64 pgid, u64, u64, u64, u64)
{
    TRY(Process::set_process_group(static_cast<pid_t>(pid), static_cast<pid_t>(pgid)));
    return static_cast<u64>(0);
}

ErrorOr<u64> sys_getpgid(InterruptFrame&, u64 pid, u64, u64, u64, u64, u64)
{
    auto* target = pid == 0 ? Process::current() : Process::by_pid(static_cast<pid_t>(pid));
    if (target == nullptr)
        return Error::from_errno(ESRCH);
    return static_cast<u64>(target->pgid());
}

ErrorOr<u64> sys_setsid(InterruptFrame&, u64, u64, u64, u64, u64, u64)
{
    return static_cast<u64>(TRY(Process::current()->start_session()));
}

ErrorOr<u64> sys_getsid(InterruptFrame&, u64 pid, u64, u64, u64, u64, u64)
{
    auto* target = pid == 0 ? Process::current() : Process::by_pid(static_cast<pid_t>(pid));
    if (target == nullptr)
        return Error::from_errno(ESRCH);
    return static_cast<u64>(target->sid());
}

ErrorOr<u64> sys_sigaction(
    InterruptFrame&, u64 signal, u64 action_pointer, u64 old_pointer, u64, u64, u64)
{
    auto* process = Process::current();
    int const number = static_cast<int>(signal);
    if (number <= 0 || number >= NSIG)
        return Error::from_errno(EINVAL);

    if (old_pointer != 0) {
        struct sigaction previous = {};
        previous.sa_handler = reinterpret_cast<void (*)(int)>(process->signal_disposition(number));
        previous.sa_restorer = reinterpret_cast<void (*)()>(process->signal_restorer(number));
        previous.sa_flags = process->signal_flags(number);
        TRY(copy_to_user(old_pointer, &previous, sizeof(previous)));
    }

    if (action_pointer != 0) {
        struct sigaction action = {};
        TRY(copy_from_user(&action, action_pointer, sizeof(action)));
        process->set_signal_action(number, reinterpret_cast<void*>(action.sa_handler),
            reinterpret_cast<void*>(action.sa_restorer), action.sa_flags);
    }

    return static_cast<u64>(0);
}

ErrorOr<u64> sys_sigreturn(InterruptFrame& frame, u64, u64, u64, u64, u64, u64)
{
    // The handler's return address was the restorer, which called us. The
    // saved context sits just above the return address it popped.
    SignalContext context;
    TRY(copy_from_user(&context, frame.rsp, sizeof(context)));

    // Only restore what userspace is allowed to influence: forcing the
    // segment selectors and the interrupt flag means a forged context cannot
    // be used to return into ring 0.
    InterruptFrame restored = context.frame;
    restored.cs = arch::SELECTOR_USER_CODE;
    restored.ss = arch::SELECTOR_USER_DATA;
    restored.rflags = (restored.rflags & 0x0000000000000CD5ULL) | 0x202;
    restored.vector = frame.vector;
    restored.error_code = 0;

    frame = restored;
    return frame.rax;
}

ErrorOr<u64> sys_nanosleep(InterruptFrame&, u64 seconds, u64 nanoseconds, u64, u64, u64, u64)
{
    u64 const milliseconds = seconds * 1000 + nanoseconds / 1000000;
    u64 const deadline = clock_monotonic_ns() + milliseconds * 1'000'000;

    // Sleep in slices and look for signals between them. Sleeping the whole
    // duration in one call would mean ^C could not interrupt `sleep 5`, which
    // is the single most common thing anyone does to a sleeping program. The
    // tick is 4 ms, so slicing at the tick costs nothing.
    auto* process = Process::current();
    while (clock_monotonic_ns() < deadline) {
        u64 const left_ns = deadline - clock_monotonic_ns();
        u64 const slice = left_ns / 1'000'000 < 4 ? 1 : 4;
        Scheduler::sleep_ms(slice);

        if (process != nullptr && process->has_pending_signals())
            return Error::from_errno(EINTR);
    }
    return static_cast<u64>(0);
}

ErrorOr<u64> sys_uname(InterruptFrame&, u64 pointer, u64, u64, u64, u64, u64)
{
    struct utsname name = {};
    strcpy(name.sysname, "shit os");
    strcpy(name.nodename, "shit-os-2");
    strcpy(name.release, "2.0.0");
    strcpy(name.version, "gen 2, hybrid kernel");
    strcpy(name.machine, "x86_64");
    TRY(copy_to_user(pointer, &name, sizeof(name)));
    return static_cast<u64>(0);
}

ErrorOr<u64> sys_sched_yield(InterruptFrame&, u64, u64, u64, u64, u64, u64)
{
    Scheduler::yield();
    return static_cast<u64>(0);
}

ErrorOr<u64> sys_isatty(InterruptFrame&, u64 fd, u64, u64, u64, u64, u64)
{
    auto* description = TRY(description_for(static_cast<int>(fd)));
    // A terminal is a character device that answers TCGETS.
    if (description->inode().type() != fs::InodeType::CharacterDevice)
        return Error::from_errno(ENOTTY);
    struct termios probe = {};
    TRY(description->inode().ioctl(TCGETS, &probe));
    return static_cast<u64>(1);
}

ErrorOr<u64> sys_clock_gettime(InterruptFrame&, u64 clock_id, u64 pointer, u64, u64, u64, u64)
{
    u64 nanoseconds = 0;
    switch (clock_id) {
    case CLOCK_REALTIME: nanoseconds = clock_realtime_ns(); break;
    case CLOCK_MONOTONIC: nanoseconds = clock_monotonic_ns(); break;
    default: return Error::from_errno(EINVAL);
    }

    struct timespec value = {
        .tv_sec = static_cast<i64>(nanoseconds / 1'000'000'000ull),
        .tv_nsec = static_cast<i64>(nanoseconds % 1'000'000'000ull),
    };
    TRY(copy_to_user(pointer, &value, sizeof(value)));
    return static_cast<u64>(0);
}

ErrorOr<u64> sys_fcntl(InterruptFrame&, u64 fd_argument, u64 command, u64 argument, u64, u64, u64)
{
    auto* process = Process::current();
    int const fd = static_cast<int>(fd_argument);
    auto* description = TRY(process->description_for(fd));

    switch (command) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        int const lowest = static_cast<int>(argument);
        if (lowest < 0)
            return Error::from_errno(EINVAL);
        int const duplicate = TRY(process->allocate_descriptor(description, lowest));
        description->ref();
        // Unlike dup(), F_DUPFD_CLOEXEC exists precisely so the new descriptor
        // can be marked without a window where a fork could inherit it.
        TRY(process->set_descriptor_close_on_exec(duplicate, command == F_DUPFD_CLOEXEC));
        return static_cast<u64>(duplicate);
    }

    case F_GETFD:
        return static_cast<u64>(TRY(process->descriptor_close_on_exec(fd)) ? FD_CLOEXEC : 0);

    case F_SETFD:
        TRY(process->set_descriptor_close_on_exec(fd, (argument & FD_CLOEXEC) != 0));
        return static_cast<u64>(0);

    case F_GETFL: return static_cast<u64>(static_cast<unsigned>(description->flags()));

    case F_SETFL: {
        // Access mode and creation flags are fixed at open time. POSIX says to
        // ignore them here rather than fail, so mask down to what can change.
        int const kept = description->flags() & ~O_SETFL_MASK;
        description->set_flags(kept | (static_cast<int>(argument) & O_SETFL_MASK));
        return static_cast<u64>(0);
    }

    default: return Error::from_errno(EINVAL);
    }
}

ErrorOr<u64> sys_rename(InterruptFrame&, u64 from_pointer, u64 to_pointer, u64, u64, u64, u64)
{
    char from[fs::PATH_MAX_LENGTH];
    char to[fs::PATH_MAX_LENGTH];
    TRY(copy_string_from_user(from, from_pointer, sizeof(from)));
    TRY(copy_string_from_user(to, to_pointer, sizeof(to)));

    TRY(fs::rename(from, to, Process::current()->working_directory()));
    return static_cast<u64>(0);
}

// The most descriptors one call may ask about. A real implementation would
// allocate; this is a fixed bounce buffer, and 64 is the size of the whole
// descriptor table, so no caller can legitimately need more.
inline constexpr usize MAX_POLL_DESCRIPTORS = MAX_FILE_DESCRIPTORS;

// Fills in revents for one entry. Returns true if anything was reported,
// which is what decides whether poll returns rather than sleeps.
bool poll_one(Process& process, struct pollfd& entry)
{
    entry.revents = 0;

    // A negative fd is how a caller says "skip this one" without shuffling
    // the array, and it is not an error.
    if (entry.fd < 0)
        return false;

    auto description = process.description_for(entry.fd);
    if (description.is_error()) {
        entry.revents = POLLNVAL;
        return true;
    }

    auto& inode = description.value()->inode();
    i16 reported = 0;

    if ((entry.events & POLLIN) != 0 && inode.can_read_without_blocking())
        reported |= POLLIN;
    if ((entry.events & POLLOUT) != 0 && inode.can_write_without_blocking())
        reported |= POLLOUT;

    // POLLHUP is reported whether or not it was asked for. A caller waiting
    // to read from a pipe whose writer has gone would otherwise wait forever.
    if (inode.is_hung_up())
        reported |= POLLHUP;

    entry.revents = reported;
    return reported != 0;
}

ErrorOr<u64> sys_poll(InterruptFrame&, u64 pointer, u64 count, u64 timeout_ms, u64, u64, u64)
{
    if (count > MAX_POLL_DESCRIPTORS)
        return Error::from_errno(EINVAL);

    auto* process = Process::current();
    struct pollfd entries[MAX_POLL_DESCRIPTORS];

    if (count > 0)
        TRY(copy_from_user(entries, pointer, count * sizeof(struct pollfd)));

    auto const timeout = static_cast<i64>(timeout_ms);
    u64 const deadline
        = timeout > 0 ? clock_monotonic_ns() + static_cast<u64>(timeout) * 1'000'000 : 0;

    for (;;) {
        usize ready = 0;
        for (usize i = 0; i < count; ++i) {
            if (poll_one(*process, entries[i]))
                ++ready;
        }

        if (ready > 0 || timeout == 0) {
            if (count > 0)
                TRY(copy_to_user(pointer, entries, count * sizeof(struct pollfd)));
            return static_cast<u64>(ready);
        }

        if (timeout > 0 && clock_monotonic_ns() >= deadline) {
            if (count > 0)
                TRY(copy_to_user(pointer, entries, count * sizeof(struct pollfd)));
            return static_cast<u64>(0);
        }

        // No wait queue spans arbitrary descriptors, so this polls on the
        // timer rather than sleeping on the right one. It is the honest
        // version of what the interface promises and the obvious thing to
        // replace once inodes carry their own poll queues.
        Scheduler::sleep_ms(4);

        if (process->has_pending_signals())
            return Error::from_errno(EINTR);
    }
}

ErrorOr<u64> sys_sigprocmask(
    InterruptFrame&, u64 how, u64 set_pointer, u64 old_pointer, u64, u64, u64)
{
    auto* process = Process::current();

    // A null set means "tell me the current mask and change nothing", which is
    // how a caller reads it without a separate call.
    u64 wanted = 0;
    if (set_pointer != 0)
        TRY(copy_from_user(&wanted, set_pointer, sizeof(wanted)));

    u64 const previous = set_pointer != 0 ? process->set_signal_mask(static_cast<int>(how), wanted)
                                          : process->signal_mask();

    if (old_pointer != 0)
        TRY(copy_to_user(old_pointer, &previous, sizeof(previous)));
    return static_cast<u64>(0);
}

ErrorOr<u64> sys_umask(InterruptFrame&, u64 mask, u64, u64, u64, u64, u64)
{
    return static_cast<u64>(Process::current()->set_umask(static_cast<u32>(mask)));
}

/*
 * Resolves a path against a directory descriptor, which is what the whole `at`
 * family is for: a recursive walk descends without rebuilding a full path at
 * every step, and without racing a rename of a directory it has already
 * passed. AT_FDCWD means the working directory, as always.
 */
ErrorOr<fs::Inode*> base_for_directory_fd(int directory)
{
    if (directory == AT_FDCWD)
        return Process::current()->working_directory();

    auto* description = TRY(description_for(directory));
    auto& inode = description->inode();
    if (!inode.is_directory())
        return Error::from_errno(ENOTDIR);
    return &inode;
}

ErrorOr<u64> sys_openat(
    InterruptFrame&, u64 directory, u64 path_pointer, u64 flags, u64 mode, u64, u64)
{
    char path[fs::PATH_MAX_LENGTH];
    TRY(copy_string_from_user(path, path_pointer, sizeof(path)));

    auto* process = Process::current();
    auto* base = TRY(base_for_directory_fd(static_cast<int>(directory)));
    auto const permissions = static_cast<u32>(mode) & ~process->umask();

    auto* description = TRY(fs::open(path, static_cast<int>(flags), permissions, base));

    auto fd = process->allocate_descriptor(description);
    if (fd.is_error()) {
        fs::release_description(description);
        return fd.error();
    }
    if ((flags & O_CLOEXEC) != 0)
        (void)process->set_descriptor_close_on_exec(fd.value(), true);

    return static_cast<u64>(fd.value());
}

ErrorOr<u64> sys_fstatat(
    InterruptFrame&, u64 directory, u64 path_pointer, u64 stat_pointer, u64, u64, u64)
{
    char path[fs::PATH_MAX_LENGTH];
    TRY(copy_string_from_user(path, path_pointer, sizeof(path)));

    auto* base = TRY(base_for_directory_fd(static_cast<int>(directory)));
    auto* inode = TRY(fs::resolve(path, base));

    struct stat status;
    TRY(inode->stat(status));
    TRY(copy_to_user(stat_pointer, &status, sizeof(status)));
    return static_cast<u64>(0);
}

ErrorOr<u64> sys_unlinkat(
    InterruptFrame&, u64 directory, u64 path_pointer, u64 flags, u64, u64, u64)
{
    char path[fs::PATH_MAX_LENGTH];
    TRY(copy_string_from_user(path, path_pointer, sizeof(path)));

    auto* base = TRY(base_for_directory_fd(static_cast<int>(directory)));

    char name[fs::FILENAME_MAX_LENGTH];
    auto* parent = TRY(fs::resolve_parent(path, base, name));
    if (parent->filesystem() != nullptr && parent->filesystem()->is_read_only())
        return Error::from_errno(EROFS);

    auto* target = TRY(parent->lookup(name));
    bool const wants_directory = (flags & AT_REMOVEDIR) != 0;
    if (wants_directory && !target->is_directory())
        return Error::from_errno(ENOTDIR);
    if (!wants_directory && target->is_directory())
        return Error::from_errno(EISDIR);

    TRY(parent->unlink(name));
    return static_cast<u64>(0);
}

ErrorOr<u64> sys_mkdirat(InterruptFrame&, u64 directory, u64 path_pointer, u64 mode, u64, u64, u64)
{
    char path[fs::PATH_MAX_LENGTH];
    TRY(copy_string_from_user(path, path_pointer, sizeof(path)));

    auto* process = Process::current();
    auto* base = TRY(base_for_directory_fd(static_cast<int>(directory)));

    if (!fs::resolve(path, base).is_error())
        return Error::from_errno(EEXIST);

    char name[fs::FILENAME_MAX_LENGTH];
    auto* parent = TRY(fs::resolve_parent(path, base, name));
    if (parent->filesystem() != nullptr && parent->filesystem()->is_read_only())
        return Error::from_errno(EROFS);

    TRY(parent->create(name, fs::InodeType::Directory, static_cast<u32>(mode) & ~process->umask()));
    return static_cast<u64>(0);
}

ErrorOr<u64> sys_fchmodat(InterruptFrame&, u64 directory, u64 path_pointer, u64 mode, u64, u64, u64)
{
    char path[fs::PATH_MAX_LENGTH];
    TRY(copy_string_from_user(path, path_pointer, sizeof(path)));

    auto* base = TRY(base_for_directory_fd(static_cast<int>(directory)));
    auto* inode = TRY(fs::resolve(path, base));
    if (inode->filesystem() != nullptr && inode->filesystem()->is_read_only())
        return Error::from_errno(EROFS);

    inode->set_mode(static_cast<u32>(mode) & 07777);
    return static_cast<u64>(0);
}

/*
 * A FIFO is the only kind of file a process can create that is not storage --
 * a name in the tree with a pipe behind it. It exists here because two
 * unrelated processes have no other way to find each other: an anonymous pipe
 * has to be inherited, and inheritance means they were related.
 */
ErrorOr<u64> sys_mkfifo(InterruptFrame&, u64 path_pointer, u64 mode, u64, u64, u64, u64)
{
    char path[fs::PATH_MAX_LENGTH];
    TRY(copy_string_from_user(path, path_pointer, sizeof(path)));

    auto* process = Process::current();
    auto* base = process->working_directory();

    if (!fs::resolve(path, base).is_error())
        return Error::from_errno(EEXIST);

    char name[fs::FILENAME_MAX_LENGTH];
    auto* parent = TRY(fs::resolve_parent(path, base, name));
    if (parent->filesystem() != nullptr && parent->filesystem()->is_read_only())
        return Error::from_errno(EROFS);

    TRY(parent->create(name, fs::InodeType::Fifo, static_cast<u32>(mode) & ~process->umask()));
    return static_cast<u64>(0);
}

/*
 * A pseudo-terminal pair, both descriptors at once.
 *
 * Not posix_openpt: that hands back a master and a *name*, and turning a name
 * back into the slave needs a /dev/pts filesystem whose contents track the
 * ptys that exist. openpty is what programs actually call -- through libutil,
 * on every system that has one -- and it needs none of that machinery. When
 * something is ported that genuinely wants ptsname, the name can be added on
 * top of this rather than the other way round.
 */
ErrorOr<u64> sys_openpty(InterruptFrame&, u64 master_pointer, u64 slave_pointer, u64, u64, u64, u64)
{
    fs::FileDescription* master = nullptr;
    fs::FileDescription* slave = nullptr;
    TRY(dev::Pty::create(master, slave));

    auto* process = Process::current();

    auto master_fd = process->allocate_descriptor(master);
    if (master_fd.is_error()) {
        fs::release_description(master);
        fs::release_description(slave);
        return master_fd.error();
    }

    auto slave_fd = process->allocate_descriptor(slave);
    if (slave_fd.is_error()) {
        (void)process->close_descriptor(master_fd.value());
        fs::release_description(slave);
        return slave_fd.error();
    }

    int const numbers[2] = { master_fd.value(), slave_fd.value() };
    if (auto copied = copy_to_user(master_pointer, &numbers[0], sizeof(int)); copied.is_error()) {
        (void)process->close_descriptor(master_fd.value());
        (void)process->close_descriptor(slave_fd.value());
        return copied.error();
    }
    if (auto copied = copy_to_user(slave_pointer, &numbers[1], sizeof(int)); copied.is_error()) {
        (void)process->close_descriptor(master_fd.value());
        (void)process->close_descriptor(slave_fd.value());
        return copied.error();
    }

    return static_cast<u64>(0);
}

ErrorOr<u64> sys_ftruncate(InterruptFrame&, u64 fd, u64 length, u64, u64, u64, u64)
{
    auto* description = TRY(description_for(static_cast<int>(fd)));
    if (!description->is_writable())
        return Error::from_errno(EBADF);
    if (static_cast<i64>(length) < 0)
        return Error::from_errno(EINVAL);
    TRY(description->inode().truncate(length));
    return static_cast<u64>(0);
}

ErrorOr<u64> sys_chmod(InterruptFrame&, u64 path_pointer, u64 mode, u64, u64, u64, u64)
{
    char path[fs::PATH_MAX_LENGTH];
    TRY(copy_string_from_user(path, path_pointer, sizeof(path)));

    auto* inode = TRY(fs::resolve(path, Process::current()->working_directory()));
    if (inode->filesystem() != nullptr && inode->filesystem()->is_read_only())
        return Error::from_errno(EROFS);

    // Nothing checks a mode bit yet, so this only decides what gets recorded.
    // Recording the wrong thing now means every file in the tree is wrong when
    // something finally does check.
    inode->set_mode(static_cast<u32>(mode) & 07777);
    return static_cast<u64>(0);
}

// --- extensions ---------------------------------------------------------

ErrorOr<u64> sys_shitos_sysinfo(InterruptFrame&, u64 pointer, u64, u64, u64, u64, u64)
{
    auto const heap = heap_stats();
    struct shitos_sysinfo info = {};
    info.uptime_ms = Scheduler::uptime_ms();
    info.mem_total_bytes = static_cast<u64>(mm::total_pages()) * PAGE_SIZE;
    info.mem_free_bytes = static_cast<u64>(mm::free_pages()) * PAGE_SIZE;
    info.mem_kernel_heap_bytes = heap.bytes_in_use;
    info.page_size = PAGE_SIZE;
    info.process_count = Process::count();
    info.thread_count = Scheduler::thread_count();
    info.module_count = ModuleLoader::module_count();
    info.context_switches = Scheduler::context_switches();

    TRY(copy_to_user(pointer, &info, sizeof(info)));
    return static_cast<u64>(0);
}

struct ProcessCollector {
    struct shitos_procinfo* entries;
    usize capacity;
    usize count;
};

void collect_process(Process& process, void* context)
{
    auto* collector = static_cast<ProcessCollector*>(context);
    if (collector->count >= collector->capacity)
        return;

    auto& entry = collector->entries[collector->count++];
    entry.pid = process.pid();
    entry.ppid = process.parent_pid();
    entry.thread_count = static_cast<u32>(process.thread_count());
    entry.rss_bytes = process.resident_bytes();
    entry.cpu_ticks = process.main_thread() != nullptr ? process.main_thread()->cpu_ticks() : 0;

    if (process.has_exited()) {
        entry.state = SHITOS_PROC_STATE_ZOMBIE;
    } else if (auto* thread = process.main_thread(); thread != nullptr) {
        switch (thread->state()) {
        case ThreadState::Running: entry.state = SHITOS_PROC_STATE_RUNNING; break;
        case ThreadState::Ready: entry.state = SHITOS_PROC_STATE_READY; break;
        case ThreadState::Zombie: entry.state = SHITOS_PROC_STATE_ZOMBIE; break;
        default: entry.state = SHITOS_PROC_STATE_BLOCKED; break;
        }
    } else {
        entry.state = SHITOS_PROC_STATE_READY;
    }

    strncpy(entry.name, process.name(), SHITOS_PROC_NAME_MAX - 1);
    entry.name[SHITOS_PROC_NAME_MAX - 1] = '\0';
}

ErrorOr<u64> sys_shitos_procs(InterruptFrame&, u64 pointer, u64 capacity, u64, u64, u64, u64)
{
    usize const wanted = min<usize>(capacity, 128);
    if (wanted == 0)
        return static_cast<u64>(Process::count());

    auto* entries
        = static_cast<struct shitos_procinfo*>(kzalloc(wanted * sizeof(struct shitos_procinfo)));
    if (entries == nullptr)
        return Error::from_errno(ENOMEM);

    ProcessCollector collector { entries, wanted, 0 };
    Process::for_each(collect_process, &collector);

    auto copied = copy_to_user(pointer, entries, collector.count * sizeof(struct shitos_procinfo));
    usize const count = collector.count;
    kfree(entries);
    TRY(copied);

    return static_cast<u64>(count);
}

struct ModuleCollector {
    struct shitos_moduleinfo* entries;
    usize capacity;
    usize count;
};

void collect_module(LoadedModule const& module, void* context)
{
    auto* collector = static_cast<ModuleCollector*>(context);
    if (collector->count >= collector->capacity)
        return;

    auto& entry = collector->entries[collector->count++];
    strncpy(entry.name, module.name(), SHITOS_MODULE_NAME_MAX - 1);
    entry.name[SHITOS_MODULE_NAME_MAX - 1] = '\0';
    entry.abi_version = module.abi_version();
    entry.flags = 0;
    entry.base = reinterpret_cast<u64>(module.base());
    entry.size = module.size();
}

ErrorOr<u64> sys_shitos_modules(InterruptFrame&, u64 pointer, u64 capacity, u64, u64, u64, u64)
{
    usize const wanted = min<usize>(capacity, 64);
    if (wanted == 0)
        return static_cast<u64>(ModuleLoader::module_count());

    auto* entries = static_cast<struct shitos_moduleinfo*>(
        kzalloc(wanted * sizeof(struct shitos_moduleinfo)));
    if (entries == nullptr)
        return Error::from_errno(ENOMEM);

    ModuleCollector collector { entries, wanted, 0 };
    ModuleLoader::for_each(collect_module, &collector);

    auto copied
        = copy_to_user(pointer, entries, collector.count * sizeof(struct shitos_moduleinfo));
    usize const count = collector.count;
    kfree(entries);
    TRY(copied);

    return static_cast<u64>(count);
}

ErrorOr<u64> sys_shitos_shutdown(InterruptFrame&, u64 mode, u64, u64, u64, u64, u64)
{
    kprintf("\n");
    klog(LOG_INFO, "boot", "shutting down");

    switch (mode) {
    case SHITOS_SHUTDOWN_REBOOT:
        // Pulse the keyboard controller's reset line, which is the oldest and
        // most widely supported way to reboot a PC.
        arch::outb(0x64, 0xFE);
        break;
    case SHITOS_SHUTDOWN_POWEROFF:
        // QEMU and Bochs watch this port; on real hardware it does nothing and
        // we fall through to halting.
        arch::outw(0x604, 0x2000);
        arch::outw(0xB004, 0x2000);
        break;
    default: break;
    }

    arch::halt_forever();
}

// --- the table ----------------------------------------------------------

using SyscallHandler = ErrorOr<u64> (*)(InterruptFrame&, u64, u64, u64, u64, u64, u64);

// Array designators are a C99 feature clang also offers in C++. Using them
// here means the table cannot silently drift out of step with the numbers in
// <shitos/abi/syscall.h>, which is worth more than the portability.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc99-designator"

SyscallHandler const POSIX_SYSCALLS[SYS_MAX_POSIX] = {
    [SYS_exit] = sys_exit,
    [SYS_read] = sys_read,
    [SYS_write] = sys_write,
    [SYS_open] = sys_open,
    [SYS_close] = sys_close,
    [SYS_lseek] = sys_lseek,
    [SYS_fork] = sys_fork,
    [SYS_execve] = sys_execve,
    [SYS_waitpid] = sys_waitpid,
    [SYS_getpid] = sys_getpid,
    [SYS_getppid] = sys_getppid,
    [SYS_brk] = sys_brk,
    [SYS_mmap] = sys_mmap,
    [SYS_munmap] = sys_munmap,
    [SYS_dup] = sys_dup,
    [SYS_dup2] = sys_dup2,
    [SYS_pipe] = sys_pipe,
    [SYS_stat] = sys_stat,
    [SYS_fstat] = sys_fstat,
    [SYS_getdents] = sys_getdents,
    [SYS_mkdir] = sys_mkdir,
    [SYS_rmdir] = sys_rmdir,
    [SYS_unlink] = sys_unlink,
    [SYS_chdir] = sys_chdir,
    [SYS_getcwd] = sys_getcwd,
    [SYS_ioctl] = sys_ioctl,
    [SYS_kill] = sys_kill,
    [SYS_sigaction] = sys_sigaction,
    [SYS_sigreturn] = sys_sigreturn,
    [SYS_nanosleep] = sys_nanosleep,
    [SYS_uname] = sys_uname,
    [SYS_sched_yield] = sys_sched_yield,
    [SYS_isatty] = sys_isatty,
    [SYS_clock_gettime] = sys_clock_gettime,
    [SYS_fcntl] = sys_fcntl,
    [SYS_rename] = sys_rename,
    [SYS_setpgid] = sys_setpgid,
    [SYS_getpgid] = sys_getpgid,
    [SYS_setsid] = sys_setsid,
    [SYS_getsid] = sys_getsid,
    [SYS_poll] = sys_poll,
    [SYS_sigprocmask] = sys_sigprocmask,
    [SYS_umask] = sys_umask,
    [SYS_ftruncate] = sys_ftruncate,
    [SYS_chmod] = sys_chmod,
    [SYS_openat] = sys_openat,
    [SYS_fstatat] = sys_fstatat,
    [SYS_unlinkat] = sys_unlinkat,
    [SYS_mkdirat] = sys_mkdirat,
    [SYS_fchmodat] = sys_fchmodat,
    [SYS_mkfifo] = sys_mkfifo,
    [SYS_openpty] = sys_openpty,
};

#pragma clang diagnostic pop

SyscallHandler const EXTENSION_SYSCALLS[SYS_MAX_EXT] = {
    sys_shitos_sysinfo,
    sys_shitos_procs,
    sys_shitos_modules,
    sys_shitos_shutdown,
};

// --- signal delivery ----------------------------------------------------

} // namespace

bool signal_terminates_by_default(int signal)
{
    switch (signal) {
    // Ignored by default.
    case SIGCHLD:
    case SIGURG:
    case SIGWINCH:
    // Handled by default, but not by dying.
    case SIGCONT:
    case SIGSTOP:
    case SIGTSTP:
    case SIGTTIN:
    case SIGTTOU: return false;
    default: return true;
    }
}

bool signal_stops_by_default(int signal)
{
    switch (signal) {
    case SIGSTOP: // cannot be caught or ignored
    case SIGTSTP: // ^Z
    case SIGTTIN: // a background job read the terminal
    case SIGTTOU: // ...or wrote to it with TOSTOP set
        return true;
    default: return false;
    }
}

// Puts an interrupted system call back so it runs again. Only ever called
// when the signal left the process running -- a call that returned EINTR to a
// program that then handled nothing, ignored the signal, or was stopped and
// continued, never happened as far as that program should be able to tell.
void restart_interrupted_syscall(InterruptFrame* frame)
{
    auto* thread = Scheduler::current();
    if (thread == nullptr || !thread->has_restartable_syscall())
        return;

    frame->rax = thread->restartable_syscall();
    frame->rip -= Thread::SYSCALL_INSTRUCTION_LENGTH;
    thread->clear_restartable_syscall();
}

InterruptFrame* deliver_pending_signal(InterruptFrame* frame)
{
    auto* process = Process::current();
    if (process == nullptr || process->pid() == 0 || !process->has_pending_signals())
        return frame;

    int const signal = process->take_pending_signal();
    if (signal == 0)
        return frame;

    void* handler = process->signal_disposition(signal);

    // SIGKILL and SIGSTOP cannot be caught or ignored. That is the whole
    // reason they exist, so the disposition is not even consulted.
    if (signal == SIGKILL)
        do_exit(W_SIGNALLED(SIGKILL));
    if (signal == SIGSTOP) {
        process->stop(SIGSTOP);
        restart_interrupted_syscall(frame);
        return frame;
    }

    // SIGCONT resumes before anything else looks at the disposition: a
    // stopped process is not running to catch it, and continuing is what
    // makes it able to.
    if (signal == SIGCONT) {
        process->resume();
        if (handler == SIG_IGN || handler == SIG_DFL) {
            restart_interrupted_syscall(frame);
            return frame;
        }
        // A caught SIGCONT still runs its handler, once running again.
    }

    if (handler == SIG_IGN) {
        // Nothing happened as far as the program is concerned, so a call this
        // signal interrupted should not report that it was interrupted.
        restart_interrupted_syscall(frame);
        return frame;
    }

    if (handler == SIG_DFL) {
        if (signal_stops_by_default(signal)) {
            process->stop(signal);
            // Continued now, and the read it was in the middle of should
            // resume rather than surface EINTR to a program that never saw
            // anything happen.
            restart_interrupted_syscall(frame);
            return frame;
        }
        if (!signal_terminates_by_default(signal)) {
            restart_interrupted_syscall(frame);
            return frame;
        }
        // The low byte of a wait status is the terminating signal.
        do_exit(signal & 0x7F);
    }

    // A real handler. Push the interrupted context onto the user stack and
    // arrange for the handler to run with the restorer as its return address;
    // the restorer calls sigreturn, which puts the context back.
    // No trampoline means no way back out of the handler, and jumping to a
    // handler that cannot return would hang the process on its next
    // instruction. libc always supplies one; a program calling the syscall
    // directly might not.
    void* restorer = process->signal_restorer(signal);
    if (restorer == nullptr) {
        // No way back out of the handler, so honouring it would hang the
        // process. Fall back to the default action, which is at least honest.
        if (signal_terminates_by_default(signal))
            do_exit(signal & 0x7F);
        return frame;
    }

    SignalContext context;
    context.frame = *frame;
    context.signal = static_cast<u64>(signal);

    // Skip the red zone the ABI lets a leaf function use below rsp.
    u64 stack = frame->rsp - 128;
    stack = align_down<u64>(stack, 16);
    stack -= sizeof(SignalContext);
    u64 const context_address = stack;

    stack -= sizeof(u64); // the return address the handler will use

    if (copy_to_user(context_address, &context, sizeof(context)).is_error()
        || copy_to_user(stack, &restorer, sizeof(restorer)).is_error()) {
        // The stack is unusable, which is usually a stack overflow. There is
        // nowhere to report it to, so terminate.
        do_exit(SIGSEGV & 0x7F);
    }

    // SA_RESTART means the handler is not supposed to be visible to a call
    // that was in progress. The context pushed above is the one sigreturn
    // restores, so the rewind has to happen there rather than here -- the
    // frame this function is editing is about to be replaced by the handler's.
    if ((process->signal_flags(signal) & SA_RESTART) != 0) {
        auto* thread = Scheduler::current();
        if (thread != nullptr && thread->has_restartable_syscall()) {
            auto* saved = reinterpret_cast<SignalContext*>(context_address);
            InterruptFrame restarted = context.frame;
            restarted.rax = thread->restartable_syscall();
            restarted.rip -= Thread::SYSCALL_INSTRUCTION_LENGTH;
            thread->clear_restartable_syscall();
            if (copy_to_user(reinterpret_cast<u64>(&saved->frame), &restarted, sizeof(restarted))
                    .is_error())
                do_exit(W_SIGNALLED(SIGSEGV));
        }
    }

    frame->rsp = stack;
    frame->rip = reinterpret_cast<u64>(handler);
    frame->rdi = static_cast<u64>(signal);
    frame->rax = 0;

    return frame;
}

[[noreturn]] void terminate_current_process(int wait_status)
{
    do_exit(wait_status);
}

} // namespace kernel::sys

extern "C" kernel::InterruptFrame* syscall_dispatch(kernel::InterruptFrame* frame)
{
    using namespace kernel;
    using namespace kernel::sys;

    __atomic_add_fetch(&s_syscall_count, 1, __ATOMIC_RELAXED);

    // Interrupts were masked by SFMASK on entry. Re-enable them: a syscall
    // that blocks must not do so with the timer off.
    interrupts_enable();

    u64 const number = frame->rax;
    SyscallHandler handler = nullptr;

    if (number < SYS_MAX_POSIX)
        handler = POSIX_SYSCALLS[number];
    else if (number >= SYS_EXT_BASE && number < SYS_EXT_BASE + SYS_MAX_EXT)
        handler = EXTENSION_SYSCALLS[number - SYS_EXT_BASE];

    auto* thread = Scheduler::current();
    if (thread != nullptr)
        thread->clear_restartable_syscall();

    if (handler == nullptr) {
        klog(LOG_WARN, "syscall", "%s called unimplemented syscall %llu",
            Process::current()->name(), number);
        frame->rax = static_cast<u64>(-ENOSYS);
    } else {
        // r10 rather than rcx for the fourth argument: the syscall instruction
        // clobbers rcx with the return address.
        auto result
            = handler(*frame, frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8, frame->r9);
        if (result.is_error()) {
            frame->rax = static_cast<u64>(-result.error().code());
            // Remember that this one can be taken again. Whether it is depends
            // on what the signal turns out to do, which only the delivery path
            // below knows.
            // nanosleep is the exception POSIX carves out: it reports the
            // interruption even under SA_RESTART, because restarting it would
            // silently sleep for longer than asked. Everything else may be
            // taken again.
            if (result.error().code() == EINTR && thread != nullptr && number != SYS_nanosleep)
                thread->set_restartable_syscall(number);
        } else {
            frame->rax = result.value();
        }
    }

    return deliver_pending_signal(frame);
}
