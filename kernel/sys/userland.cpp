// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- starting the first user process.

#include <kernel/dev/console.h>
#include <kernel/fs/vfs.h>
#include <kernel/mm/heap.h>
#include <kernel/sched/process.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sys/elf_loader.h>
#include <kernel/sys/userland.h>

namespace kernel::sys {

ErrorOr<void> start_init(char const* path)
{
    auto* inode = TRY(fs::resolve(path));
    if (inode->is_directory())
        return Error::from_errno(EACCES);

    auto* process = TRY(Process::create("init", nullptr));

    auto* space = TRY(mm::AddressSpace::create_user_space());
    process->set_address_space(space);
    process->set_working_directory(fs::root_inode());

    auto executable = load_executable(*inode, *space);
    if (executable.is_error()) {
        space->destroy_user_mappings();
        kfree(space);
        return executable.error();
    }

    char const* argv[] = { path, nullptr };
    char const* envp[] = { "PATH=/bin", "HOME=/", "TERM=shitos", nullptr };

    auto stack = setup_user_stack(*space, executable.value(), argv, envp);
    if (stack.is_error()) {
        space->destroy_user_mappings();
        kfree(space);
        return stack.error();
    }

    process->set_brk_start(executable.value().brk_start);

    // Standard input, output and error all point at the terminal. init is
    // where that convention starts; every later process inherits it across
    // fork and execve.
    auto* terminal = TRY(fs::open("/dev/tty0", O_RDWR, 0));
    TRY(process->allocate_descriptor(terminal, 0));
    terminal->ref();
    TRY(process->allocate_descriptor(terminal, 1));
    terminal->ref();
    TRY(process->allocate_descriptor(terminal, 2));

    auto* thread = TRY(Thread::create_user_thread("init", executable.value().entry, stack.value()));
    process->add_thread(thread);
    Scheduler::enqueue(thread);

    klog(LOG_INFO, "init", "exec %s as pid %d, entry %p", path, process->pid(),
        reinterpret_cast<void*>(executable.value().entry));
    return {};
}

} // namespace kernel::sys
