/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- buffered I/O.
 *
 * Real FILE buffering, because unbuffered output over a serial console is
 * unusably slow and because a shell that writes a prompt without a newline
 * needs flush-on-demand to work properly.
 */

#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FLAG_READABLE 0x01
#define FLAG_WRITABLE 0x02
#define FLAG_EOF 0x04
#define FLAG_ERROR 0x08
#define FLAG_LINE_BUFFERED 0x10
#define FLAG_UNBUFFERED 0x20
#define FLAG_STATIC 0x40

struct _FILE {
    int fd;
    int flags;

    char* buffer;
    size_t capacity;
    size_t pending; /* bytes waiting in the buffer to be written */
    size_t available; /* bytes read into the buffer but not yet returned */
    size_t position; /* how far through `available` the reader has got */

    int unget;
};

#define MAX_STREAMS 16
static FILE s_streams[MAX_STREAMS];
static char s_stdin_buffer[BUFSIZ];
static char s_stdout_buffer[BUFSIZ];

static FILE s_stdin;
static FILE s_stdout;
static FILE s_stderr;

FILE* stdin = &s_stdin;
FILE* stdout = &s_stdout;
FILE* stderr = &s_stderr;

void __stdio_initialize(void)
{
    s_stdin.fd = STDIN_FILENO;
    s_stdin.flags = FLAG_READABLE | FLAG_STATIC;
    s_stdin.buffer = s_stdin_buffer;
    s_stdin.capacity = sizeof(s_stdin_buffer);

    s_stdout.fd = STDOUT_FILENO;
    s_stdout.flags = FLAG_WRITABLE | FLAG_STATIC;
    s_stdout.buffer = s_stdout_buffer;
    s_stdout.capacity = sizeof(s_stdout_buffer);
    /* Line buffered to a terminal, so a prompt appears when it is written;
     * fully buffered into a pipe, so `ls | cat` is not one syscall per byte. */
    s_stdout.flags |= isatty(STDOUT_FILENO) ? FLAG_LINE_BUFFERED : 0;

    /* stderr is never buffered: a message printed just before a crash has to
     * have already left the process. */
    s_stderr.fd = STDERR_FILENO;
    s_stderr.flags = FLAG_WRITABLE | FLAG_UNBUFFERED | FLAG_STATIC;
    s_stderr.buffer = 0;
    s_stderr.capacity = 0;
}

int fflush(FILE* stream)
{
    if (stream == 0) {
        __stdio_flush_all();
        return 0;
    }
    if (stream->pending == 0)
        return 0;

    size_t written = 0;
    while (written < stream->pending) {
        ssize_t count = write(stream->fd, stream->buffer + written, stream->pending - written);
        if (count <= 0) {
            stream->flags |= FLAG_ERROR;
            /* Drop what could not be written rather than trying forever. */
            stream->pending = 0;
            return EOF;
        }
        written += (size_t)count;
    }
    stream->pending = 0;
    return 0;
}

void __stdio_flush_all(void)
{
    fflush(&s_stdout);
    for (int i = 0; i < MAX_STREAMS; ++i) {
        if (s_streams[i].fd > 0 || (s_streams[i].flags & (FLAG_READABLE | FLAG_WRITABLE)))
            fflush(&s_streams[i]);
    }
}

int fputc(int c, FILE* stream)
{
    if (!(stream->flags & FLAG_WRITABLE)) {
        stream->flags |= FLAG_ERROR;
        return EOF;
    }

    char const byte = (char)c;

    if ((stream->flags & FLAG_UNBUFFERED) || stream->buffer == 0) {
        if (write(stream->fd, &byte, 1) != 1) {
            stream->flags |= FLAG_ERROR;
            return EOF;
        }
        return (unsigned char)c;
    }

    stream->buffer[stream->pending++] = byte;

    if (stream->pending == stream->capacity
        || ((stream->flags & FLAG_LINE_BUFFERED) && byte == '\n')) {
        if (fflush(stream) == EOF)
            return EOF;
    }

    return (unsigned char)c;
}

int fputs(const char* s, FILE* stream)
{
    for (; *s; ++s) {
        if (fputc(*s, stream) == EOF)
            return EOF;
    }
    return 0;
}

