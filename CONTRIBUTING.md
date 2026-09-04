# Contributing

## Getting it building

```sh
sudo apt install clang lld cmake ninja-build xorriso qemu-system-x86 \
                 grub-common grub-pc-bin grub-efi-amd64-bin
cmake -B build -G Ninja
ninja -C build iso
./tools/run-qemu.sh --headless
```

There is no cross-toolchain to bootstrap. If `cmake` cannot find clang or lld,
that is the whole problem.

## Testing a change

There is no test harness an OS can run under, so the tests run on the machine
during boot. Any change to the kernel should keep them passing:

```sh
ninja -C build iso && ./tools/run-qemu.sh --expect-ok
```

That builds the image, boots it headless, and fails unless a shell prompt
appears. For anything below the shell, watch the assertion counts in the boot
log — 66 boot checks, 14 scheduler, 47 filesystem, 17 module.

**Add assertions for what you add.** `kernel/selftest.cpp` is the right place,
and it is cheap: a new subsystem should arrive with a handful of checks that
would have caught the bugs you hit writing it. Several of the checks in there
exist because they caught something real.

The libm is checked separately, against the host's glibc in ULPs, because the
boot-time tests have nothing to compare numerical results against:

```sh
./tools/check-libm.sh
```

And the real test of the C library is software nobody here wrote:

```sh
lua /usr/share/lua/selftest.lua      # inside the OS
```

To see the framebuffer rather than the serial log:

```sh
./tools/screenshot.sh --after 7 --keys "l s spc slash ret" /tmp/shot.png
```

Debugging with gdb:

```sh
./tools/run-qemu.sh --debug &
gdb build/kernel/kernel.elf -ex 'target remote :1234'
```

## Style

`.clang-format` is authoritative; run it before sending anything. The shape of
it, in prose:

- **Types are `CamelCase`, functions and variables are `snake_case`, private
  members carry `m_`.** Constants are `SCREAMING_SNAKE_CASE`.
- **Fallible operations return `ErrorOr<T>`,** and callers use `TRY()`. `MUST()`
  is only for failures that mean the kernel is already broken beyond repair —
  never for anything a user can trigger.
- **`VERIFY()` for invariants, `TODO()` for honest gaps.** A stub that returns
  a plausible wrong answer is worse than one that says it does not exist.
- **Four spaces, no tabs. Braces on the same line except for functions.**
- **`u8`/`u32`/`usize`, not `uint8_t`.** `PhysAddr` and `VirtAddr` are distinct
  types on purpose; do not paper over a mix-up with a cast.

C++ in the kernel is the zero-cost subset: no exceptions, no RTTI, no
heap-allocating standard containers. Use `kernel/lib/` — there is a `Vector`,
an intrusive list, `ErrorOr`, spinlocks and RAII guards.

## Comments

Comment **why**, not **what**. `// increment the counter` is noise; the line
below is not:

```cpp
// Page permissions are page-granular, so an executable section has to own
// whole pages. Packing .text and .data into one page means dropping write
// permission on .text also drops it on .data, and the module faults on its
// own first store.
```

If a piece of code exists because of something surprising — a hardware quirk, a
specification detail, an ordering constraint — say so where the code is. That
is the comment someone will need at 2am, and it is the only kind worth the
space.

## Where things go

| You are adding | It goes in |
|---|---|
| Anything CPU-specific | `kernel/arch/x86_64/` — and nowhere else |
| A device driver | `modules/` as a `.ko`, not the kernel |
| A filesystem | `kernel/fs/`, implementing `Inode` |
| A syscall | `kernel/sys/syscall.cpp` plus `include/shitos/abi/syscall.h` |
| A libc function | `user/libc/src/`, declared in the standard header |
| A program | `user/bin/`, one file, registered in its `CMakeLists.txt` |
| A third-party program | `ports/<name>/build.sh`, fetched and checksummed, never vendored |
| A file to ship in the image as-is | `rootfs/`, mirrored into the initrd |

**A port that needs patching is a bug report about our libc.** If a program
will not build, the interesting question is what we are missing, not how to
work around it. Lua is in the tree unpatched for that reason, and it found four
real bugs on the way in.

Two boundaries are worth defending:

**Nothing outside `arch/` may include an `arch/` header.** Core code reaches
hardware through drivers and the HAL, not directly. The whole point of the
boundary is that a second architecture is possible.

**A driver belongs in a module, not the kernel.** If you find yourself adding
device code to `kernel/dev/`, it probably wants to be a `.ko` — see
[docs/driver-abi.md](docs/driver-abi.md). The exceptions currently in there
(console, framebuffer, TTY) are things needed before the module loader exists.

## Adding a syscall

1. Give it a number in `include/shitos/abi/syscall.h`, and bump `SYS_MAX_POSIX`.
2. Write `sys_yourcall` in `kernel/sys/syscall.cpp` and add it to the table.
   **Every user pointer goes through `copy_from_user`/`copy_to_user`** — a bad
   pointer must be `EFAULT`, never a kernel fault. There are no exceptions to
   this and a patch that makes one will not be taken.
3. Add the wrapper to `user/libc/src/unistd.c` and the declaration to the
   header the standard puts it in.
4. Document it in `docs/syscalls.md`, including anything it does not do.

## Changing the module ABI

Bump `SHITOS_MODULE_ABI_VERSION` for any change to the shape of anything in
`include/shitos/module/api.h`. Adding a function to `KernelApi` counts.
Changing what a function is *implemented in terms of* does not.

## Commits

Explain why the change is right, not just what it does. If you found a bug
while writing it, say what it was — that is usually the most useful sentence
in the message. The existing history is the format.
