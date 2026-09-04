# Architecture

shit os 2 is a hybrid kernel for x86-64. It is not based on Linux, or on any
other kernel; the only third-party content in the tree is a bitmap font.

The short version of the design:

- **Hybrid, not monolithic.** The kernel owns memory, scheduling, the VFS and
  the syscall surface. Drivers are separate ELF objects loaded at runtime
  against a versioned ABI, and can be moved into userspace later without being
  rewritten.
- **Modularity is a runtime property.** A module resolves exactly one symbol
  from the kernel and calls everything else through a table. That is what lets
  the kernel be refactored without breaking drivers.
- **SMP-ready with one CPU.** Real spinlocks, per-CPU state behind a `gs`
  accessor, a run queue in the per-CPU block. Bringing up more cores is
  bring-up code, not a restructuring.
- **Errors are values.** `ErrorOr<T>` and `TRY()` everywhere in the kernel, so
  ignoring a failure has to be deliberate.

---

## Boot

```
  GRUB (multiboot2)
        |  loads kernel.elf and initrd.tar, enters at 1 MiB
        |  in 32-bit protected mode with paging off
        v
  arch/x86_64/boot.S            .boot, identity mapped at 1 MiB
        |  build bootstrap page tables (2 MiB pages)
        |  CR4.PAE -> EFER.LME|NXE -> CR0.PG -> lgdt -> ljmp
        v
  long_mode_entry                still identity mapped
        |  movabs + jmp: the only way out of the low mapping
        v
  higher_half_entry              0xFFFFFFFF80000000
        |
        v
  kernel_entry(magic, mbi)      kernel/main.cpp
```

`.boot` is the one part of the kernel linked at its physical address.
Everything else is linked at `-2 GiB` and loaded low via `AT()` in the linker
script. The bootstrap tables map three things — a 4 GiB identity map so the
trampoline keeps executing, the direct map, and the kernel image — and are
replaced wholesale once the real VMM is up.

Bring-up order in `kernel_entry`, and why:

| Step | Why here |
|---|---|
| `serial_initialize` | So a failure in any later step can be reported. |
| `parse_multiboot2` | Everything below needs the memory map. |
| `framebuffer_initialize` | Console for a human; serial already works. |
| `cpu_initialize` | CR0.WP before any mapping is trusted to be read-only. |
| `gdt_initialize` | Must precede the per-CPU block: loading `%gs` zeroes the GS base. |
| `percpu_initialize_bootstrap` | Reads itself back to prove the GDT did not clobber it. |
| `idt_initialize` | A working IDT is worth the most *during* memory bring-up. |
| `physical_initialize` | Bitmap over usable RAM. |
| `virtual_memory_initialize` | Real address space; the identity map disappears here. |
| `heap_initialize` | Needs pages from the physical allocator. |
| `pit_initialize`, `Scheduler::initialize` | Preemption. |
| `mount_boot_filesystems` | initrd, tmpfs, devfs. |
| `ModuleLoader::load_all_from` | Drivers, which need the heap, the VFS and IRQs. |
| `syscall_initialize`, `faults_initialize` | The ring 3 boundary. |
| `Tty::initialize` | Needs `/dev/kbd0`, which a module registered. |
| `start_init` | pid 1. |

---

## Memory layout

```
0x0000000000000000  +--------------------------------+
                    |  user space                    |  per process
                    |    0x400000    program image   |
                    |    0x10000000000000  mmap area |
                    |    ~0x7FFFFFFFF000  stack      |
0x0000800000000000  +--------------------------------+
                    :  non-canonical hole            :
0xFFFF800000000000  +--------------------------------+
                    |  direct map of physical memory |  shared
                    |  (>= 4 GiB, 2 MiB pages, NX)   |
0xFFFFC00000000000  +--------------------------------+
                    |  MMIO window for drivers       |  shared
0xFFFFFFFF80000000  +--------------------------------+
                    |  kernel image, W^X per section |  shared
0xFFFFFFFF90000000  +--------------------------------+
                    |  loadable modules              |  shared
                    +--------------------------------+
```

Two of these placements are load-bearing rather than arbitrary:

- The **direct map** always covers at least the low 4 GiB even on a machine
  with less RAM than that, because legacy MMIO — the framebuffer above all —
  lives in that range and has to stay addressable.
