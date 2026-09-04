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
- A C library and twelve userland programs, reaching an interactive shell.
- FPU and SSE state preserved across context switches.
- **Lua 5.4 runs**, unpatched, from `ports/lua`. 68 of its own checks pass,
  including the floating point, string formatting, file I/O, garbage
  collection and error-unwinding paths.
- 158 kernel self-test assertions at every boot, plus a host-side libm
  accuracy check against glibc.

## Next

Roughly in the order that each one unblocks the most.

**Reference-counted inodes.** The most serious known bug, and demonstrated
rather than theorised. `TmpfsInode::unlink` destroys the inode immediately, so
a process that still has the file open is left holding a dangling pointer into
the kernel heap. Once that chunk is reused, the open handle reads whatever now
occupies it:

```lua
local f = io.open("/tmp/x", "w")  f:write("CANARY-DATA-1234")  f:close()
local g = io.open("/tmp/x", "r")
os.remove("/tmp/x")
for i = 1, 200 do                      -- churn the kernel heap
  local h = io.open("/tmp/c" .. i, "w")  h:write("ZZZZZZZZ")  h:close()
end
print(g:read("a"))                     --> ZZZZZZZZ, not CANARY-DATA-1234
```

So it is not only a use-after-free, it discloses another file's contents to a
process reading its own. The fix is a reference count on Inode, taken when a
FileDescription is created and dropped when the last one closes, with the free
deferred until it reaches zero. That also lets `tmpfile()` use the usual
create-then-unlink trick, which it currently cannot.

**A CMOS real-time clock.** `time()` currently reports seconds since boot,
because there is no clock to ask. That makes every timestamp and every date a
script prints wrong. The driver is small, and it would be the second loadable
module -- useful in itself, since one driver is not much evidence that the
module ABI generalises.

**rename(2).** There is no syscall for it, so `os.rename` fails with ENOSYS.

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
