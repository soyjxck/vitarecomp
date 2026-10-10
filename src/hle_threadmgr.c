/* hle_threadmgr.c — SceThreadmgr: threads, mutexes, thread-local storage.
 *
 * One thread of recompiled control exists, so far. What the C runtime asks
 * for at start-up is the bookkeeping of a multi-threaded world: a mutex for
 * malloc, a lightweight one per stdio stream, its own thread id, and a
 * thread-local slot to keep its reent pointer in. All of it is answered
 * truthfully for a process with one thread: the locks succeed at once, the
 * id is constant, and the TLS slots are real memory.
 *
 * Calling convention: see hle.h. Arguments in r0-r3, the fifth on the
 * stack, result in r0, every pointer a guest address.
 */

#include "vitarecomp/hle.h"
#include "vitarecomp/host.h"
#include "vitarecomp/recomp_rt.h"

#include <string.h>

#define SCE_KERNEL_ERROR_INVALID_ARGUMENT 0x80020005u
#define SCE_KERNEL_ERROR_INVALID_UID      0x80024501u
#define SCE_KERNEL_ERROR_NO_MEMORY        0x8002710Bu

#define MAIN_THREAD_UID  0x40010100u
#define PROCESS_UID      0x40010000u
#define MUTEX_UID_FIRST  0x40020000u
#define MAX_MUTEXES      64

/* --- a small region for kernel-side objects --------------------------------- */

static uint32_t g_kbase, g_ksize, g_knext;

void vita_kernel_init(uint32_t base, uint32_t size) {
    g_kbase = base; g_ksize = size; g_knext = base;
}

static uint32_t kalloc(uint32_t size) {
    uint32_t start = (g_knext + 15) & ~15u;
    if (start + size > g_kbase + g_ksize) return 0;
    void *p = vita_mem_ptr(start, size);
    if (!p) return 0;
    memset(p, 0, size);
    g_knext = start + size;
    return start;
}

/* --- thread identity -------------------------------------------------------- */

/* SceUID sceKernelGetThreadId(void) */
void vita_hle_sceKernelGetThreadId(void) { r0 = MAIN_THREAD_UID; }

/* SceUID sceKernelGetProcessId(void) */
void vita_hle_sceKernelGetProcessId(void) { r0 = PROCESS_UID; }

/* --- thread-local storage ----------------------------------------------------
 *
 * A thread's TLS block has the layout the console gives it, which Vita3K
 * reproduces (kernel/src/thread.cpp, kernel.cpp): 0x800 bytes of kernel-side
 * slots, then the module's thread-local image, and TPIDRURO points at the
 * image. The kernel part holds the 0x101 word slots that sceKernelGetTLSAddr
 * hands out by key (the C runtime keeps its reent pointer in one); the image
 * is what `mrc p15, 0, rt, c13, c0, 3` plus a linker-chosen offset reaches,
 * and how Sony's own runtime finds its per-thread state. Getting the two
 * into one block matters because both kinds of access happen in one program.
 * One thread, so one block. */

#define TLS_KERNEL_SIZE 0x800
#define TLS_SLOTS       0x101

static uint32_t g_tls_image, g_tls_filesz, g_tls_memsz;
static uint32_t g_tls;                  /* the block, made on first use */

void vita_tls_init(uint32_t image, uint32_t filesz, uint32_t memsz) {
    g_tls_image  = image;
    g_tls_filesz = filesz;
    g_tls_memsz  = memsz;
}

static uint32_t tls_block(void) {
    if (!g_tls) {
        g_tls = kalloc(TLS_KERNEL_SIZE + ((g_tls_memsz + 15) & ~15u));
        if (g_tls && g_tls_filesz) {
            void       *dst = vita_mem_ptr(g_tls + TLS_KERNEL_SIZE, g_tls_filesz);
            const void *src = vita_mem_ptr(g_tls_image, g_tls_filesz);
            if (dst && src) memcpy(dst, src, g_tls_filesz);
        }
    }
    return g_tls;
}

uint32_t vita_tls_base(void) {
    uint32_t b = tls_block();
    return b ? b + TLS_KERNEL_SIZE : 0;
}

static uint32_t tls_slot(uint32_t key) {
    if (key >= TLS_SLOTS) return 0;
    uint32_t b = tls_block();
    return b ? b + key * 4 : 0;
}

/* void *sceKernelGetTLSAddr(int key) */
void vita_hle_sceKernelGetTLSAddr(void) {
    uint32_t key = r0;
    r0 = tls_slot(key);
    vita_hle_trace("sceKernelGetTLSAddr(0x%X) -> 0x%08X (holds 0x%08X)", key, r0, r0 ? vita_read32(r0) : 0);
}

/* void *sceKernelGetThreadTLSAddr(SceUID thid, int key) */
void vita_hle_sceKernelGetThreadTLSAddr(void) {
    uint32_t thid = r0, key = r1;
    if (thid != MAIN_THREAD_UID && thid != 0) { r0 = 0; return; }
    r0 = tls_slot(key);
    vita_hle_trace("sceKernelGetThreadTLSAddr(0x%08X, 0x%X) -> 0x%08X", thid, key, r0);
}

/* --- lightweight mutexes -----------------------------------------------------
 *
 * The object lives in the caller's memory (a 32-byte SceKernelLwMutexWork);
 * the functions take its address. With one thread a lock never waits, so the
 * only thing worth keeping is the recursion count, and only so that a
 * program which unlocks more than it locked is told. */