int putchar(int c)
{
    return fputc(c, stdout);
}

int puts(const char* s)
{
    if (fputs(s, stdout) == EOF)
        return EOF;
    return fputc('\n', stdout) == EOF ? EOF : 0;
}

size_t fwrite(const void* buffer, size_t size, size_t count, FILE* stream)
{
    const unsigned char* bytes = buffer;
    size_t const total = size * count;
    for (size_t i = 0; i < total; ++i) {
        if (fputc(bytes[i], stream) == EOF)
            return size ? i / size : 0;
    }
    return count;
}

static int refill(FILE* stream)
{
    if (!(stream->flags & FLAG_READABLE) || stream->buffer == 0)
        return EOF;

    ssize_t count = read(stream->fd, stream->buffer, stream->capacity);
    if (count < 0) {
        stream->flags |= FLAG_ERROR;
        return EOF;
    }
    if (count == 0) {
        stream->flags |= FLAG_EOF;
        return EOF;
    }

    stream->available = (size_t)count;
    stream->position = 0;
    return 0;
}

int fgetc(FILE* stream)
{
    if (stream->unget != 0) {
        int const c = stream->unget;
        stream->unget = 0;
        return c;
    }

    if (stream->position >= stream->available) {
        /* Anything buffered for output has to reach the terminal before we
         * block waiting for input, or the prompt appears after the answer. */
        fflush(stdout);
        if (refill(stream) == EOF)
            return EOF;
    }

    return (unsigned char)stream->buffer[stream->position++];
}

int ungetc(int c, FILE* stream)
{
    if (c == EOF)
        return EOF;
    stream->unget = (unsigned char)c;
    stream->flags &= ~FLAG_EOF;
    return c;
}

char* fgets(char* buffer, int size, FILE* stream)
{
    if (size <= 0)
        return 0;

    int written = 0;
    while (written < size - 1) {
        int const c = fgetc(stream);
        if (c == EOF) {
            if (written == 0)
                return 0;
            break;
        }
        buffer[written++] = (char)c;
        if (c == '\n')
            break;
    }
    buffer[written] = '\0';
    return buffer;
}

int getchar(void)
{
    return fgetc(stdin);
}

size_t fread(void* buffer, size_t size, size_t count, FILE* stream)
{
    unsigned char* bytes = buffer;
    size_t const total = size * count;
    for (size_t i = 0; i < total; ++i) {
        int const c = fgetc(stream);
        if (c == EOF)
            return size ? i / size : 0;
        bytes[i] = (unsigned char)c;
    }
    return count;
}

FILE* fopen(const char* path, const char* mode)
{
    int flags = 0;
    int stream_flags = 0;

    switch (mode[0]) {
    case 'r':
        flags = (mode[1] == '+') ? O_RDWR : O_RDONLY;
        stream_flags = FLAG_READABLE | ((mode[1] == '+') ? FLAG_WRITABLE : 0);
        break;
    case 'w':
        flags = O_CREAT | O_TRUNC | ((mode[1] == '+') ? O_RDWR : O_WRONLY);
        stream_flags = FLAG_WRITABLE | ((mode[1] == '+') ? FLAG_READABLE : 0);
        break;
    case 'a':
        flags = O_CREAT | O_APPEND | ((mode[1] == '+') ? O_RDWR : O_WRONLY);
        stream_flags = FLAG_WRITABLE | ((mode[1] == '+') ? FLAG_READABLE : 0);
        break;
    default: errno = EINVAL; return 0;
    }

    FILE* stream = 0;
    for (int i = 0; i < MAX_STREAMS; ++i) {
        if (s_streams[i].flags == 0) {
            stream = &s_streams[i];
            break;
        }
    }
    if (!stream) {
        errno = EMFILE;
        return 0;
    }

    int const fd = open(path, flags, 0644);
    if (fd < 0)
        return 0;

    stream->fd = fd;
    stream->flags = stream_flags;
    stream->buffer = malloc(BUFSIZ);
    stream->capacity = stream->buffer ? BUFSIZ : 0;
    if (!stream->buffer)
        stream->flags |= FLAG_UNBUFFERED;
    stream->pending = 0;
    stream->available = 0;
    stream->position = 0;
    stream->unget = 0;
    return stream;
}

