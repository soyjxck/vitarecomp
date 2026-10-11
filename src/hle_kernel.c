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
 * sceSysmoduleLoadModule brings a firmware library into the process; here every
 * library the HLE answers for is already present, so loading succeeds and
 * "is it loaded" says yes (SCE_SYSMODULE_LOADED is 0). Power configuration is
 * a clock-speed request the host has no equivalent of. */
void vita_hle_sceSysmoduleLoadModule(void)   { vita_hle_trace("sceSysmoduleLoadModule(0x%X)", r0); r0 = 0; }
void vita_hle_sceSysmoduleUnloadModule(void) { r0 = 0; }
void vita_hle_sceSysmoduleIsLoaded(void)     { r0 = 0; }
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
