/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- multiboot2 entry and the climb into long mode.
 *
 * GRUB hands control here in 32-bit protected mode with paging off, which is
 * the only code in the whole kernel that runs that way. Its job is narrow:
 * stash what GRUB told us, build just enough page tables to have a virtual
 * address space, turn on long mode, and jump into the higher half where the
 * rest of the kernel lives.
 *
 * The bootstrap tables map three things, all with 2 MiB pages:
 *
 *   0x0000000000000000 + 4 GiB  identity     (so this code keeps executing)
 *   0xFFFF800000000000 + 4 GiB  HHDM         (physical memory, directly addressable)
 *   0xFFFFFFFF80000000 + 2 GiB  kernel image (where the kernel is linked)
 *
 * They are replaced wholesale once the real VMM comes up; nothing here needs
 * to be right beyond "enough to reach C++".
 */

#define KERNEL_VMA 0xFFFFFFFF80000000
#define PHYS(sym) ((sym) - KERNEL_VMA)

#define MB2_MAGIC 0xE85250D6
#define MB2_ARCH_I386 0

#define PTE_PRESENT 0x001
#define PTE_WRITE 0x002
#define PTE_HUGE 0x080

/* ------------------------------------------------------------------ */
/* multiboot2 header                                                   */
/* ------------------------------------------------------------------ */

.section .multiboot, "a", @progbits
.align 8
mb2_header:
    .long MB2_MAGIC
    .long MB2_ARCH_I386
    .long mb2_header_end - mb2_header
    .long -(MB2_MAGIC + MB2_ARCH_I386 + (mb2_header_end - mb2_header))

    /* Tell GRUB which tags we actually care about. */
    .align 8
mb2_tag_request:
    .short 1                      /* MULTIBOOT_HEADER_TAG_INFORMATION_REQUEST */
    .short 0
    .long mb2_tag_request_end - mb2_tag_request
    .long 6                       /* MULTIBOOT_TAG_TYPE_MMAP */
    .long 8                       /* MULTIBOOT_TAG_TYPE_FRAMEBUFFER */
    .long 3                       /* MULTIBOOT_TAG_TYPE_MODULE */
mb2_tag_request_end:

    /*
     * Ask for a linear RGB framebuffer. GRUB may refuse and hand back an EGA
     * text buffer instead, which the console code copes with.
     */
    .align 8
mb2_tag_framebuffer:
    .short 5                      /* MULTIBOOT_HEADER_TAG_FRAMEBUFFER */
    .short 0
    .long mb2_tag_framebuffer_end - mb2_tag_framebuffer
    .long 1024                    /* width */
    .long 768                     /* height */
    .long 32                      /* bits per pixel */
mb2_tag_framebuffer_end:

    /* Page-align loaded modules so the initrd starts on a page boundary. */
    .align 8
mb2_tag_module_align:
    .short 6                      /* MULTIBOOT_HEADER_TAG_MODULE_ALIGN */
    .short 0
    .long 8
mb2_tag_module_align_end:

    .align 8
mb2_tag_end:
    .short 0
    .short 0
    .long 8
mb2_header_end:

/* ------------------------------------------------------------------ */
/* 32-bit entry                                                        */
/* ------------------------------------------------------------------ */

.section .boot, "ax", @progbits
.code32
.global _start
.type _start, @function
_start:
    cli
    cld

    /* A stack, before anything else. The .bss lives high, so translate. */
    movl $PHYS(boot_stack_top), %esp

    /* GRUB's handshake: eax holds the magic, ebx the info structure. */
    movl %eax, PHYS(g_multiboot_magic)
    movl %ebx, PHYS(g_multiboot_info_phys)

    /*
     * Fill four page directories with 2 MiB pages covering 0..4 GiB. That is
     * 2048 entries; the carry into edx only matters for the very last one.
     */
    movl $PHYS(boot_pd), %edi
    xorl %eax, %eax
    xorl %edx, %edx
    movl $2048, %ecx
.Lfill_pd:
    movl %eax, %ebx
    orl $(PTE_PRESENT | PTE_WRITE | PTE_HUGE), %ebx
    movl %ebx, (%edi)
    movl %edx, 4(%edi)
    addl $0x200000, %eax
    adcl $0, %edx
    addl $8, %edi
    loop .Lfill_pd

    /* Low PDPT: four entries, one per PD, covering the first 4 GiB. */
    movl $PHYS(boot_pdpt_low), %edi
    movl $PHYS(boot_pd), %eax
    orl $(PTE_PRESENT | PTE_WRITE), %eax
    movl $4, %ecx
