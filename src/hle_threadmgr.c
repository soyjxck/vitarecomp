/* hle_threadmgr.c — SceThreadmgr: threads, synchronisation, thread-local storage.
 *
 * The objects are answered the way the console's kernel answers them, over
 * the scheduler in sched.c: a lock that is free is taken at once; one that
 * is held puts the caller to sleep until the holder lets go; a semaphore
 * count that is short sleeps until a signal; a condition variable releases
 * its mutex, sleeps, and takes it back. Every sleep is a point where another
 * guest thread runs. A woken thread re-checks what it waited for, because
 * the thread that woke it may not be the only one that was waiting.
 *
 * Calling convention: see hle.h. Arguments in r0-r3, the fifth on the
 * stack, result in r0, every pointer a guest address.
 */

#include "vitarecomp/hle.h"
#include "vitarecomp/host.h"
#include "vitarecomp/recomp_rt.h"
#include "sched.h"

#include <string.h>

#define SCE_KERNEL_ERROR_INVALID_ARGUMENT 0x80020005u
#define SCE_KERNEL_ERROR_INVALID_UID      0x80024501u
#define SCE_KERNEL_ERROR_NO_MEMORY        0x8002710Bu
#define SCE_KERNEL_ERROR_WAIT_TIMEOUT     0x80028005u
#define SCE_KERNEL_ERROR_SEMA_ZERO        0x80028241u
#define SCE_KERNEL_ERROR_MUTEX_FAILED_TO_OWN 0x80028185u

#define PROCESS_UID      0x40010000u
#define MUTEX_UID_FIRST  0x40020000u
#define MAX_MUTEXES      128
#define SEMA_UID_FIRST   0x40030000u
#define MAX_SEMAS        256
#define COND_UID_FIRST   0x40040000u
#define MAX_CONDS        128

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

/* A timeout argument: a pointer to microseconds, or NULL for forever. */
static uint32_t timeout_us(uint32_t ptr) {
    if (!ptr || !vita_mem_ptr(ptr, 4)) return 0;
    uint32_t us = vita_read32(ptr);
    return us ? us : 1;
}

static void copy_name(char *dst, uint32_t addr) {
    uint32_t n = vita_mem_avail(addr);
    const char *s = n ? (const char *)vita_mem_ptr(addr, 1) : NULL;
    uint32_t i = 0;
    for (; s && i < 31 && i < n && s[i]; i++) dst[i] = s[i];
    dst[i] = '\0';
}

/* --- thread identity -------------------------------------------------------- */

/* SceUID sceKernelGetThreadId(void) */
void vita_hle_sceKernelGetThreadId(void) { r0 = vita_thread_self(); }

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
 * One block per thread, made the first time the thread asks. */

#define TLS_KERNEL_SIZE 0x800
#define TLS_SLOTS       0x101

static uint32_t g_tls_image, g_tls_filesz, g_tls_memsz;

void vita_tls_init(uint32_t image, uint32_t filesz, uint32_t memsz) {
    g_tls_image  = image;
    g_tls_filesz = filesz;
    g_tls_memsz  = memsz;
}

static uint32_t tls_block_of(uint32_t uid) {
    uint32_t *slot = vita_thread_tls_slot(uid);
    if (!slot) return 0;
    if (!*slot) {
        *slot = kalloc(TLS_KERNEL_SIZE + ((g_tls_memsz + 15) & ~15u));
        if (*slot && g_tls_filesz) {
            void       *dst = vita_mem_ptr(*slot + TLS_KERNEL_SIZE, g_tls_filesz);
            const void *src = vita_mem_ptr(g_tls_image, g_tls_filesz);
            if (dst && src) memcpy(dst, src, g_tls_filesz);
        }
    }
    return *slot;
}

uint32_t vita_tls_base(void) {
    uint32_t b = tls_block_of(vita_thread_self());
    return b ? b + TLS_KERNEL_SIZE : 0;
}

