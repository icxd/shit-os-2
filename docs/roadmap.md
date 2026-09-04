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
- Reference-counted inodes: unlinking a file that is still open no longer
  frees it, so `tmpfile()` works and an open handle cannot read another
  file's bytes.
- **Lua 5.4 runs**, unpatched, from `ports/lua`. 68 of its own checks pass,
  including the floating point, string formatting, file I/O, garbage
  collection and error-unwinding paths.
- A libc allocator that is not linear in the size of the heap. It was: `free`
  walked every block on every call, and Lua building and collecting 20000
  small tables took 41 seconds. Now 41 ns per malloc/free pair on the host,
  and the whole boot-plus-test sequence finishes in about a second.
- 176 kernel self-test assertions at every boot, a userland suite run from
  `/etc/rc` before the shell, and two host-side checks -- libm accuracy
  against glibc, and the allocator.

## Next

Roughly in the order that each one unblocks the most.

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

**An inode cache with eviction.** Inodes are reference counted now, but a live
one is never dropped from memory while its name exists. That is fine for
RAM-backed filesystems, where the inode *is* the file, and not fine the moment
a disk is involved and the tree is larger than RAM.

**Job control.** Process groups, sessions and `tcsetpgrp`, so `^C` goes to a
foreground *group* rather than to whichever process last read the terminal.

## Fixed, and worth remembering

**The unlink-while-open use-after-free.** `TmpfsInode::unlink` used to destroy
the inode immediately, so a process that still had the file open was left with
a dangling pointer into the kernel heap. Once that chunk was reused, the open
handle read whatever now occupied it:

```lua
local f = io.open("/tmp/x", "w")  f:write("CANARY-DATA-1234")  f:close()
local g = io.open("/tmp/x", "r")
os.remove("/tmp/x")
for i = 1, 200 do                      -- churn the kernel heap
  local h = io.open("/tmp/c" .. i, "w")  h:write("ZZZZZZZZ")  h:close()
end
print(g:read("a"))                     --> ZZZZZZZZ, not CANARY-DATA-1234
```

So it was not only a use-after-free: it disclosed another file's contents to a
process reading its own. Inodes now carry a reference count, taken by the
directory that names them and by every open `FileDescription`, with the free
deferred until it reaches zero. `/tests/uaf.lua` runs that exact reproduction
at every boot, and `test_inode_lifetime` in `kernel/selftest.cpp` checks the
counts directly.

**A libc `free` that was linear in the heap.** Every call walked the entire
block chain looking for adjacent free runs, which is invisible until something
allocates in earnest. Lua's garbage collection test took 41 seconds, virtually
all of it in that walk. Blocks are now doubly linked in address order so
coalescing is two O(1) checks, and free blocks are threaded onto size-class
lists through their own payloads, so the header did not have to grow.
`tools/check-malloc.sh` builds the shipped `stdlib.c` for the host and fails if
throughput goes back to linear.

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
