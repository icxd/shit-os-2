/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- CMOS real-time clock.
 *
 * The second driver, and the reason for it is as much the module ABI as the
 * clock: one driver is not much evidence that a driver interface generalises.
 * This one needs something ps2kbd did not -- to hand the kernel a service
 * rather than take one -- which is what ABI version 2's time_source_register
 * exists for, and what proves the table can grow without breaking the module
 * that was written against version 1.
 *
 * Reading the CMOS is fiddly in three separate ways, and all three are
 * historical rather than difficult:
 *
 *   1. The chip updates itself once a second and the registers are garbage
 *      while it does. Status register A bit 7 says so, so we wait it out.
 *   2. Values may be BCD or binary, and 12- or 24-hour, depending on how the
 *      firmware left status register B. Both encodings are still in use.
 *   3. The century register moved around and is only reliable via ACPI's FADT,
 *      which we do not parse. Anything below 70 is taken as 20xx.
 */

#include <shitos/module/api.h>
#include <shitos/types.h>

#define CMOS_ADDRESS_PORT 0x70
#define CMOS_DATA_PORT 0x71

/* Bit 7 of the address port masks the NMI; leaving it set wedges the machine
 * against anything that needs one, so every write clears it. */
#define CMOS_NMI_DISABLE 0x80

#define CMOS_SECONDS 0x00
#define CMOS_MINUTES 0x02
#define CMOS_HOURS 0x04
#define CMOS_DAY_OF_MONTH 0x07
#define CMOS_MONTH 0x08
#define CMOS_YEAR 0x09
#define CMOS_STATUS_A 0x0A
#define CMOS_STATUS_B 0x0B

#define STATUS_A_UPDATE_IN_PROGRESS 0x80
#define STATUS_B_24_HOUR 0x02
#define STATUS_B_BINARY 0x04

#define HOUR_PM_FLAG 0x80

/* An update takes well under a millisecond; this is a ceiling, not a wait. */
#define UPDATE_TIMEOUT_ITERATIONS 1000000

typedef struct Rtc {
    const KernelApi* kernel;
    /* Latched at init so /dev/rtc0 can report what the hardware said without
     * reading it again -- the kernel's clock is the authority after boot. */
    i64 epoch_at_boot;
    bool valid;
} Rtc;

static Rtc g_rtc;

static u8 cmos_read(const KernelApi* kernel, u8 reg)
{
    kernel->outb(CMOS_ADDRESS_PORT, (u8)(reg & ~CMOS_NMI_DISABLE));
    return kernel->inb(CMOS_DATA_PORT);
}

static bool update_in_progress(const KernelApi* kernel)
{
    return (cmos_read(kernel, CMOS_STATUS_A) & STATUS_A_UPDATE_IN_PROGRESS) != 0;
}

static u8 from_bcd(u8 value)
{
    return (u8)((value & 0x0F) + ((value >> 4) * 10));
}

typedef struct RawTime {
    u8 second, minute, hour, day, month, year;
} RawTime;

static bool read_raw(const KernelApi* kernel, RawTime* out)
{
    long spins = 0;
    while (update_in_progress(kernel)) {
        if (++spins > UPDATE_TIMEOUT_ITERATIONS)
            return false;
    }

    out->second = cmos_read(kernel, CMOS_SECONDS);
    out->minute = cmos_read(kernel, CMOS_MINUTES);
    out->hour = cmos_read(kernel, CMOS_HOURS);
    out->day = cmos_read(kernel, CMOS_DAY_OF_MONTH);
    out->month = cmos_read(kernel, CMOS_MONTH);
    out->year = cmos_read(kernel, CMOS_YEAR);
    return true;
}

static bool same_time(const RawTime* a, const RawTime* b)
{
    return a->second == b->second && a->minute == b->minute && a->hour == b->hour
        && a->day == b->day && a->month == b->month && a->year == b->year;
}

/* Days from 1970-01-01 to the given civil date. Howard Hinnant's algorithm:
 * it shifts the year to start in March so that the leap day lands at the end
 * and the month-length pattern becomes a straight line. */
static i64 days_from_civil(i64 year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const i64 era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = (unsigned)(year - era * 400);
    const unsigned day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned day_of_era
        = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + (i64)day_of_era - 719468;
}

