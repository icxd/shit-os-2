/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- PS/2 keyboard driver.
 *
 * Written in C, on purpose: the module ABI is a C ABI, and the first driver
 * ought to prove that a module needs nothing from the kernel's language or
 * headers beyond <shitos/module/api.h>.
 *
 * Everything this file can do arrives through the KernelApi pointer handed to
 * module_init. There are no kernel symbols linked here -- try adding a call to
 * one and the loader will refuse the module by name.
 */

#include <shitos/module/api.h>
#include <shitos/types.h>

#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64
#define PS2_STATUS_OUTPUT_FULL 0x01

#define PS2_IRQ 1

#define SCANCODE_RELEASED 0x80
#define SCANCODE_EXTENDED 0xE0

#define BUFFER_SIZE 256

/*
 * Scancode set 1, US layout, indexed by make code. The row structure is
 * meaningful -- each line is a physical row of the keyboard -- so the formatter
 * is told to leave it alone.
 */
/* clang-format off */
static const char SCANCODE_TO_ASCII[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, /* 0x1d left control */
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, /* 0x2a left shift */
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0, /* 0x36 right shift */
    '*',
    0, /* 0x38 left alt */
    ' ',
    0, /* 0x3a caps lock */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* f1..f10 */
    0, /* num lock */
    0, /* scroll lock */
    '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.',
    0, 0, 0, 0, 0, /* f11, f12 and friends */
};

static const char SCANCODE_TO_ASCII_SHIFTED[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0,
    '*',
    0,
    ' ',
    0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,
    0,
    '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.',
    0, 0, 0, 0, 0,
};
/* clang-format on */

#define KEY_LEFT_CONTROL 0x1D
#define KEY_LEFT_SHIFT 0x2A
#define KEY_RIGHT_SHIFT 0x36
#define KEY_LEFT_ALT 0x38
#define KEY_CAPS_LOCK 0x3A

struct Ps2Keyboard {
    const KernelApi* kernel;
    WaitQueue* readers;

    /* Single-producer (the IRQ) single-consumer (a reading thread) ring. */
    char buffer[BUFFER_SIZE];
    volatile unsigned head;
    volatile unsigned tail;

    bool shift_held;
    bool control_held;
    bool caps_lock;
    bool expecting_extended;

    unsigned long keys_seen;
    unsigned long bytes_dropped;
};

static struct Ps2Keyboard g_keyboard;

static bool buffer_is_empty(const struct Ps2Keyboard* keyboard)
{
    return keyboard->head == keyboard->tail;
}

static void buffer_push(struct Ps2Keyboard* keyboard, char c)
{
    unsigned next = (keyboard->head + 1) % BUFFER_SIZE;
    if (next == keyboard->tail) {
        /* Full. Dropping the newest keystroke is less confusing than
         * overwriting the oldest, which would scramble what was typed. */
        ++keyboard->bytes_dropped;
        return;
    }
    keyboard->buffer[keyboard->head] = c;
    keyboard->head = next;
}

static bool buffer_pop(struct Ps2Keyboard* keyboard, char* out)
{
    if (buffer_is_empty(keyboard))
        return false;
    *out = keyboard->buffer[keyboard->tail];
    keyboard->tail = (keyboard->tail + 1) % BUFFER_SIZE;
    return true;
}

static char translate(struct Ps2Keyboard* keyboard, u8 scancode)
{
    bool uppercase = keyboard->shift_held;

    /* Caps lock only affects letters, and it inverts shift rather than
     * overriding it, which is why this is an xor and not an or. */
    char base = SCANCODE_TO_ASCII[scancode];
    if (keyboard->caps_lock && base >= 'a' && base <= 'z')
        uppercase = !uppercase;

    char c = uppercase ? SCANCODE_TO_ASCII_SHIFTED[scancode] : base;

    /* Control turns a letter into its control code: ^C is 3, ^D is 4. */
    if (keyboard->control_held && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 1);
    else if (keyboard->control_held && c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 1);

    return c;
}

