// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- turning what GRUB says into what the kernel believes.

#include <kernel/boot/boot_info.h>
#include <kernel/boot/multiboot2.h>
#include <kernel/lib/kstd.h>
#include <kernel/lib/string.h>

extern "C" {
extern u8 __kernel_phys_start[];
extern u8 __kernel_phys_end[];
}

namespace kernel::boot {

namespace {

BootInfo s_boot_info;

MemoryKind kind_from_multiboot(u32 type)
{
    switch (static_cast<MmapEntryType>(type)) {
    case MmapEntryType::Available: return MemoryKind::Usable;
    case MmapEntryType::AcpiReclaimable: return MemoryKind::AcpiReclaimable;
    case MmapEntryType::AcpiNvs: return MemoryKind::AcpiNvs;
    case MmapEntryType::BadRam: return MemoryKind::Bad;
    case MmapEntryType::Reserved:
    default: return MemoryKind::Reserved;
    }
}

void add_region(BootInfo& info, u64 base, u64 length, MemoryKind kind)
{
    if (length == 0 || info.memory_region_count >= MAX_MEMORY_REGIONS)
        return;
    info.memory_regions[info.memory_region_count++] = { base, length, kind };
    if (kind == MemoryKind::Usable)
        info.total_usable_bytes += length;
    info.highest_address = max(info.highest_address, base + length);
}

} // namespace

BootInfo& boot_info()
{
    return s_boot_info;
}

bool parse_multiboot2(u32 magic, u64 info_phys, BootInfo& out)
{
    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC)
        return false;

    // The info block sits in low physical memory; reach it through the direct
    // map rather than assuming the identity mapping survives.
    InfoBlock block(phys_to_virt(phys(info_phys)));

    out.kernel_phys_start = reinterpret_cast<u64>(__kernel_phys_start);
    out.kernel_phys_end = reinterpret_cast<u64>(__kernel_phys_end);

    for (auto const& tag : block) {
        switch (static_cast<TagType>(tag.type)) {
        case TagType::BootLoaderName:
            out.bootloader_name = reinterpret_cast<StringTag const*>(&tag)->string;
            break;

        case TagType::Cmdline:
            out.command_line = reinterpret_cast<StringTag const*>(&tag)->string;
            break;

        case TagType::Module: {
            auto const* module_tag = reinterpret_cast<ModuleTag const*>(&tag);
            if (out.module_count < MAX_BOOT_MODULES) {
                out.modules[out.module_count++] = {
                    module_tag->mod_start,
                    module_tag->mod_end,
                    module_tag->string,
                };
            }
            break;
        }

        case TagType::Mmap: {
            auto const* mmap = reinterpret_cast<MmapTag const*>(&tag);
            // entry_size is authoritative and may exceed sizeof(MmapEntry) if
            // a future revision grows the struct, so step by it explicitly.
            auto const* cursor = reinterpret_cast<u8 const*>(mmap->entries);
            auto const* limit = reinterpret_cast<u8 const*>(mmap) + mmap->header.size;
            for (; cursor + mmap->entry_size <= limit; cursor += mmap->entry_size) {
                auto const* entry = reinterpret_cast<MmapEntry const*>(cursor);
                add_region(out, entry->addr, entry->len, kind_from_multiboot(entry->type));
            }
            break;
        }

        case TagType::Framebuffer: {
            auto const* fb = reinterpret_cast<FramebufferTag const*>(&tag);
            auto& info = out.framebuffer;
            info.phys_address = fb->addr;
            info.pitch = fb->pitch;
            info.width = fb->width;
            info.height = fb->height;
            info.bits_per_pixel = fb->bpp;
            switch (static_cast<FramebufferKind>(fb->type)) {
            case FramebufferKind::Rgb:
                info.format = FramebufferFormat::Rgb;
                info.red_shift = fb->red_field_position;
                info.red_bits = fb->red_mask_size;
                info.green_shift = fb->green_field_position;
                info.green_bits = fb->green_mask_size;
                info.blue_shift = fb->blue_field_position;
                info.blue_bits = fb->blue_mask_size;
                break;
            case FramebufferKind::EgaText: info.format = FramebufferFormat::EgaText; break;
            case FramebufferKind::Indexed:
            default:
                // We do not do palettes. Fall back to serial only.
                info.format = FramebufferFormat::None;
                break;
            }
            break;
        }

        case TagType::AcpiOld:
        case TagType::AcpiNew:
            out.rsdp_phys
                = reinterpret_cast<u64>(reinterpret_cast<AcpiTag const*>(&tag)->rsdp) - HHDM_BASE;
            break;

        default: break;
        }
    }

    // The kernel image and every boot module sit inside regions GRUB reported
    // as usable. Record them so the physical allocator can carve them out.
    add_region(out, out.kernel_phys_start, out.kernel_phys_end - out.kernel_phys_start,
        MemoryKind::KernelImage);
    for (usize i = 0; i < out.module_count; ++i) {
        add_region(out, out.modules[i].phys_start,
            out.modules[i].phys_end - out.modules[i].phys_start, MemoryKind::BootModule);
    }

    return true;
}

} // namespace kernel::boot
