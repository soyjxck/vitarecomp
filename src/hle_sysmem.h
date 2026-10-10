/* hle_sysmem.h — what the runtime itself takes from the memory-block arena. */

#ifndef VITARECOMP_HLE_SYSMEM_H
#define VITARECOMP_HLE_SYSMEM_H

#include <stdint.h>

/* A page-granular, zeroed region out of the arena with no uid: thread
 * stacks. Returns the base, or 0 when the arena is spent. */
uint32_t vita_sysmem_reserve(uint32_t size);

#endif /* VITARECOMP_HLE_SYSMEM_H */