.Lfill_pdpt_low:
    movl %eax, (%edi)
    movl $0, 4(%edi)
    addl $4096, %eax
    addl $8, %edi
    loop .Lfill_pdpt_low

    /*
     * High PDPT. Slot 510 is 0xFFFFFFFF80000000 and slot 511 the gigabyte
     * above it, both aimed at the bottom of physical memory where the kernel
     * image actually sits.
     */
    movl $PHYS(boot_pdpt_high), %edi
    movl $PHYS(boot_pd), %eax
    orl $(PTE_PRESENT | PTE_WRITE), %eax
    movl %eax, 510*8(%edi)
    movl $0, 510*8+4(%edi)
    addl $4096, %eax
    movl %eax, 511*8(%edi)
    movl $0, 511*8+4(%edi)

    /* PML4: identity at 0, HHDM at slot 256, kernel at slot 511. */
    movl $PHYS(boot_pml4), %edi
    movl $PHYS(boot_pdpt_low), %eax
    orl $(PTE_PRESENT | PTE_WRITE), %eax
    movl %eax, 0(%edi)
    movl $0, 4(%edi)
    movl %eax, 256*8(%edi)
    movl $0, 256*8+4(%edi)
    movl $PHYS(boot_pdpt_high), %eax
    orl $(PTE_PRESENT | PTE_WRITE), %eax
    movl %eax, 511*8(%edi)
    movl $0, 511*8+4(%edi)

    /* CR4.PAE -- mandatory before long mode. */
    movl %cr4, %eax
    orl $(1 << 5), %eax
    movl %eax, %cr4

    movl $PHYS(boot_pml4), %eax
    movl %eax, %cr3

    /* EFER: LME to arm long mode, NXE so the NX bit in PTEs is not reserved. */
    movl $0xC0000080, %ecx
    rdmsr
    orl $((1 << 8) | (1 << 11)), %eax
    wrmsr

    /* Paging on. The instant this retires we are in compatibility mode. */
    movl %cr0, %eax
    orl $((1 << 31) | (1 << 0)), %eax
    movl %eax, %cr0

    lgdt boot_gdt_descriptor
    ljmp $0x08, $long_mode_entry

/* ------------------------------------------------------------------ */
/* 64-bit, still identity-mapped                                       */
/* ------------------------------------------------------------------ */

.code64
.type long_mode_entry, @function
long_mode_entry:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    /* An absolute jump is the only way out of the low identity mapping. */
    movabs $higher_half_entry, %rax
    jmp *%rax

/* ------------------------------------------------------------------ */
/* bootstrap GDT (flat, no TSS -- the real one is installed in C++)    */
/* ------------------------------------------------------------------ */

.align 16
boot_gdt:
    .quad 0x0000000000000000      /* null */
    .quad 0x00AF9A000000FFFF      /* 0x08 kernel code, L=1 */
    .quad 0x00CF92000000FFFF      /* 0x10 kernel data */
boot_gdt_end:

.align 16
boot_gdt_descriptor:
    .short boot_gdt_end - boot_gdt - 1
    .long boot_gdt   /* .boot is identity-mapped, so VMA == LMA here */

/* ------------------------------------------------------------------ */
/* higher half                                                         */
/* ------------------------------------------------------------------ */

.section .text, "ax", @progbits
.code64
.type higher_half_entry, @function
higher_half_entry:
    movabs $boot_stack_top, %rsp

    /* A zeroed rbp and a null return address stop backtraces cleanly. */
    xorq %rbp, %rbp
    pushq $0
    pushq $0

    movl g_multiboot_magic(%rip), %edi
    movl g_multiboot_info_phys(%rip), %esi
    call kernel_entry

    /* kernel_entry does not return, but if it ever did, stop the machine. */
.Lhalt:
    cli
    hlt
    jmp .Lhalt

/* ------------------------------------------------------------------ */
/* bootstrap page tables and stack                                     */
/* ------------------------------------------------------------------ */

.section .bss, "aw", @nobits

.global g_multiboot_magic
.global g_multiboot_info_phys
.align 8
g_multiboot_magic:
    .skip 4
g_multiboot_info_phys:
    .skip 4

.align 4096
.global boot_pml4
boot_pml4:
    .skip 4096
boot_pdpt_low:
    .skip 4096
boot_pdpt_high:
    .skip 4096
boot_pd:
    .skip 4096 * 4

.align 16
boot_stack_bottom:
    .skip 16384
.global boot_stack_top
boot_stack_top:

.section .note.GNU-stack, "", @progbits
