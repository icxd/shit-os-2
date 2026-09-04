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

// The eight ANSI colours and their bright variants, in the order SGR numbers
// them: 30..37 index the first half, 90..97 the second.
inline constexpr Rgb ANSI_PALETTE[16] = {
    { 0x1c, 0x1c, 0x22 }, // black -- lifted off the background so it is visible
    { 0xd3, 0x54, 0x4f }, // red
    { 0x67, 0xb0, 0x6a }, // green
    { 0xc9, 0xa5, 0x54 }, // yellow
    { 0x5a, 0x8f, 0xd6 }, // blue
    { 0xa8, 0x76, 0xc9 }, // magenta
    { 0x56, 0xaf, 0xb0 }, // cyan
    { 0xcc, 0xcc, 0xcc }, // white -- the default foreground
    { 0x5c, 0x5c, 0x68 }, // bright black, i.e. what dim text uses
    { 0xea, 0x72, 0x6d },
    { 0x86, 0xcd, 0x88 },
    { 0xe3, 0xc0, 0x72 },
    { 0x7a, 0xab, 0xea },
    { 0xc2, 0x94, 0xe3 },
    { 0x74, 0xc9, 0xca },
    { 0xff, 0xff, 0xff },
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
    // Returns true if the byte was consumed as part of an escape sequence.
    bool consume_escape(char c);
    void apply_sgr();
    void repack_colors();
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

    // What set_colors was last given, so SGR 0 has something to reset to.
    Rgb m_default_foreground { 0xcc, 0xcc, 0xcc };
    Rgb m_default_background { 0x0d, 0x0d, 0x12 };

    // Enough of an ANSI parser for colour. Anything else in a CSI sequence is
    // swallowed rather than printed as mojibake: a terminal that ignores a
    // cursor movement still reads correctly, one that prints "[2J" does not.
    enum class EscapeState : u8 {
        None,
        Escape, // saw ESC, waiting for '['
        Csi, // inside a CSI sequence, collecting parameters
    };

    static constexpr usize MAX_SGR_PARAMETERS = 8;

    EscapeState m_escape { EscapeState::None };
    u32 m_parameters[MAX_SGR_PARAMETERS] {};
    usize m_parameter_count { 0 };
    bool m_bold { false };
};

FramebufferConsole& framebuffer_console();
bool framebuffer_initialize();

} // namespace kernel::dev
