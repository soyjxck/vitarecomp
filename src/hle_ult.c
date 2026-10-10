/* hle_ult.c — SceUlt: the user-level thread library's synchronisation
 * objects, for a program with one thread.
 *
 * SceUlt lets a program run its own fibres over a few kernel threads, with
 * mutexes, condition variables and semaphores whose storage the program
 * supplies (the objects are opaque blocks in its memory, sized by constants
 * in the SDK) and a resource pool of waiting queues it also supplies, sized
 * by a call. The leading-underscore entry points are what the SDK's inline
 * wrappers call, with the object's size as a trailing argument.
 *
 * Over the scheduler (sched.c): a mutex is its recursion count and owner, a
 * semaphore its count, a condition variable the mutex it pairs with; a wait
 * that cannot be satisfied sleeps until the matching signal. The storage the
 * program hands over holds exactly those words, and nothing in the program
 * reads it. Calling convention: see hle.h.
 */

#include "vitarecomp/hle.h"
#include "vitarecomp/recomp_rt.h"
#include "sched.h"

#define SCE_ULT_ERROR_NULL        0x80810001u
#define SCE_ULT_ERROR_BUSY        0x80810004u

/* SceInt32 sceUltWaitingQueueResourcePoolGetWorkAreaSize(SceUInt32 numThreads, SceUInt32 numSyncObjects):
 * a size the program allocates for the pool; nothing here needs any of it. */
void vita_hle_sceUltWaitingQueueResourcePoolGetWorkAreaSize(void) {
    r0 = 0x100 + 0x40 * (r0 + r1);
}

/* SceInt32 _sceUltWaitingQueueResourcePoolCreate(pool, name, numThreads, numSyncObjects, workArea, opt, size) */
void vita_hle__sceUltWaitingQueueResourcePoolCreate(void) {
    if (!vita_mem_ptr(r0, 4)) { r0 = SCE_ULT_ERROR_NULL; return; }
    vita_hle_trace("sceUltWaitingQueueResourcePoolCreate(0x%08X, threads %u, objects %u)", r0, r2, r3);
    r0 = 0;
}
void vita_hle_sceUltWaitingQueueResourcePoolDestroy(void) { r0 = 0; }

/* SceInt32 sceUltQueueDataResourcePoolGetWorkAreaSize(numData, dataSize, numQueueObjects) */
void vita_hle_sceUltQueueDataResourcePoolGetWorkAreaSize(void) { r0 = 0x100 + r0 * r1 + 0x40 * r2; }
void vita_hle__sceUltQueueDataResourcePoolCreate(void) { r0 = vita_mem_ptr(r0, 4) ? 0 : SCE_ULT_ERROR_NULL; }
void vita_hle_sceUltQueueDataResourcePoolDestroy(void) { r0 = 0; }

/* --- mutexes: the first word of the object is the recursion count --------------- */

/* SceInt32 _sceUltMutexOptParamInitialize(SceUltMutexOptParam *opt, SceSize size) */
void vita_hle__sceUltMutexOptParamInitialize(void) {
    if (!vita_mem_ptr(r0, 4)) { r0 = SCE_ULT_ERROR_NULL; return; }
    vita_write32(r0, 0);
    r0 = 0;
}

/* SceInt32 _sceUltMutexCreate(SceUltMutex *mutex, const char *name, pool, const opt, SceSize size) */
void vita_hle__sceUltMutexCreate(void) {
    if (!vita_mem_ptr(r0, 8)) { r0 = SCE_ULT_ERROR_NULL; return; }
    vita_write32(r0, 0);
    vita_hle_trace("sceUltMutexCreate(0x%08X)", r0);
    r0 = 0;
}

static int ult_lock(uint32_t m, int try_only) {
    uint32_t me = vita_thread_self();
    for (;;) {
        uint32_t c = vita_read32(m), owner = vita_read32(m + 4);
        if (c == 0 || owner == me) { vita_write32(m, c + 1); vita_write32(m + 4, me); return 0; }
        if (try_only) return (int)SCE_ULT_ERROR_BUSY;
        vita_sched_block(VW_ULT, m, 0);
    }
}

static void ult_unlock(uint32_t m) {
    uint32_t c = vita_read32(m);
    c = c ? c - 1 : 0;
    vita_write32(m, c);
    if (c == 0) { vita_write32(m + 4, 0); vita_sched_wake(VW_ULT, m, 1); }
}

/* SceInt32 sceUltMutexLock(SceUltMutex *mutex) */
void vita_hle_sceUltMutexLock(void) {
    if (!vita_mem_ptr(r0, 8)) { r0 = SCE_ULT_ERROR_NULL; return; }
    r0 = (uint32_t)ult_lock(r0, 0);
}
void vita_hle_sceUltMutexTryLock(void) {
    if (!vita_mem_ptr(r0, 8)) { r0 = SCE_ULT_ERROR_NULL; return; }
    r0 = (uint32_t)ult_lock(r0, 1);
}