static uint32_t tls_slot(uint32_t uid, uint32_t key) {
    if (key >= TLS_SLOTS) return 0;
    uint32_t b = tls_block_of(uid);
    return b ? b + key * 4 : 0;
}

/* void *sceKernelGetTLSAddr(int key) */
void vita_hle_sceKernelGetTLSAddr(void) {
    uint32_t key = r0;
    r0 = tls_slot(vita_thread_self(), key);
    vita_hle_trace("sceKernelGetTLSAddr(0x%X) -> 0x%08X (holds 0x%08X)", key, r0, r0 ? vita_read32(r0) : 0);
}

/* void *sceKernelGetThreadTLSAddr(SceUID thid, int key) */
void vita_hle_sceKernelGetThreadTLSAddr(void) {
    uint32_t thid = r0 ? r0 : vita_thread_self(), key = r1;
    r0 = tls_slot(thid, key);
    vita_hle_trace("sceKernelGetThreadTLSAddr(0x%08X, 0x%X) -> 0x%08X", thid, key, r0);
}

/* --- lightweight mutexes -----------------------------------------------------
 *
 * The object lives in the caller's memory (a 32-byte SceKernelLwMutexWork);
 * the functions take its address. Word 0 is the recursion count, word 1 the
 * owning thread while held. */

#define LWMUTEX_WORK_SIZE 32

/* int sceKernelCreateLwMutex(SceKernelLwMutexWork *work, const char *name,
 *                            unsigned attr, int initCount, const void *opt) */
void vita_hle_sceKernelCreateLwMutex(void) {
    uint32_t work = r0;
    if (!vita_mem_ptr(work, LWMUTEX_WORK_SIZE)) { r0 = SCE_KERNEL_ERROR_INVALID_ARGUMENT; return; }
    for (uint32_t k = 0; k < LWMUTEX_WORK_SIZE; k += 4) vita_write32(work + k, 0);
    vita_write32(work + 0, r3);
    vita_write32(work + 4, r3 ? vita_thread_self() : 0);
    vita_hle_trace("sceKernelCreateLwMutex(work 0x%08X, attr 0x%X, count %d) -> 0", work, r2, (int)r3);
    r0 = 0;
}

static int lw_lock(uint32_t work, uint32_t count, uint32_t timeout, int try_only) {
    uint32_t me = vita_thread_self();
    for (;;) {
        uint32_t c = vita_read32(work), owner = vita_read32(work + 4);
        if (c == 0 || owner == me) {
            vita_write32(work, c + count);
            vita_write32(work + 4, me);
            return 0;
        }
        if (try_only) return (int)SCE_KERNEL_ERROR_MUTEX_FAILED_TO_OWN;
        if (vita_sched_block(VW_MUTEX, work, timeout) < 0) return (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT;
    }
}

static void lw_unlock(uint32_t work, uint32_t count) {
    uint32_t c = vita_read32(work);
    c = c >= count ? c - count : 0;
    vita_write32(work, c);
    if (c == 0) { vita_write32(work + 4, 0); vita_sched_wake(VW_MUTEX, work, 1); }
}

/* int sceKernelLockLwMutex(SceKernelLwMutexWork *work, int lockCount, unsigned *timeout) */
void vita_hle_sceKernelLockLwMutex(void) {
    uint32_t work = r0;
    if (!vita_mem_ptr(work, LWMUTEX_WORK_SIZE)) { r0 = SCE_KERNEL_ERROR_INVALID_ARGUMENT; return; }
    r0 = (uint32_t)lw_lock(work, r1, timeout_us(r2), 0);
}
void vita_hle_sceKernelLockLwMutexCB(void) { vita_hle_sceKernelLockLwMutex(); }

/* int sceKernelTryLockLwMutex(SceKernelLwMutexWork *work, int lockCount) */
void vita_hle_sceKernelTryLockLwMutex(void) {
    uint32_t work = r0;
    if (!vita_mem_ptr(work, LWMUTEX_WORK_SIZE)) { r0 = SCE_KERNEL_ERROR_INVALID_ARGUMENT; return; }
    r0 = (uint32_t)lw_lock(work, r1, 0, 1);
}

/* int sceKernelUnlockLwMutex(SceKernelLwMutexWork *work, int unlockCount) */
void vita_hle_sceKernelUnlockLwMutex(void) {
    uint32_t work = r0;
    if (!vita_mem_ptr(work, LWMUTEX_WORK_SIZE)) { r0 = SCE_KERNEL_ERROR_INVALID_ARGUMENT; return; }
    lw_unlock(work, r1);
    r0 = 0;
}
void vita_hle_sceKernelUnlockLwMutex2(void) { vita_hle_sceKernelUnlockLwMutex(); }

/* int sceKernelDeleteLwMutex(SceKernelLwMutexWork *work) */
void vita_hle_sceKernelDeleteLwMutex(void) {
    if (!vita_mem_ptr(r0, LWMUTEX_WORK_SIZE)) { r0 = SCE_KERNEL_ERROR_INVALID_ARGUMENT; return; }
    r0 = 0;
}

/* --- mutexes ------------------------------------------------------------------ */

typedef struct { int used; int count; uint32_t owner; char name[32]; } mutex;
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
    g_mutexes[i].owner = r2 ? vita_thread_self() : 0;
    copy_name(g_mutexes[i].name, r0);
    r0 = MUTEX_UID_FIRST + (uint32_t)i;
    vita_hle_trace("sceKernelCreateMutex(\"%s\", attr 0x%X, count %d) -> 0x%08X", g_mutexes[i].name, r1, (int)r2, r0);
}

