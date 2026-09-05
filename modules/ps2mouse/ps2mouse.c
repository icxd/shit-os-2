/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- PS/2 mouse driver.
 *
 * The third loadable module, and the first one written after the ABI settled,
 * so it is a fair test of whether that ABI is actually usable: it needed no
 * additions at all. Ports, an IRQ, a wait queue and a device node were already
 * there.
 *
 * The mouse hangs off the same 8042 controller as the keyboard, on its second
 * ("auxiliary") channel. That means every byte here goes through the same two
 * ports the keyboard uses, distinguished only by a bit in the status register,
 * and that commands to the mouse have to be prefixed with 0xD4 or the
 * controller answers them itself.
 */

#include <shitos/abi/input.h>
#include <shitos/module/api.h>
#include <shitos/types.h>

#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64
#define PS2_COMMAND_PORT 0x64

#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_INPUT_FULL 0x02
/* Set when the pending byte came from the mouse rather than the keyboard. */
#define PS2_STATUS_FROM_AUX 0x20

#define PS2_COMMAND_ENABLE_AUX 0xA8
#define PS2_COMMAND_DISABLE_KEYBOARD 0xAD
#define PS2_COMMAND_ENABLE_KEYBOARD 0xAE
#define PS2_COMMAND_READ_CONFIG 0x20
#define PS2_COMMAND_WRITE_CONFIG 0x60
#define PS2_COMMAND_TO_AUX 0xD4

#define PS2_CONFIG_AUX_INTERRUPT 0x02
#define PS2_CONFIG_AUX_CLOCK_DISABLED 0x20

#define MOUSE_COMMAND_SET_SAMPLE_RATE 0xF3
#define MOUSE_COMMAND_GET_DEVICE_ID 0xF2
#define MOUSE_COMMAND_SET_DEFAULTS 0xF6
#define MOUSE_COMMAND_ENABLE_REPORTING 0xF4
#define MOUSE_ACK 0xFA

#define MOUSE_IRQ 12

/* First byte of every packet. Bit 3 is always set; a packet whose first byte
 * has it clear means we are reading mid-packet and should resynchronise. */
#define PACKET_ALWAYS_ONE 0x08
#define PACKET_Y_OVERFLOW 0x80
#define PACKET_X_OVERFLOW 0x40
#define PACKET_Y_SIGN 0x20
#define PACKET_X_SIGN 0x10

#define EVENT_CAPACITY 128

struct Ps2Mouse {
    const KernelApi* kernel;
    WaitQueue* readers;

    /* Single-producer (the IRQ) single-consumer ring, same shape as the
     * keyboard's, but of events rather than bytes. */
    struct mouse_event events[EVENT_CAPACITY];
    volatile unsigned head;
    volatile unsigned tail;

    /* Packet reassembly. A packet is three bytes, or four with a wheel. */
    u8 packet[4];
    unsigned packet_length;
    unsigned packet_size;

    bool has_wheel;

    unsigned long packets_seen;
    unsigned long events_dropped;
    unsigned long resynchronisations;
};

static struct Ps2Mouse g_mouse;

/* --- talking to the 8042 -------------------------------------------------- */

/*
 * The controller is far slower than the CPU and has no interrupt for "ready
 * for another byte", so every exchange is a poll. The bounds are arbitrary but
 * finite: a missing or wedged controller must not hang the boot.
 */
