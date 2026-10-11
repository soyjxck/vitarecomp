/* hle_kernel.c — SceLibKernel: the process.
 *
 * The first of the kernel surface. Threads, synchronisation and timing follow
 * here as the milestone needs them (docs/PLAN.md, milestone A).
 */

#include "clock.h"
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
 * Microseconds since the process started, from the runtime's clock
 * (clock.h): the host's unless the host asked for a virtual one, and never
 * the frame counter alone, because a program that spins on this clock until
 * it moves must see it move. */

/* SceUInt64 sceKernelGetProcessTimeWide(void): a 64-bit result, in r0:r1 */
void vita_hle_sceKernelGetProcessTimeWide(void) {
    uint64_t t = vita_clock_us();
    r0 = (uint32_t)t;
    r1 = (uint32_t)(t >> 32);
}

/* SceUInt32 sceKernelGetProcessTimeLow(void) */
void vita_hle_sceKernelGetProcessTimeLow(void) { r0 = (uint32_t)vita_clock_us(); }

/* int sceKernelPowerTick(int type): keeps the console awake; nothing to keep. */
void vita_hle_sceKernelPowerTick(void) { r0 = 0; }

/* --- modules and power -------------------------------------------------------------
 *
 * sceSysmoduleLoadModule brings a firmware library into the process; here
 * every library the HLE answers for is already present, so loading succeeds
 * and "is it loaded" says yes (SCE_SYSMODULE_LOADED is 0). Except for a
 * library this runtime does not have at all: that one is not loaded and
 * cannot be, which is the truth and is an answer titles are written to take.
 * The movie container library is such a one (hle_movie.c): told that it
 * cannot be loaded, a title's movie player fails to start, and the title
 * goes on to what comes after the movie. Power configuration is a
 * clock-speed request the host has no equivalent of. */
#define SCE_SYSMODULE_MP4              0x2Du
#define SCE_SYSMODULE_ERROR_UNLOADED   0x805A1001u
#define SCE_SYSMODULE_ERROR_FATAL      0x805A10FFu

static int module_absent(uint32_t id) { return id == SCE_SYSMODULE_MP4; }

void vita_hle_sceSysmoduleLoadModule(void) {
    uint32_t id = r0;
    r0 = module_absent(id) ? SCE_SYSMODULE_ERROR_FATAL : 0;
    vita_hle_trace("sceSysmoduleLoadModule(0x%X) -> 0x%X", id, r0);
}
void vita_hle_sceSysmoduleUnloadModule(void) { r0 = 0; }
void vita_hle_sceSysmoduleIsLoaded(void)     { r0 = module_absent(r0) ? SCE_SYSMODULE_ERROR_UNLOADED : 0; }
void vita_hle_scePowerSetConfigurationMode(void) { r0 = 0; }
void vita_hle_scePowerSetArmClockFrequency(void) { r0 = 0; }
void vita_hle_scePowerSetGpuClockFrequency(void) { r0 = 0; }
void vita_hle_scePowerSetBusClockFrequency(void) { r0 = 0; }
void vita_hle_scePowerSetGpuXbarClockFrequency(void) { r0 = 0; }

/* --- SceAppUtil: the application utility library ------------------------------------
 * Initialisation succeeds; the save-data and system-parameter calls come as a
 * title asks for them. */
void vita_hle_sceAppUtilInit(void)     { vita_hle_trace("sceAppUtilInit(0x%08X, 0x%08X)", r0, r1); r0 = 0; }
void vita_hle_sceAppUtilShutdown(void) { r0 = 0; }
