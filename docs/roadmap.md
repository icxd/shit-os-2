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
- Ring 3: syscall gate, 51 POSIX calls, static ELF loading with correct auxv,
  fork/execve/waitpid, pipes, signals with real handler delivery, a TTY with
  canonical line discipline.
- A C library, reaching an interactive shell.
- FPU and SSE state preserved across context switches.
- Reference-counted inodes: unlinking a file that is still open no longer
  frees it, so `tmpfile()` works and an open handle cannot read another
  file's bytes.
- A CMOS real-time clock, as the second loadable module and the first user of
  driver ABI v2. `clock_gettime`, `gettimeofday`, real file timestamps.
- `fcntl`, `rename(2)`, `poll`, `select`, `sigprocmask`, `umask`.
- **Job control**: process groups, sessions, `tcsetpgrp`, stop signals, a
  terminal that changes hands, and `jobs`/`fg`/`bg`/`&` in the shell.
- **dash runs**, unpatched, from `ports/dash` -- which is what makes the job
  control layer believable, since dash did not have this kernel in mind.
- **Lua 5.4 runs**, unpatched, from `ports/lua`. 68 of its own checks pass,
  including the floating point, string formatting, file I/O, garbage
  collection and error-unwinding paths.
- A libc allocator that is not linear in the size of the heap. It was: `free`
  walked every block on every call, and Lua building and collecting 20000
  small tables took 41 seconds. Now 41 ns per malloc/free pair on the host,
  and the whole boot-plus-test sequence finishes in about a second.
- **sbase runs**: ninety-four of suckless's coreutils, unpatched, from
  `ports/sbase`. `grep`, `sed`, `find`, `sort`, `du`, `xargs`, `tar` and the
  rest of the set a shell script actually reaches for.
- A **POSIX regular expression engine** in the libc, written from the
  specification. sbase's `util.h` includes `<regex.h>`, so every one of those
  ninety-four programs needed it. Checked against glibc over 6498 cases.
- 220 kernel self-test assertions at every boot, 412 more from ring 3 run by
  `/etc/rc` before the shell, and five host-side checks -- libm accuracy in
  ULPs, the allocator, the regex engine against glibc, the TrueType rasteriser
  under the sanitizers, and the widget toolkit laid out, painted and fed
  escape sequences.

- **A widget toolkit, and a TrueType rasteriser under it.**
  `user/libui/truetype.c` parses the tables and rasterises quadratic outlines
  with anti-aliasing, written from the specification -- no FreeType, no stb.
  Above it, a retained-mode toolkit: a widget tree, a layout pass, boxes,
  labels, buttons, checkboxes, text fields with a caret, hover and focus.
- **A desktop.** `user/wsys/` is a window server running as an ordinary
  process -- real windows with titlebars, dragging, stacking, focus and
  click-to-raise. Clients reach it over named FIFOs and draw into shared
  memory both sides map, so a window's pixels are never sent anywhere; the
  compositor reads them where they already are, into a back buffer, and blits
  only the rectangle that changed.
- **A terminal, with a shell in it.** `openpty` is a syscall returning both
  ends of a pseudo-terminal at once, there being no `/dev/pts` to open them by
  name. Getting there forced the line discipline out of the console TTY and
  into a `LineDiscipline` that the console and every pty share, which is where
  it should have been all along. `user/libui/terminal.c` is the emulator as a
  widget -- a cell grid, 2000 lines of scrollback in a ring, and an
  interruptible parser for SGR colour, cursor addressing and erase -- and
  `user/wsys/terminal.c` is a hundred lines of wiring between the two.
- **Graphics, from the bottom up.** `/dev/fb0` hands a process the real
  framebuffer through `mmap` rather than a copy of it; a PS/2 mouse driver is
  the third loadable module and needed no ABI additions at all, which is the
  first real evidence that ABI was designed rather than guessed; and
  `/dev/kbdraw` carries key *press and release* with modifiers, which is
  everything the TTY's character stream throws away. `gfxtest --check` proves
  all of it from ring 3 at every boot.

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