#define LWMUTEX_WORK_SIZE 32

/* int sceKernelCreateLwMutex(SceKernelLwMutexWork *work, const char *name,
 *                            unsigned attr, int initCount, const void *opt) */
void vita_hle_sceKernelCreateLwMutex(void) {
    uint32_t work = r0;
    if (!vita_mem_ptr(work, LWMUTEX_WORK_SIZE)) { r0 = SCE_KERNEL_ERROR_INVALID_ARGUMENT; return; }
    for (uint32_t k = 0; k < LWMUTEX_WORK_SIZE; k += 4) vita_write32(work + k, 0);
    vita_write32(work + 0, r3);                 /* the current count */
    vita_write32(work + 4, MAIN_THREAD_UID);    /* the owner, when held */
    vita_hle_trace("sceKernelCreateLwMutex(work 0x%08X, attr 0x%X, count %d) -> 0", work, r2, (int)r3);
    r0 = 0;
}

/* int sceKernelLockLwMutex(SceKernelLwMutexWork *work, int lockCount, unsigned *timeout) */
void vita_hle_sceKernelLockLwMutex(void) {
    uint32_t work = r0;
    if (!vita_mem_ptr(work, LWMUTEX_WORK_SIZE)) { r0 = SCE_KERNEL_ERROR_INVALID_ARGUMENT; return; }
    vita_write32(work, vita_read32(work) + r1);
    r0 = 0;
}

/* int sceKernelTryLockLwMutex(SceKernelLwMutexWork *work, int lockCount) */
void vita_hle_sceKernelTryLockLwMutex(void) { vita_hle_sceKernelLockLwMutex(); }

/* int sceKernelUnlockLwMutex(SceKernelLwMutexWork *work, int unlockCount) */
void vita_hle_sceKernelUnlockLwMutex(void) {
    uint32_t work = r0;
    if (!vita_mem_ptr(work, LWMUTEX_WORK_SIZE)) { r0 = SCE_KERNEL_ERROR_INVALID_ARGUMENT; return; }
    uint32_t c = vita_read32(work);
    vita_write32(work, c >= r1 ? c - r1 : 0);
    r0 = 0;
}

/* int sceKernelDeleteLwMutex(SceKernelLwMutexWork *work) */
void vita_hle_sceKernelDeleteLwMutex(void) {
    if (!vita_mem_ptr(r0, LWMUTEX_WORK_SIZE)) { r0 = SCE_KERNEL_ERROR_INVALID_ARGUMENT; return; }
    r0 = 0;
}

/* --- mutexes ------------------------------------------------------------------ */

typedef struct { int used; int count; } mutex;
static mutex g_mutexes[MAX_MUTEXES];

static int mutex_index(uint32_t uid) {
    if (uid < MUTEX_UID_FIRST || uid >= MUTEX_UID_FIRST + MAX_MUTEXES) return -1;
    int i = (int)(uid - MUTEX_UID_FIRST);
    return g_mutexes[i].used ? i : -1;
}

/* SceUID sceKernelCreateMutex(const char *name, unsigned attr, int initCount, const void *opt) */
void vita_hle_sceKernelCreateMutex(void) {
    int i;
    for (i = 0; i < MAX_MUTEXES && g_mutexes[i].used; i++) {}
    if (i == MAX_MUTEXES) { r0 = SCE_KERNEL_ERROR_NO_MEMORY; return; }
    g_mutexes[i].used  = 1;
    g_mutexes[i].count = (int)r2;
    r0 = MUTEX_UID_FIRST + (uint32_t)i;
    vita_hle_trace("sceKernelCreateMutex(attr 0x%X, count %d) -> 0x%08X", r1, (int)r2, r0);
}

/* int sceKernelLockMutex(SceUID id, int lockCount, unsigned *timeout) */
void vita_hle_sceKernelLockMutex(void) {
    int i = mutex_index(r0);
    if (i < 0) { r0 = SCE_KERNEL_ERROR_INVALID_UID; return; }
    g_mutexes[i].count += (int)r1;
    r0 = 0;
}

/* int sceKernelTryLockMutex(SceUID id, int lockCount) */
void vita_hle_sceKernelTryLockMutex(void) { vita_hle_sceKernelLockMutex(); }

/* int sceKernelUnlockMutex(SceUID id, int unlockCount) */
void vita_hle_sceKernelUnlockMutex(void) {
    int i = mutex_index(r0);
    if (i < 0) { r0 = SCE_KERNEL_ERROR_INVALID_UID; return; }
    g_mutexes[i].count -= (int)r1;
    if (g_mutexes[i].count < 0) g_mutexes[i].count = 0;
    r0 = 0;
}

/* int sceKernelDeleteMutex(SceUID id) */
void vita_hle_sceKernelDeleteMutex(void) {
    int i = mutex_index(r0);
    if (i < 0) { r0 = SCE_KERNEL_ERROR_INVALID_UID; return; }
    g_mutexes[i].used = 0;
    r0 = 0;
}

/* --- leaving --------------------------------------------------------------------
 *
 * The main thread ending is the process ending, for a process with one
 * thread. newlib's exit path reaches sceKernelExitProcess first; these are
 * for the runtime's thread-exit paths. */

/* int sceKernelExitThread(int status) */
void vita_hle_sceKernelExitThread(void) { vita_host_exit((int)r0); }

/* int sceKernelExitDeleteThread(int status) */
void vita_hle_sceKernelExitDeleteThread(void) { vita_host_exit((int)r0); }
