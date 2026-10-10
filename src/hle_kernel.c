/* hle_kernel.c — SceLibKernel: the process.
 *
 * The first of the kernel surface. Threads, synchronisation and timing follow
 * here as the milestone needs them (docs/PLAN.md, milestone A).
 */

#include "vitarecomp/hle.h"
#include "vitarecomp/host.h"
#include "vitarecomp/recomp_rt.h"

#include <time.h>

/* int sceKernelExitProcess(int status): never returns. */
void vita_hle_sceKernelExitProcess(void) {
    vita_host_exit((int)r0);
}

/* --- the process clock ---------------------------------------------------------
 *
 * Microseconds since the process started, from the host's clock. The host's
 * and not the frame counter, because a program that spins on this clock
 * until it moves must see it move. */

static uint64_t now_us(void) {
    static int started;
    static struct timespec t0;
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    if (!started) { t0 = t; started = 1; }
    return (uint64_t)(t.tv_sec - t0.tv_sec) * 1000000ull + (uint64_t)(t.tv_nsec - t0.tv_nsec) / 1000ull;
}

/* SceUInt64 sceKernelGetProcessTimeWide(void): a 64-bit result, in r0:r1 */
void vita_hle_sceKernelGetProcessTimeWide(void) {
    uint64_t t = now_us();
    r0 = (uint32_t)t;
    r1 = (uint32_t)(t >> 32);
}

/* SceUInt32 sceKernelGetProcessTimeLow(void) */
void vita_hle_sceKernelGetProcessTimeLow(void) { r0 = (uint32_t)now_us(); }

/* int sceKernelPowerTick(int type): keeps the console awake; nothing to keep. */
void vita_hle_sceKernelPowerTick(void) { r0 = 0; }
