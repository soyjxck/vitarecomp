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
#define SEMA_UID_FIRST   0x40030000u
#define MAX_SEMAS        128
#define COND_UID_FIRST   0x40040000u
#define MAX_CONDS        64
#define THREAD_UID_FIRST 0x40050000u
#define MAX_THREADS      64

#define SCE_KERNEL_ERROR_WAIT_TIMEOUT     0x80028005u
#define SCE_KERNEL_ERROR_SEMA_ZERO        0x80028241u

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

/* --- semaphores ---------------------------------------------------------------
 *
 * One thread, so a wait either succeeds at once or can never succeed: nobody
 * else will signal. The second case is named, not spun on. When threads
 * arrive these become the points a thread gives the host up at. */

typedef struct { int used; int count, max; char name[32]; } sema;
static sema g_semas[MAX_SEMAS];

static int sema_index(uint32_t uid) {
    if (uid < SEMA_UID_FIRST || uid >= SEMA_UID_FIRST + MAX_SEMAS) return -1;
    int i = (int)(uid - SEMA_UID_FIRST);
    return g_semas[i].used ? i : -1;
}

static void copy_name(char *dst, uint32_t addr) {
    uint32_t n = vita_mem_avail(addr);
    const char *s = n ? (const char *)vita_mem_ptr(addr, 1) : NULL;
    uint32_t i = 0;
    for (; s && i < 31 && i < n && s[i]; i++) dst[i] = s[i];
    dst[i] = '\0';
}

/* SceUID sceKernelCreateSema(const char *name, SceUInt attr, int initVal, int maxVal, const void *opt) */
void vita_hle_sceKernelCreateSema(void) {
    int i;
    for (i = 0; i < MAX_SEMAS && g_semas[i].used; i++) {}
    if (i == MAX_SEMAS) { r0 = SCE_KERNEL_ERROR_NO_MEMORY; return; }
    g_semas[i].used  = 1;
    g_semas[i].count = (int)r2;
    g_semas[i].max   = (int)r3;
    copy_name(g_semas[i].name, r0);
    r0 = SEMA_UID_FIRST + (uint32_t)i;
    vita_hle_trace("sceKernelCreateSema(\"%s\", init %d, max %d) -> 0x%08X", g_semas[i].name, (int)r2, (int)r3, r0);
}
void vita_hle_sceKernelCreateSema_16XX(void) { vita_hle_sceKernelCreateSema(); }

/* int sceKernelWaitSema(SceUID id, int need, SceUInt *timeout) */
void vita_hle_sceKernelWaitSema(void) {
    int i = sema_index(r0);
    if (i < 0) { r0 = SCE_KERNEL_ERROR_INVALID_UID; return; }
    int need = (int)r1;
    if (g_semas[i].count >= need) { g_semas[i].count -= need; r0 = 0; return; }
    vita_trap_unimpl(r0, (uint32_t)need, "sceKernelWaitSema would block: one thread, no signaller");
    r0 = r2 ? SCE_KERNEL_ERROR_WAIT_TIMEOUT : 0;
}
void vita_hle_sceKernelWaitSemaCB(void) { vita_hle_sceKernelWaitSema(); }

/* int sceKernelPollSema(SceUID id, int need) */
void vita_hle_sceKernelPollSema(void) {
    int i = sema_index(r0);
    if (i < 0) { r0 = SCE_KERNEL_ERROR_INVALID_UID; return; }
    if (g_semas[i].count >= (int)r1) { g_semas[i].count -= (int)r1; r0 = 0; }
    else r0 = SCE_KERNEL_ERROR_SEMA_ZERO;
}

/* int sceKernelSignalSema(SceUID id, int count) */
void vita_hle_sceKernelSignalSema(void) {
    int i = sema_index(r0);
    if (i < 0) { r0 = SCE_KERNEL_ERROR_INVALID_UID; return; }
    g_semas[i].count += (int)r1;
    if (g_semas[i].max > 0 && g_semas[i].count > g_semas[i].max) g_semas[i].count = g_semas[i].max;
    r0 = 0;
}

/* int sceKernelDeleteSema(SceUID id) */
void vita_hle_sceKernelDeleteSema(void) {
    int i = sema_index(r0);
    if (i < 0) { r0 = SCE_KERNEL_ERROR_INVALID_UID; return; }
    g_semas[i].used = 0;
    r0 = 0;
}

/* SceUID sceKernelOpenSema(const char *name): a named one, by name */
void vita_hle_sceKernelOpenSema(void) {
    char name[32];
    copy_name(name, r0);
    for (int i = 0; i < MAX_SEMAS; i++)
        if (g_semas[i].used && strcmp(g_semas[i].name, name) == 0) { r0 = SEMA_UID_FIRST + (uint32_t)i; return; }
    r0 = SCE_KERNEL_ERROR_INVALID_UID;
}

/* int sceKernelCloseSema(SceUID id) */
void vita_hle_sceKernelCloseSema(void) { r0 = sema_index(r0) < 0 ? SCE_KERNEL_ERROR_INVALID_UID : 0; }

/* --- condition variables ---------------------------------------------------------- */