int fclose(FILE* stream)
{
    if (!stream)
        return EOF;
    fflush(stream);
    int const result = close(stream->fd);
    if (!(stream->flags & FLAG_STATIC)) {
        free(stream->buffer);
        memset(stream, 0, sizeof(*stream));
    }
    return result;
}

int feof(FILE* stream)
{
    return (stream->flags & FLAG_EOF) != 0;
}
int ferror(FILE* stream)
{
    return (stream->flags & FLAG_ERROR) != 0;
}
int fileno(FILE* stream)
{
    return stream->fd;
}

/* --- formatting -------------------------------------------------------- */

/* --- floating point conversion ------------------------------------------- */

/*
 * %f, %e and %g. Written out longhand rather than deferred to a dtoa, because
 * a correctly-rounded shortest-representation converter is a research project
 * and this only has to be right to the requested precision.
 *
 * The integer part is rendered through an unsigned long, which caps %f at
 * values below 2^64; past that the conversion switches to %e, where the
 * mantissa is always a single digit and the range problem disappears.
 */

#define FLOAT_SCRATCH 512
#define MAX_FLOAT_PRECISION 60

static int is_nan_double(double v)
{
    return v != v;
}
static int is_inf_double(double v)
{
    return v != 0.0 && v * 0.5 == v;
}

static double round_half_away(double v)
{
    double integral = (double)(long long)v;
    double const fraction = v - integral;
    if (fraction >= 0.5)
        integral += 1.0;
    else if (fraction <= -0.5)
        integral -= 1.0;
    return integral;
}

static size_t render_fixed(char* out, double magnitude, int precision)
{
    size_t length = 0;

    /* Separate the parts, rounding the fraction first so that a carry out of
     * it (0.999... at precision 2) increments the integer part. */
    double integral = (double)(unsigned long long)magnitude;
    double fraction = magnitude - integral;

    double scale = 1.0;
    for (int i = 0; i < precision; ++i)
        scale *= 10.0;

    double scaled = round_half_away(fraction * scale);
    if (scaled >= scale) {
        scaled -= scale;
        integral += 1.0;
    }

    unsigned long long whole = (unsigned long long)integral;
    char digits[32];
    size_t digit_count = 0;
    if (whole == 0) {
        digits[digit_count++] = '0';
    } else {
        while (whole != 0) {
            digits[digit_count++] = (char)('0' + (whole % 10));
            whole /= 10;
        }
    }
    while (digit_count != 0)
        out[length++] = digits[--digit_count];

    if (precision > 0) {
        out[length++] = '.';
        unsigned long long frac = (unsigned long long)scaled;
        /* Emit most significant first, which means filling backwards. */
        char frac_digits[MAX_FLOAT_PRECISION + 1];
        for (int i = precision - 1; i >= 0; --i) {
            frac_digits[i] = (char)('0' + (frac % 10));
            frac /= 10;
        }
        for (int i = 0; i < precision; ++i)
            out[length++] = frac_digits[i];
    }

    out[length] = '\0';
    return length;
}

/*
 * Powers of ten, exactly. 10^0 through 10^22 are representable in a double, so
 * these literals are the correctly rounded values and comparisons against them
 * are exact.
 *
 * This matters more than it looks: the %g rule switches between fixed and
 * scientific notation on whether the exponent is below -4, and computing the
 * boundary with our own pow -- accurate to a few ulp, not exact -- put
 * 0.0001 on the wrong side of it.
 */
static double exact_power_of_ten(int exponent)
{
    static const double POSITIVE[23] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10,
        1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
    static const double NEGATIVE[23] = { 1e0, 1e-1, 1e-2, 1e-3, 1e-4, 1e-5, 1e-6, 1e-7, 1e-8, 1e-9,
        1e-10, 1e-11, 1e-12, 1e-13, 1e-14, 1e-15, 1e-16, 1e-17, 1e-18, 1e-19, 1e-20, 1e-21, 1e-22 };

    if (exponent >= 0 && exponent <= 22)
        return POSITIVE[exponent];
    if (exponent < 0 && exponent >= -22)
        return NEGATIVE[-exponent];

    /* Outside the exact range the value cannot be represented exactly anyway,
     * so an approximation is all there is. */
    return pow(10.0, (double)exponent);
}

