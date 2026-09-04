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
    size_t pending;   /* bytes waiting in the buffer to be written */
    size_t available; /* bytes read into the buffer but not yet returned */
    size_t position;  /* how far through `available` the reader has got */

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

int putchar(int c) { return fputc(c, stdout); }

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

int getchar(void) { return fgetc(stdin); }

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
    default:
        errno = EINVAL;
        return 0;
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

int feof(FILE* stream) { return (stream->flags & FLAG_EOF) != 0; }
int ferror(FILE* stream) { return (stream->flags & FLAG_ERROR) != 0; }
int fileno(FILE* stream) { return stream->fd; }

/* --- formatting -------------------------------------------------------- */

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
            case '-': left_align = 1; ++p; break;
            case '0': zero_pad = 1; ++p; break;
            case '+': force_sign = 1; ++p; break;
            case '#': alternate = 1; ++p; break;
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
        const char* body = 0;
        const char* prefix = 0;
        char single[2] = { 0, 0 };

        switch (*p) {
        case 'd':
        case 'i': {
            long long value = length_modifier >= 2 ? va_arg(args, long long)
                : length_modifier == 1              ? va_arg(args, long)
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
            unsigned long long value = length_modifier >= 2 ? va_arg(args, unsigned long long)
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
        case 'c':
            single[0] = (char)va_arg(args, int);
            body = single;
            break;
        case 's':
            body = va_arg(args, const char*);
            if (!body)
                body = "(null)";
            break;
        case '\0':
            return (int)sink->written;
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

int vprintf(const char* format, va_list args) { return vfprintf(stdout, format, args); }

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
