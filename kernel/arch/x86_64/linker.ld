/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- kernel link map.
 *
 * The kernel is loaded by GRUB at 1 MiB physical but runs at -2 GiB virtual.
 * The one exception is .boot: the trampoline that builds the first page tables
 * has to execute before paging exists, so it is linked identity-mapped and
 * stays that way. Everything after it is linked high and loaded low via AT().
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;
KERNEL_LMA = 0x00100000;

PHDRS
{
    boot PT_LOAD FLAGS(7);   /* RWX: 32-bit trampoline, discarded conceptually after boot */
    text PT_LOAD FLAGS(5);   /* R-X */
    rodata PT_LOAD FLAGS(4); /* R-- */
    data PT_LOAD FLAGS(6);   /* RW- */
}

SECTIONS
{
    . = KERNEL_LMA;
    __kernel_phys_start = .;

    .boot ALIGN(4K) :
    {
        /* GRUB scans the first 32 KiB of the file for this, so it goes first. */
        KEEP(*(.multiboot))
        *(.boot .boot.*)
    } :boot

    . = ALIGN(4K);
    . += KERNEL_VMA;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA)
    {
        __text_start = .;
        *(.text .text.*)
        __text_end = .;
    } :text

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA)
    {
        __rodata_start = .;
        *(.rodata .rodata.*)

        /* Constructors for the handful of global objects the kernel has. */
        . = ALIGN(8);
        __init_array_start = .;
        KEEP(*(.init_array .init_array.*))
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        __init_array_end = .;

        __rodata_end = .;
    } :rodata

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA)
    {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    } :data

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA)
    {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        . = ALIGN(4K);
        __bss_end = .;
    } :data

    __kernel_end = .;
    __kernel_phys_end = . - KERNEL_VMA;

    /DISCARD/ :
    {
        *(.eh_frame*)
        *(.note.GNU-stack)
        *(.comment)
        *(.interp)
    }
}