static int decimal_exponent_of(double magnitude)
{
    if (magnitude == 0.0)
        return 0;

    int exponent = (int)floor(log10(magnitude));

    /* log10 can land a place either side at a power-of-ten boundary, so the
     * answer is confirmed by comparison against exact powers rather than
     * trusted. Two steps in each direction covers any plausible error. */
    for (int i = 0; i < 2 && exact_power_of_ten(exponent) > magnitude; ++i)
        --exponent;
    for (int i = 0; i < 2 && exact_power_of_ten(exponent + 1) <= magnitude; ++i)
        ++exponent;

    return exponent;
}

static size_t render_scientific(char* out, double magnitude, int precision, char exponent_char)
{
    int exponent = 0;
    double mantissa = magnitude;

    if (magnitude != 0.0) {
        exponent = decimal_exponent_of(magnitude);
        mantissa = magnitude / exact_power_of_ten(exponent);

        /* Rounding the mantissa can push it to 10.0, which is not normalised. */
        double check = mantissa;
        double scale = 1.0;
        for (int i = 0; i < precision; ++i)
            scale *= 10.0;
        check = round_half_away(check * scale) / scale;
        if (check >= 10.0) {
            mantissa /= 10.0;
            ++exponent;
        }
    }

    size_t length = render_fixed(out, mantissa, precision);

    out[length++] = exponent_char;
    if (exponent < 0) {
        out[length++] = '-';
        exponent = -exponent;
    } else {
        out[length++] = '+';
    }

    /* At least two exponent digits, as the standard requires. */
    if (exponent >= 100) {
        out[length++] = (char)('0' + exponent / 100);
        out[length++] = (char)('0' + (exponent / 10) % 10);
        out[length++] = (char)('0' + exponent % 10);
    } else {
        out[length++] = (char)('0' + exponent / 10);
        out[length++] = (char)('0' + exponent % 10);
    }

    out[length] = '\0';
    return length;
}

static void strip_trailing_zeros(char* text)
{
    char* const exponent = strchr(text, 'e');
    char* const exponent_upper = strchr(text, 'E');
    char* const marker = exponent ? exponent : exponent_upper;

    char* end = marker ? marker : text + strlen(text);
    if (!memchr(text, '.', (size_t)(end - text)))
        return;

    char* last = end - 1;
    while (last > text && *last == '0')
        --last;
    if (*last == '.')
        --last;

    /* Close the gap between the trimmed mantissa and any exponent. */
    if (marker) {
        size_t const tail = strlen(marker) + 1;
        memmove(last + 1, marker, tail);
    } else {
        last[1] = '\0';
    }
}

static size_t render_double(char* out, double value, int precision, char conversion, int alternate)
{
    int const uppercase = (conversion == 'F' || conversion == 'E' || conversion == 'G');
    char const lowered = (char)(uppercase ? conversion + 32 : conversion);

    if (is_nan_double(value)) {
        strcpy(out, uppercase ? "NAN" : "nan");
        return 3;
    }
    if (is_inf_double(value)) {
        strcpy(out, uppercase ? "INF" : "inf");
        return 3;
    }

    if (precision < 0)
        precision = 6;
    if (precision > MAX_FLOAT_PRECISION)
        precision = MAX_FLOAT_PRECISION;

    double const magnitude = value < 0 ? -value : value;

    if (lowered == 'f') {
        /* Beyond what an unsigned long can hold, fixed notation cannot be
         * rendered by this method; scientific says the same thing correctly. */
        if (magnitude >= 18446744073709551616.0)
            return render_scientific(out, magnitude, precision, uppercase ? 'E' : 'e');
        return render_fixed(out, magnitude, precision);
    }

    if (lowered == 'e')
        return render_scientific(out, magnitude, precision, uppercase ? 'E' : 'e');

    /* %g: the standard's rule for choosing between the two. */
    if (precision == 0)
        precision = 1;

    int const exponent = magnitude == 0.0 ? 0 : decimal_exponent_of(magnitude);

    size_t length;
    if (exponent < -4 || exponent >= precision)
        length = render_scientific(out, magnitude, precision - 1, uppercase ? 'E' : 'e');
    else
        length = render_fixed(out, magnitude, precision - 1 - exponent);

    if (!alternate) {
        strip_trailing_zeros(out);
        length = strlen(out);
    }

    return length;
}