- The **module window** sits 256 MiB above the kernel image specifically so
  that a module's `R_X86_64_PC32` relocations to compiler intrinsics still
  fit in a 32-bit displacement. Modules in the direct map would be more than
  2 GiB away and every such relocation would overflow.

Every address space shares the upper half. `virtual_memory_initialize`
pre-allocates all 256 upper-half PDPTs up front, so a new process copies 256
PML4 entries once and then sees every later kernel mapping automatically.

**Physical memory** is a bitmap, one bit per 4 KiB frame, seeded from the
multiboot2 map. It starts from "everything is taken" and frees only what the
firmware explicitly called usable: being wrong in that direction wastes
memory, being wrong the other way corrupts it.

**The kernel heap** is segregated free lists over slab pages for allocations
up to 2032 bytes, and whole pages above that. Every allocation carries a
16-byte header with a magic value, so a double free or an overrun into the
next header is caught rather than acted on.

---

## Scheduling

A thread is a kernel stack with a saved `InterruptFrame` on top of it. There is
no separate context-switch routine, because the interrupt entry path already
saves and restores exactly the state a switch needs:

```
  interrupt or syscall
        |
        v
  isr_common / syscall_entry     push the frame
        |
        v
  interrupt_dispatch(frame) -> InterruptFrame*
        |                          |
        |                          +-- usually the same frame
        |                          +-- a different thread's frame == a switch
        v
  movq %rax, %rsp                  <- the switch happens here
  isr_return                       pop, iretq
```

New threads get a hand-built frame so their first run is indistinguishable
from a resume. A voluntary yield raises `int $0xFE` rather than doing anything
special, so voluntary and involuntary switches are the same code.

Round robin, five ticks (20 ms) at 250 Hz. Exited threads are reaped by the
idle thread, so a thread is never freeing the stack it is standing on.

---

## Filesystems

```
  /        ustar    ro    the initrd GRUB loaded, parsed at mount time
  /dev     devfs    rw    synthetic; where device_register lands
  /tmp     tmpfs    rw    heap backed
```

Three filesystems with nothing in common, all implementing `Inode`. Every
`Inode` operation has a default that fails with the errno POSIX specifies for
that operation, so a filesystem implements only what it supports and the rest
is correct rather than absent.

A regular file in the initrd does not copy its contents: the inode points
straight into the pages GRUB loaded, which the physical allocator reserved.

Inodes are reference counted. A directory holds one reference to each child it
names, an open `FileDescription` holds one, and so does anything else keeping a
pointer for a while -- a process's working directory, a mount point, the TTY's
keyboard. `unlink` drops the directory's reference and marks the inode
unlinked, which severs its parent link; the memory goes back only when the
count reaches zero. That is what POSIX promises about a file that is removed
while it is open, and getting it wrong cost a use-after-free that leaked one
file's contents into another's reader -- see `docs/roadmap.md`.

There is still no cache eviction: a live inode stays in memory, which is fine
when the inode *is* the file, and will need revisiting when a disk driver
arrives.

Two counts are in play and they are not the same one. A `FileDescription` is
also counted, because `dup` and `fork` share one; three descriptors onto one
description still hold the inode exactly once. `fs::release_description` is the
single place the two meet, so the pairing lives in one function rather than at
every call site that closes a file.

---

## The ring 3 boundary

The rules the kernel holds to:

1. **No user pointer is ever dereferenced directly.** Every syscall argument
   that is a pointer goes through `copy_from_user`/`copy_to_user`, which
   validate the range against the calling process's own page tables. A bad
   pointer is `EFAULT`, never a kernel fault.
2. **A fault in ring 3 is a signal, not a panic.** `sys/fault.cpp` maps the
   faults a program can plausibly cause onto signals. A fault in ring 0 is
   still a panic, because the kernel is already wrong at that point.
3. **The kernel never executes user memory.** SMEP is enabled where the CPU
   has it, and the direct map is NX.
4. **`sigreturn` cannot be used to escape.** The restored frame's segment
   selectors and interrupt flag are forced, so a forged signal context cannot
   return into ring 0.

See [syscalls.md](syscalls.md) for the call surface and
[driver-abi.md](driver-abi.md) for the module contract.
