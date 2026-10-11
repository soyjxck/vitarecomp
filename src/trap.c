/* trap.c — the gaps, announcing themselves.
 *
 * Every one of these is a place the recompiler knew it did not know something.
 * The default is to abort, because a program that continues past an
 * untranslated instruction produces wrong results with full confidence, and
 * that is far more expensive to debug than a stop with an address on it.
 *
 * VITARECOMP_TRACE=1 changes that to log-and-continue. It is a DISCOVERY AID,
 * not a correctness mode: past the first unimplemented import the machine state
 * is wrong, and every subsequent trap is reached through a path that would not
 * have happened on hardware. What it is good for is one run naming twenty
 * firmware functions instead of one, which is far better prioritisation than a
 * static count of what the import table contains. Anything it reports should be
 * treated as "the module wanted this" and not as "the module wants these, in
 * this order, this many times".
 */

#include "vitarecomp/recomp_rt.h"
#include "vitarecomp/hle.h"
#include "vitarecomp/host.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int   g_trace = -1;
static FILE *g_out;

/* --- function-entry trace ------------------------------------------------------
 *
 * Every generated function begins with a call here. The cost while off is
 * one global load and a branch; while on, it is the call log that turns
 * "it exited with 2" into "it exited from _malloc_r after _sbrk_r returned
 * early", which is the question bring-up keeps asking. */

int vita_trace_fns;
static uint32_t g_depth_hint;

/* The quiet form of the same: with vita_trace_fns at 2 the entries are
 * counted, by function, and nothing is printed until the count is asked
 * for. Two runs that differ in one thing, a finger on the screen in one and
 * not in the other, then differ in the functions that deal with it, and
 * the list of those is short where the call log of either run is not. One
 * guest thread runs at a time, so nothing here is locked. */
#define RAN_SLOTS (1u << 17)
static struct { uint32_t addr, count; } g_ran[RAN_SLOTS];

static void ran(uint32_t addr) {
    uint32_t i = (addr * 2654435761u) >> 15;
    for (uint32_t n = 0; n < RAN_SLOTS; n++, i = (i + 1) & (RAN_SLOTS - 1)) {
        if (g_ran[i].addr == addr) { g_ran[i].count++; return; }
        if (!g_ran[i].count) { g_ran[i].addr = addr; g_ran[i].count = 1; return; }
    }
}

static int by_addr(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

void vita_coverage_write(FILE *f) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < RAN_SLOTS; i++)
        if (g_ran[i].count) g_ran[n++] = g_ran[i];
    qsort(g_ran, n, sizeof(g_ran[0]), by_addr);
    for (uint32_t i = 0; i < n; i++) fprintf(f, "0x%08X %u\n", g_ran[i].addr, g_ran[i].count);
    memset(g_ran, 0, sizeof(g_ran));
}

void vita_trace_fn(uint32_t addr) {
    if (!vita_trace_fns) return;
    if (vita_trace_fns == 2) { ran(addr); return; }
    uint32_t off = 0;
    const char *name = vita_symbol_name(addr, &off);
    g_depth_hint++;
    if (name && off == 0) fprintf(stderr, "TRACE fn  %6u  0x%08X  %s\n", g_depth_hint, addr, name);
    else if (name)        fprintf(stderr, "TRACE fn  %6u  0x%08X  %s+0x%X\n", g_depth_hint, addr, name, off);
    else                  fprintf(stderr, "TRACE fn  %6u  0x%08X\n", g_depth_hint, addr);
}

/* Distinct traps already reported, so a loop does not bury the log. */
#define SEEN_SLOTS 4096
static uint32_t g_seen[SEEN_SLOTS];
static uint32_t g_hits;

static int trace_on(void) {
    if (g_trace < 0) {
        const char *e = getenv("VITARECOMP_TRACE");
        g_trace = (e && *e && *e != '0') ? 1 : 0;
        g_out = stderr;
    }
    return g_trace;
}

/* Report each distinct key once. Returns 1 the first time a key is seen. */
static int first_time(uint32_t key) {
    uint32_t h = (key * 2654435761u) & (SEEN_SLOTS - 1);
    for (uint32_t i = 0; i < SEEN_SLOTS; i++) {
        uint32_t s = (h + i) & (SEEN_SLOTS - 1);
        if (g_seen[s] == key) return 0;
        if (g_seen[s] == 0)   { g_seen[s] = key; return 1; }
    }
    return 1;   /* table full: report rather than lose it */
}

uint32_t vita_trap_count(void) { return g_hits; }

void vita_hle_trace(const char *fmt, ...) {
    if (!trace_on()) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("TRACE hle     ", g_out);
    vfprintf(g_out, fmt, ap);
    fputc('\n', g_out);
    va_end(ap);
}

void vita_trap_unimpl(uint32_t addr, uint32_t raw, const char *what) {
    g_hits++;
    if (trace_on()) {
        if (first_time(addr))
            fprintf(g_out, "TRACE unimpl  0x%08X  raw 0x%08X  %s\n",
                    addr, raw, what ? what : "?");
        return;
    }
    fprintf(stderr,
            "vitarecomp: untranslated instruction at 0x%08X (raw 0x%08X): %s\n",
            addr, raw, what ? what : "?");
    abort();
}

void vita_trap_indirect(uint32_t addr, uint32_t target) {
    g_hits++;
    if (trace_on()) {
        if (first_time(addr))
            fprintf(g_out, "TRACE indirect 0x%08X -> 0x%08X\n", addr, target);
        return;
    }
    fprintf(stderr,
            "vitarecomp: unresolved indirect transfer at 0x%08X -> 0x%08X\n",
            addr, target);
    abort();
}

/* `name` is the resolved "Library::function", or NULL when the NID database did
 * not know it. Naming the function is the entire point of binding imports — a
 * NID is a hash, and "implement 0xBFE02B3A" is not a task anyone can start. */
void vita_trap_import(uint32_t addr, uint32_t nid, const char *name) {
    g_hits++;
    if (!name) name = "(unresolved NID)";
    if (trace_on()) {
        /* Keyed on the NID, not the call site: the interesting question is
         * which firmware function is wanted, and the same one called from
         * three places is one thing to implement, not three. */
        if (first_time(nid ? nid : addr))
            fprintf(g_out, "TRACE import  %s  NID 0x%08X  (stub 0x%08X)\n",
                    name, nid, addr);
        return;
    }
    fprintf(stderr,
            "vitarecomp: unimplemented firmware import %s"
            " at 0x%08X (NID 0x%08X)\n",
            name, addr, nid);
    abort();
}
