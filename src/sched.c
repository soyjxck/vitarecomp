/* sched.c — guest threads on the host. See sched.h.
 *
 * C11 threads: one host thread per guest thread, a single mutex, and one
 * condition variable per thread that it sleeps on while another holds the
 * baton. The baton is `g_cur`; the thread that holds it is the only one
 * touching the register globals or guest memory.
 */

#include "sched.h"
#include "hle_sysmem.h"
#include "vitarecomp/cpu.h"
#include "vitarecomp/dispatch.h"
#include "vitarecomp/hle.h"
#include "vitarecomp/host.h"
#include "vitarecomp/mem.h"
#include "vitarecomp/recomp_rt.h"
#include "vitarecomp/vfp.h"

#include <stdio.h>
#include <string.h>
#include <threads.h>
#include <time.h>

#define MAX_THREADS      64
#define THREAD_UID_FIRST 0x40010100u      /* the main thread's; the rest follow */

#define SCE_KERNEL_ERROR_INVALID_UID      0x80024501u
#define SCE_KERNEL_ERROR_WAIT_TIMEOUT     0x80028005u
#define SCE_KERNEL_ERROR_NO_MEMORY        0x8002710Bu
#define SCE_KERNEL_ERROR_NOT_DORMANT      0x80028013u

enum { T_FREE = 0, T_NEW, T_READY, T_RUNNING, T_WAITING, T_DONE };

typedef struct {
    uint32_t r[13], sp, lr;
    int      n, z, c, v, ge;
    vfp_reg  vfp[64];
    int      fn, fz, fc, fv;
    uint32_t last_pc;
} regs_t;

typedef struct {
    int      state;
    uint32_t uid;
    char     name[32];
    uint32_t entry, prio, stack_base, stack_size, tls;
    int      wait_kind;
    uint32_t wait_obj;
    uint64_t wait_until;         /* 0: no timeout                        */
    int      timed_out;
    int      exit_status;
    regs_t   regs;
    thrd_t   host;
    int      host_started;
    cnd_t    wake;
    int      go;
} vthread;

static vthread  g_t[MAX_THREADS];
static mtx_t    g_mtx;
static vthread *g_cur;
static int      g_init;

int32_t vita_slice = VITA_SLICE;

static uint64_t now_us(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (uint64_t)t.tv_sec * 1000000ull + (uint64_t)t.tv_nsec / 1000ull;
}

static void save_regs(vthread *t) {
    uint32_t *rr = t->regs.r;
    rr[0] = r0; rr[1] = r1; rr[2] = r2;  rr[3] = r3;  rr[4] = r4;  rr[5] = r5; rr[6] = r6;
    rr[7] = r7; rr[8] = r8; rr[9] = r9; rr[10] = r10; rr[11] = r11; rr[12] = r12;
    t->regs.sp = sp; t->regs.lr = lr;
    t->regs.n = flag_n; t->regs.z = flag_z; t->regs.c = flag_c; t->regs.v = flag_v; t->regs.ge = flag_ge;
    memcpy(t->regs.vfp, vfp_s, sizeof(vfp_s));
    t->regs.fn = fpscr_n; t->regs.fz = fpscr_z; t->regs.fc = fpscr_c; t->regs.fv = fpscr_v;
    t->regs.last_pc = vita_last_pc;
}

static void load_regs(const vthread *t) {
    const uint32_t *rr = t->regs.r;
    r0 = rr[0]; r1 = rr[1]; r2 = rr[2];  r3 = rr[3];  r4 = rr[4];  r5 = rr[5]; r6 = rr[6];
    r7 = rr[7]; r8 = rr[8]; r9 = rr[9]; r10 = rr[10]; r11 = rr[11]; r12 = rr[12];
    sp = t->regs.sp; lr = t->regs.lr;
    flag_n = t->regs.n; flag_z = t->regs.z; flag_c = t->regs.c; flag_v = t->regs.v; flag_ge = t->regs.ge;
    memcpy(vfp_s, t->regs.vfp, sizeof(vfp_s));
    fpscr_n = t->regs.fn; fpscr_z = t->regs.fz; fpscr_c = t->regs.fc; fpscr_v = t->regs.fv;
    vita_last_pc = t->regs.last_pc;
}

/* The main thread is the one that called first; it already runs. */
static void init_once(void) {
    if (g_init) return;
    g_init = 1;
    mtx_init(&g_mtx, mtx_plain);
    vthread *m = &g_t[0];
    memset(m, 0, sizeof(*m));
    m->state = T_RUNNING;
    m->uid = THREAD_UID_FIRST;
    snprintf(m->name, sizeof(m->name), "main");
    m->prio = 0x40;
    cnd_init(&m->wake);
    g_cur = m;
}

