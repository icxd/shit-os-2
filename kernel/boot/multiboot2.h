// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- multiboot2 information structures.
//
// This is a transcription of the parts of the multiboot2 specification we
// actually consume, plus a range-for-able view over the tag list. Everything
// GRUB tells us about the machine arrives through here, and it is the only
// place in the kernel that knows the boot protocol exists -- the rest of the
// kernel sees a BootInfo (kernel/boot/boot_info.h) instead.

#pragma once

#include <shitos/types.h>

namespace kernel::boot {

inline constexpr u32 MULTIBOOT2_BOOTLOADER_MAGIC = 0x36D76289;

enum class TagType : u32 {
    End = 0,
    Cmdline = 1,
    BootLoaderName = 2,
    Module = 3,
    BasicMeminfo = 4,
    BootDev = 5,
    Mmap = 6,
    Vbe = 7,
    Framebuffer = 8,
    ElfSections = 9,
    Apm = 10,
    Efi32 = 11,
    Efi64 = 12,
    Smbios = 13,
    AcpiOld = 14,
    AcpiNew = 15,
    Network = 16,
    EfiMmap = 17,
    EfiBootServices = 18,
    LoadBaseAddr = 21,
};

struct [[gnu::packed]] Tag {
    u32 type;
    u32 size;
};

struct [[gnu::packed]] StringTag {
    Tag header;
    char string[];
};

struct [[gnu::packed]] ModuleTag {
    Tag header;
    u32 mod_start;
    u32 mod_end;
    char string[];
};

struct [[gnu::packed]] BasicMeminfoTag {
    Tag header;
    u32 mem_lower;
    u32 mem_upper;
};

enum class MmapEntryType : u32 {
    Available = 1,
    Reserved = 2,
    AcpiReclaimable = 3,
    AcpiNvs = 4,
    BadRam = 5,
};

struct [[gnu::packed]] MmapEntry {
    u64 addr;
    u64 len;
    u32 type;
    u32 zero;
};

struct [[gnu::packed]] MmapTag {
    Tag header;
    u32 entry_size;
    u32 entry_version;
    MmapEntry entries[];
};

enum class FramebufferKind : u8 {
    Indexed = 0,
    Rgb = 1,
    EgaText = 2,
};

struct [[gnu::packed]] FramebufferTag {
    Tag header;
    u64 addr;
    u32 pitch;
    u32 width;
    u32 height;
    u8 bpp;
    u8 type;
    // The specification says u16 here, not u8. Getting this wrong shifts every
    // colour mask by a byte and produces a screen that renders but is the
    // wrong colour, which is a memorable way to lose an hour.
    u16 reserved;
    // For Rgb, the field/mask layout follows here.
    u8 red_field_position;
    u8 red_mask_size;
    u8 green_field_position;
    u8 green_mask_size;
    u8 blue_field_position;
    u8 blue_mask_size;
};

struct [[gnu::packed]] AcpiTag {
    Tag header;
    u8 rsdp[];
};

// The multiboot2 information block: a header followed by 8-byte-aligned tags,
// terminated by a tag of type End.
class InfoBlock {
public:
    explicit InfoBlock(void* base)
        : m_base(static_cast<u8*>(base))
    {
    }

    u32 total_size() const { return *reinterpret_cast<u32 const*>(m_base); }

    class Iterator {
    public:
        explicit Iterator(u8* tag)
            : m_tag(tag)
        {
        }

        Tag const& operator*() const { return *reinterpret_cast<Tag const*>(m_tag); }
        Tag const* operator->() const { return reinterpret_cast<Tag const*>(m_tag); }

        Iterator& operator++()
        {
            auto const* tag = reinterpret_cast<Tag const*>(m_tag);
            // Tags are padded up to the next multiple of eight.
            m_tag += (tag->size + 7) & ~7u;
            return *this;
        }

        bool operator!=(Iterator const& other) const
        {
            // Iteration stops at the End tag, not at a computed address.
            if (other.m_tag == nullptr)
                return reinterpret_cast<Tag const*>(m_tag)->type != static_cast<u32>(TagType::End);
            return m_tag != other.m_tag;
        }

    private:
        u8* m_tag;
    };

    Iterator begin() const { return Iterator(m_base + 8); }
    Iterator end() const { return Iterator(nullptr); }

    template<typename T>
    T const* find(TagType type) const
    {
        for (auto const& tag : *this) {
            if (tag.type == static_cast<u32>(type))
                return reinterpret_cast<T const*>(&tag);
        }
        return nullptr;
    }

private:
    u8* m_base;
};

} // namespace kernel::boot
