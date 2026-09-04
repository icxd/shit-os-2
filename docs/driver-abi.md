# Driver ABI, version 2

A driver in shit os 2 is a `.ko`: an ordinary ELF64 relocatable object that the
kernel loads at runtime. The whole contract is in
[`include/shitos/module/api.h`](../include/shitos/module/api.h).

## The one rule

**A module never links against kernel symbols.** The loader resolves exactly
one symbol out of the object — `shitos_module` — and everything the module is
allowed to touch arrives through the single `KernelApi` table handed to its
init function.

Two things follow from that, and they are the reason for the design:

1. The kernel can be refactored freely. Nothing internal is ABI. Only the
   structs in `api.h` are, and they are versioned.
2. A version mismatch is caught at load time by an integer comparison, instead
   of showing up later as an unexplained crash.

Because a module only ever calls through a table of function pointers, the
same driver source can later run in a userspace server with the table backed
by IPC stubs instead of direct calls. That is the intended migration path for
anything that does not need to be in the kernel.

### The one exception

The loader will satisfy a small set of compiler intrinsics from outside the
object: `memcpy`, `memmove`, `memset`, `memcmp`, `strlen`, `strcmp`, `strncmp`,
`strcpy`, `strncpy`.

These are not kernel API. They are the freestanding C runtime clang assumes
exists — it lowers struct assignment and array initialisation into `memcpy` and
`memset` calls no matter what the source says. Reference anything else and the
module is refused, by name:

```
[ERR] module   ahci.ko: undefined symbol 'kmalloc'
[ERR] module     modules may only call through KernelApi; see docs/driver-abi.md
```

## Writing one

```c
#include <shitos/module/api.h>
#include <shitos/types.h>

static const KernelApi* g_kernel;

static isize thing_read(void* self, void* buffer, usize length, u64 offset)
{
    (void)self; (void)offset;
    ((char*)buffer)[0] = '!';
    return length ? 1 : 0;
}

static const DeviceOps THING_OPS = {
    .read = thing_read,
    .write = 0,
    .ioctl = 0,
    .poll_readable = 0,
};

static const DeviceDescriptor THING = {
    .name = "thing0",              /* becomes /dev/thing0 */
    .type = DEVICE_TYPE_CHAR,
    .self = 0,
    .ops = &THING_OPS,
};

static ModuleResult module_init(const KernelApi* kernel)
{
    if (kernel->abi_version < SHITOS_MODULE_ABI_VERSION)
        return MODULE_ERR_ABI_MISMATCH;

    g_kernel = kernel;
    return kernel->device_register(&THING);
}

static void module_fini(void)
{
    g_kernel->device_unregister("thing0");
}

SHITOS_MODULE("thing", "an example", "you", "GPL-3.0-or-later",
              module_init, module_fini);
```

Then in `modules/CMakeLists.txt`:

```cmake
add_shitos_module(thing SOURCES thing/thing.c)
```

`mkinitrd` picks up every `.ko` in the build tree and puts it in
`/lib/modules`, which the kernel scans at boot.

[`modules/ps2kbd/ps2kbd.c`](../modules/ps2kbd/ps2kbd.c) is a real one — an
interrupt handler, a ring buffer, a wait queue and a device node in about 250
lines. It is written in C deliberately, to demonstrate that a module needs
nothing from the kernel's language or headers.

## What the table offers

| Group | Entries |
|---|---|
| Diagnostics | `log`, `panic` |
| Memory | `kmalloc`, `kzalloc`, `kfree`, `map_mmio`, `unmap_mmio` |
| Port I/O | `inb` `inw` `inl` `outb` `outw` `outl` (null on non-x86) |
| Interrupts | `irq_register`, `irq_unregister` |
| Devices | `device_register`, `device_unregister` |
| Blocking | `waitqueue_create` / `_destroy` / `_wait` / `_wake_all` |
| Time | `uptime_ms`, `sleep_ms` |
| Scheduling | `yield` |

## Rules the kernel will not enforce for you

- **An IRQ handler runs in interrupt context.** Do not block, do not allocate,
  do not take a lock that anything outside interrupt context holds. Return
  `true` only if the interrupt was yours; returning `false` lets the kernel
  keep walking the shared-IRQ chain.
- **`waitqueue_wait` needs a thread.** It is illegal from an interrupt handler,
  because there is nothing there to block. `waitqueue_wake_all` is the one that
  is safe from interrupt context, and is how a device interrupt releases a
  reader.
- **`self` and `ops` must outlive the registration.** The kernel keeps the
  pointers, not copies. `module_fini` is where you take them back.
- **A module is in the kernel.** There is no fault isolation: a null
  dereference in a driver is a kernel panic. That is the trade for speed, and
  it is what moving a driver into a userspace server would buy back.

## What the loader does

1. Reject anything that is not an x86-64 `ET_REL` object.
2. Lay out the allocatable sections, giving executable sections whole pages of
   their own — page permissions are page-granular, so packing `.text` and
   `.data` together would mean `.text` losing write permission takes `.data`
   with it.
3. Resolve the symbol table, refusing any undefined symbol that is not an
   intrinsic.
4. Apply relocations: `R_X86_64_64`, `PC32`, `PLT32`, `PC64`, `32`, `32S`.
   A displacement that does not fit is an error rather than a truncation.
5. Find `shitos_module` and check `abi_version` **before** running any of its
   code.
6. Drop `.text` to read-execute.
7. Call `init`.

## Changing this ABI

Bump `SHITOS_MODULE_ABI_VERSION` whenever anything in `api.h` changes shape:
a new field, a reordered struct, a changed signature. Adding a function to
`KernelApi` counts. Changing what a function is *implemented in terms of* does
not — that is the entire point.

**`KernelApi` only ever gains entries, at the end.** That single rule is what
makes the version number useful rather than decorative: a kernel can serve any
module built against a version at or below its own, because every entry that
module knows about is still exactly where it expects it. The reverse cannot
work, and the loader refuses it by number rather than letting a module call
through a pointer past the end of the struct.

So a module asks whether the kernel is new enough, not whether it matches:

```c
if (kernel->abi_version < SHITOS_MODULE_ABI_VERSION)
    return MODULE_ERR_ABI_MISMATCH;
```

and must not touch an entry newer than the version it tested for.

Reordering or removing an entry, or changing a signature, is a different kind
of change: it breaks every existing module and needs `SHITOS_MODULE_ABI_MIN_VERSION`
raised so the loader stops accepting them.

### What version 2 added

`time_source_register` and `time_source_unregister`, so a driver can hand the
kernel a wall clock rather than only take services from it. `modules/rtc` reads
the CMOS and registers one; the kernel reads it once, pins the difference
against its own monotonic clock, and answers every later query from that
offset. See `kernel/sys/clock.h`.

Version 2 is also the first real test of the compatibility rule above — the
PS/2 keyboard driver was written against version 1 and needed no change beyond
relaxing its own check from `!=` to `<`.