static bool wait_writable(const KernelApi* kernel)
{
    for (int i = 0; i < 100000; ++i) {
        if ((kernel->inb(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL) == 0)
            return true;
    }
    return false;
}

static bool wait_readable(const KernelApi* kernel)
{
    for (int i = 0; i < 100000; ++i) {
        if ((kernel->inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) != 0)
            return true;
    }
    return false;
}

static bool write_command(const KernelApi* kernel, u8 command)
{
    if (!wait_writable(kernel))
        return false;
    kernel->outb(PS2_COMMAND_PORT, command);
    return true;
}

static bool write_data(const KernelApi* kernel, u8 value)
{
    if (!wait_writable(kernel))
        return false;
    kernel->outb(PS2_DATA_PORT, value);
    return true;
}

static bool read_data(const KernelApi* kernel, u8* out)
{
    if (!wait_readable(kernel))
        return false;
    *out = kernel->inb(PS2_DATA_PORT);
    return true;
}

/* A command *to the mouse*, as opposed to one to the controller. */
static bool mouse_command(const KernelApi* kernel, u8 command, u8* response)
{
    if (!write_command(kernel, PS2_COMMAND_TO_AUX))
        return false;
    if (!write_data(kernel, command))
        return false;

    u8 acknowledgement = 0;
    if (!read_data(kernel, &acknowledgement))
        return false;
    if (acknowledgement != MOUSE_ACK)
        return false;

    if (response != 0)
        return read_data(kernel, response);
    return true;
}

/* --- the event ring ------------------------------------------------------- */

static void push_event(struct Ps2Mouse* mouse, const struct mouse_event* event)
{
    unsigned next = (mouse->head + 1) % EVENT_CAPACITY;
    if (next == mouse->tail) {
        /* A full ring means nobody is reading. Motion is only useful while it
         * is fresh, so drop the newest rather than letting the pointer replay
         * a second of history when the reader wakes up. */
        ++mouse->events_dropped;
        return;
    }
    mouse->events[mouse->head] = *event;
    mouse->head = next;
}

/* --- the interrupt -------------------------------------------------------- */

static bool mouse_irq(void* self, u8 irq)
{
    struct Ps2Mouse* mouse = (struct Ps2Mouse*)self;
    const KernelApi* kernel = mouse->kernel;

    (void)irq;

    u8 status = kernel->inb(PS2_STATUS_PORT);
    if ((status & PS2_STATUS_OUTPUT_FULL) == 0)
        return false; /* not ours; a shared line */
    if ((status & PS2_STATUS_FROM_AUX) == 0)
        return false; /* the keyboard's byte, on the keyboard's line */

    u8 byte = kernel->inb(PS2_DATA_PORT);

    /*
     * Resynchronise on the first byte. If bit 3 is clear this cannot be the
     * start of a packet, so we have lost our place -- usually because a byte
     * arrived before the IRQ was hooked up. Dropping it is how we get back in
     * step; keeping it would misinterpret every packet from here on.
     */
    if (mouse->packet_length == 0 && (byte & PACKET_ALWAYS_ONE) == 0) {
        ++mouse->resynchronisations;
        return true;
    }

    mouse->packet[mouse->packet_length++] = byte;
    if (mouse->packet_length < mouse->packet_size)
        return true;

    mouse->packet_length = 0;
    ++mouse->packets_seen;

    u8 const flags = mouse->packet[0];

    /*
     * An overflow means the mouse moved further between packets than a byte
     * can describe. The delta in the packet is meaningless in that case, so
     * report no motion rather than a wrong amount -- a pointer that stops for
     * one packet is less wrong than one that jumps backwards.
     */
    struct mouse_event event;
    if ((flags & (PACKET_X_OVERFLOW | PACKET_Y_OVERFLOW)) != 0) {
        event.dx = 0;
        event.dy = 0;
    } else {
        /* Nine-bit two's complement: eight bits in the byte, the sign in the
         * flags. Sign-extend by hand rather than casting and hoping. */
        i16 dx = (i16)mouse->packet[1];
        i16 dy = (i16)mouse->packet[2];
        if ((flags & PACKET_X_SIGN) != 0)
            dx |= (i16)0xFF00;
        if ((flags & PACKET_Y_SIGN) != 0)
            dy |= (i16)0xFF00;
        event.dx = dx;
        event.dy = dy;
    }

    event.dz = 0;
    if (mouse->has_wheel) {
        /* Four bits of signed scroll in the low nibble of the fourth byte. */
        i8 dz = (i8)(mouse->packet[3] & 0x0F);
        if (dz > 7)
            dz = (i8)(dz - 16);
        event.dz = dz;
    }

    event.buttons = (u8)(flags & (MOUSE_BUTTON_LEFT | MOUSE_BUTTON_RIGHT | MOUSE_BUTTON_MIDDLE));
    event._reserved = 0;

    /* A packet with no motion, no scroll and no button change still arrives
     * when the mouse is jostled; forwarding it would wake every reader for
     * nothing. */
    if (event.dx != 0 || event.dy != 0 || event.dz != 0 || event.buttons != 0
        || mouse->head != mouse->tail) {
        push_event(mouse, &event);
        kernel->waitqueue_wake_all(mouse->readers);
    }

    return true;
}

/* --- the device ----------------------------------------------------------- */

static isize mouse_read(void* self, void* buffer, usize length, u64 offset)
{
    struct Ps2Mouse* mouse = (struct Ps2Mouse*)self;
    const KernelApi* kernel = mouse->kernel;

    (void)offset;

    /* Whole events only. A caller that asks for less than one gets EINVAL
     * rather than half a packet it would have to reassemble. */
    if (length < sizeof(struct mouse_event))
        return -MODULE_ERR_INVALID;

    while (mouse->head == mouse->tail)
        kernel->waitqueue_wait(mouse->readers);

    u8* out = (u8*)buffer;
    usize written = 0;
    while (written + sizeof(struct mouse_event) <= length && mouse->head != mouse->tail) {
        struct mouse_event const event = mouse->events[mouse->tail];
        mouse->tail = (mouse->tail + 1) % EVENT_CAPACITY;

        const u8* source = (const u8*)&event;
        for (usize i = 0; i < sizeof(event); ++i)
            out[written + i] = source[i];
        written += sizeof(event);
    }

    return (isize)written;
}

static bool mouse_poll_readable(void* self)
{
    struct Ps2Mouse* mouse = (struct Ps2Mouse*)self;
    return mouse->head != mouse->tail;
}

static const DeviceOps MOUSE_OPS = {
    .read = mouse_read,
    .write = 0,
    .ioctl = 0,
    .poll_readable = mouse_poll_readable,
};

/* --- bring-up ------------------------------------------------------------- */

/*
 * The knock that asks an ordinary two-button mouse whether it is secretly an
 * IntelliMouse: sample rates 200, 100, 80 in that order, then ask for the
 * device id. A wheel mouse answers 3 and starts sending four-byte packets; a
 * plain one answers 0 and carries on with three.
 */
static bool detect_wheel(const KernelApi* kernel)
{
    static const u8 KNOCK[] = { 200, 100, 80 };

    for (unsigned i = 0; i < sizeof(KNOCK); ++i) {
        if (!mouse_command(kernel, MOUSE_COMMAND_SET_SAMPLE_RATE, 0))
            return false;
        if (!write_command(kernel, PS2_COMMAND_TO_AUX))
            return false;
        if (!write_data(kernel, KNOCK[i]))
            return false;

        u8 acknowledgement = 0;
        if (!read_data(kernel, &acknowledgement) || acknowledgement != MOUSE_ACK)
            return false;
    }

    u8 identifier = 0;
    if (!mouse_command(kernel, MOUSE_COMMAND_GET_DEVICE_ID, &identifier))
        return false;

    return identifier == 3;
}

/*
 * The conversation itself, split out so that its caller can guarantee the
 * keyboard comes back on however this ends.
 */
static ModuleResult setup_mouse(const KernelApi* kernel)
{
    /* Give the controller its second channel. */
    if (!write_command(kernel, PS2_COMMAND_ENABLE_AUX)) {
        kernel->log(LOG_ERROR, "ps2mouse", "the 8042 will not enable its aux port");
        return MODULE_ERR_IO;
    }

    /* Turn on the interrupt for that channel, and make sure its clock is not
     * disabled. Read-modify-write: the keyboard's bits are in the same byte
     * and clearing them would take the console with it. */
    if (!write_command(kernel, PS2_COMMAND_READ_CONFIG)) {
        kernel->log(LOG_ERROR, "ps2mouse", "cannot ask for the configuration byte");
        return MODULE_ERR_IO;
    }
    u8 config = 0;
    if (!read_data(kernel, &config)) {
        kernel->log(LOG_ERROR, "ps2mouse", "the configuration byte never arrived");
        return MODULE_ERR_IO;
    }

    config |= PS2_CONFIG_AUX_INTERRUPT;
    config &= (u8)~PS2_CONFIG_AUX_CLOCK_DISABLED;

    if (!write_command(kernel, PS2_COMMAND_WRITE_CONFIG) || !write_data(kernel, config)) {
        kernel->log(LOG_ERROR, "ps2mouse", "cannot write the configuration byte back");
        return MODULE_ERR_IO;
    }

    if (!mouse_command(kernel, MOUSE_COMMAND_SET_DEFAULTS, 0)) {
        kernel->log(LOG_WARN, "ps2mouse", "no mouse answered on the aux port");
        return MODULE_ERR_NO_DEVICE;
    }

    g_mouse.has_wheel = detect_wheel(kernel);
    g_mouse.packet_size = g_mouse.has_wheel ? 4 : 3;

    if (!mouse_command(kernel, MOUSE_COMMAND_ENABLE_REPORTING, 0)) {
        kernel->log(LOG_ERROR, "ps2mouse", "the mouse will not start reporting");
        return MODULE_ERR_IO;
    }

    return MODULE_OK;
}

/*
 * Setting the mouse up means having a conversation with the 8042, and every
 * reply it gives lands in the same one-byte output buffer the keyboard uses --
 * and raises the keyboard's interrupt on the way. The already-loaded ps2kbd
 * module then reads our reply as a scancode and throws it away, and every read
 * here times out.
 *
 * So the keyboard is switched off at the controller for the length of the
 * exchange. It is switched back on at the end, including on every failure
 * path, because leaving it off would cost the machine its console.
 */
static ModuleResult mouse_init(const KernelApi* kernel)
{
    if (kernel->abi_version < SHITOS_MODULE_ABI_VERSION)
        return MODULE_ERR_ABI_MISMATCH;

    g_mouse.kernel = kernel;
    g_mouse.packet_size = 3;

    if (!write_command(kernel, PS2_COMMAND_DISABLE_KEYBOARD)) {
        kernel->log(LOG_ERROR, "ps2mouse", "the 8042 will not stop the keyboard");
        return MODULE_ERR_IO;
    }

    /* Anything already pending is a scancode nobody will now read. Drop it, so
     * the first byte we wait for is genuinely ours. */
    while ((kernel->inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) != 0)
        (void)kernel->inb(PS2_DATA_PORT);

    ModuleResult status = setup_mouse(kernel);

    /* Unconditionally, and last: this also clears the port-1-disabled bit that
     * the command above set in the configuration byte. */
    (void)write_command(kernel, PS2_COMMAND_ENABLE_KEYBOARD);

    if (status != MODULE_OK)
        return status;

    g_mouse.readers = kernel->waitqueue_create();
    if (g_mouse.readers == 0)
        return MODULE_ERR_NO_MEMORY;

    ModuleResult result = kernel->irq_register(MOUSE_IRQ, mouse_irq, &g_mouse);
    if (result != MODULE_OK) {
        kernel->waitqueue_destroy(g_mouse.readers);
        g_mouse.readers = 0;
        return result;
    }

    DeviceDescriptor device = {
        .name = "mouse0",
        .type = DEVICE_TYPE_CHAR,
        .self = &g_mouse,
        .ops = &MOUSE_OPS,
    };

    result = kernel->device_register(&device);
    if (result != MODULE_OK) {
        kernel->irq_unregister(MOUSE_IRQ, &g_mouse);
        kernel->waitqueue_destroy(g_mouse.readers);
        g_mouse.readers = 0;
        return result;
    }

    kernel->log(LOG_INFO, "ps2mouse", "attached to irq %d, /dev/mouse0 ready (%s)", MOUSE_IRQ,
        g_mouse.has_wheel ? "with wheel" : "three button, no wheel");
    return MODULE_OK;
}

static void mouse_fini(void)
{
    const KernelApi* kernel = g_mouse.kernel;
    if (kernel == 0)
        return;

    kernel->device_unregister("mouse0");
    kernel->irq_unregister(MOUSE_IRQ, &g_mouse);

    if (g_mouse.readers != 0) {
        /* Anything blocked in read must be let go before the queue dies. */
        kernel->waitqueue_wake_all(g_mouse.readers);
        kernel->waitqueue_destroy(g_mouse.readers);
        g_mouse.readers = 0;
    }

    kernel->log(LOG_INFO, "ps2mouse", "detached after %lu packets, %lu dropped, %lu resyncs",
        g_mouse.packets_seen, g_mouse.events_dropped, g_mouse.resynchronisations);
}

SHITOS_MODULE("ps2mouse", "PS/2 mouse", "icxd", "GPL-3.0-or-later", mouse_init, mouse_fini);