static bool keyboard_irq(void* self, u8 irq)
{
    struct Ps2Keyboard* keyboard = (struct Ps2Keyboard*)self;
    const KernelApi* kernel = keyboard->kernel;

    (void)irq;

    /* Nothing pending means this interrupt belonged to somebody else on a
     * shared line; say so rather than eating it. */
    if ((kernel->inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) == 0)
        return false;

    u8 scancode = kernel->inb(PS2_DATA_PORT);

    if (scancode == SCANCODE_EXTENDED) {
        keyboard->expecting_extended = true;
        return true;
    }

    bool released = (scancode & SCANCODE_RELEASED) != 0;
    u8 code = (u8)(scancode & 0x7F);

    if (keyboard->expecting_extended) {
        /* Arrow keys, right control and so on. Not translated yet; the TTY
         * gains escape sequences in a later stage. */
        keyboard->expecting_extended = false;
        return true;
    }

    switch (code) {
    case KEY_LEFT_SHIFT:
    case KEY_RIGHT_SHIFT: keyboard->shift_held = !released; return true;
    case KEY_LEFT_CONTROL: keyboard->control_held = !released; return true;
    case KEY_CAPS_LOCK:
        if (!released)
            keyboard->caps_lock = !keyboard->caps_lock;
        return true;
    case KEY_LEFT_ALT: return true;
    default: break;
    }

    if (released)
        return true;

    char c = translate(keyboard, code);
    if (c != 0) {
        buffer_push(keyboard, c);
        ++keyboard->keys_seen;
        /* Safe from interrupt context, and the only reason a reader is not
         * spinning on an empty buffer. */
        kernel->waitqueue_wake_all(keyboard->readers);
    }

    return true;
}

static isize keyboard_read(void* self, void* buffer, usize length, u64 offset)
{
    struct Ps2Keyboard* keyboard = (struct Ps2Keyboard*)self;
    char* out = (char*)buffer;
    usize written = 0;

    (void)offset;

    if (length == 0)
        return 0;

    /* Block until there is at least one character, then drain what is there
     * without waiting again -- the same shape as a read from a terminal. */
    while (buffer_is_empty(keyboard))
        keyboard->kernel->waitqueue_wait(keyboard->readers);

    while (written < length) {
        char c;
        if (!buffer_pop(keyboard, &c))
            break;
        out[written++] = c;
    }

    return (isize)written;
}

static bool keyboard_poll_readable(void* self)
{
    return !buffer_is_empty((struct Ps2Keyboard*)self);
}

static const DeviceOps KEYBOARD_OPS = {
    .read = keyboard_read,
    .write = 0,
    .ioctl = 0,
    .poll_readable = keyboard_poll_readable,
};

static const DeviceDescriptor KEYBOARD_DEVICE = {
    .name = "kbd0",
    .type = DEVICE_TYPE_CHAR,
    .self = &g_keyboard,
    .ops = &KEYBOARD_OPS,
};

static ModuleResult module_init(const KernelApi* kernel)
{
    /* Newer is fine: the ABI only ever appends, so everything this driver
     * knows about is still where it expects. Older is not. */
    if (kernel->abi_version < SHITOS_MODULE_ABI_VERSION)
        return MODULE_ERR_ABI_MISMATCH;

    g_keyboard.kernel = kernel;
    g_keyboard.head = 0;
    g_keyboard.tail = 0;
    g_keyboard.shift_held = false;
    g_keyboard.control_held = false;
    g_keyboard.caps_lock = false;
    g_keyboard.expecting_extended = false;
    g_keyboard.keys_seen = 0;
    g_keyboard.bytes_dropped = 0;

    g_keyboard.readers = kernel->waitqueue_create();
    if (g_keyboard.readers == 0)
        return MODULE_ERR_NO_MEMORY;

    /* Drain anything the firmware left in the controller, or the first
     * interrupt never arrives because the output buffer is already full. */
    while (kernel->inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL)
        (void)kernel->inb(PS2_DATA_PORT);

    ModuleResult result = kernel->irq_register(PS2_IRQ, keyboard_irq, &g_keyboard);
    if (result != MODULE_OK) {
        kernel->waitqueue_destroy(g_keyboard.readers);
        return result;
    }

    result = kernel->device_register(&KEYBOARD_DEVICE);
    if (result != MODULE_OK) {
        kernel->irq_unregister(PS2_IRQ, &g_keyboard);
        kernel->waitqueue_destroy(g_keyboard.readers);
        return result;
    }

    kernel->log(LOG_INFO, "ps2kbd", "attached to irq %d, /dev/kbd0 ready", PS2_IRQ);
    return MODULE_OK;
}

static void module_fini(void)
{
    const KernelApi* kernel = g_keyboard.kernel;
    if (kernel == 0)
        return;

    kernel->device_unregister("kbd0");
    kernel->irq_unregister(PS2_IRQ, &g_keyboard);
    kernel->waitqueue_destroy(g_keyboard.readers);
    g_keyboard.readers = 0;
}

SHITOS_MODULE("ps2kbd", "PS/2 keyboard", "icxd", "GPL-3.0-or-later", module_init, module_fini);