static i64 rtc_read_epoch(void* self)
{
    Rtc* rtc = (Rtc*)self;
    const KernelApi* kernel = rtc->kernel;

    /*
     * Read twice and require the same answer. The update flag closes the
     * window where the registers are being rewritten, but not the one where
     * the update starts between our first and last register read -- and a
     * reading taken across a second boundary can be a whole minute or hour
     * wrong, not just a second.
     */
    RawTime first, second;
    if (!read_raw(kernel, &first))
        return -1;
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (!read_raw(kernel, &second))
            return -1;
        if (same_time(&first, &second))
            break;
        first = second;
    }

    const u8 status_b = cmos_read(kernel, CMOS_STATUS_B);
    const bool binary = (status_b & STATUS_B_BINARY) != 0;
    const bool twenty_four_hour = (status_b & STATUS_B_24_HOUR) != 0;

    u8 hour_raw = second.hour;
    const bool pm = !twenty_four_hour && (hour_raw & HOUR_PM_FLAG) != 0;
    hour_raw = (u8)(hour_raw & ~HOUR_PM_FLAG);

    unsigned sec = binary ? second.second : from_bcd(second.second);
    unsigned min = binary ? second.minute : from_bcd(second.minute);
    unsigned hour = binary ? hour_raw : from_bcd(hour_raw);
    unsigned day = binary ? second.day : from_bcd(second.day);
    unsigned month = binary ? second.month : from_bcd(second.month);
    unsigned year = binary ? second.year : from_bcd(second.year);

    if (!twenty_four_hour) {
        /* 12-hour clocks number noon and midnight 12, not 0. */
        if (hour == 12)
            hour = 0;
        if (pm)
            hour += 12;
    }

    /* No century register we can trust without the FADT. The RTC will be
     * wrong in 2070 and so will a great deal else. */
    const i64 full_year = year >= 70 ? 1900 + (i64)year : 2000 + (i64)year;

    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || min > 59 || sec > 60) {
        kernel->log(LOG_WARN, "rtc", "nonsensical reading %04lld-%02u-%02u %02u:%02u:%02u",
            (long long)full_year, month, day, hour, min, sec);
        return -1;
    }

    const i64 days = days_from_civil(full_year, month, day);
    return days * 86400 + (i64)hour * 3600 + (i64)min * 60 + (i64)sec;
}

/*
 * /dev/rtc0 reads back what the hardware said at boot, as decimal seconds
 * since the epoch and a newline. It exists so the value is inspectable from a
 * shell; the kernel's own clock is what everything else should use, and it
 * does not go back to the hardware.
 */
static isize rtc_device_read(void* self, void* buffer, usize length, u64 offset)
{
    Rtc* rtc = (Rtc*)self;
    char text[32];
    usize written = 0;

    if (!rtc->valid)
        return 0;

    /* Render backwards into the tail, then copy the used part forward. */
    char digits[24];
    usize digit_count = 0;
    i64 value = rtc->epoch_at_boot;
    if (value == 0)
        digits[digit_count++] = '0';
    while (value > 0 && digit_count < sizeof(digits)) {
        digits[digit_count++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (digit_count > 0)
        text[written++] = digits[--digit_count];
    text[written++] = '\n';

    if (offset >= written)
        return 0;
    usize available = written - (usize)offset;
    if (available > length)
        available = length;
    for (usize i = 0; i < available; ++i)
        ((char*)buffer)[i] = text[(usize)offset + i];
    return (isize)available;
}

static const DeviceOps RTC_OPS = {
    .read = rtc_device_read,
    .write = 0,
    .ioctl = 0,
    .poll_readable = 0,
};

static const DeviceDescriptor RTC_DEVICE = {
    .name = "rtc0",
    .type = DEVICE_TYPE_CHAR,
    .self = &g_rtc,
    .ops = &RTC_OPS,
};

static ModuleResult module_init(const KernelApi* kernel)
{
    /* Built against version 2 and uses a version 2 entry, so a version 1
     * kernel genuinely cannot run this module. */
    if (kernel->abi_version < SHITOS_MODULE_ABI_VERSION)
        return MODULE_ERR_ABI_MISMATCH;

    g_rtc.kernel = kernel;
    g_rtc.valid = false;

    const i64 epoch = rtc_read_epoch(&g_rtc);
    if (epoch < 0) {
        kernel->log(LOG_ERROR, "rtc", "could not read the CMOS clock");
        return MODULE_ERR_IO;
    }
    g_rtc.epoch_at_boot = epoch;
    g_rtc.valid = true;

    if (kernel->time_source_register(rtc_read_epoch, &g_rtc) != MODULE_OK) {
        kernel->log(LOG_WARN, "rtc", "another time source got there first");
        return MODULE_ERR_BUSY;
    }

    ModuleResult result = kernel->device_register(&RTC_DEVICE);
    if (result != MODULE_OK) {
        kernel->time_source_unregister(&g_rtc);
        return result;
    }

    kernel->log(LOG_INFO, "rtc", "CMOS clock read, /dev/rtc0 ready");
    return MODULE_OK;
}

static void module_fini(void)
{
    const KernelApi* kernel = g_rtc.kernel;
    if (kernel == 0)
        return;

    kernel->device_unregister("rtc0");
    kernel->time_source_unregister(&g_rtc);
}

SHITOS_MODULE("rtc", "CMOS real-time clock", "icxd", "GPL-3.0-or-later", module_init, module_fini);
