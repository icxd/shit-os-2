<div align="center">

# shit os 2

**A hybrid x86-64 operating system, written from scratch.**
Not based on Linux. Not based on much else either.

</div>

![shit os 2 running](docs/screenshot.png)

---

Gen 1 was a 900-line 32-bit toy: no interrupts, no paging, no processes, and a
blue screen whenever anything at all went wrong. Gen 2 is a different animal.
It boots on real firmware through GRUB, runs in long mode at `-2 GiB`, loads
drivers at runtime against a versioned ABI, and reaches an interactive shell in
ring 3 with a C library written for it.

It is still not useful. It is now genuinely an operating system.

## What works

| | |
|---|---|
| **Boot** | Multiboot2 via GRUB, 32-bit trampoline into long mode, higher-half kernel, ISO built by `grub-mkrescue` |
| **CPU** | GDT/TSS, 256-vector IDT, named exceptions, 8259 with shared IRQ chains, CR0.WP + SMEP + NX |
| **Memory** | Bitmap physical allocator, 4-level paging, W^X kernel image, 4 GiB direct map, MMIO and module windows, slab-backed kernel heap |
| **Scheduling** | Preemptive round-robin at 250 Hz, per-CPU run queue behind a `gs` accessor, wait queues, sleeping, zombie reaping |
| **Filesystems** | VFS over a ustar initrd (ro), tmpfs, devfs |
| **Graphics** | `/dev/fb0` mapped straight into a process, `/dev/mouse0`, `/dev/kbdraw` with press and release events |
| **Modules** | ELF64 `.ko` loaded at runtime against a versioned ABI; PS/2 keyboard, PS/2 mouse and CMOS clock drivers, written in C |
| **Userland** | Ring 3, 50 POSIX syscalls, static ELF loading with a correct auxv, `fork`/`execve`/`waitpid`, pipes, signals with masking, `poll`/`select`, the `at` family, job control with process groups and sessions, a TTY with canonical line discipline |
| **libc** | Our own: stdio, an allocator that is not linear in the heap, a libm checked in ULPs, and a POSIX regex engine |
| **Programs** | 103 in `/bin`. Ours are `init` `sh` `ps` `free` `lsmod` `stty`; the coreutils come from sbase |
| **Ports** | **Lua 5.4**, **dash** and **sbase**, all unpatched, built against our libc |
| **Tests** | 220 assertions in the kernel at every boot, 343 more from ring 3 run by `/etc/rc` before the shell, and three host-side differential checks against glibc |

The shell has builtins, `PATH` lookup, pipelines, `<` `>` `>>` redirection and
quoting. `^C` interrupts the foreground command. A null dereference in a
program kills that program and nothing else.

`docs/syscalls.md` lists exactly what is *not* implemented, deliberately, so
nothing here has to be taken on trust.

## It runs Lua

![Lua running on shit os 2](docs/lua.png)

Lua 5.4.7 builds against our libc with **no patches** — its generic ISO C
configuration compiles unmodified. That was the point of choosing it: a port
needing patches would be a bug report about the libc, not about the program.

```
/ $ lua -v
Lua 5.4.7  Copyright (C) 1994-2024 Lua.org, PUC-Rio
/ $ lua
> print(("%.10f"):format(math.pi))
3.1415926536
> print(select(2, pcall(function() error("caught") end)))
stdin:1: caught
```

68 checks pass, covering integer and float arithmetic, the libm, string
formatting, pattern matching, tables, closures, metatables, coroutines,
`pcall` unwinding through `longjmp`, the garbage collector, and file I/O with
`seek`/`tell`/append. Run them yourself with
`lua /usr/share/lua/selftest.lua`.

Porting it found four real bugs, described in
[the roadmap](docs/roadmap.md) and in the commits that fixed them — including
one where the kernel was corrupting SSE registers on *every single context
switch*.

The source is not vendored. `ports/lua/build.sh` downloads the official
tarball, verifies its SHA-256 and builds it; `cmake -B build -DSHITOS_PORTS=OFF`
skips it if you would rather not have the network involved.

## It runs the coreutils

`ports/sbase/` builds **94 of suckless's coreutils**, unpatched, against our
libc. Ninety-four small programs is ninety-four different corners of POSIX, and
that is exactly why they are here.

```
/ $ ls /bin | wc -l
103
/ $ printf 'pear\napple\npear\nfig\n' | sort | uniq -c
      1 apple
      1 fig
      2 pear
/ $ find /bin -name 'sha*sum' | sed 's|.*/||' | sort | head -3
sha1sum
sha224sum
sha256sum
/ $ echo 'shit os 2' | sed 's/2/two/' | tr a-z A-Z
SHIT OS TWO
/ $ du -sh /usr/share/lua
6.0K    /usr/share/lua
```

That `sed` is running against a POSIX regular expression engine written for
this from the specification, because sbase's `util.h` includes `<regex.h>` and
so all ninety-four programs need one. `tools/check-regex.sh` builds it for the
host and asks it and glibc the same 6498 questions; they agree on all of them.

Porting it found **six real bugs**, all of them reachable long before anything
reached them. The worst: our `printf` did not recognise `%j`, so it never
consumed the length modifier, never saw the conversion after it, and took no
argument for it -- every conversion later in the format string then read the
wrong argument. `du`'s `"%jd\t%s\n"` passed a block count to `%s` and
segfaulted on `0x1`. That bug was as old as the libc.

