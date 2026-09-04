---
name: Driver
about: Propose or discuss a device driver
labels: driver
---

**Device**
What hardware, and how it is reached (port I/O, MMIO, PCI...).

**Why now**
What it unblocks. A block device unblocks a real filesystem; a serial mouse
unblocks nothing yet.

**Interface**
What it would register, and what a program would do with it:

```
/dev/...        character | block
ioctls:
```

**KernelApi**
Does everything it needs already exist in
[`include/shitos/module/api.h`](../../include/shitos/module/api.h)? If not, say
what is missing — adding to that table is an ABI version bump, so it is worth
deciding deliberately.

**Notes**
Datasheet links, spec sections, or the QEMU device it can be tested against.