/* SceInt32 sceUltMutexUnlock(SceUltMutex *mutex) */
void vita_hle_sceUltMutexUnlock(void) {
    if (!vita_mem_ptr(r0, 8)) { r0 = SCE_ULT_ERROR_NULL; return; }
    ult_unlock(r0);
    r0 = 0;
}
void vita_hle_sceUltMutexDestroy(void) { r0 = vita_mem_ptr(r0, 4) ? 0 : SCE_ULT_ERROR_NULL; }

/* --- condition variables ---------------------------------------------------------- */

/* SceInt32 _sceUltConditionVariableCreate(cond, name, SceUltMutex *mutex, opt, SceSize size) */
void vita_hle__sceUltConditionVariableCreate(void) {
    if (!vita_mem_ptr(r0, 8)) { r0 = SCE_ULT_ERROR_NULL; return; }
    vita_write32(r0, r2);                     /* the mutex it pairs with */
    r0 = 0;
}
void vita_hle__sceUltConditionVariableOptParamInitialize(void) { if (vita_mem_ptr(r0, 4)) vita_write32(r0, 0); r0 = 0; }

/* SceInt32 sceUltConditionVariableWait(SceUltConditionVariable *cond): give
 * the mutex up, sleep until a signal, take it back */
void vita_hle_sceUltConditionVariableWait(void) {
    uint32_t cv = r0;
    if (!vita_mem_ptr(cv, 4)) { r0 = SCE_ULT_ERROR_NULL; return; }
    uint32_t m = vita_read32(cv);
    uint32_t held = (m && vita_mem_ptr(m, 8)) ? vita_read32(m) : 0;
    if (held) { vita_write32(m, 0); vita_write32(m + 4, 0); vita_sched_wake(VW_ULT, m, 1); }
    vita_sched_block(VW_ULT, cv, 0);
    if (held) { ult_lock(m, 0); vita_write32(m, held); }
    r0 = 0;
}
void vita_hle_sceUltConditionVariableSignal(void) {
    if (!vita_mem_ptr(r0, 4)) { r0 = SCE_ULT_ERROR_NULL; return; }
    vita_sched_wake(VW_ULT, r0, 0);
    r0 = 0;
}
void vita_hle_sceUltConditionVariableSignalAll(void) {
    if (!vita_mem_ptr(r0, 4)) { r0 = SCE_ULT_ERROR_NULL; return; }
    vita_sched_wake(VW_ULT, r0, 1);
    r0 = 0;
}
void vita_hle_sceUltConditionVariableDestroy(void)   { vita_hle_sceUltConditionVariableSignal(); }

/* --- semaphores: the first word is the count ---------------------------------------- */

/* SceInt32 _sceUltSemaphoreCreate(sema, name, SceInt32 numResource, pool, opt, SceSize size) */
void vita_hle__sceUltSemaphoreCreate(void) {
    if (!vita_mem_ptr(r0, 8)) { r0 = SCE_ULT_ERROR_NULL; return; }
    vita_write32(r0, r2);
    vita_hle_trace("sceUltSemaphoreCreate(0x%08X, %d)", r0, (int)r2);
    r0 = 0;
}
void vita_hle__sceUltSemaphoreOptParamInitialize(void) { if (vita_mem_ptr(r0, 4)) vita_write32(r0, 0); r0 = 0; }

/* SceInt32 sceUltSemaphoreAcquire(sema, SceInt32 numResource) */
void vita_hle_sceUltSemaphoreAcquire(void) {
    uint32_t s = r0;
    int32_t n = (int32_t)r1;
    if (!vita_mem_ptr(s, 4)) { r0 = SCE_ULT_ERROR_NULL; return; }
    for (;;) {
        int32_t c = (int32_t)vita_read32(s);
        if (c >= n) { vita_write32(s, (uint32_t)(c - n)); r0 = 0; return; }
        vita_sched_block(VW_ULT, s, 0);
    }
}

/* SceInt32 sceUltSemaphoreTryAcquire(sema, SceInt32 numResource) */
void vita_hle_sceUltSemaphoreTryAcquire(void) {
    if (!vita_mem_ptr(r0, 4)) { r0 = SCE_ULT_ERROR_NULL; return; }
    int32_t c = (int32_t)vita_read32(r0), n = (int32_t)r1;
    if (c >= n) { vita_write32(r0, (uint32_t)(c - n)); r0 = 0; }
    else r0 = SCE_ULT_ERROR_BUSY;
}

/* SceInt32 sceUltSemaphoreRelease(sema, SceInt32 numResource) */
void vita_hle_sceUltSemaphoreRelease(void) {
    if (!vita_mem_ptr(r0, 4)) { r0 = SCE_ULT_ERROR_NULL; return; }
    vita_write32(r0, vita_read32(r0) + r1);
    vita_sched_wake(VW_ULT, r0, 1);
    r0 = 0;
}
void vita_hle_sceUltSemaphoreDestroy(void) { r0 = vita_mem_ptr(r0, 4) ? 0 : SCE_ULT_ERROR_NULL; }
