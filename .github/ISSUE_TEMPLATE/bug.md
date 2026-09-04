---
name: Bug
about: Something behaves wrongly, panics, or does not boot
labels: bug
---

**What happened**

**What you expected**

**How to reproduce**
The exact commands, starting from a clean tree:

```sh
cmake -B build -G Ninja
ninja -C build iso
./tools/run-qemu.sh --headless
# then: ...
```

**Boot log**
Everything from the banner to where it goes wrong. `--headless` puts the
serial console on stdout, which is the easiest way to capture it.

```
paste here
```

**If it panicked**
Include the register dump and backtrace. Addresses can be resolved with:

```sh
llvm-addr2line -e build/kernel/kernel.elf -f -C 0xffffffff801...
```

**Environment**
- QEMU version:
- clang version:
- Host:
- Did the boot self tests pass? (the log says how many, out of 144)
