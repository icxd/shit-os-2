// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- ELF64 structures.
//
// Shared by the loadable-module loader (which consumes ET_REL objects) and by
// the userland program loader (which consumes ET_EXEC). Only the fields we
// actually read are named; the rest keep their layout so offsets stay right.

#pragma once

#include <shitos/types.h>

namespace kernel::elf {

using Elf64_Addr = u64;
using Elf64_Off = u64;
using Elf64_Half = u16;
using Elf64_Word = u32;
using Elf64_Sword = i32;
using Elf64_Xword = u64;
using Elf64_Sxword = i64;

inline constexpr usize EI_NIDENT = 16;

inline constexpr u8 ELFMAG0 = 0x7F;
inline constexpr u8 ELFMAG1 = 'E';
inline constexpr u8 ELFMAG2 = 'L';
inline constexpr u8 ELFMAG3 = 'F';

inline constexpr u8 ELFCLASS64 = 2;
inline constexpr u8 ELFDATA2LSB = 1;

inline constexpr Elf64_Half ET_REL = 1;
inline constexpr Elf64_Half ET_EXEC = 2;
inline constexpr Elf64_Half ET_DYN = 3;

inline constexpr Elf64_Half EM_X86_64 = 62;

struct [[gnu::packed]] Elf64_Ehdr {
    u8 e_ident[EI_NIDENT];
    Elf64_Half e_type;
    Elf64_Half e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off e_phoff;
    Elf64_Off e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize;
    Elf64_Half e_phentsize;
    Elf64_Half e_phnum;
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;
    Elf64_Half e_shstrndx;
};

// --- program headers (executables) --------------------------------------

inline constexpr Elf64_Word PT_NULL = 0;
inline constexpr Elf64_Word PT_LOAD = 1;
inline constexpr Elf64_Word PT_DYNAMIC = 2;
inline constexpr Elf64_Word PT_INTERP = 3;
inline constexpr Elf64_Word PT_PHDR = 6;
inline constexpr Elf64_Word PT_TLS = 7;

inline constexpr Elf64_Word PF_X = 1;
inline constexpr Elf64_Word PF_W = 2;
inline constexpr Elf64_Word PF_R = 4;

struct [[gnu::packed]] Elf64_Phdr {
    Elf64_Word p_type;
    Elf64_Word p_flags;
    Elf64_Off p_offset;
    Elf64_Addr p_vaddr;
    Elf64_Addr p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
};

// --- section headers (relocatable objects) ------------------------------

inline constexpr Elf64_Word SHT_NULL = 0;
inline constexpr Elf64_Word SHT_PROGBITS = 1;
inline constexpr Elf64_Word SHT_SYMTAB = 2;
inline constexpr Elf64_Word SHT_STRTAB = 3;
inline constexpr Elf64_Word SHT_RELA = 4;
inline constexpr Elf64_Word SHT_NOBITS = 8;
inline constexpr Elf64_Word SHT_REL = 9;

inline constexpr Elf64_Xword SHF_WRITE = 0x1;
inline constexpr Elf64_Xword SHF_ALLOC = 0x2;
inline constexpr Elf64_Xword SHF_EXECINSTR = 0x4;

inline constexpr Elf64_Half SHN_UNDEF = 0;
inline constexpr Elf64_Half SHN_ABS = 0xFFF1;
inline constexpr Elf64_Half SHN_COMMON = 0xFFF2;

struct [[gnu::packed]] Elf64_Shdr {
    Elf64_Word sh_name;
    Elf64_Word sh_type;
    Elf64_Xword sh_flags;
    Elf64_Addr sh_addr;
    Elf64_Off sh_offset;
    Elf64_Xword sh_size;
    Elf64_Word sh_link;
    Elf64_Word sh_info;
    Elf64_Xword sh_addralign;
    Elf64_Xword sh_entsize;
};

struct [[gnu::packed]] Elf64_Sym {
    Elf64_Word st_name;
    u8 st_info;
    u8 st_other;
    Elf64_Half st_shndx;
    Elf64_Addr st_value;
    Elf64_Xword st_size;
};

constexpr u8 symbol_binding(u8 info) { return info >> 4; }
constexpr u8 symbol_type(u8 info) { return info & 0xF; }

inline constexpr u8 STB_LOCAL = 0;
inline constexpr u8 STB_GLOBAL = 1;
inline constexpr u8 STB_WEAK = 2;

inline constexpr u8 STT_NOTYPE = 0;
inline constexpr u8 STT_OBJECT = 1;
inline constexpr u8 STT_FUNC = 2;
inline constexpr u8 STT_SECTION = 3;

struct [[gnu::packed]] Elf64_Rela {
    Elf64_Addr r_offset;
    Elf64_Xword r_info;
    Elf64_Sxword r_addend;
};

constexpr u32 relocation_type(Elf64_Xword info) { return static_cast<u32>(info & 0xFFFFFFFF); }
constexpr u32 relocation_symbol(Elf64_Xword info) { return static_cast<u32>(info >> 32); }

// x86-64 relocation types the module loader implements.
inline constexpr u32 R_X86_64_NONE = 0;
inline constexpr u32 R_X86_64_64 = 1;     // S + A
inline constexpr u32 R_X86_64_PC32 = 2;   // S + A - P
inline constexpr u32 R_X86_64_PLT32 = 4;  // L + A - P, same as PC32 when static
inline constexpr u32 R_X86_64_32 = 10;    // S + A, zero-extended, must fit
inline constexpr u32 R_X86_64_32S = 11;   // S + A, sign-extended, must fit
inline constexpr u32 R_X86_64_PC64 = 24;  // S + A - P

// Dynamic-linking relocations, for the userland loader later.
inline constexpr u32 R_X86_64_RELATIVE = 8;
inline constexpr u32 R_X86_64_GLOB_DAT = 6;
inline constexpr u32 R_X86_64_JUMP_SLOT = 7;

// --- auxiliary vector (userland) ----------------------------------------

inline constexpr u64 AT_NULL = 0;
inline constexpr u64 AT_IGNORE = 1;
inline constexpr u64 AT_PHDR = 3;
inline constexpr u64 AT_PHENT = 4;
inline constexpr u64 AT_PHNUM = 5;
inline constexpr u64 AT_PAGESZ = 6;
inline constexpr u64 AT_BASE = 7;
inline constexpr u64 AT_FLAGS = 8;
inline constexpr u64 AT_ENTRY = 9;
inline constexpr u64 AT_UID = 11;
inline constexpr u64 AT_EUID = 12;
inline constexpr u64 AT_GID = 13;
inline constexpr u64 AT_EGID = 14;
inline constexpr u64 AT_SECURE = 23;
inline constexpr u64 AT_RANDOM = 25;

inline bool is_valid_elf64(Elf64_Ehdr const& header)
{
    return header.e_ident[0] == ELFMAG0 && header.e_ident[1] == ELFMAG1
        && header.e_ident[2] == ELFMAG2 && header.e_ident[3] == ELFMAG3
        && header.e_ident[4] == ELFCLASS64 && header.e_ident[5] == ELFDATA2LSB
        && header.e_machine == EM_X86_64;
}

} // namespace kernel::elf
