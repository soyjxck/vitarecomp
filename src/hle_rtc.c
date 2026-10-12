/* hle_rtc.c — SceRtc: the clock.
 *
 * The runtime's clock (clock.h), in the console's units. Enough for a C
 * library's time() and for a program that stamps its frames.
 */

#include "vitarecomp/recomp_rt.h"
#include "clock.h"

#include <string.h>
#include <time.h>

#define SCE_RTC_ERROR_INVALID_POINTER 0x80251001u

/* int sceRtcGetTime_t(const SceDateTime *time, time_t *t)
 *
 * Despite the name this converts a SceDateTime; the C library's time() goes
 * through sceRtcGetCurrentClock. Both are answered with the host's clock:
 * a SceDateTime of now, and the seconds since the epoch. */
void vita_hle_sceRtcGetTime_t(void) {
    if (!vita_mem_ptr(r1, 4)) { r0 = SCE_RTC_ERROR_INVALID_POINTER; return; }
    vita_write32(r1, (uint32_t)(vita_clock_unix_us() / 1000000ull));
    r0 = 0;
}

/* int sceRtcGetCurrentClockLocalTime(SceDateTime *time)
 * SceDateTime: { u16 year, month, day, hour, minute, second; u32 microsecond } */
void vita_hle_sceRtcGetCurrentClockLocalTime(void) {
    if (!vita_mem_ptr(r0, 16)) { r0 = SCE_RTC_ERROR_INVALID_POINTER; return; }
    time_t now = (time_t)(vita_clock_unix_us() / 1000000ull);
    struct tm *t = localtime(&now);
    vita_write16(r0 + 0,  (uint16_t)(t->tm_year + 1900));
    vita_write16(r0 + 2,  (uint16_t)(t->tm_mon + 1));
    vita_write16(r0 + 4,  (uint16_t)t->tm_mday);
    vita_write16(r0 + 6,  (uint16_t)t->tm_hour);
    vita_write16(r0 + 8,  (uint16_t)t->tm_min);
    vita_write16(r0 + 10, (uint16_t)t->tm_sec);
    vita_write32(r0 + 12, 0);
    r0 = 0;
}

/* int sceRtcGetCurrentClock(SceDateTime *time, int tz): UTC */
void vita_hle_sceRtcGetCurrentClock(void) {
    if (!vita_mem_ptr(r0, 16)) { r0 = SCE_RTC_ERROR_INVALID_POINTER; return; }
    time_t now = (time_t)(vita_clock_unix_us() / 1000000ull);
    struct tm *t = gmtime(&now);
    vita_write16(r0 + 0,  (uint16_t)(t->tm_year + 1900));
    vita_write16(r0 + 2,  (uint16_t)(t->tm_mon + 1));
    vita_write16(r0 + 4,  (uint16_t)t->tm_mday);
    vita_write16(r0 + 6,  (uint16_t)t->tm_hour);
    vita_write16(r0 + 8,  (uint16_t)t->tm_min);
    vita_write16(r0 + 10, (uint16_t)t->tm_sec);
    vita_write32(r0 + 12, 0);
    r0 = 0;
}

/* SceUInt64 sceRtcGetCurrentTick / int sceKernelGetProcessTimeLow and
 * friends live with the kernel; the C library's gettimeofday goes through
 * sceKernelLibcGettimeofday. */
void vita_hle_sceKernelLibcGettimeofday(void) {
    /* int sceKernelLibcGettimeofday(SceKernelTimeval *tv, SceKernelTimezone *tz)
     * SceKernelTimeval: { u32 sec; u32 usec } */
    if (r0 && vita_mem_ptr(r0, 8)) {
        vita_write32(r0 + 0, (uint32_t)(vita_clock_unix_us() / 1000000ull));
        vita_write32(r0 + 4, 0);
    }
    if (r1 && vita_mem_ptr(r1, 8)) {
        vita_write32(r1 + 0, 0);
        vita_write32(r1 + 4, 0);
    }
    r0 = 0;
}

/* --- ticks -----------------------------------------------------------------------
 * A tick is a microsecond since 0001-01-01: the Unix epoch is 62,135,596,800
 * seconds in. SceRtcTick is { SceUInt64 tick }. */
#define RTC_EPOCH_OFFSET_S 62135596800ull

static uint64_t now_tick(void) { return vita_clock_unix_us() + RTC_EPOCH_OFFSET_S * 1000000ull; }

/* int sceRtcGetCurrentTick(SceRtcTick *tick) */
void vita_hle_sceRtcGetCurrentTick(void) {
    if (!vita_mem_ptr(r0, 8)) { r0 = SCE_RTC_ERROR_INVALID_POINTER; return; }
    uint64_t t = now_tick();
    vita_write32(r0, (uint32_t)t);
    vita_write32(r0 + 4, (uint32_t)(t >> 32));
    r0 = 0;
}

/* unsigned int sceRtcGetTickResolution(void): ticks per second */
void vita_hle_sceRtcGetTickResolution(void) { r0 = 1000000u; }

/* int sceRtcGetTime64_t(const SceDateTime *time, SceUInt64 *out): seconds since 1970 */
void vita_hle_sceRtcGetTime64_t(void) {
    if (!vita_mem_ptr(r0, 16) || !vita_mem_ptr(r1, 8)) { r0 = SCE_RTC_ERROR_INVALID_POINTER; return; }
    /* the calendar worked out here: the C library's own way from a broken-down
     * time in UTC to seconds has a different name on every system, or none */
    int y = vita_read16(r0), m = vita_read16(r0 + 2), d = vita_read16(r0 + 4);
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = (int64_t)era * 146097 + doe - 719468;      /* since 1970-01-01 */
    uint64_t secs = (uint64_t)(days * 86400 + vita_read16(r0 + 6) * 3600 + vita_read16(r0 + 8) * 60 + vita_read16(r0 + 10));
    vita_write32(r1, (uint32_t)secs);
    vita_write32(r1 + 4, (uint32_t)(secs >> 32));
    r0 = 0;
}
