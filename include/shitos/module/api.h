/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- loadable module ABI, version 1.
 *
 * A module is an ordinary ELF64 relocatable object (a .ko) that the kernel
 * loads at runtime. The important property of this ABI is what it does *not*
 * do: a module never links against kernel symbols. The loader resolves exactly
 * one symbol out of the object -- `shitos_module` -- and everything the module
 * is allowed to touch arrives through the single KernelApi table handed to its
 * init function.
 *
 * That has two consequences worth stating plainly:
 *
 *   1. The kernel can refactor freely. Nothing internal is ABI. Only the
 *      structs in this header are, and they are versioned.
 *   2. A version mismatch is caught at load time, by an integer comparison,
 *      instead of showing up later as an unexplained crash.
 *
 * Because a module only ever calls through a table of function pointers, the
 * same driver source can later run in a userspace server with the table backed
 * by IPC stubs instead of direct calls. That is the intended migration path;
 * see docs/driver-abi.md.
 */

#pragma once

#include <shitos/types.h>

#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bump this whenever anything below changes shape. Modules built against an
 * older value are refused by the loader rather than loaded and trusted.
 */
#define SHITOS_MODULE_ABI_VERSION 1

/* --- results ------------------------------------------------------------ */

typedef enum ModuleResult {
    MODULE_OK = 0,
    MODULE_ERR_ABI_MISMATCH = 1,
    MODULE_ERR_NO_DEVICE = 2,
    MODULE_ERR_NO_MEMORY = 3,
    MODULE_ERR_BUSY = 4,
    MODULE_ERR_INVALID = 5,
    MODULE_ERR_IO = 6,
} ModuleResult;

/* --- logging ------------------------------------------------------------ */

typedef enum LogLevel {
    LOG_DEBUG = 0,
    LOG_INFO = 1,
    LOG_WARN = 2,
    LOG_ERROR = 3,
} LogLevel;

/* --- interrupts --------------------------------------------------------- */

/*
 * Runs in interrupt context on the interrupted stack. Do not block, do not
 * allocate, do not take a lock that anything outside interrupt context holds
 * without disabling interrupts around it. Return true if the interrupt was
 * yours; returning false lets the kernel keep walking the shared-IRQ chain.
 */
typedef bool (*IrqHandler)(void* self, u8 irq);

/* --- devices ------------------------------------------------------------ */

typedef enum DeviceType {
    DEVICE_TYPE_CHAR = 0,
    DEVICE_TYPE_BLOCK = 1,
} DeviceType;

typedef struct DeviceOps {
    /* Any of these may be null; the kernel then fails the call with ENOTSUP. */
    isize (*read)(void* self, void* buf, usize len, u64 offset);
    isize (*write)(void* self, const void* buf, usize len, u64 offset);
    int (*ioctl)(void* self, u32 request, void* arg);
    /* Return true when a read would not block. Null means "always ready". */
    bool (*poll_readable)(void* self);
} DeviceOps;

typedef struct DeviceDescriptor {
    const char* name; /* appears in the tree as /dev/<name> */
    DeviceType type;
    void* self; /* passed back as the first argument of every op */
    const DeviceOps* ops;
} DeviceDescriptor;

/* --- the kernel-facing table -------------------------------------------- */

typedef struct WaitQueue WaitQueue; /* opaque to modules */

typedef struct KernelApi {
    u32 abi_version;
    u32 _reserved;

    /* Diagnostics. Output goes to every registered console. */
    void (*log)(LogLevel level, const char* module, const char* fmt, ...);
    __attribute__((noreturn)) void (*panic)(const char* fmt, ...);

    /* Memory. kmalloc returns null on failure; it never panics. */
    void* (*kmalloc)(usize size);
    void* (*kzalloc)(usize size);
    void (*kfree)(void* ptr);
    /*
     * Map device memory into the kernel address space, uncached. Returns null
     * if the range could not be mapped. Modules must not touch physical
     * addresses any other way.
     */
    void* (*map_mmio)(u64 phys, usize len);
    void (*unmap_mmio)(void* virt, usize len);

    /* Port I/O. x86-specific; on other architectures these are null. */
    u8 (*inb)(u16 port);
    u16 (*inw)(u16 port);
    u32 (*inl)(u16 port);
    void (*outb)(u16 port, u8 value);
    void (*outw)(u16 port, u16 value);
    void (*outl)(u16 port, u32 value);

    /* Interrupts. */
    ModuleResult (*irq_register)(u8 irq, IrqHandler handler, void* self);
    void (*irq_unregister)(u8 irq, void* self);

    /* Device registration. The name must stay valid for the module's life. */
    ModuleResult (*device_register)(const DeviceDescriptor* device);
    void (*device_unregister)(const char* name);

    /* Sleeping and waking. Only legal outside interrupt context. */
    WaitQueue* (*waitqueue_create)(void);
    void (*waitqueue_destroy)(WaitQueue* queue);
    void (*waitqueue_wait)(WaitQueue* queue);
    void (*waitqueue_wake_all)(WaitQueue* queue); /* safe from interrupt context */

    /* Time. */
    u64 (*uptime_ms)(void);
    void (*sleep_ms)(u64 ms);

    /* Cooperative yield, for drivers polling something slow. */
    void (*yield)(void);
} KernelApi;

/* --- the module descriptor ---------------------------------------------- */

typedef struct ModuleDescriptor {
    u32 abi_version; /* must equal SHITOS_MODULE_ABI_VERSION */
    u32 _reserved;
    const char* name;
    const char* description;
    const char* author;
    const char* license;
    ModuleResult (*init)(const KernelApi* kernel);
    void (*fini)(void);
} ModuleDescriptor;

/*
 * Every module ends with one of these. It defines the single symbol the loader
 * looks for, and wires it to the module's init/fini functions.
 *
 *   static ModuleResult module_init(const KernelApi* k) { ... }
 *   static void module_fini(void) { ... }
 *   SHITOS_MODULE("ps2kbd", "PS/2 keyboard", "icxd", "GPL-3.0-or-later",
 *                 module_init, module_fini);
 */
#define SHITOS_MODULE(mod_name, mod_desc, mod_author, mod_license, init_fn, fini_fn)               \
    __attribute__((used, visibility("default"))) const ModuleDescriptor shitos_module = {          \
        .abi_version = SHITOS_MODULE_ABI_VERSION,                                                  \
        ._reserved = 0,                                                                            \
        .name = (mod_name),                                                                        \
        .description = (mod_desc),                                                                 \
        .author = (mod_author),                                                                    \
        .license = (mod_license),                                                                  \
        .init = (init_fn),                                                                         \
        .fini = (fini_fn),                                                                         \
    }

#ifdef __cplusplus
}
#endif