The other five, and why each one only showed up now, are in
[the roadmap](docs/roadmap.md).

## Building

You need **clang**, **lld**, **cmake**, **ninja**, **grub-mkrescue**,
**xorriso** and **qemu**. You do not need to build a cross-compiler: clang is
one already, and the toolchain file points it at `x86_64-elf`.

```sh
# Debian / Ubuntu
sudo apt install clang lld cmake ninja-build xorriso qemu-system-x86 \
                 grub-common grub-pc-bin grub-efi-amd64-bin

# Arch
sudo pacman -S clang lld cmake ninja libisoburn qemu-system-x86 grub
```

Then:

```sh
cmake -B build -G Ninja
ninja -C build iso
```

## Running

```sh
ninja -C build run              # a window, with serial on stdout
ninja -C build run-headless     # no window; the terminal is the console

./tools/run-qemu.sh --debug     # wait for gdb on :1234
./tools/run-qemu.sh --expect-ok # boot headless, fail unless a shell appears
```

Both the emulated PS/2 keyboard and the serial line are wired to the same
terminal, so headless works exactly like the windowed version.

Once it boots:

```
/ $ help
/ $ uname -a
/ $ ls -l /bin
/ $ echo hello | cat
/ $ echo written > /tmp/a && cat /tmp/a
/ $ ps
/ $ free
/ $ lsmod
/ $ shutdown
```

## Layout

```
include/shitos/      ABI shared by the kernel, modules and libc
  abi/                 syscall numbers, errno, POSIX structs
  module/api.h         the driver ABI -- KernelApi v2

kernel/
  arch/x86_64/         everything CPU-specific lives behind this boundary
  boot/                multiboot2, parsed into a BootInfo the rest consumes
  mm/                  physical allocator, address spaces, heap
  sched/               threads, processes, scheduler, wait queues
  fs/                  VFS, ustar, tmpfs, devfs, pipes
  dev/                 console, framebuffer, TTY
  module/              the .ko loader and the KernelApi implementation
  sys/                 syscalls, ELF loading, fault handling
  lib/                 ErrorOr, spinlocks, containers, formatting

modules/ps2kbd/      a loadable driver, in C, using nothing but KernelApi
modules/rtc/         the CMOS clock, the second one
user/libc/           the C library, including the regex engine
user/libc/test/      host-side checks: libm accuracy, allocator, regex
user/bin/            init, sh, and the programs only this kernel can have
ports/               Lua, dash and sbase -- fetched and verified, never vendored
rootfs/              files copied into the image as-is, including /tests
tools/               mkinitrd, run-qemu, screenshot, genfont, and the check-* suites
```

## Documentation

- [Architecture](docs/architecture.md) — boot flow, memory layout, subsystems
- [Driver ABI](docs/driver-abi.md) — the module contract and how to write one
- [System calls](docs/syscalls.md) — the surface, and what is missing
- [Roadmap](docs/roadmap.md) — what is next and what is deliberately not
- [Contributing](CONTRIBUTING.md) — style, conventions, how to test
- [Third-party content](docs/third-party.md) — the little that is not written here

## Design decisions worth knowing

**It is hybrid, not monolithic.** The kernel owns memory, scheduling, the VFS
and syscalls. Drivers are separate objects loaded at runtime, and a module
never links against kernel symbols — it resolves one symbol and calls
everything else through a versioned table. That is what makes the kernel
refactorable, and what would let a driver move into a userspace server without
being rewritten.

**One CPU, but SMP-shaped.** Real spinlocks rather than `cli`/`sti` tricks,
per-CPU state reached through the GS base rather than globals, a run queue in
the per-CPU block. Bringing up the application processors is bring-up code,
not a restructuring.

**Errors are values.** `ErrorOr<T>` and `TRY()` throughout the kernel. No
exceptions, no RTTI, no sentinel return codes that can be ignored by accident.

**The self tests run on the machine.** An OS has no harness to run under, so
198 assertions run during boot, covering the physical allocator, the heap, W^X,
interrupt delivery, FPU state across context switches, inode lifetime, all
three filesystems, and the module loader. Then `init` runs `/etc/rc`, which
runs the ring 3 suite before the shell appears: the POSIX surface, a POSIX
shell script run by dash, Lua's own checks, and the file-lifetime regressions.
Most of the real bugs in this repository were found by these rather than by
inspection, and the ones dash found are written up in
[the roadmap](docs/roadmap.md).

Two things are better asked on the host than inside QEMU, and are asked of the
same source that ships: the libm, compared against glibc in ULPs
(`./tools/check-libm.sh`), and the allocator, for both correctness and
throughput (`./tools/check-malloc.sh`) — heap corruption surfaces long after
the call that caused it, and timing an allocator under emulation measures the
emulator.

**Ports are the other test.** Everything in `user/bin` was written against a
libc that was written for it, which proves nothing. Software nobody here wrote
is the only honest check, which is why Lua and dash are in the tree and why
neither is patched. dash in particular is what makes the job control layer
believable: it was written against Unix in 1997 and does not know or care what
it is running on.

## License

GPLv3. See [LICENSE](LICENSE).

The bitmap font is Terminus, under the SIL Open Font License 1.1 — see
[docs/third-party.md](docs/third-party.md). Everything else was written for
this project.
