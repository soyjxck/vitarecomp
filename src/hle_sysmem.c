/* hle_sysmem.c — SceSysmem: memory blocks.
 *
 * A memblock is the Vita's unit of allocation: a named, typed, page-granular
 * region the program then carves up itself. Here it is a bump allocation out
 * of an arena the host reserves inside guest memory, with the console's
 * granularity kept — 4 KiB for main memory, 256 KiB for CDRAM — because a
 * program that rounds its request to that is one that works on hardware, and
 * one that does not should hear about it here rather than there.
 *
 * Calling convention: see hle.h. Arguments in r0-r3, result in r0, every
 * pointer a guest address.
 */

#include "vitarecomp/hle.h"
#include "vitarecomp/host.h"
#include "vitarecomp/recomp_rt.h"
#include "hle_sysmem.h"

#include <string.h>

#define SCE_KERNEL_ERROR_INVALID_ARGUMENT 0x80020005u
#define SCE_KERNEL_ERROR_INVALID_UID      0x80024501u
#define SCE_KERNEL_ERROR_NO_MEMORY        0x8002710Bu

#define MAX_BLOCKS 256
#define UID_FIRST  0x40010000u      /* positive, and unlike any pointer */

typedef struct { uint32_t base, size; int used; } block;

static block    g_blocks[MAX_BLOCKS];
static uint32_t g_base, g_size, g_next;

void vita_memblock_init(uint32_t base, uint32_t size) {
    g_base = base;
    g_size = size;
    g_next = base;
    memset(g_blocks, 0, sizeof(g_blocks));
}

static int uid_index(uint32_t uid) {
    if (uid < UID_FIRST || uid >= UID_FIRST + MAX_BLOCKS) return -1;
    int i = (int)(uid - UID_FIRST);
    return g_blocks[i].used ? i : -1;
}

/* SceUID sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
 *                               SceSize size, SceKernelAllocMemBlockOpt *opt)
 *
 * The top byte of the type names the memory: 0x09 is CDRAM (the GPU's, where
 * framebuffers go, 256 KiB blocks); everything else is main memory in 4 KiB
 * pages. The name and the options are accepted and ignored: nothing here
 * needs a name to find a block again, and the option block's alignment field
 * is a refinement for later. */
void vita_hle_sceKernelAllocMemBlock(void) {
    uint32_t type = r1, size = r2;
    uint32_t gran = (type >> 24) == 0x09 ? 0x40000u : 0x1000u;

    if (size == 0 || (size & (gran - 1))) {
        vita_hle_trace("sceKernelAllocMemBlock(type 0x%08X, size 0x%X): size is not a multiple of 0x%X, INVALID_ARGUMENT", type, size, gran);
        r0 = SCE_KERNEL_ERROR_INVALID_ARGUMENT;
        return;
    }

    uint32_t start = (g_next + gran - 1) & ~(gran - 1);
    if (start < g_next || start + size < start || start + size > g_base + g_size) {
        vita_hle_trace("sceKernelAllocMemBlock(type 0x%08X, size 0x%X): arena exhausted, NO_MEMORY", type, size);
        r0 = SCE_KERNEL_ERROR_NO_MEMORY;
        return;
    }

    int i;
    for (i = 0; i < MAX_BLOCKS && g_blocks[i].used; i++) {}
    if (i == MAX_BLOCKS) { r0 = SCE_KERNEL_ERROR_NO_MEMORY; return; }

    void *p = vita_mem_ptr(start, size);
    if (!p) { r0 = SCE_KERNEL_ERROR_NO_MEMORY; return; }
    memset(p, 0, size);                     /* blocks come zeroed on the console */

    g_blocks[i].base = start;
    g_blocks[i].size = size;
    g_blocks[i].used = 1;
    g_next = start + size;
    r0 = UID_FIRST + (uint32_t)i;
    vita_hle_trace("sceKernelAllocMemBlock(type 0x%08X, size 0x%X) -> 0x%08X at 0x%08X", type, size, r0, start);
}

/* int sceKernelGetMemBlockBase(SceUID uid, void **base) */
void vita_hle_sceKernelGetMemBlockBase(void) {
    int i = uid_index(r0);
    if (i < 0)                  { r0 = SCE_KERNEL_ERROR_INVALID_UID; return; }
    if (!vita_mem_ptr(r1, 4))   { r0 = SCE_KERNEL_ERROR_INVALID_ARGUMENT; return; }
    vita_write32(r1, g_blocks[i].base);
    r0 = 0;
}

/* int sceKernelFreeMemBlock(SceUID uid)
 *
 * The slot is released; the bytes are not reused, because the arena is a bump
 * allocator. Enough for a program that allocates at start-up and frees at
 * exit. A real allocator is on the roadmap alongside the SceLibc heap. */
void vita_hle_sceKernelFreeMemBlock(void) {
    int i = uid_index(r0);
    if (i < 0) { r0 = SCE_KERNEL_ERROR_INVALID_UID; return; }
    g_blocks[i].used = 0;
    r0 = 0;
}

/* A region with no uid, for the runtime's own needs (thread stacks). */
uint32_t vita_sysmem_reserve(uint32_t size) {
    uint32_t start = (g_next + 0xFFFu) & ~0xFFFu;
    size = (size + 0xFFFu) & ~0xFFFu;
    if (start < g_next || start + size < start || start + size > g_base + g_size) return 0;
    void *p = vita_mem_ptr(start, size);
    if (!p) return 0;
    memset(p, 0, size);
    g_next = start + size;
    return start;
}