**An inode cache with eviction.** Inodes are reference counted now, but a live
one is never dropped from memory while its name exists. That is fine for
RAM-backed filesystems, where the inode *is* the file, and not fine the moment
a disk is involved and the tree is larger than RAM.

**Users, and a real `/etc/passwd`.** Everything runs as uid 0, `getpwnam`
answers for one hardcoded account, and no mode bit is ever checked. `umask` is
applied at creation, which is the half that matters for getting the recorded
modes right before there is anything to check them against.

**A wait queue per inode, so `poll` can sleep on the right one.** It sleeps on
a single queue that every wake touches, which is correct and wakes more
threads than it needs to. Per-inode queues need a thread to be able to wait on
several at once, which needs more than the one list node a thread has.

**`sigsuspend` and `sigpending` as syscalls.** The libc `sigsuspend` polls,
because there is no call that swaps the mask and waits atomically. It is
race-free -- see the comment on it -- but a real one would not need a comment.

## Fixed, and worth remembering

**A desktop that ran at four frames a second.** Three causes, each locally
reasonable. The compositor tracked damage and applied it to the final blit --
after drawing the whole desktop gradient and every window in full, so moving
the cursor one pixel repainted the screen; the painter's clip existed and was
honoured everywhere, and the compositor simply never set it. A focused
window's shadow is fourteen stacked rounded rectangles and each was filled
across its whole area, including the part the opaque window covers a moment
later -- about five million alpha blends a frame, all discarded. And `poll`
slept on a four-millisecond timer, which a keystroke crosses three times on
its way to the screen.

206 ms a frame became 7.8 ms. The lesson worth keeping is that all three were
invisible until something printed a number: the code looked careful, the
damage tracking was real, and it was being applied one step too late. The
measurement went in first, and `wsysbench` is still there.

**Waking a poll without losing the wake.** One queue that every `wake_all`
wakes, rather than a per-inode queue a poll cannot register on -- a thread can
only be on one wait list, because the node is shared with the sleeper list.
The race that comes with it is the interesting part: a descriptor that becomes
ready between the readiness scan and the sleep would wake a queue nobody is on
yet, and with no timeout that sleep never ends. A generation counter read
*before* the scan and compared under the same lock the wake takes closes it --
the wake is either seen by the scan or seen by the compare.

**A shell that would not start, because `/dev/tty` did not exist.** The
terminal emulator came up, keystrokes echoed, and dash printed nothing. It was
stopped by `SIGTTIN`, sent by itself, from the loop that waits until the
terminal's foreground group is its own -- it was asking the wrong terminal.
`_PATH_TTY` was `/dev/tty0`, a real device: the console. So a shell under a pty
asked the console who owned the foreground, got somebody else's group, and
correctly concluded it was a background job. The fix was the missing piece:
a controlling terminal per process, claimed with `TIOCSCTTY`, inherited across
fork and exec, dropped by `setsid` -- and a `/dev/tty` that resolves to it on
every call rather than naming a device. The general lesson: *a name that looks
like a device but means "whichever one is yours" cannot be aliased to a real
one*, and the failure is silent until something asks the question properly.

**A pseudo-terminal that hung up on itself.** The pty copied the console and
adopted its first reader as the foreground process group. Closing the master
sends `SIGHUP` to the foreground group, and the master's owner *was* the
foreground group, so a terminal closing killed the program that closed it --
and, because that program shared a group with the boot script, the boot script
too. A console adopts readers because nothing else can decide who is in the
foreground; a pty has an emulator that knows, so it now waits to be told.

**A window that outlived the process that owned it.** The window server learns
a client is gone by reading end of file on its channel, and end of file needs
the last writer to close. The terminal forks a shell, and the shell inherited
a copy of the write end -- so closing the terminal left its window on screen
for as long as the shell lived. The channel is `O_CLOEXEC` now. The general
shape is worth keeping: *end of file is about the last holder, not the first*,
and any descriptor whose closing means something must not be allowed to leak
across an `exec`.