static vthread *by_uid(uint32_t uid) {
    for (int i = 0; i < MAX_THREADS; i++)
        if (g_t[i].state != T_FREE && g_t[i].uid == uid) return &g_t[i];
    return NULL;
}

uint32_t vita_thread_self(void) { init_once(); return g_cur->uid; }

void vita_main_stack_init(uint32_t base, uint32_t size) {
    init_once();
    g_t[0].stack_base = base;
    g_t[0].stack_size = size;
}

uint32_t *vita_thread_tls_slot(uint32_t uid) {
    init_once();
    vthread *t = by_uid(uid);
    return t ? &t->tls : NULL;
}

uint32_t vita_sched_threads(uint32_t *runnable) {
    uint32_t n = 0, run = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (g_t[i].state == T_FREE) continue;
        n++;
        if (g_t[i].state == T_READY || g_t[i].state == T_RUNNING) run++;
    }
    if (runnable) *runnable = run;
    return n;
}

uint32_t vita_sched_running(char *name, uint32_t cap) {
    init_once();
    mtx_lock(&g_mtx);
    const vthread *t = g_cur;
    uint32_t uid = t->state == T_RUNNING ? t->uid : 0;
    if (name && cap) snprintf(name, cap, "%s", uid ? t->name : "(idle)");
    mtx_unlock(&g_mtx);
    return uid;
}

/* Timed waiters whose time has come become runnable; returns the earliest
 * time still pending, or 0. */
static uint64_t expire(uint64_t now) {
    uint64_t earliest = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        vthread *t = &g_t[i];
        if (t->state != T_WAITING || !t->wait_until) continue;
        if (t->wait_until <= now) { t->state = T_READY; t->timed_out = 1; t->wait_until = 0; }
        else if (!earliest || t->wait_until < earliest) earliest = t->wait_until;
    }
    return earliest;
}

/* The next thread to run after `cur`, round-robin. When nothing is runnable
 * the host sleeps until the earliest timed wait, and when there is none of
 * those either the program has deadlocked, which is said rather than spun
 * on. Called with the mutex held. */
static vthread *pick_next(vthread *cur) {
    for (;;) {
        uint64_t earliest = expire(now_us());
        int start = (int)(cur - g_t);
        for (int k = 1; k <= MAX_THREADS; k++) {
            vthread *t = &g_t[(start + k) % MAX_THREADS];
            if (t->state == T_READY) return t;
        }
        if (cur->state == T_READY || cur->state == T_RUNNING) return cur;
        if (!earliest) {
            uint32_t n = 0, run = 0;
            n = vita_sched_threads(&run);
            fprintf(stderr, "vitarecomp: every thread is waiting (%u threads); the program is deadlocked\n", n);
            for (int i = 0; i < MAX_THREADS; i++)
                if (g_t[i].state == T_WAITING)
                    fprintf(stderr, "  %s (0x%08X) waits on kind %d object 0x%08X\n",
                            g_t[i].name, g_t[i].uid, g_t[i].wait_kind, g_t[i].wait_obj);
            vita_trap_unimpl(0, n, "deadlock: every thread is waiting");
            vita_host_exit(3);
        }
        uint64_t now = now_us();
        if (earliest > now) {
            uint64_t d = earliest - now;
            struct timespec ts = { (time_t)(d / 1000000ull), (long)((d % 1000000ull) * 1000ull) };
            mtx_unlock(&g_mtx);
            thrd_sleep(&ts, NULL);
            mtx_lock(&g_mtx);
        }
    }
}

/* Hand the baton from the running thread to `next` and sleep until it comes
 * back. Called with the mutex held; returns with it held. */
static void switch_to(vthread *cur, vthread *next) {
    if (next == cur) { cur->state = T_RUNNING; return; }
    save_regs(cur);
    load_regs(next);
    next->state = T_RUNNING;
    g_cur = next;
    next->go = 1;
    cnd_signal(&next->wake);
    while (!cur->go) cnd_wait(&cur->wake, &g_mtx);
    cur->go = 0;
    cur->state = T_RUNNING;
    vita_slice = VITA_SLICE;
}

/* A new guest thread's host thread: waits for its first turn, then runs the
 * entry through the dispatch table with the registers switch_to loaded, and
 * ends the guest thread when the entry returns. */
static int thread_main(void *arg) {
    vthread *t = (vthread *)arg;
    mtx_lock(&g_mtx);
    while (!t->go) cnd_wait(&t->wake, &g_mtx);
    t->go = 0;
    t->state = T_RUNNING;
    vita_slice = VITA_SLICE;
    mtx_unlock(&g_mtx);

    vita_dispatch(0, t->entry);
    vita_thread_exit((int)r0);
    return 0;
}