typedef struct { int used; } cond;
static cond g_conds[MAX_CONDS];

static int cond_index(uint32_t uid) {
    if (uid < COND_UID_FIRST || uid >= COND_UID_FIRST + MAX_CONDS) return -1;
    int i = (int)(uid - COND_UID_FIRST);
    return g_conds[i].used ? i : -1;
}

/* SceUID sceKernelCreateCond(const char *name, SceUInt attr, SceUID mutexId, const void *opt) */
void vita_hle_sceKernelCreateCond(void) {
    int i;
    for (i = 0; i < MAX_CONDS && g_conds[i].used; i++) {}
    if (i == MAX_CONDS) { r0 = SCE_KERNEL_ERROR_NO_MEMORY; return; }
    g_conds[i].used = 1;
    r0 = COND_UID_FIRST + (uint32_t)i;
}

/* int sceKernelWaitCond(SceUID id, SceUInt *timeout): nobody to signal it */
void vita_hle_sceKernelWaitCond(void) {
    if (cond_index(r0) < 0) { r0 = SCE_KERNEL_ERROR_INVALID_UID; return; }
    vita_trap_unimpl(r0, 0, "sceKernelWaitCond would block: one thread, no signaller");
    r0 = r1 ? SCE_KERNEL_ERROR_WAIT_TIMEOUT : 0;
}

/* int sceKernelSignalCond(SceUID id) / SignalCondAll / SignalCondTo */
void vita_hle_sceKernelSignalCond(void)    { r0 = cond_index(r0) < 0 ? SCE_KERNEL_ERROR_INVALID_UID : 0; }
void vita_hle_sceKernelSignalCondAll(void) { vita_hle_sceKernelSignalCond(); }
void vita_hle_sceKernelSignalCondTo(void)  { vita_hle_sceKernelSignalCond(); }

/* int sceKernelDeleteCond(SceUID id) */
void vita_hle_sceKernelDeleteCond(void) {
    int i = cond_index(r0);
    if (i < 0) { r0 = SCE_KERNEL_ERROR_INVALID_UID; return; }
    g_conds[i].used = 0;
    r0 = 0;
}

/* Lightweight condition variables live in the caller's memory, like the
 * lightweight mutexes, and wait on one of those. */
#define LWCOND_WORK_SIZE 32

/* int sceKernelCreateLwCond(SceKernelLwCondWork *work, const char *name, SceUInt attr,
 *                           SceKernelLwMutexWork *mutex, const void *opt) */
void vita_hle_sceKernelCreateLwCond(void) {
    if (!vita_mem_ptr(r0, LWCOND_WORK_SIZE)) { r0 = SCE_KERNEL_ERROR_INVALID_ARGUMENT; return; }
    for (uint32_t k = 0; k < LWCOND_WORK_SIZE; k += 4) vita_write32(r0 + k, 0);
    vita_write32(r0 + 4, r3);                     /* the mutex it pairs with */
    r0 = 0;
}

/* int sceKernelWaitLwCond(SceKernelLwCondWork *work, SceUInt *timeout) */
void vita_hle_sceKernelWaitLwCond(void) {
    if (!vita_mem_ptr(r0, LWCOND_WORK_SIZE)) { r0 = SCE_KERNEL_ERROR_INVALID_ARGUMENT; return; }
    vita_trap_unimpl(r0, 0, "sceKernelWaitLwCond would block: one thread, no signaller");
    r0 = r1 ? SCE_KERNEL_ERROR_WAIT_TIMEOUT : 0;
}

void vita_hle_sceKernelSignalLwCond(void)    { r0 = vita_mem_ptr(r0, LWCOND_WORK_SIZE) ? 0 : SCE_KERNEL_ERROR_INVALID_ARGUMENT; }
void vita_hle_sceKernelSignalLwCondAll(void) { vita_hle_sceKernelSignalLwCond(); }
void vita_hle_sceKernelDeleteLwCond(void)    { vita_hle_sceKernelSignalLwCond(); }

/* int sceKernelUnlockLwMutex2(SceKernelLwMutexWork *work, int unlockCount): the same call */
void vita_hle_sceKernelUnlockLwMutex2(void) { vita_hle_sceKernelUnlockLwMutex(); }

/* --- threads, before there is a scheduler -----------------------------------------
 *
 * Creation is bookkeeping and is answered. Starting one is the thing this
 * runtime cannot do yet: it would need a second flow of control with its own
 * registers and stack, switched at the waits above. So a start is a named
 * trap that says which entry point the program wanted running, which is the
 * work list for the scheduler. */

typedef struct { int used; uint32_t entry, stack_size, prio; char name[32]; } thread;
static thread g_threads[MAX_THREADS];

static int thread_index(uint32_t uid) {
    if (uid < THREAD_UID_FIRST || uid >= THREAD_UID_FIRST + MAX_THREADS) return -1;
    int i = (int)(uid - THREAD_UID_FIRST);
    return g_threads[i].used ? i : -1;
}

/* SceUID sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int initPriority,
 *                              SceSize stackSize, SceUInt attr, int cpuAffinityMask, const void *opt) */
