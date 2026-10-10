/* hle_kernel.c — SceLibKernel: the process.
 *
 * The first of the kernel surface. Threads, synchronisation and timing follow
 * here as the milestone needs them (docs/PLAN.md, milestone A).
 */

#include "vitarecomp/host.h"
#include "vitarecomp/recomp_rt.h"

/* int sceKernelExitProcess(int status): never returns. */
void vita_hle_sceKernelExitProcess(void) {
    vita_host_exit((int)r0);
}
