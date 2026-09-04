# Roadmap

What exists, what is next, and what is deliberately not being done yet.

## Done

- Multiboot2 boot into a higher-half long-mode kernel, GRUB-built ISO.
- GDT/TSS, IDT with named exceptions, 8259 remap, shared IRQ chains.
- CPU configuration: CR0.WP, CR4.PGE, CR4.SMEP, EFER.NXE/SCE.
- Bitmap physical allocator, 4-level paging, W^X kernel image, direct map,
  MMIO and module windows, kernel heap with guarded headers.
- Per-CPU state, PIT, preemptive round-robin scheduling, wait queues.
- VFS with a ustar initrd, tmpfs and devfs.
- Runtime-loadable driver modules against a versioned ABI. PS/2 keyboard.
- Ring 3: syscall gate, 33 POSIX calls, static ELF loading with correct auxv,
  fork/execve/waitpid, pipes, signals with real handler delivery, a TTY with
  canonical line discipline.
- A C library and eleven userland programs, reaching an interactive shell.
- 144 kernel self-test assertions that run at every boot.

## Next

Roughly in the order that each one unblocks the most.

**Copy-on-write fork.** `fork` currently copies every page eagerly, which is
pure waste in the fork-then-exec case a shell spends all its time in. The page
tables and the fault handler are both in place; this is a write-protect pass
plus a case in the page-fault path.

**A local APIC timer, then SMP.** The PIT works but is coarse and singular.
Parsing the MADT, starting the APs and giving each a run queue is the point of
the per-CPU indirection that is already there.

**A block layer and a real filesystem.** AHCI, a buffer cache, and ext2 behind
the same `Inode` interface. This is what turns the initrd from the root
filesystem into what it is supposed to be.

**procfs.** `shitos_procs`, `shitos_sysinfo` and `shitos_modules` are three
extension syscalls doing a filesystem's job. `/proc/<pid>/status` and
`/proc/meminfo` would let `ps` and `free` be ordinary programs that read files.

**A dynamic linker.** The kernel already lays out a correct auxiliary vector,
so `ld.so` and a shared `libc.so` are userland work rather than kernel work.

**Inode lifetime.** Inodes are owned by their filesystem and never freed. That
is fine for RAM-backed filesystems and not fine the moment a disk is involved.

**Job control.** Process groups, sessions and `tcsetpgrp`, so `^C` goes to a
foreground *group* rather than to whichever process last read the terminal.

## Later

- `select`/`poll`, and non-blocking I/O that means something.
- Users, permissions, and mode bits that are actually checked.
- A second architecture. The `arch/` boundary exists for it; aarch64 on QEMU
  `virt` is the obvious candidate, and would prove the boundary is honest.
- Userspace drivers. The module ABI was designed so a driver can move behind
  IPC without being rewritten; nothing has actually made that move yet.
- Networking. A long way out.
- A compositor. The framebuffer console is a text console on a linear
  framebuffer, chosen over VGA text mode precisely so this stays possible.

## Not planned

- Being useful.
- POSIX conformance as a goal in itself. The target is running real software,
  which is a different and more honest bar.
- Supporting anything that is not QEMU without someone testing it on real
  hardware first and saying so.