typedef struct {
    FILE* stream;
    char* buffer;
    size_t capacity;
    size_t written;
} Sink;

static void emit(Sink* sink, char c)
{
    if (sink->stream) {
        fputc(c, sink->stream);
    } else if (sink->capacity && sink->written + 1 < sink->capacity) {
        sink->buffer[sink->written] = c;
    }
    ++sink->written;
}

static void emit_string(Sink* sink, const char* s, size_t length)
{
    for (size_t i = 0; i < length; ++i)
        emit(sink, s[i]);
}

static char* render_unsigned(unsigned long long value, unsigned base, int uppercase, char* end)
{
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char* p = end;
    *--p = '\0';
    if (value == 0) {
        *--p = '0';
        return p;
    }
    while (value) {
        *--p = digits[value % base];
        value /= base;
    }
    return p;
}

static int format_into(Sink* sink, const char* format, va_list args)
{
    for (const char* p = format; *p; ++p) {
        if (*p != '%') {
            emit(sink, *p);
            continue;
        }

        ++p;
        if (*p == '%') {
            emit(sink, '%');
            continue;
        }

        int left_align = 0, zero_pad = 0, force_sign = 0, alternate = 0;
        for (int parsing = 1; parsing;) {
            switch (*p) {
            case '-':
                left_align = 1;
                ++p;
                break;
            case '0':
                zero_pad = 1;
                ++p;
                break;
            case '+':
                force_sign = 1;
                ++p;
                break;
            case '#':
                alternate = 1;
                ++p;
                break;
            case ' ': ++p; break;
            default: parsing = 0; break;
            }
        }

        int width = 0;
        if (*p == '*') {
            width = va_arg(args, int);
            if (width < 0) {
                left_align = 1;
                width = -width;
            }
            ++p;
        } else {
            while (*p >= '0' && *p <= '9')
                width = width * 10 + (*p++ - '0');
        }

        int precision = -1;
        if (*p == '.') {
            ++p;
            precision = 0;
            if (*p == '*') {
                precision = va_arg(args, int);
                ++p;
            } else {
                while (*p >= '0' && *p <= '9')
                    precision = precision * 10 + (*p++ - '0');
            }
        }

        int length_modifier = 0; /* 0 int, 1 long, 2 long long, 3 size_t */
        if (*p == 'z') {
            length_modifier = 3;
            ++p;
        } else if (*p == 'l') {
            length_modifier = 1;
            ++p;
            if (*p == 'l') {
                length_modifier = 2;
                ++p;
            }
        } else if (*p == 'h') {
            ++p;
            if (*p == 'h')
                ++p;
        }

        char scratch[32];
        char* const scratch_end = scratch + sizeof(scratch);
        /* Floating point needs far more room than an integer, and the buffer
         * has to outlive the switch so the shared padding below can read it. */
        char rendered[FLOAT_SCRATCH];
        const char* body = 0;
        const char* prefix = 0;
        char single[2] = { 0, 0 };

        switch (*p) {
        case 'd':
        case 'i': {
            long long value = length_modifier >= 2 ? va_arg(args, long long)
                : length_modifier == 1             ? va_arg(args, long)
                                                   : va_arg(args, int);
            int const negative = value < 0;
            unsigned long long magnitude
                = negative ? ~(unsigned long long)value + 1 : (unsigned long long)value;
            body = render_unsigned(magnitude, 10, 0, scratch_end);
            prefix = negative ? "-" : (force_sign ? "+" : 0);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            unsigned long long value = length_modifier >= 2      ? va_arg(args, unsigned long long)
                : (length_modifier == 1 || length_modifier == 3) ? va_arg(args, unsigned long)
                                                                 : va_arg(args, unsigned int);
            unsigned base = 10;
            int uppercase = 0;
            if (*p == 'x') {
                base = 16;
                prefix = alternate ? "0x" : 0;
            } else if (*p == 'X') {
                base = 16;
                uppercase = 1;
                prefix = alternate ? "0X" : 0;
            } else if (*p == 'o') {
                base = 8;
            }
            body = render_unsigned(value, base, uppercase, scratch_end);
            break;
        }
        case 'p': {
            void* value = va_arg(args, void*);
            body = render_unsigned((unsigned long long)value, 16, 0, scratch_end);
            prefix = "0x";
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G': {
            double const value = va_arg(args, double);
            /* Rendered whole, then run through the same padding and sign
             * handling as every other conversion. */
            render_double(rendered, value, precision, *p, alternate);
            body = rendered;
            if (value < 0.0 || (value == 0.0 && __builtin_signbit(value)))
                prefix = "-";
            else if (force_sign)
                prefix = "+";
            /* The precision was consumed by the conversion itself; it must not
             * also truncate the result the way it does for %s. */
            precision = -1;
            break;
        }
        case 'c':
            single[0] = (char)va_arg(args, int);
            body = single;
            break;
        case 's':
            body = va_arg(args, const char*);
            if (!body)
                body = "(null)";
            break;
        case '\0': return (int)sink->written;
        default:
            emit(sink, '%');
            emit(sink, *p);
            continue;
        }

        size_t body_length = strlen(body);
        if (precision >= 0 && *p == 's' && (size_t)precision < body_length)
            body_length = (size_t)precision;

        size_t const prefix_length = prefix ? strlen(prefix) : 0;
        size_t const total = prefix_length + body_length;
        size_t const padding = (size_t)width > total ? (size_t)width - total : 0;

        if (!left_align && !zero_pad) {
            for (size_t i = 0; i < padding; ++i)
                emit(sink, ' ');
        }
        if (prefix)
            emit_string(sink, prefix, prefix_length);
        if (!left_align && zero_pad) {
            for (size_t i = 0; i < padding; ++i)
                emit(sink, '0');
        }
        emit_string(sink, body, body_length);
        if (left_align) {
            for (size_t i = 0; i < padding; ++i)
                emit(sink, ' ');
        }
    }

    return (int)sink->written;
}