static int mutex_lock(int i, int count, uint32_t timeout, int try_only) {
    uint32_t me = vita_thread_self();
    for (;;) {
        if (g_mutexes[i].count == 0 || g_mutexes[i].owner == me) {
            g_mutexes[i].count += count;
            g_mutexes[i].owner = me;
            return 0;
        }
        if (try_only) return (int)SCE_KERNEL_ERROR_MUTEX_FAILED_TO_OWN;
        if (vita_sched_block(VW_MUTEX, MUTEX_UID_FIRST + (uint32_t)i, timeout) < 0) return (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT;
    }
}

static void mutex_unlock(int i, int count) {
    g_mutexes[i].count -= count;
    if (g_mutexes[i].count <= 0) {
        g_mutexes[i].count = 0;
        g_mutexes[i].owner = 0;
        vita_sched_wake(VW_MUTEX, MUTEX_UID_FIRST + (uint32_t)i, 1);
    }
}

/* int sceKernelLockMutex(SceUID id, int lockCount, unsigned *timeout) */
void vita_hle_sceKernelLockMutex(void) {
    int i = mutex_index(r0);
    if (i < 0) { r0 = SCE_KERNEL_ERROR_INVALID_UID; return; }
    r0 = (uint32_t)mutex_lock(i, (int)r1, timeout_us(r2), 0);
}
void vita_hle_sceKernelLockMutexCB(void) { vita_hle_sceKernelLockMutex(); }

/* int sceKernelTryLockMutex(SceUID id, int lockCount) */
void vita_hle_sceKernelTryLockMutex(void) {
    int i = mutex_index(r0);
    if (i < 0) { r0 = SCE_KERNEL_ERROR_INVALID_UID; return; }
    r0 = (uint32_t)mutex_lock(i, (int)r1, 0, 1);
}

/* int sceKernelUnlockMutex(SceUID id, int unlockCount) */
void vita_hle_sceKernelUnlockMutex(void) {
    int i = mutex_index(r0);
    if (i < 0) { r0 = SCE_KERNEL_ERROR_INVALID_UID; return; }
    mutex_unlock(i, (int)r1);
    r0 = 0;
}

/* int sceKernelDeleteMutex(SceUID id) */
void vita_hle_sceKernelDeleteMutex(void) {
    int i = mutex_index(r0);
    if (i < 0) { r0 = SCE_KERNEL_ERROR_INVALID_UID; return; }
    g_mutexes[i].used = 0;
    r0 = 0;
}
void vita_hle_sceKernelCloseMutex(void) { r0 = mutex_index(r0) < 0 ? SCE_KERNEL_ERROR_INVALID_UID : 0; }

/* --- semaphores --------------------------------------------------------------- */

typedef struct { int used; int count, max; char name[32]; } sema;
static sema g_semas[MAX_SEMAS];

static int sema_index(uint32_t uid) {
    if (uid < SEMA_UID_FIRST || uid >= SEMA_UID_FIRST + MAX_SEMAS) return -1;
    int i = (int)(uid - SEMA_UID_FIRST);
    return g_semas[i].used ? i : -1;
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
    uint32_t timeout = timeout_us(r2);
    for (;;) {
        if (g_semas[i].count >= need) { g_semas[i].count -= need; r0 = 0; return; }
        if (vita_sched_block(VW_SEMA, SEMA_UID_FIRST + (uint32_t)i, timeout) < 0) { r0 = SCE_KERNEL_ERROR_WAIT_TIMEOUT; return; }
        if (sema_index(SEMA_UID_FIRST + (uint32_t)i) < 0) { r0 = SCE_KERNEL_ERROR_INVALID_UID; return; }
    }
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
    vita_sched_wake(VW_SEMA, SEMA_UID_FIRST + (uint32_t)i, 1);
    r0 = 0;
}

/* int sceKernelDeleteSema(SceUID id) */
void vita_hle_sceKernelDeleteSema(void) {
    int i = sema_index(r0);
    if (i < 0) { r0 = SCE_KERNEL_ERROR_INVALID_UID; return; }
    g_semas[i].used = 0;
    vita_sched_wake(VW_SEMA, SEMA_UID_FIRST + (uint32_t)i, 1);
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

typedef struct { int used; uint32_t mutex_uid; } cond;
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
    g_conds[i].mutex_uid = r2;
    r0 = COND_UID_FIRST + (uint32_t)i;
}

/* int sceKernelWaitCond(SceUID id, SceUInt *timeout): give the mutex up, sleep, take it back */
void vita_hle_sceKernelWaitCond(void) {
    int i = cond_index(r0);
    if (i < 0) { r0 = SCE_KERNEL_ERROR_INVALID_UID; return; }
    uint32_t timeout = timeout_us(r1);
    int m = mutex_index(g_conds[i].mutex_uid);
    int held = m >= 0 ? g_mutexes[m].count : 0;
    if (m >= 0 && held) mutex_unlock(m, held);
    int rc = vita_sched_block(VW_COND, COND_UID_FIRST + (uint32_t)i, timeout);
    if (m >= 0 && held) mutex_lock(m, held, 0, 0);
    r0 = rc < 0 ? SCE_KERNEL_ERROR_WAIT_TIMEOUT : 0;
}
void vita_hle_sceKernelWaitCondCB(void) { vita_hle_sceKernelWaitCond(); }

/* int sceKernelSignalCond(SceUID id) / SignalCondAll / SignalCondTo */
void vita_hle_sceKernelSignalCond(void) {
    int i = cond_index(r0);
    if (i < 0) { r0 = SCE_KERNEL_ERROR_INVALID_UID; return; }
    vita_sched_wake(VW_COND, COND_UID_FIRST + (uint32_t)i, 0);
    r0 = 0;
}
void vita_hle_sceKernelSignalCondAll(void) {
    int i = cond_index(r0);
    if (i < 0) { r0 = SCE_KERNEL_ERROR_INVALID_UID; return; }
    vita_sched_wake(VW_COND, COND_UID_FIRST + (uint32_t)i, 1);
    r0 = 0;
}
void vita_hle_sceKernelSignalCondTo(void) { vita_hle_sceKernelSignalCondAll(); }

/* int sceKernelDeleteCond(SceUID id) */
void vita_hle_sceKernelDeleteCond(void) {
    int i = cond_index(r0);
    if (i < 0) { r0 = SCE_KERNEL_ERROR_INVALID_UID; return; }
    g_conds[i].used = 0;
    vita_sched_wake(VW_COND, COND_UID_FIRST + (uint32_t)i, 1);
    r0 = 0;
}

/* Lightweight condition variables live in the caller's memory, like the
 * lightweight mutexes, and wait on one of those; word 1 of the work area
 * holds that mutex's address.
 *
 * The work area is sixteen bytes, half a mutex's. It was cleared as
 * thirty-two here once, and a title's sound library keeps a condition, a
 * thread's id and a mutex one after another in a record: creating the
 * condition wiped the id of the thread created the line before, the start
 * that followed started thread 0, and the title played its first level
 * without the thread that streams its sound. */
#define LWCOND_WORK_SIZE 16

/* int sceKernelCreateLwCond(SceKernelLwCondWork *work, const char *name, SceUInt attr,
 *                           SceKernelLwMutexWork *mutex, const void *opt) */
void vita_hle_sceKernelCreateLwCond(void) {
    if (!vita_mem_ptr(r0, LWCOND_WORK_SIZE)) { r0 = SCE_KERNEL_ERROR_INVALID_ARGUMENT; return; }
    for (uint32_t k = 0; k < LWCOND_WORK_SIZE; k += 4) vita_write32(r0 + k, 0);
    vita_write32(r0 + 4, r3);
    r0 = 0;
}

/* int sceKernelWaitLwCond(SceKernelLwCondWork *work, SceUInt *timeout) */
void vita_hle_sceKernelWaitLwCond(void) {
    uint32_t work = r0;
    if (!vita_mem_ptr(work, LWCOND_WORK_SIZE)) { r0 = SCE_KERNEL_ERROR_INVALID_ARGUMENT; return; }
    uint32_t timeout = timeout_us(r1);
    uint32_t mwork = vita_read32(work + 4);
    uint32_t held = (mwork && vita_mem_ptr(mwork, LWMUTEX_WORK_SIZE)) ? vita_read32(mwork) : 0;
    if (held) lw_unlock(mwork, held);
    int rc = vita_sched_block(VW_COND, work, timeout);
    if (held) lw_lock(mwork, held, 0, 0);
    r0 = rc < 0 ? SCE_KERNEL_ERROR_WAIT_TIMEOUT : 0;
}
void vita_hle_sceKernelWaitLwCondCB(void) { vita_hle_sceKernelWaitLwCond(); }

void vita_hle_sceKernelSignalLwCond(void) {
    if (!vita_mem_ptr(r0, LWCOND_WORK_SIZE)) { r0 = SCE_KERNEL_ERROR_INVALID_ARGUMENT; return; }
    vita_sched_wake(VW_COND, r0, 0);
    r0 = 0;
}
void vita_hle_sceKernelSignalLwCondAll(void) {
    if (!vita_mem_ptr(r0, LWCOND_WORK_SIZE)) { r0 = SCE_KERNEL_ERROR_INVALID_ARGUMENT; return; }
    vita_sched_wake(VW_COND, r0, 1);
    r0 = 0;
}
void vita_hle_sceKernelSignalLwCondTo(void) { vita_hle_sceKernelSignalLwCondAll(); }
void vita_hle_sceKernelDeleteLwCond(void)   { vita_hle_sceKernelSignalLwCondAll(); }

/* --- threads ------------------------------------------------------------------------ */

/* SceUID sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int initPriority,
 *                              SceSize stackSize, SceUInt attr, int cpuAffinityMask, const void *opt) */
void vita_hle_sceKernelCreateThread(void) {
    char name[32];
    copy_name(name, r0);
    uint32_t uid = vita_thread_create(name, r1, r2, r3);
    if (!uid) { r0 = SCE_KERNEL_ERROR_NO_MEMORY; return; }
    vita_hle_trace("sceKernelCreateThread(\"%s\", entry 0x%08X, prio 0x%X, stack 0x%X) -> 0x%08X", name, r1, r2, r3, uid);
    r0 = uid;
}

/* int sceKernelStartThread(SceUID id, SceSize argLen, void *argp) */
void vita_hle_sceKernelStartThread(void) {
    r0 = (uint32_t)vita_thread_start(r0, r1, r2);
}

/* int sceKernelWaitThreadEnd(SceUID id, int *stat, SceUInt *timeout) */
void vita_hle_sceKernelWaitThreadEnd(void) {
    int status = 0;
    int rc = vita_thread_wait_end(r0, timeout_us(r2), &status);
    if (rc == 0 && r1 && vita_mem_ptr(r1, 4)) vita_write32(r1, (uint32_t)status);
    r0 = (uint32_t)rc;
}
void vita_hle_sceKernelWaitThreadEndCB(void) { vita_hle_sceKernelWaitThreadEnd(); }

/* int sceKernelDeleteThread(SceUID id) */
void vita_hle_sceKernelDeleteThread(void) { r0 = (uint32_t)vita_thread_delete(r0); }

/* int sceKernelExitThread(int status) / sceKernelExitDeleteThread */
void vita_hle_sceKernelExitThread(void)       { vita_thread_exit((int)r0); }
void vita_hle_sceKernelExitDeleteThread(void) { vita_thread_exit((int)r0); }

/* int sceKernelGetThreadInfo(SceUID id, SceKernelThreadInfo *info)
 * SceKernelThreadInfo begins { SceSize size; SceUID processId; char name[32];
 * SceUInt attr; SceUInt status; SceKernelThreadEntry entry; void *stack;
 * SceInt32 stackSize; SceInt32 initPriority; SceInt32 currentPriority; ... }
 * and the caller sets size first. Everything past the fields named here is
 * zeroed. */
void vita_hle_sceKernelGetThreadInfo(void) {
    uint32_t thid = r0 ? r0 : vita_thread_self(), info = r1;
    uint32_t size = vita_mem_ptr(info, 4) ? vita_read32(info) : 0;
    if (size < 8 || !vita_mem_ptr(info, size)) { r0 = SCE_KERNEL_ERROR_INVALID_ARGUMENT; return; }
    char name[32];
    uint32_t entry = 0, stack = 0, stack_size = 0, prio = 0x40;
    if (vita_thread_info(thid, name, sizeof(name), &entry, &stack, &stack_size, &prio) != 0) { r0 = SCE_KERNEL_ERROR_INVALID_UID; return; }
    for (uint32_t k = 4; k + 4 <= size; k += 4) vita_write32(info + k, 0);
    vita_write32(info + 4, PROCESS_UID);
    for (uint32_t k = 0; k < 32 && 8 + k < size; k++) vita_write8(info + 8 + k, (uint8_t)(k < strlen(name) ? name[k] : 0));
    if (size >= 0x30) vita_write32(info + 0x2C, 1);          /* status: running */
    if (size >= 0x34) vita_write32(info + 0x30, entry);
    if (size >= 0x38) vita_write32(info + 0x34, stack);
    if (size >= 0x3C) vita_write32(info + 0x38, stack_size);
    if (size >= 0x40) vita_write32(info + 0x3C, prio);
    if (size >= 0x44) vita_write32(info + 0x40, prio);
    r0 = 0;
}

/* Thread attributes: accepted. The scheduler is round-robin. */
void vita_hle_sceKernelChangeThreadPriority(void)        { r0 = 0; }
void vita_hle_sceKernelChangeThreadCpuAffinityMask(void) { r0 = 0; }
void vita_hle_sceKernelChangeThreadVfpException(void)    { r0 = 0; }
void vita_hle_sceKernelGetThreadCpuAffinityMask(void)    { r0 = 0; }
void vita_hle_sceKernelGetThreadCurrentPriority(void)    { r0 = 0x40; }

/* int sceKernelDelayThread(SceUInt usec): the others run meanwhile */
void vita_hle_sceKernelDelayThread(void)   { vita_sched_delay(r0); r0 = 0; }
void vita_hle_sceKernelDelayThreadCB(void) { vita_hle_sceKernelDelayThread(); }
