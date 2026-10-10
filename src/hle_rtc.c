/* hle_rtc.c — SceRtc: the clock.
 *
 * The host's clock, in the console's units. Enough for a C library's
 * time() and for a program that stamps its frames.
 */

#include "vitarecomp/recomp_rt.h"

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