int vfprintf(FILE* stream, const char* format, va_list args)
{
    Sink sink = { stream, 0, 0, 0 };
    return format_into(&sink, format, args);
}

int vprintf(const char* format, va_list args)
{
    return vfprintf(stdout, format, args);
}

int vsnprintf(char* buffer, size_t size, const char* format, va_list args)
{
    Sink sink = { 0, buffer, size, 0 };
    int const result = format_into(&sink, format, args);
    if (size)
        buffer[sink.written < size ? sink.written : size - 1] = '\0';
    return result;
}

int printf(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int const result = vfprintf(stdout, format, args);
    va_end(args);
    return result;
}

int fprintf(FILE* stream, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int const result = vfprintf(stream, format, args);
    va_end(args);
    return result;
}

int snprintf(char* buffer, size_t size, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int const result = vsnprintf(buffer, size, format, args);
    va_end(args);
    return result;
}

int sprintf(char* buffer, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int const result = vsnprintf(buffer, (size_t)-1, format, args);
    va_end(args);
    return result;
}

/* --- positioning, buffering and temporary files -------------------------- */

int getc(FILE* stream)
{
    return fgetc(stream);
}
int putc(int c, FILE* stream)
{
    return fputc(c, stream);
}

void clearerr(FILE* stream)
{
    stream->flags &= ~(FLAG_EOF | FLAG_ERROR);
}

int fseek(FILE* stream, long offset, int whence)
{
    /* Buffered output has to reach the file before the position moves, and
     * anything read ahead has to be discarded: it came from a place we are no
     * longer going to be. */
    if (fflush(stream) == EOF)
        return -1;

    if (whence == SEEK_CUR) {
        /* The caller's idea of "here" is the logical position, which lags the
         * descriptor by whatever was read ahead and not yet consumed. */
        offset -= (long)(stream->available - stream->position);
        if (stream->unget != 0)
            offset -= 1;
    }

    stream->available = 0;
    stream->position = 0;
    stream->unget = 0;
    stream->flags &= ~FLAG_EOF;

    if (lseek(stream->fd, offset, whence) < 0) {
        stream->flags |= FLAG_ERROR;
        return -1;
    }
    return 0;
}

