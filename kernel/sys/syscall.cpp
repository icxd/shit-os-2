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
#include <kernel/sys/elf_loader.h>
#include <kernel/sys/syscall.h>
#include <shitos/abi/fcntl.h>
#include <shitos/abi/mman.h>
#include <shitos/abi/signal.h>
#include <shitos/abi/syscall.h>
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

    klog(LOG_INFO, "syscall", "gate installed, %d POSIX calls + %d extensions",
        SYS_MAX_POSIX, SYS_MAX_EXT);
}

u64 syscall_count() { return __atomic_load_n(&s_syscall_count, __ATOMIC_RELAXED); }

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
    auto* description = TRY(fs::open(path, static_cast<int>(flags), static_cast<u32>(mode),
        process->working_directory()));

    auto fd = process->allocate_descriptor(description);
    if (fd.is_error()) {
        description->inode().on_description_closed(description->flags());
        description->~FileDescription();
        kfree(description);
        return fd.error();
    }

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
    for (int fd = 0; fd < static_cast<int>(MAX_FILE_DESCRIPTORS); ++fd) {
        auto description = parent->description_for(fd);
        if (description.is_error())
            continue;
        description.value()->ref();
        (void)child->allocate_descriptor(description.value(), fd);
    }

    auto thread = Thread::create_from_frame(parent->name(), frame);
    if (thread.is_error())
        return thread.error();

    child->add_thread(thread.value());
    child->set_brk_start(parent->brk());
    Scheduler::enqueue(thread.value());

    return static_cast<u64>(child->pid());
}

ErrorOr<u64> sys_execve(InterruptFrame& frame, u64 path_pointer, u64 argv_pointer, u64 envp_pointer,
    u64, u64, u64)
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
    auto reaped = process->reap_child(static_cast<pid_t>(pid), status, (options & WNOHANG) == 0);

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

ErrorOr<u64> sys_mmap(InterruptFrame&, u64 address, u64 length, u64 protection, u64 flags, u64 fd, u64)
{
    if (length == 0)
        return Error::from_errno(EINVAL);
    if ((flags & MAP_ANONYMOUS) == 0)
        return Error::from_errno(ENOTSUP); // file-backed mappings need a page cache
    if (static_cast<int>(fd) != -1 && (flags & MAP_ANONYMOUS) == 0)
        return Error::from_errno(EINVAL);

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

    TRY(process->address_space()->map_anonymous(virt(target), rounded, page_flags));
    return target;
}

ErrorOr<u64> sys_munmap(InterruptFrame&, u64 address, u64 length, u64, u64, u64, u64)
{
    if (length == 0 || (address & (PAGE_SIZE - 1)) != 0)
        return Error::from_errno(EINVAL);
    Process::current()->address_space()->unmap_range(virt(address), align_up<usize>(length, PAGE_SIZE));
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
    if (read_fd.is_error())
        return read_fd.error();
    auto write_fd = process->allocate_descriptor(write_end);
    if (write_fd.is_error()) {
        (void)process->close_descriptor(read_fd.value());
        return write_fd.error();
    }

    int const fds[2] = { read_fd.value(), write_fd.value() };
    TRY(copy_to_user(fds_pointer, fds, sizeof(fds)));
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

    TRY(parent->create(name, fs::InodeType::Directory, static_cast<u32>(mode)));
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

ErrorOr<u64> sys_ioctl(InterruptFrame&, u64 fd, u64 request, u64 argument, u64, u64, u64)
{
    auto* description = TRY(description_for(static_cast<int>(fd)));

    // ioctl arguments are small structs; bounce through the kernel so the
    // device never sees a raw user pointer.
    u8 scratch[256] = {};
    if (argument != 0)
        (void)copy_from_user(scratch, argument, sizeof(scratch));

    int const result = TRY(description->inode().ioctl(static_cast<u32>(request), scratch));

    if (argument != 0)
        (void)copy_to_user(argument, scratch, sizeof(scratch));

    return static_cast<u64>(result);
}

ErrorOr<u64> sys_kill(InterruptFrame&, u64 pid, u64 signal, u64, u64, u64, u64)
{
    auto* target = Process::by_pid(static_cast<pid_t>(pid));
    if (target == nullptr)
        return Error::from_errno(ESRCH);
    if (signal == 0)
        return static_cast<u64>(0); // the existence check form of kill(2)

    target->raise_signal(static_cast<int>(signal));
    return static_cast<u64>(0);
}

ErrorOr<u64> sys_sigaction(InterruptFrame&, u64 signal, u64 action_pointer, u64 old_pointer, u64, u64, u64)
{
    auto* process = Process::current();
    int const number = static_cast<int>(signal);
    if (number <= 0 || number >= NSIG)
        return Error::from_errno(EINVAL);

    if (old_pointer != 0) {
        struct sigaction previous = {};
        previous.sa_handler = reinterpret_cast<void (*)(int)>(process->signal_disposition(number));
        previous.sa_restorer = reinterpret_cast<void (*)()>(process->signal_restorer(number));
        TRY(copy_to_user(old_pointer, &previous, sizeof(previous)));
    }

    if (action_pointer != 0) {
        struct sigaction action = {};
        TRY(copy_from_user(&action, action_pointer, sizeof(action)));
        process->set_signal_action(number, reinterpret_cast<void*>(action.sa_handler),
            reinterpret_cast<void*>(action.sa_restorer));
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
    Scheduler::sleep_ms(milliseconds);
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
        case ThreadState::Running:
            entry.state = SHITOS_PROC_STATE_RUNNING;
            break;
        case ThreadState::Ready:
            entry.state = SHITOS_PROC_STATE_READY;
            break;
        case ThreadState::Zombie:
            entry.state = SHITOS_PROC_STATE_ZOMBIE;
            break;
        default:
            entry.state = SHITOS_PROC_STATE_BLOCKED;
            break;
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

    auto* entries = static_cast<struct shitos_procinfo*>(
        kzalloc(wanted * sizeof(struct shitos_procinfo)));
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

    auto copied = copy_to_user(pointer, entries, collector.count * sizeof(struct shitos_moduleinfo));
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
    default:
        break;
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
    case SIGCHLD:
    case SIGCONT:
    case SIGURG:
    case SIGWINCH:
        return false;
    default:
        return true;
    }
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

    if (handler == SIG_IGN)
        return frame;

    if (handler == SIG_DFL) {
        if (!signal_terminates_by_default(signal))
            return frame;
        // The low byte of a wait status is the terminating signal.
        do_exit(signal & 0x7F);
    }

    // A real handler. Push the interrupted context onto the user stack and
    // arrange for the handler to run with the restorer as its return address;
    // the restorer calls sigreturn, which puts the context back.
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

    frame->rsp = stack;
    frame->rip = reinterpret_cast<u64>(handler);
    frame->rdi = static_cast<u64>(signal);
    frame->rax = 0;

    return frame;
}

[[noreturn]] void terminate_current_process(int wait_status) { do_exit(wait_status); }

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

    if (handler == nullptr) {
        klog(LOG_WARN, "syscall", "%s called unimplemented syscall %llu",
            Process::current()->name(), number);
        frame->rax = static_cast<u64>(-ENOSYS);
    } else {
        // r10 rather than rcx for the fourth argument: the syscall instruction
        // clobbers rcx with the return address.
        auto result = handler(*frame, frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8,
            frame->r9);
        if (result.is_error())
            frame->rax = static_cast<u64>(-result.error().code());
        else
            frame->rax = result.value();
    }

    return deliver_pending_signal(frame);
}