**A signal handler that returned to a random address.** `sa_restorer` is a
libc-internal field, so a program that fills a `struct sigaction` in field by
field -- as dash does, and as most software does -- leaves stack garbage in it.
Our libc only supplied its own trampoline when that field was zero, so dash's
first caught SIGTERM returned into the middle of `strdup` and died there. It
looked exactly like a corrupt heap, which is where two hours went. The libc now
overwrites the field unconditionally, which is what glibc does and why nobody
else hits this.

**A `sigsuspend` that waited for a signal already delivered.** The first
version looped until `nanosleep` reported EINTR. Unblocking the mask is itself
a syscall, so a pending signal is delivered on the way out of *that* call --
before the sleep starts -- and the loop then waited forever for a second one.
dash hung in `wait` perhaps half the time. It now waits briefly and returns
unconditionally, which every correct caller of sigsuspend already copes with,
because every one of them is a loop around a condition it re-tests.

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

**Six bugs the coreutils found.** Every one of them was reachable before and
nothing had reached it. That is the argument for porting software you did not
write: Lua exercises arithmetic, dash exercises signals, and ninety-four small
programs exercise the parts nobody thought to test.

- `printf` silently ignored `%j`, `%t` and `%L`. Not "printed them wrong" --
  the length modifier was not recognised, so the loop never consumed it, the
  conversion character after it was never seen, and no argument was taken for
  it. Everything after that point in the format string read the wrong
  argument. `du`'s `"%jd\t%s\n"` therefore handed a block *count* to `%s`,
  and it segfaulted dereferencing `0x1`. This had been in the libc since the
  first day it could print.
- `fmemopen` left the stream's descriptor at 0 rather than -1, so the first
  refill called `read(0)`. `grep`, `sed` and everything else that reads a
  buffer as a file failed with `EBADF`.
- `getcwd` failed anywhere under a mount point. Rebuilding the path walks
  parent pointers, and a mount root has no parent -- the walk stopped there
  instead of stepping across to the directory the filesystem is mounted on.
- The `at` family was a libc fiction that only handled `AT_FDCWD`. Anything
  recursive -- `du`, `rm -r`, `cp -r` -- descends by opening each directory and
  resolving against *that*, so all of it was broken. Now five real syscalls.
- `ARG_MAX` was 4096. `xargs` reserves exactly 4096 bytes for the environment
  before deciding how much room is left for arguments, which left none.
- `tsearch` returned the tree slot rather than the node. `du` writes through
  the result to replace its key, so it was corrupting the tree.

One thing that looked like a seventh was not: sbase's `rev` prints its input
unreversed. Compiling its loop on the host against glibc does the same, so it
is upstream's bug, not ours. `rootfs/tests/coreutils.sh` says so where the
check would have gone.

## Later

- Non-blocking I/O that means something. `poll` and `select` exist and are
  honest about pipes and terminals, but `O_NONBLOCK` can only be *set*: no
  `read` or `write` path consults it, so a reader still blocks.
- Users, permissions, and mode bits that are actually checked.
- A second architecture. The `arch/` boundary exists for it; aarch64 on QEMU
  `virt` is the obvious candidate, and would prove the boundary is honest.
- Userspace drivers. The module ABI was designed so a driver can move behind
  IPC without being rewritten; nothing has actually made that move yet.
- Networking. A long way out.
- The rest of the desktop. Windows do not resize, and there is no way for a
  client to ask to be a different size. There is no scroll view, no list, no
  menu. And there is no terminal emulator, because there are no
  pseudo-terminals -- which is the single thing that would make the desktop
  worth using rather than worth looking at.

## Not planned

- Being useful.
- POSIX conformance as a goal in itself. The target is running real software,
  which is a different and more honest bar.
- Supporting anything that is not QEMU without someone testing it on real
  hardware first and saying so.