long ftell(FILE* stream)
{
    off_t where = lseek(stream->fd, 0, SEEK_CUR);
    if (where < 0) {
        stream->flags |= FLAG_ERROR;
        return -1;
    }

    /* Correct for what the buffer holds: bytes read ahead put the descriptor
     * too far forward, bytes pending output put it too far back. */
    where -= (off_t)(stream->available - stream->position);
    where += (off_t)stream->pending;
    if (stream->unget != 0)
        where -= 1;

    return (long)where;
}

void rewind(FILE* stream)
{
    fseek(stream, 0, SEEK_SET);
    clearerr(stream);
}

int setvbuf(FILE* stream, char* buffer, int mode, size_t size)
{
    /* Changing buffering under data that is already buffered would lose it. */
    fflush(stream);

    stream->flags &= ~(FLAG_LINE_BUFFERED | FLAG_UNBUFFERED);

    switch (mode) {
    case _IONBF: stream->flags |= FLAG_UNBUFFERED; return 0;
    case _IOLBF: stream->flags |= FLAG_LINE_BUFFERED; break;
    case _IOFBF: break;
    default: errno = EINVAL; return -1;
    }

    if (buffer != 0 && size > 0) {
        /* Only free a buffer we allocated; a static stream's is not ours. */
        if (!(stream->flags & FLAG_STATIC) && stream->buffer)
            free(stream->buffer);
        stream->buffer = buffer;
        stream->capacity = size;
        stream->pending = 0;
        stream->available = 0;
        stream->position = 0;
    }

    return 0;
}

void setbuf(FILE* stream, char* buffer)
{
    setvbuf(stream, buffer, buffer ? _IOFBF : _IONBF, BUFSIZ);
}

FILE* freopen(const char* path, const char* mode, FILE* stream)
{
    if (!stream)
        return 0;

    fflush(stream);

    /* freopen(NULL, mode, stream) changes the mode of an already-open stream.
     * There is no fcntl to do that with, so say so rather than ignore it. */
    if (path == 0) {
        errno = ENOSYS;
        return 0;
    }

    int const old_fd = stream->fd;
    FILE* const replacement = fopen(path, mode);
    if (!replacement)
        return 0;

    /* Move the new file onto the old descriptor number, so freopen(..., stdout)
     * really does redirect a program's output. */
    if (dup2(replacement->fd, old_fd) < 0) {
        fclose(replacement);
        return 0;
    }
    close(replacement->fd);

    int const was_static = stream->flags & FLAG_STATIC;
    if (!was_static && stream->buffer)
        free(stream->buffer);

    stream->fd = old_fd;
    stream->flags = replacement->flags | was_static;
    stream->buffer = replacement->buffer;
    stream->capacity = replacement->capacity;
    stream->pending = 0;
    stream->available = 0;
    stream->position = 0;
    stream->unget = 0;

    memset(replacement, 0, sizeof(*replacement));
    return stream;
}

char* tmpnam(char* buffer)
{
    static char fallback[L_tmpnam];
    static unsigned counter;

    char* const out = buffer ? buffer : fallback;
    snprintf(out, L_tmpnam, "/tmp/t%d-%u", (int)getpid(), counter++);
    return out;
}

FILE* tmpfile(void)
{
    char name[L_tmpnam];
    tmpnam(name);

    /*
     * The usual trick is to unlink immediately and let the open descriptor
     * keep the file alive. Inodes here are not reference counted, so that
     * would leave this stream pointing at freed memory. The file is left in
     * /tmp instead, which is a tmpfs and does not outlive the boot.
     */
    return fopen(name, "w+");
}

int remove(const char* path)
{
    if (unlink(path) == 0)
        return 0;
    /* unlink refuses directories; remove is specified to handle both. */
    if (errno == EISDIR || errno == EPERM)
        return rmdir(path);
    return -1;
}

int rename(const char* from, const char* to)
{
    (void)from;
    (void)to;
    /* No rename syscall exists yet, and emulating it with copy-then-unlink
     * would be neither atomic nor correct for directories. */
    errno = ENOSYS;
    return -1;
}
