// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- printf-style formatting.

#include <kernel/lib/format.h>
#include <kernel/lib/kstd.h>
#include <kernel/lib/string.h>

namespace kernel {

namespace {

struct Emitter {
    FormatSink sink;
    void* context;
    usize written { 0 };

    void put(char c)
    {
        sink(context, c);
        ++written;
    }

    void put_string(char const* s, usize length)
    {
        for (usize i = 0; i < length; ++i)
            put(s[i]);
    }

    void pad(char fill, usize count)
    {
        for (usize i = 0; i < count; ++i)
            put(fill);
    }
};

struct Spec {
    bool left_align { false };
    bool zero_pad { false };
    bool force_sign { false };
    bool alternate { false };
    usize width { 0 };
    int length { 0 }; // 0 = int, 1 = long, 2 = long long, 3 = size_t
};

char const* const LOWER_DIGITS = "0123456789abcdef";
char const* const UPPER_DIGITS = "0123456789ABCDEF";

// Renders into a caller-supplied scratch buffer and returns where the digits
// begin, so the padding logic can measure before it emits.
char* render_unsigned(u64 value, unsigned base, bool uppercase, char* end)
{
    char const* digits = uppercase ? UPPER_DIGITS : LOWER_DIGITS;
    char* p = end;
    *--p = '\0';
    if (value == 0) {
        *--p = '0';
        return p;
    }
    while (value != 0) {
        *--p = digits[value % base];
        value /= base;
    }
    return p;
}

void emit_padded(Emitter& out, Spec const& spec, char const* prefix, char const* body)
{
    usize const prefix_length = prefix != nullptr ? strlen(prefix) : 0;
    usize const body_length = strlen(body);
    usize const total = prefix_length + body_length;
    usize const padding = spec.width > total ? spec.width - total : 0;

    if (!spec.left_align && !spec.zero_pad)
        out.pad(' ', padding);
    if (prefix_length != 0)
        out.put_string(prefix, prefix_length);
    // Zero padding goes after any sign, otherwise "-0042" becomes "000-42".
    if (!spec.left_align && spec.zero_pad)
        out.pad('0', padding);
    out.put_string(body, body_length);
    if (spec.left_align)
        out.pad(' ', padding);
}

} // namespace

usize vformat(FormatSink sink, void* context, char const* format, va_list args)
{
    Emitter out { sink, context };

    for (char const* p = format; *p != '\0'; ++p) {
        if (*p != '%') {
            out.put(*p);
            continue;
        }

        ++p;
        if (*p == '%') {
            out.put('%');
            continue;
        }

        Spec spec;
        for (bool parsing_flags = true; parsing_flags;) {
            switch (*p) {
            case '-':
                spec.left_align = true;
                ++p;
                break;
            case '0':
                spec.zero_pad = true;
                ++p;
                break;
            case '+':
                spec.force_sign = true;
                ++p;
                break;
            case '#':
                spec.alternate = true;
                ++p;
                break;
            default: parsing_flags = false; break;
            }
        }

        while (*p >= '0' && *p <= '9') {
            spec.width = spec.width * 10 + static_cast<usize>(*p - '0');
            ++p;
        }

        if (*p == 'z') {
            spec.length = 3;
            ++p;
        } else if (*p == 'l') {
            spec.length = 1;
            ++p;
            if (*p == 'l') {
                spec.length = 2;
                ++p;
            }
        } else if (*p == 'h') {
            ++p;
            if (*p == 'h')
                ++p;
        }

        char scratch[32];
        char* const scratch_end = scratch + sizeof(scratch);

        switch (*p) {
        case 'd':
        case 'i': {
            i64 value;
            if (spec.length >= 2)
                value = va_arg(args, long long);
            else if (spec.length == 1)
                value = va_arg(args, long);
            else
                value = va_arg(args, int);

            bool const negative = value < 0;
            // Negate in unsigned space so INT64_MIN does not overflow.
            u64 const magnitude
                = negative ? (~static_cast<u64>(value) + 1) : static_cast<u64>(value);
            char const* body = render_unsigned(magnitude, 10, false, scratch_end);
            char const* prefix = negative ? "-" : (spec.force_sign ? "+" : nullptr);
            emit_padded(out, spec, prefix, body);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            u64 value;
            if (spec.length >= 2)
                value = va_arg(args, unsigned long long);
            else if (spec.length == 1 || spec.length == 3)
                value = va_arg(args, unsigned long);
            else
                value = va_arg(args, unsigned int);

            unsigned base = 10;
            bool uppercase = false;
            char const* prefix = nullptr;
            if (*p == 'x') {
                base = 16;
                prefix = spec.alternate ? "0x" : nullptr;
            } else if (*p == 'X') {
                base = 16;
                uppercase = true;
                prefix = spec.alternate ? "0X" : nullptr;
            } else if (*p == 'o') {
                base = 8;
            }
            char const* body = render_unsigned(value, base, uppercase, scratch_end);
            emit_padded(out, spec, prefix, body);
            break;
        }
        case 'p': {
            auto const value = reinterpret_cast<u64>(va_arg(args, void*));
            char* body = render_unsigned(value, 16, false, scratch_end);
            // Pointers always print full width; a short one is more confusing
            // than a long one when you are staring at a fault address.
            Spec pointer_spec;
            pointer_spec.width = 16;
            pointer_spec.zero_pad = true;
            emit_padded(out, pointer_spec, "0x", body);
            break;
        }
        case 'c': {
            char const value = static_cast<char>(va_arg(args, int));
            char body[2] = { value, '\0' };
            emit_padded(out, spec, nullptr, body);
            break;
        }
        case 's': {
            char const* value = va_arg(args, char const*);
            if (value == nullptr)
                value = "(null)";
            emit_padded(out, spec, nullptr, value);
            break;
        }
        case '\0':
            // Trailing '%' with nothing after it; stop rather than run off.
            return out.written;
        default:
            out.put('%');
            out.put(*p);
            break;
        }
    }

    return out.written;
}

usize format(FormatSink sink, void* context, char const* format_string, ...)
{
    va_list args;
    va_start(args, format_string);
    usize const written = vformat(sink, context, format_string, args);
    va_end(args);
    return written;
}

namespace {

struct BufferSink {
    char* buffer;
    usize capacity;
    usize offset { 0 };
};

void buffer_put(void* context, char c)
{
    auto* sink = static_cast<BufferSink*>(context);
    // Always leave room for the terminator; keep counting past the end so the
    // return value reports what would have been needed.
    if (sink->capacity != 0 && sink->offset + 1 < sink->capacity)
        sink->buffer[sink->offset] = c;
    ++sink->offset;
}

} // namespace

usize vsnprintf(char* buffer, usize size, char const* format_string, va_list args)
{
    BufferSink sink { buffer, size };
    vformat(buffer_put, &sink, format_string, args);
    if (size != 0)
        buffer[min(sink.offset, size - 1)] = '\0';
    return sink.offset;
}

usize snprintf(char* buffer, usize size, char const* format_string, ...)
{
    va_list args;
    va_start(args, format_string);
    usize const written = vsnprintf(buffer, size, format_string, args);
    va_end(args);
    return written;
}

} // namespace kernel
