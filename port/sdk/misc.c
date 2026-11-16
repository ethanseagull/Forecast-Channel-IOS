// Small SDK pieces: caches (coherent on the host), the locked cache DMA queue,
// CRC32, and calendar time.
#include <SDL3/SDL.h>

#include <revolution/NET.h>
#include <revolution/OS.h>

#include <port_log.h>

void DCInvalidateRange(const void* buf, u32 len) {
    (void)buf;
    (void)len;
}

void DCFlushRangeNoSync(const void* buf, u32 len) {
    (void)buf;
    (void)len;
}

void PPCSync(void) {}

// The locked cache DMA queue: transfers are synchronous here (LCLoadBlocks /
// LCStoreBlocks copy immediately), so it is always empty.
u32 LCQueueLength(void) { return 0; }

void LCQueueWait(u32 n) { (void)n; }

// NET's CRC32 is the standard reflected CRC-32 (polynomial 0xEDB88320).
u32 NETCalcCRC32(const void* data, u32 size) {
    static u32 table[256];
    static BOOL init = FALSE;
    const u8* p = (const u8*)data;
    u32 crc = 0xFFFFFFFF;
    if (!init) {
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) {
                c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
            }
            table[i] = c;
        }
        init = TRUE;
    }
    while (size--) {
        crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

// --- calendar time ---------------------------------------------------------------
// The Wii's clock counts OS_TIME_SPEED ticks since 2000-01-01 00:00 local time
// (port/sdk/os.c: OSGetTime). These convert ticks to calendar fields.

static const int kDaysBeforeMonth[2][13] = {
    {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365},
    {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366},
};

static int is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static void days_to_calendar(s64 days, OSCalendarTime* cal) {
    // days since 2000-01-01 (a Saturday)
    int year = 2000;
    int leap;
    cal->wday = (int)((days + 6) % 7);
    if (cal->wday < 0) {
        cal->wday += 7;
    }
    while (days < 0) {
        year--;
        days += is_leap(year) ? 366 : 365;
    }
    for (;;) {
        int len = is_leap(year) ? 366 : 365;
        if (days < len) {
            break;
        }
        days -= len;
        year++;
    }
    leap = is_leap(year);
    cal->year = year;
    cal->yday = (s32)days;
    cal->month = 0;
    while (days >= kDaysBeforeMonth[leap][cal->month + 1]) {
        cal->month++;
    }
    cal->mday = (s32)(days - kDaysBeforeMonth[leap][cal->month]) + 1;
}

void OSTicksToCalendarTime(s64 ticks, OSCalendarTime* cal) {
    // ticks -> microseconds (OS_TIME_SPEED = 60.75 MHz = 243 / 4 per us)
    s64 us = ticks * 4 / 243;
    s64 secs = us / 1000000;
    s64 rem = us % 1000000;
    s64 days;
    if (rem < 0) {
        rem += 1000000;
        secs--;
    }
    cal->usec = (s32)(rem % 1000);
    cal->msec = (s32)(rem / 1000);
    days = secs / 86400;
    secs %= 86400;
    if (secs < 0) {
        secs += 86400;
        days--;
    }
    cal->hour = (s32)(secs / 3600);
    cal->min = (s32)(secs / 60 % 60);
    cal->sec = (s32)(secs % 60);
    days_to_calendar(days, cal);
}

BOOL NETGetUniversalCalendar(OSCalendarTime* cal) {
    SDL_Time now;
    if (!SDL_GetCurrentTime(&now)) {
        return FALSE;
    }
    // ns since 1970 UTC -> ticks since 2000 UTC
    OSTicksToCalendarTime((s64)((now / 1000 - 946684800000000LL) * 243 / 4), cal);
    return TRUE;
}