void vita_hle_sceKernelCreateThread(void) {
    int i;
    for (i = 0; i < MAX_THREADS && g_threads[i].used; i++) {}
    if (i == MAX_THREADS) { r0 = SCE_KERNEL_ERROR_NO_MEMORY; return; }
    g_threads[i].used = 1;
    g_threads[i].entry = r1;
    g_threads[i].prio = r2;
    g_threads[i].stack_size = r3;
    copy_name(g_threads[i].name, r0);
    r0 = THREAD_UID_FIRST + (uint32_t)i;
    vita_hle_trace("sceKernelCreateThread(\"%s\", entry 0x%08X, prio %d, stack 0x%X) -> 0x%08X",
                   g_threads[i].name, g_threads[i].entry, (int)g_threads[i].prio, g_threads[i].stack_size, r0);
}

/* int sceKernelStartThread(SceUID id, SceSize argLen, void *argp) */
void vita_hle_sceKernelStartThread(void) {
    int i = thread_index(r0);
    if (i < 0) { r0 = SCE_KERNEL_ERROR_INVALID_UID; return; }
    vita_trap_unimpl(g_threads[i].entry, r0, "sceKernelStartThread: no scheduler yet; this entry point wants a thread");
    r0 = 0;
}

/* int sceKernelWaitThreadEnd(SceUID id, int *stat, SceUInt *timeout) */
void vita_hle_sceKernelWaitThreadEnd(void) {
    if (thread_index(r0) < 0) { r0 = SCE_KERNEL_ERROR_INVALID_UID; return; }
    if (r1 && vita_mem_ptr(r1, 4)) vita_write32(r1, 0);
    r0 = 0;
}
void vita_hle_sceKernelWaitThreadEndCB(void) { vita_hle_sceKernelWaitThreadEnd(); }

/* int sceKernelDeleteThread(SceUID id) */
void vita_hle_sceKernelDeleteThread(void) {
    int i = thread_index(r0);
    if (i < 0) { r0 = SCE_KERNEL_ERROR_INVALID_UID; return; }
    g_threads[i].used = 0;
    r0 = 0;
}

/* int sceKernelGetThreadInfo(SceUID id, SceKernelThreadInfo *info)
 * SceKernelThreadInfo begins { SceSize size; SceUID processId; char name[32];
 * SceUInt attr; SceUInt status; SceKernelThreadEntry entry; void *stack;
 * SceInt32 stackSize; SceInt32 initPriority; SceInt32 currentPriority; ... }
 * and the caller sets size first. Everything past the fields named here is
 * zeroed. */
void vita_hle_sceKernelGetThreadInfo(void) {
    uint32_t thid = r0, info = r1;
    uint32_t size = vita_mem_ptr(info, 4) ? vita_read32(info) : 0;
    if (size < 8 || !vita_mem_ptr(info, size)) { r0 = SCE_KERNEL_ERROR_INVALID_ARGUMENT; return; }
    for (uint32_t k = 4; k + 4 <= size; k += 4) vita_write32(info + k, 0);
    const char *name = "main";
    uint32_t entry = 0, stack_size = 0, prio = 0x40;
    int i = thread_index(thid);
    if (i >= 0) { name = g_threads[i].name; entry = g_threads[i].entry; stack_size = g_threads[i].stack_size; prio = g_threads[i].prio; }
    else if (thid != 0 && thid != MAIN_THREAD_UID) { r0 = SCE_KERNEL_ERROR_INVALID_UID; return; }
    vita_write32(info + 4, PROCESS_UID);
    for (uint32_t k = 0; k < 32 && 8 + k < size; k++) vita_write8(info + 8 + k, (uint8_t)(k < strlen(name) ? name[k] : 0));
    if (size >= 0x30) vita_write32(info + 0x2C, 1);          /* status: running */
    if (size >= 0x34) vita_write32(info + 0x30, entry);
    if (size >= 0x3C) vita_write32(info + 0x38, stack_size);
    if (size >= 0x40) vita_write32(info + 0x3C, prio);
    if (size >= 0x44) vita_write32(info + 0x40, prio);
    r0 = 0;
}

/* Thread attributes: accepted. There is nothing to schedule differently. */
void vita_hle_sceKernelChangeThreadPriority(void)        { r0 = 0; }
void vita_hle_sceKernelChangeThreadCpuAffinityMask(void) { r0 = 0; }
void vita_hle_sceKernelChangeThreadVfpException(void)    { r0 = 0; }
void vita_hle_sceKernelGetThreadCpuAffinityMask(void)    { r0 = 0; }
void vita_hle_sceKernelGetThreadCurrentPriority(void)    { r0 = 0x40; }

/* int sceKernelDelayThread(SceUInt usec): the only thread sleeping is the
 * program waiting, so the wait is spent in frames. */
void vita_hle_sceKernelDelayThread(void) {
    uint32_t us = r0;
    uint32_t frames = us >= 16667 ? (us + 16666) / 16667 : 0;
    for (uint32_t i = 0; i < frames; i++) vita_host_vblank();
    r0 = 0;
}
void vita_hle_sceKernelDelayThreadCB(void) { vita_hle_sceKernelDelayThread(); }