uint32_t vita_thread_create(const char *name, uint32_t entry, uint32_t prio, uint32_t stack_size) {
    init_once();
    int i;
    for (i = 0; i < MAX_THREADS && g_t[i].state != T_FREE; i++) {}
    if (i == MAX_THREADS) return 0;
    vthread *t = &g_t[i];
    memset(t, 0, sizeof(*t));
    t->state = T_NEW;
    t->uid = THREAD_UID_FIRST + (uint32_t)i;
    snprintf(t->name, sizeof(t->name), "%s", name ? name : "");
    t->entry = entry & ~1u;
    t->prio = prio;
    t->stack_size = (stack_size + 0xFFFu) & ~0xFFFu;
    if (t->stack_size < 0x1000) t->stack_size = 0x1000;
    cnd_init(&t->wake);
    return t->uid;
}

int vita_thread_start(uint32_t uid, uint32_t arglen, uint32_t argp) {
    init_once();
    vthread *t = by_uid(uid);
    if (!t) return (int)SCE_KERNEL_ERROR_INVALID_UID;
    if (t->state != T_NEW) return (int)SCE_KERNEL_ERROR_NOT_DORMANT;
    if (!t->stack_base) {
        t->stack_base = vita_sysmem_reserve(t->stack_size);
        if (!t->stack_base) return (int)SCE_KERNEL_ERROR_NO_MEMORY;
    }
    /* The arguments are copied onto the new stack, as the kernel does, and
     * the entry gets their length and the copy's address. */
    uint32_t top = t->stack_base + t->stack_size;
    uint32_t args = 0;
    if (arglen && argp) {
        args = (top - arglen) & ~15u;
        void *d = vita_mem_ptr(args, arglen);
        const void *s = vita_mem_ptr(argp, arglen);
        if (d && s) memcpy(d, s, arglen); else args = 0;
        top = args;
    }
    memset(&t->regs, 0, sizeof(t->regs));
    t->regs.r[0] = arglen;
    t->regs.r[1] = args;
    t->regs.sp = top - 16;
    t->regs.lr = 0;
    t->state = T_READY;

    if (thrd_create(&t->host, thread_main, t) != thrd_success) { t->state = T_NEW; return (int)SCE_KERNEL_ERROR_NO_MEMORY; }
    thrd_detach(t->host);
    t->host_started = 1;
    vita_hle_trace("thread \"%s\" (0x%08X) starts at 0x%08X, stack 0x%08X+0x%X", t->name, t->uid, t->entry, t->stack_base, t->stack_size);

    /* The new thread runs first, up to its first wait: that is what a
     * higher-priority thread would do, and it is what lets a worker set
     * itself up before the thread that made it goes on. */
    mtx_lock(&g_mtx);
    vthread *cur = g_cur;
    cur->state = T_READY;
    switch_to(cur, t);
    mtx_unlock(&g_mtx);
    return 0;
}

void vita_thread_exit(int status) {
    init_once();
    mtx_lock(&g_mtx);
    vthread *cur = g_cur;
    if (cur == &g_t[0]) {
        mtx_unlock(&g_mtx);
        vita_host_exit(status);
    }
    cur->state = T_DONE;
    cur->exit_status = status;
    vita_hle_trace("thread \"%s\" (0x%08X) ends with %d", cur->name, cur->uid, status);
    for (int i = 0; i < MAX_THREADS; i++) {
        vthread *w = &g_t[i];
        if (w->state == T_WAITING && w->wait_kind == VW_THREAD_END && w->wait_obj == cur->uid) { w->state = T_READY; w->wait_until = 0; }
    }
    vthread *next = pick_next(cur);
    if (next == cur) {                     /* cannot happen: main is never DONE here */
        mtx_unlock(&g_mtx);
        vita_host_exit(status);
    }
    load_regs(next);
    next->state = T_RUNNING;
    g_cur = next;
    next->go = 1;
    cnd_signal(&next->wake);
    mtx_unlock(&g_mtx);
    thrd_exit(0);
}

