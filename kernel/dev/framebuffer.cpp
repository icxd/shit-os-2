// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- text on a linear framebuffer.

#include <kernel/dev/font8x16.h>
#include <kernel/dev/framebuffer.h>
#include <kernel/lib/kstd.h>
#include <kernel/lib/string.h>

namespace kernel::dev {

namespace {

FramebufferConsole s_console;

// EGA text mode, for when GRUB refuses the mode we asked for.
constexpr u32 EGA_COLUMNS = 80;
constexpr u32 EGA_ROWS = 25;
constexpr u8 EGA_ATTRIBUTE = 0x07; // light grey on black

} // namespace

bool FramebufferConsole::initialize(boot::FramebufferInfo const& info)
{
    m_format = info.format;
    if (m_format == boot::FramebufferFormat::None)
        return false;

    // The framebuffer is physical memory; reach it through the direct map.
    m_pixels = static_cast<u8*>(phys_to_virt(phys(info.phys_address)));
    m_pitch = info.pitch;
    m_width = info.width;
    m_height = info.height;

    if (m_format == boot::FramebufferFormat::EgaText) {
        m_columns = min(EGA_COLUMNS, info.width != 0 ? info.width : EGA_COLUMNS);
        m_rows = min(EGA_ROWS, info.height != 0 ? info.height : EGA_ROWS);
        m_bytes_per_pixel = 2;
        if (m_pitch == 0)
            m_pitch = m_columns * 2;
        clear();
        return true;
    }

    m_bytes_per_pixel = info.bits_per_pixel / 8;
    if (m_bytes_per_pixel < 3 || m_bytes_per_pixel > 4) {
        // 15/16-bit modes are possible in principle and not worth the code
        // until something actually hands us one.
        m_format = boot::FramebufferFormat::None;
        return false;
    }

    m_red_shift = info.red_shift;
    m_green_shift = info.green_shift;
    m_blue_shift = info.blue_shift;
    m_red_bits = info.red_bits;
    m_green_bits = info.green_bits;
    m_blue_bits = info.blue_bits;

    m_columns = m_width / FONT_WIDTH;
    m_rows = m_height / FONT_HEIGHT;

    m_packed_foreground = pack(m_foreground);
    m_packed_background = pack(m_background);
    clear();
    return true;
}

u32 FramebufferConsole::pack(Rgb color) const
{
    // Scale each channel down to however many bits this mode actually has,
    // then drop it at the shift the bootloader reported.
    auto scale = [](u8 value, u8 bits) -> u32 {
        if (bits >= 8)
            return value;
        return static_cast<u32>(value) >> (8 - bits);
    };
    return (scale(color.r, m_red_bits) << m_red_shift)
        | (scale(color.g, m_green_bits) << m_green_shift)
        | (scale(color.b, m_blue_bits) << m_blue_shift);
}

void FramebufferConsole::set_colors(Rgb foreground, Rgb background)
{
    m_default_foreground = foreground;
    m_default_background = background;
    m_foreground = foreground;
    m_background = background;
    m_bold = false;
    repack_colors();
}

void FramebufferConsole::repack_colors()
{
    if (m_format != boot::FramebufferFormat::Rgb)
        return;
    m_packed_foreground = pack(m_foreground);
    m_packed_background = pack(m_background);
}

// --- ANSI escape handling -------------------------------------------------
//
// Only SGR is acted on, because only colour is wanted here. Everything else in
// a CSI sequence is recognised and dropped: a console that ignores a cursor
// move still reads correctly, one that prints "[2J" into the middle of a line
// does not.

void FramebufferConsole::apply_sgr()
{
    // A bare ESC[m means ESC[0m.
    if (m_parameter_count == 0) {
        m_parameter_count = 1;
        m_parameters[0] = 0;
    }

    for (usize i = 0; i < m_parameter_count; ++i) {
        u32 const parameter = m_parameters[i];

        if (parameter == 0) {
            m_foreground = m_default_foreground;
            m_background = m_default_background;
            m_bold = false;
        } else if (parameter == 1) {
            // Bold is the bright half of the palette, which is the convention
            // every terminal of this era settled on.
            m_bold = true;
            for (u8 index = 0; index < 8; ++index) {
                if (m_foreground.r == ANSI_PALETTE[index].r
                    && m_foreground.g == ANSI_PALETTE[index].g
                    && m_foreground.b == ANSI_PALETTE[index].b) {
                    m_foreground = ANSI_PALETTE[index + 8];
                    break;
                }
            }
        } else if (parameter == 22) {
            m_bold = false;
        } else if (parameter >= 30 && parameter <= 37) {
            m_foreground = ANSI_PALETTE[(parameter - 30) + (m_bold ? 8 : 0)];
        } else if (parameter == 39) {
            m_foreground = m_default_foreground;
        } else if (parameter >= 40 && parameter <= 47) {
            m_background = ANSI_PALETTE[parameter - 40];
        } else if (parameter == 49) {
            m_background = m_default_background;
        } else if (parameter >= 90 && parameter <= 97) {
            m_foreground = ANSI_PALETTE[(parameter - 90) + 8];
        } else if (parameter >= 100 && parameter <= 107) {
            m_background = ANSI_PALETTE[(parameter - 100) + 8];
        }
    }

    repack_colors();
}

bool FramebufferConsole::consume_escape(char c)
{
    switch (m_escape) {
    case EscapeState::None:
        if (c != '\033')
            return false;
        m_escape = EscapeState::Escape;
        return true;

    case EscapeState::Escape:
        if (c == '[') {
            m_escape = EscapeState::Csi;
            m_parameter_count = 0;
            m_parameters[0] = 0;
            return true;
        }
        // Not a CSI. Two-character sequences are not supported, so drop it
        // rather than printing half of one.
        m_escape = EscapeState::None;
        return true;

    case EscapeState::Csi:
        if (c >= '0' && c <= '9') {
            if (m_parameter_count == 0)
                m_parameter_count = 1;
            u32& parameter = m_parameters[m_parameter_count - 1];
            // Clamp rather than overflow: a hostile or corrupt sequence must
            // not be able to wrap this into a valid colour.
            if (parameter < 100000)
                parameter = parameter * 10 + static_cast<u32>(c - '0');
            return true;
        }
        if (c == ';') {
            if (m_parameter_count < MAX_SGR_PARAMETERS)
                m_parameters[m_parameter_count++] = 0;
            return true;
        }
        // Any final byte ends the sequence; 'm' is the only one that does
        // something.
        if (c >= '@' && c <= '~') {
            if (c == 'm')
                apply_sgr();
            m_escape = EscapeState::None;
            return true;
        }
        // Intermediate bytes and anything unexpected: keep swallowing until a
        // final byte arrives, so a malformed sequence cannot leak text.
        return true;
    }

    return false;
}

void FramebufferConsole::draw_pixel(u32 x, u32 y, u32 packed)
{
    u8* p = m_pixels + static_cast<usize>(y) * m_pitch + static_cast<usize>(x) * m_bytes_per_pixel;
    if (m_bytes_per_pixel == 4) {
        *reinterpret_cast<u32*>(p) = packed;
    } else {
        p[0] = static_cast<u8>(packed);
        p[1] = static_cast<u8>(packed >> 8);
        p[2] = static_cast<u8>(packed >> 16);
    }
}

void FramebufferConsole::clear()
{
    if (m_format == boot::FramebufferFormat::EgaText) {
        auto* cells = reinterpret_cast<u16*>(m_pixels);
        u16 const blank = static_cast<u16>(' ') | (static_cast<u16>(EGA_ATTRIBUTE) << 8);
        for (u32 i = 0; i < m_columns * m_rows; ++i)
            cells[i] = blank;
    } else if (m_format == boot::FramebufferFormat::Rgb) {
        for (u32 y = 0; y < m_height; ++y) {
            for (u32 x = 0; x < m_width; ++x)
                draw_pixel(x, y, m_packed_background);
        }
    }
    m_cursor_column = 0;
    m_cursor_row = 0;
}

void FramebufferConsole::put_glyph(char c, u32 column, u32 row)
{
    auto const index = static_cast<u8>(c);
    u8 const* glyph = FONT_8X16[index < FONT_GLYPHS ? index : static_cast<u8>('?')];

    u32 const origin_x = column * FONT_WIDTH;
    u32 const origin_y = row * FONT_HEIGHT;

    for (u32 gy = 0; gy < FONT_HEIGHT; ++gy) {
        u8 const bits = glyph[gy];
        for (u32 gx = 0; gx < FONT_WIDTH; ++gx) {
            bool const lit = (bits & (0x80 >> gx)) != 0;
            draw_pixel(
                origin_x + gx, origin_y + gy, lit ? m_packed_foreground : m_packed_background);
        }
    }
}

void FramebufferConsole::scroll()
{
    if (m_format == boot::FramebufferFormat::EgaText) {
        auto* cells = reinterpret_cast<u16*>(m_pixels);
        memmove(cells, cells + m_columns, (m_rows - 1) * m_columns * sizeof(u16));
        u16 const blank = static_cast<u16>(' ') | (static_cast<u16>(EGA_ATTRIBUTE) << 8);
        for (u32 i = 0; i < m_columns; ++i)
            cells[(m_rows - 1) * m_columns + i] = blank;
        return;
    }

    // Shift the whole image up by one text row. Reading back from video memory
    // is not fast, but a scroll is rare compared to a character write and this
    // keeps us from needing a shadow buffer.
    usize const row_bytes = static_cast<usize>(m_pitch) * FONT_HEIGHT;
    usize const visible_bytes = static_cast<usize>(m_pitch) * m_rows * FONT_HEIGHT;
    memmove(m_pixels, m_pixels + row_bytes, visible_bytes - row_bytes);

    for (u32 y = (m_rows - 1) * FONT_HEIGHT; y < m_rows * FONT_HEIGHT; ++y) {
        for (u32 x = 0; x < m_width; ++x)
            draw_pixel(x, y, m_packed_background);
    }
}

void FramebufferConsole::newline()
{
    m_cursor_column = 0;
    if (++m_cursor_row >= m_rows) {
        scroll();
        m_cursor_row = m_rows - 1;
    }
}

void FramebufferConsole::advance_cursor()
{
    if (++m_cursor_column >= m_columns)
        newline();
}

void FramebufferConsole::write_char(char c)
{
    if (m_format == boot::FramebufferFormat::None)
        return;

    if (consume_escape(c))
        return;

    switch (c) {
    case '\n': newline(); return;
    case '\r': m_cursor_column = 0; return;
    case '\t': do { write_char(' ');
        } while (m_cursor_column % 8 != 0);
        return;
    case '\b':
        if (m_cursor_column > 0) {
            --m_cursor_column;
            if (m_format == boot::FramebufferFormat::EgaText) {
                auto* cells = reinterpret_cast<u16*>(m_pixels);
                cells[m_cursor_row * m_columns + m_cursor_column]
                    = static_cast<u16>(' ') | (static_cast<u16>(EGA_ATTRIBUTE) << 8);
            } else {
                put_glyph(' ', m_cursor_column, m_cursor_row);
            }
        }
        return;
    default: break;
    }

    if (m_format == boot::FramebufferFormat::EgaText) {
        auto* cells = reinterpret_cast<u16*>(m_pixels);
        cells[m_cursor_row * m_columns + m_cursor_column]
            = static_cast<u16>(c) | (static_cast<u16>(EGA_ATTRIBUTE) << 8);
    } else {
        put_glyph(c, m_cursor_column, m_cursor_row);
    }
    advance_cursor();
}

FramebufferConsole& framebuffer_console()
{
    return s_console;
}

bool framebuffer_initialize()
{
    return s_console.initialize(boot::boot_info().framebuffer);
}

} // namespace kernel::dev
