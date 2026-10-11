/* profile.c — where the time goes.
 *
 * A recompiled program that makes no firmware call says nothing: the trace
 * is as silent while it computes as while it spins on a flag another thread
 * should have set, or walks a list that has become a ring. This is the
 * view for that silence. A host thread samples the guest address the
 * generated code records before each instruction (C emitted with
 * VITARECOMP_EMIT_PC=1; without it every sample reads zero) and the guest
 * thread that holds the baton, and every period prints the functions the
 * samples fell in and the instruction each was most often at.
 *
 * Time inside a firmware call is charged to the instruction that made it.
 * The read of the recorded address is not synchronised: a sample is a
 * word, and a stale one is still somewhere the program just was.
 */

#include "sched.h"
#include "vitarecomp/cpu.h"
#include "vitarecomp/dispatch.h"
#include "vitarecomp/host.h"

#include <stdio.h>
#include <string.h>
#include <threads.h>
#include <time.h>

#define PC_SLOTS  8192u        /* a power of two; a period holds far fewer */
#define FN_MAX    1024u
#define THR_MAX   16u
#define TOP       12u

typedef struct { uint32_t key, count; } slot;
typedef struct { uint32_t fn, count, hot_pc, hot_count; } fn_row;

static slot     g_pc[PC_SLOTS];
static struct { uint32_t uid, count; char name[32]; } g_thr[THR_MAX];
static uint32_t g_period;
static volatile int g_sampling;

static void bump_pc(uint32_t pc) {
    uint32_t i = (pc * 2654435761u) >> 19;
    for (uint32_t n = 0; n < PC_SLOTS; n++, i = (i + 1) & (PC_SLOTS - 1)) {
        if (g_pc[i].count && g_pc[i].key == pc) { g_pc[i].count++; return; }
        if (!g_pc[i].count) { g_pc[i].key = pc; g_pc[i].count = 1; return; }
    }
}

static void bump_thread(uint32_t uid, const char *name) {
    for (uint32_t i = 0; i < THR_MAX; i++) {
        if (g_thr[i].count && g_thr[i].uid == uid) { g_thr[i].count++; return; }
        if (!g_thr[i].count) {
            g_thr[i].uid = uid;
            g_thr[i].count = 1;
            snprintf(g_thr[i].name, sizeof(g_thr[i].name), "%s", name);
            return;
        }
    }
}

static void report(uint32_t total) {
    static fn_row rows[FN_MAX];
    uint32_t nrows = 0;
    for (uint32_t i = 0; i < PC_SLOTS; i++) {
        if (!g_pc[i].count) continue;
        uint32_t fn = vita_dispatch_floor(g_pc[i].key), k;
        for (k = 0; k < nrows && rows[k].fn != fn; k++) {}
        if (k == nrows) {
            if (nrows == FN_MAX) continue;
            memset(&rows[nrows++], 0, sizeof(rows[0]));
            rows[k].fn = fn;
        }
        rows[k].count += g_pc[i].count;
        if (g_pc[i].count > rows[k].hot_count) { rows[k].hot_count = g_pc[i].count; rows[k].hot_pc = g_pc[i].key; }
    }

    fprintf(stderr, "PROFILE %u s, %u samples in %u functions:", g_period, total, nrows);
    for (uint32_t i = 0; i < THR_MAX && g_thr[i].count; i++)
        fprintf(stderr, "  %s %u%%", g_thr[i].name, g_thr[i].count * 100u / (total ? total : 1u));
    fputc('\n', stderr);

    for (uint32_t n = 0; n < TOP && n < nrows; n++) {
        uint32_t best = n;
        for (uint32_t k = n + 1; k < nrows; k++) if (rows[k].count > rows[best].count) best = k;
        fn_row t = rows[n]; rows[n] = rows[best]; rows[best] = t;
        fprintf(stderr, "PROFILE   %5.1f%%  fn 0x%08X  most at 0x%08X (%u of %u)\n",
                rows[n].count * 100.0 / (total ? total : 1u), rows[n].fn,
                rows[n].hot_pc, rows[n].hot_count, rows[n].count);
    }
    fflush(stderr);
}

static int sampler(void *arg) {
    (void)arg;
    const struct timespec tick = { 0, 2000000 };      /* the host rounds it up */
    time_t   next  = time(NULL) + (time_t)g_period;
    uint32_t total = 0;
    while (g_sampling) {
        thrd_sleep(&tick, NULL);
        char name[32];
        uint32_t uid = vita_sched_running(name, sizeof(name));
        bump_pc(vita_last_pc);
        bump_thread(uid, name);
        total++;
        if (time(NULL) < next) continue;
        report(total);
        memset(g_pc, 0, sizeof(g_pc));
        memset(g_thr, 0, sizeof(g_thr));
        total = 0;
        next += (time_t)g_period;
    }
    return 0;
}

void vita_profile_start(uint32_t period_s) {
    if (!period_s || g_period) return;
    g_period = period_s;
    g_sampling = 1;
    (void)vita_thread_self();          /* the scheduler exists before it is asked */
    thrd_t t;
    if (thrd_create(&t, sampler, NULL) == thrd_success) thrd_detach(t);
}
