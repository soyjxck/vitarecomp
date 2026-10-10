/* hle_rtc.c — SceRtc: the clock.
 *
 * The host's clock, in the console's units. Enough for a C library's
 * time() and for a program that stamps its frames.
 */

#include "vitarecomp/recomp_rt.h"

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
    vita_write32(r1, (uint32_t)time(NULL));
    r0 = 0;
}

/* int sceRtcGetCurrentClockLocalTime(SceDateTime *time)
 * SceDateTime: { u16 year, month, day, hour, minute, second; u32 microsecond } */
void vita_hle_sceRtcGetCurrentClockLocalTime(void) {
    if (!vita_mem_ptr(r0, 16)) { r0 = SCE_RTC_ERROR_INVALID_POINTER; return; }
    time_t now = time(NULL);
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
    time_t now = time(NULL);
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
        vita_write32(r0 + 0, (uint32_t)time(NULL));
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

static uint64_t now_tick(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return ((uint64_t)t.tv_sec + RTC_EPOCH_OFFSET_S) * 1000000ull + (uint64_t)t.tv_nsec / 1000ull;
}

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
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = vita_read16(r0) - 1900;
    t.tm_mon  = vita_read16(r0 + 2) - 1;
    t.tm_mday = vita_read16(r0 + 4);
    t.tm_hour = vita_read16(r0 + 6);
    t.tm_min  = vita_read16(r0 + 8);
    t.tm_sec  = vita_read16(r0 + 10);
#ifdef _WIN32
    uint64_t secs = (uint64_t)_mkgmtime(&t);
#else
    uint64_t secs = (uint64_t)timegm(&t);
#endif
    vita_write32(r1, (uint32_t)secs);
    vita_write32(r1 + 4, (uint32_t)(secs >> 32));
    r0 = 0;
}