int vita_thread_wait_end(uint32_t uid, uint32_t timeout_us, int *status) {
    init_once();
    vthread *t = by_uid(uid);
    if (!t) return (int)SCE_KERNEL_ERROR_INVALID_UID;
    if (t->state != T_DONE) {
        if (vita_sched_block(VW_THREAD_END, uid, timeout_us) < 0) return (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT;
        t = by_uid(uid);
        if (!t) return (int)SCE_KERNEL_ERROR_INVALID_UID;
    }
    if (status) *status = t->exit_status;
    return 0;
}

int vita_thread_delete(uint32_t uid) {
    init_once();
    vthread *t = by_uid(uid);
    if (!t) return (int)SCE_KERNEL_ERROR_INVALID_UID;
    if (t->state == T_NEW || t->state == T_DONE) { cnd_destroy(&t->wake); t->state = T_FREE; return 0; }
    return (int)SCE_KERNEL_ERROR_NOT_DORMANT;
}

int vita_thread_info(uint32_t uid, char *name, uint32_t cap, uint32_t *entry,
                     uint32_t *stack, uint32_t *stack_size, uint32_t *prio) {
    init_once();
    vthread *t = by_uid(uid);
    if (!t) return -1;
    if (name && cap) snprintf(name, cap, "%s", t->name);
    if (entry) *entry = t->entry;
    if (stack) *stack = t->stack_base;
    if (stack_size) *stack_size = t->stack_size;
    if (prio) *prio = t->prio;
    return 0;
}

int vita_sched_block(int kind, uint32_t obj, uint32_t timeout_us) {
    init_once();
    mtx_lock(&g_mtx);
    vthread *cur = g_cur;
    cur->state = T_WAITING;
    cur->wait_kind = kind;
    cur->wait_obj = obj;
    cur->wait_until = timeout_us ? now_us() + timeout_us : 0;
    cur->timed_out = 0;
    vthread *next = pick_next(cur);
    switch_to(cur, next);
    int r = cur->timed_out ? -1 : 0;
    cur->wait_kind = VW_NONE;
    mtx_unlock(&g_mtx);
    return r;
}

void vita_sched_wake(int kind, uint32_t obj, int all) {
    init_once();
    for (int i = 0; i < MAX_THREADS; i++) {
        vthread *t = &g_t[i];
        if (t->state != T_WAITING || t->wait_kind != kind || t->wait_obj != obj) continue;
        t->state = T_READY;
        t->wait_until = 0;
        t->timed_out = 0;
        if (!all) break;
    }
}

void vita_sched_yield(void) {
    init_once();
    mtx_lock(&g_mtx);
    vthread *cur = g_cur;
    cur->state = T_READY;
    vthread *next = pick_next(cur);
    switch_to(cur, next);
    mtx_unlock(&g_mtx);
}

/* The running thread's slice ran out: the other runnable threads go, each
 * for a slice of its own, and this one resumes when the round comes back.
 * Timed waits that have come due count as runnable, so a thread that
 * computes without a firmware call no longer holds a sleeper past its time. */
void vita_slice_end(void) {
    vita_slice = VITA_SLICE;
    if (!g_init) return;                   /* one thread, and no one to give way to */
    mtx_lock(&g_mtx);
    vthread *cur = g_cur;
    (void)expire(now_us());
    int start = (int)(cur - g_t);
    for (int k = 1; k < MAX_THREADS; k++) {
        vthread *t = &g_t[(start + k) % MAX_THREADS];
        if (t->state != T_READY) continue;
        cur->state = T_READY;
        switch_to(cur, t);
        break;
    }
    mtx_unlock(&g_mtx);
}

/* The running thread is in a loop that only another thread can end (see
 * recomp_rt.h). Another runnable thread goes at once. With none, a sleeper
 * on the clock may still end it, so the host sleeps a little and the loop
 * looks again; with none of those either nothing can ever change the word,
 * which is a deadlock and is said. */
void vita_spin_wait(uint32_t addr) {
    init_once();
    mtx_lock(&g_mtx);
    vthread *cur = g_cur;
    uint64_t now = now_us(), earliest = expire(now);
    int start = (int)(cur - g_t);
    for (int k = 1; k < MAX_THREADS; k++) {
        vthread *t = &g_t[(start + k) % MAX_THREADS];
        if (t->state != T_READY) continue;
        cur->state = T_READY;
        switch_to(cur, t);
        mtx_unlock(&g_mtx);
        return;
    }
    if (!earliest) {
        uint32_t n = vita_sched_threads(NULL);
        fprintf(stderr, "vitarecomp: \"%s\" waits at 0x%08X on memory that no thread can change (%u threads, the rest waiting); the program is deadlocked\n",
                cur->name, addr, n);
        for (int i = 0; i < MAX_THREADS; i++)
            if (g_t[i].state == T_WAITING)
                fprintf(stderr, "  %s (0x%08X) waits on kind %d object 0x%08X\n",
                        g_t[i].name, g_t[i].uid, g_t[i].wait_kind, g_t[i].wait_obj);
        mtx_unlock(&g_mtx);
        vita_trap_unimpl(addr, n, "deadlock: a spin on memory no thread can change");
        vita_host_exit(3);
        return;
    }
    mtx_unlock(&g_mtx);
    uint64_t d = earliest > now ? earliest - now : 0;
    if (d > 1000) d = 1000;
    if (d) {
        struct timespec ts = { 0, (long)(d * 1000ull) };
        thrd_sleep(&ts, NULL);
    }
}

void vita_sched_delay(uint32_t us) {
    if (us == 0) { vita_sched_yield(); return; }
    (void)vita_sched_block(VW_DELAY, 0, us);
}
