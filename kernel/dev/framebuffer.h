// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- text on a linear framebuffer.
//
// GRUB is asked for 1024x768x32. If it can only give us an EGA text buffer we
// take that instead and drive character cells directly; both paths present the
// same ConsoleSink so nothing above here cares which one it got.

#pragma once

#include <kernel/boot/boot_info.h>
#include <kernel/dev/console.h>

#include <shitos/types.h>

namespace kernel::dev {

struct Rgb {
    u8 r, g, b;
};

class FramebufferConsole final : public ConsoleSink {
public:
    bool initialize(boot::FramebufferInfo const& info);

    void write_char(char c) override;
    char const* name() const override { return "fbcon"; }

    void clear();
    void set_colors(Rgb foreground, Rgb background);

    u32 columns() const { return m_columns; }
    u32 rows() const { return m_rows; }
    bool is_usable() const { return m_format != boot::FramebufferFormat::None; }

private:
    void put_glyph(char c, u32 column, u32 row);
    void draw_pixel(u32 x, u32 y, u32 packed);
    u32 pack(Rgb color) const;
    void scroll();
    void newline();
    void advance_cursor();

    boot::FramebufferFormat m_format { boot::FramebufferFormat::None };
    u8* m_pixels { nullptr };
    u32 m_pitch { 0 };
    u32 m_width { 0 };
    u32 m_height { 0 };
    u32 m_bytes_per_pixel { 0 };

    u8 m_red_shift { 0 }, m_green_shift { 0 }, m_blue_shift { 0 };
    u8 m_red_bits { 8 }, m_green_bits { 8 }, m_blue_bits { 8 };

    u32 m_columns { 0 };
    u32 m_rows { 0 };
    u32 m_cursor_column { 0 };
    u32 m_cursor_row { 0 };

    Rgb m_foreground { 0xcc, 0xcc, 0xcc };
    Rgb m_background { 0x0d, 0x0d, 0x12 };
    u32 m_packed_foreground { 0 };
    u32 m_packed_background { 0 };
};

FramebufferConsole& framebuffer_console();
bool framebuffer_initialize();

} // namespace kernel::dev
