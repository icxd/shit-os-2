// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- the KernelApi table handed to every module.
//
// This is the whole of the kernel as far as a driver is concerned. Adding a
// function here is an ABI change and needs SHITOS_MODULE_ABI_VERSION bumped;
// changing anything a function is implemented in terms of is not, which is the
// entire point of routing through a table instead of exporting symbols.

#include <kernel/arch/x86_64/interrupts.h>
#include <kernel/arch/x86_64/io.h>
#include <kernel/dev/console.h>
#include <kernel/fs/devfs.h>
#include <kernel/lib/new.h>
#include <kernel/mm/address_space.h>
#include <kernel/mm/heap.h>
#include <kernel/module/loader.h>
#include <kernel/panic.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/waitqueue.h>

namespace kernel {

namespace {

void api_log(LogLevel level, char const* module, char const* format, ...)
{
    va_list args;
    va_start(args, format);
    kvlog(level, module, format, args);
    va_end(args);
}

// __attribute__ rather than [[noreturn]]: clang folds the GNU attribute into
// the function *type*, and the ABI declares the table entry that way, so the
// C++ attribute spelling would not match.
__attribute__((noreturn)) void api_panic(char const* format, ...)
{
    // A module panicking is still a kernel panic; there is no fault isolation
    // for an in-kernel module, which is the trade we made for speed. Moving a
    // driver to a userspace server is what buys that back.
    va_list args;
    va_start(args, format);
    kvprintf(format, args);
    va_end(args);
    panic("module requested a panic");
}

void* api_kmalloc(usize size) { return kmalloc(size); }
void* api_kzalloc(usize size) { return kzalloc(size); }
void api_kfree(void* pointer) { kfree(pointer); }

void* api_map_mmio(u64 physical, usize length)
{
    auto result = mm::map_mmio(phys(physical), length);
    return result.is_error() ? nullptr : result.value();
}

void api_unmap_mmio(void* address, usize length) { mm::unmap_mmio(address, length); }

u8 api_inb(u16 port) { return arch::inb(port); }
u16 api_inw(u16 port) { return arch::inw(port); }
u32 api_inl(u16 port) { return arch::inl(port); }
void api_outb(u16 port, u8 value) { arch::outb(port, value); }
void api_outw(u16 port, u16 value) { arch::outw(port, value); }
void api_outl(u16 port, u32 value) { arch::outl(port, value); }

ModuleResult api_irq_register(u8 irq, IrqHandler handler, void* self)
{
    return arch::register_irq_handler(irq, handler, self);
}

void api_irq_unregister(u8 irq, void* self) { arch::unregister_irq_handler(irq, self); }

ModuleResult api_device_register(DeviceDescriptor const* device)
{
    if (device == nullptr)
        return MODULE_ERR_INVALID;

    auto* devfs = fs::DevfsFileSystem::the();
    if (devfs == nullptr)
        return MODULE_ERR_NO_DEVICE;

    auto result = devfs->register_device(*device);
    if (result.is_error())
        return result.error().code() == ENOMEM ? MODULE_ERR_NO_MEMORY : MODULE_ERR_BUSY;
    return MODULE_OK;
}

void api_device_unregister(char const* name)
{
    if (auto* devfs = fs::DevfsFileSystem::the(); devfs != nullptr)
        devfs->unregister_device(name);
}

// The module ABI declares WaitQueue as an opaque struct in the global
// namespace; internally it is kernel::WaitQueue. The casts are confined here.
::WaitQueue* api_waitqueue_create()
{
    auto* queue = static_cast<kernel::WaitQueue*>(kmalloc(sizeof(kernel::WaitQueue)));
    if (queue == nullptr)
        return nullptr;
    new (queue) kernel::WaitQueue();
    return reinterpret_cast<::WaitQueue*>(queue);
}

void api_waitqueue_destroy(::WaitQueue* queue)
{
    auto* real = reinterpret_cast<kernel::WaitQueue*>(queue);
    if (real == nullptr)
        return;
    real->wake_all();
    real->~WaitQueue();
    kfree(real);
}

void api_waitqueue_wait(::WaitQueue* queue)
{
    reinterpret_cast<kernel::WaitQueue*>(queue)->wait();
}

void api_waitqueue_wake_all(::WaitQueue* queue)
{
    reinterpret_cast<kernel::WaitQueue*>(queue)->wake_all();
}

u64 api_uptime_ms() { return Scheduler::uptime_ms(); }
void api_sleep_ms(u64 milliseconds) { Scheduler::sleep_ms(milliseconds); }
void api_yield() { Scheduler::yield(); }

constinit KernelApi const s_kernel_api = {
    .abi_version = SHITOS_MODULE_ABI_VERSION,
    ._reserved = 0,

    .log = api_log,
    .panic = api_panic,

    .kmalloc = api_kmalloc,
    .kzalloc = api_kzalloc,
    .kfree = api_kfree,
    .map_mmio = api_map_mmio,
    .unmap_mmio = api_unmap_mmio,

    .inb = api_inb,
    .inw = api_inw,
    .inl = api_inl,
    .outb = api_outb,
    .outw = api_outw,
    .outl = api_outl,

    .irq_register = api_irq_register,
    .irq_unregister = api_irq_unregister,

    .device_register = api_device_register,
    .device_unregister = api_device_unregister,

    .waitqueue_create = api_waitqueue_create,
    .waitqueue_destroy = api_waitqueue_destroy,
    .waitqueue_wait = api_waitqueue_wait,
    .waitqueue_wake_all = api_waitqueue_wake_all,

    .uptime_ms = api_uptime_ms,
    .sleep_ms = api_sleep_ms,

    .yield = api_yield,
};

} // namespace

KernelApi const& ModuleLoader::kernel_api() { return s_kernel_api; }

} // namespace kernel
