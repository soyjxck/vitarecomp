/* mem.c — a flat guest address space.
 *
 * One allocation and a subtraction. The Vita's MMU means there is no fixed map
 * to model; what matters for a recompiled module is that the addresses it
 * actually touches are backed and that the ones it should not touch are
 * noticed rather than silently absorbed.
 */

#include "vitarecomp/mem.h"
#include "vitarecomp/cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

uint32_t vita_mem_bad_access;

static uint8_t *g_mem;
static uint32_t g_base;
static size_t   g_size;

/* what the reads and writes done in line go by (mem.h) */
uint8_t *vita_mem_host;
uint32_t vita_mem_first;
uint64_t vita_mem_read_room, vita_mem_write_room;

static int watching(void);

/* The first bad accesses are named under VITARECOMP_TRACE, because "24 bad
 * accesses" says something went wrong and nothing about what: the addresses
 * do. After the first few the count alone is kept, so a loop reading garbage
 * does not drown the log. */
static void bad(uint32_t addr, uint32_t len) {
    static int trace = -1, shown = 0;
    vita_mem_bad_access++;
    if (trace < 0) {
        const char *e = getenv("VITARECOMP_TRACE");
        trace = (e && *e && *e != '0') ? 1 : 0;
    }
    if (trace && shown < 16) {
        shown++;
        if (vita_last_pc)
            fprintf(stderr, "TRACE badmem  0x%08X  (%u bytes)  at pc 0x%08X%s\n", addr, len, vita_last_pc,
                    shown == 16 ? "  [further bad accesses counted only]" : "");
        else
            fprintf(stderr, "TRACE badmem  0x%08X  (%u bytes)%s\n", addr, len,
                    shown == 16 ? "  [further bad accesses counted only]" : "");
    }
}

/* --- which pages have been written --------------------------------------------------------------
 * Windows keeps count for memory that asks for it when it is allocated:
 * the pages written since they were last asked about. A moment is a number
 * that grows, and a page remembers the last moment it was found written
 * at.
 *
 * The system is asked about the pages somebody asks about, when they ask,
 * and about a page once a moment. Asking about all of the program's memory
 * at every moment was half a millisecond each time, twenty times a frame:
 * the cost is in the pages found written, each of which the system makes
 * ready to be caught again, and a program's stack and heap are written
 * all the time by nobody who is ever asked about. A page found written
 * is given the moment it was found at, which is not before it was
 * written: a reader who read it after the writing and before the asking
 * is told once that it changed, and reads what it had. */

#define PAGE 4096u
#define CHUNK 16u                           /* pages asked about together: see vita_mem_written */

static uint32_t *g_page_moment;             /* a page: the last moment it was found written at; NULL where nothing keeps count */
static uint32_t *g_page_asked;              /* a page: the moment the system was last asked about it */
static uint32_t  g_moment = 1;

/* What the counting has come to, for whoever profiles: how many times the
 * system was asked, how many pages it said were written, and, while
 * vita_mem_timed is set, how long the asking took in all, in the units of
 * the system's finest clock (vita_mem_ticks of them to a second). */
uint64_t vita_mem_counts, vita_mem_counted, vita_mem_asking, vita_mem_ticks;
int vita_mem_timed;

#ifdef _WIN32
static uint8_t *region_make(size_t size) {
    uint8_t *p = (uint8_t *)VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (p) {
        g_page_moment = (uint32_t *)calloc(size / PAGE + 1, sizeof(uint32_t));
        g_page_asked = (uint32_t *)calloc(size / PAGE + 1, sizeof(uint32_t));
        if (!g_page_moment || !g_page_asked) { free(g_page_moment); free(g_page_asked); g_page_moment = g_page_asked = NULL; }
    }
    return p;
}
static void region_free(uint8_t *p) { if (p) VirtualFree(p, 0, MEM_RELEASE); }

/* Ask the system about `pages` pages from page `first`, none of them asked
 * about at this moment yet. */
static void pages_ask(size_t first, size_t pages) {
    static void *written[4096];
    LARGE_INTEGER began, ended;
    if (vita_mem_timed) QueryPerformanceCounter(&began);
    for (;;) {
        ULONG_PTR count = sizeof(written) / sizeof(written[0]);
        DWORD granularity = 0;
        vita_mem_counts++;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, g_mem + first * PAGE, pages * PAGE, written, &count, &granularity)) {
            /* the count cannot be had: nothing is known, and everything may have been written */
            free(g_page_moment);
            free(g_page_asked);
            g_page_moment = g_page_asked = NULL;
            return;
        }
        for (ULONG_PTR i = 0; i < count; i++) g_page_moment[(size_t)((uint8_t *)written[i] - g_mem) / PAGE] = g_moment;
        vita_mem_counted += count;
        if (count < sizeof(written) / sizeof(written[0])) break;
    }
    for (size_t i = 0; i < pages; i++) g_page_asked[first + i] = g_moment;
    if (vita_mem_timed) {
        if (!vita_mem_ticks) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); vita_mem_ticks = (uint64_t)f.QuadPart; }
        QueryPerformanceCounter(&ended);
        vita_mem_asking += (uint64_t)(ended.QuadPart - began.QuadPart);
    }
}

uint32_t vita_mem_moment(void) {
    if (g_mem && g_page_moment) g_moment++;
    return g_moment;
}
#else
static uint8_t *region_make(size_t size) { return (uint8_t *)calloc(size, 1); }
static void region_free(uint8_t *p) { free(p); }
static void pages_ask(size_t first, size_t pages) { (void)first; (void)pages; }
uint32_t vita_mem_moment(void) { return g_moment; }
#endif

int vita_mem_written(const void *bytes, size_t count, uint32_t since) {
    const uint8_t *p = (const uint8_t *)bytes;
    if (!g_page_moment || !p || p < g_mem || p + count > g_mem + g_size) return 1;
    if (!count) return 0;
    size_t first = (size_t)(p - g_mem) / PAGE, last = (size_t)(p - g_mem + count - 1) / PAGE;
    /* What is known of these pages is brought up to this moment, a run of them not yet asked about at a time; and
     * of their neighbours, sixteen pages together. What lies beside a draw's vertices is the next draw's, as a
     * rule, and one asking for both is half the askings; larger pieces take in more of what the program writes
     * for itself, which the system then has to make ready again. */
    size_t wide_first = first & ~(size_t)(CHUNK - 1), wide_last = last | (CHUNK - 1), pages = g_size / PAGE;
    if (wide_last >= pages) wide_last = pages - 1;
    for (size_t i = wide_first; i <= wide_last && g_page_moment;) {
        size_t run = i;
        if (g_page_asked[i] == g_moment) { i++; continue; }
        while (run <= wide_last && g_page_asked[run] != g_moment) run++;
        pages_ask(i, run - i);
        i = run;
    }
    if (!g_page_moment) return 1;
    for (size_t i = first; i <= last; i++) if (g_page_moment[i] > since) return 1;
    return 0;
}

int vita_mem_init(uint32_t base, size_t size) {
    vita_mem_free();
    g_mem = region_make(size);
    if (!g_mem) return 1;
    g_base = base;
    g_size = size;
    vita_mem_host = g_mem;
    vita_mem_first = base;
    vita_mem_read_room = size;
    vita_mem_write_room = watching() ? 0 : size;       /* a watched write is looked at, every one */
    return 0;
}

void vita_mem_free(void) {
    region_free(g_mem);
    free(g_page_moment);
    free(g_page_asked);
    g_page_moment = g_page_asked = NULL;
    g_mem = NULL;
    g_base = 0;
    g_size = 0;
    vita_mem_host = NULL;
    vita_mem_first = 0;
    vita_mem_read_room = vita_mem_write_room = 0;
}

void *vita_mem_ptr(uint32_t addr, uint32_t len) {
    if (!g_mem || addr < g_base) { bad(addr, len); return NULL; }
    uint64_t off = (uint64_t)addr - g_base;
    /* Reject a straddling access rather than reading past the allocation. */
    if (off + len > g_size) { bad(addr, len); return NULL; }
    return g_mem + off;
}

uint32_t vita_mem_avail(uint32_t addr) {
    if (!g_mem || addr < g_base) return 0;
    uint64_t off = (uint64_t)addr - g_base;
    if (off >= g_size) return 0;
    return (uint32_t)(g_size - off);
}

/* What the reads and writes in line leave: the access that is not in the
 * region, counted and reported, and the write that is watched. Unaligned
 * access is legal on ARMv7 for ordinary loads and stores, so these go
 * through memcpy rather than a cast. A cast would be undefined behaviour on
 * an unaligned address and would fault outright on some hosts. */

uint32_t vita_read32_slow(uint32_t a) {
    void *p = vita_mem_ptr(a, 4);
    uint32_t v = 0;
    if (p) memcpy(&v, p, 4);
    return v;
}

uint16_t vita_read16_slow(uint32_t a) {
    void *p = vita_mem_ptr(a, 2);
    uint16_t v = 0;
    if (p) memcpy(&v, p, 2);
    return v;
}

uint8_t vita_read8_slow(uint32_t a) {
    void *p = vita_mem_ptr(a, 1);
    return p ? *(uint8_t *)p : 0;
}

/* A write watchpoint: VITARECOMP_WATCH=<hex address> names a guest address,
 * and every store that covers it is logged with the value and the PC (when
 * the C was emitted with VITARECOMP_EMIT_PC). The question it answers is
 * "who overwrote this stack slot", which no other trace can. */
static uint32_t g_watch, g_watch_set = (uint32_t)-1;

static int watching(void) {
    if (g_watch_set == (uint32_t)-1) {
        const char *e = getenv("VITARECOMP_WATCH");
        g_watch_set = (e && *e) ? 1 : 0;
        if (g_watch_set) g_watch = (uint32_t)strtoul(e, NULL, 16);
    }
    return (int)g_watch_set;
}

static void watch(uint32_t a, uint32_t len, uint32_t v) {
    if (watching() && g_watch >= a && g_watch < a + len)
        fprintf(stderr, "TRACE watch   write 0x%08X (%u bytes) = 0x%08X  at pc 0x%08X\n",
                a, len, v, vita_last_pc);
}

void vita_write32_slow(uint32_t a, uint32_t v) {
    void *p = vita_mem_ptr(a, 4);
    if (p) { watch(a, 4, v); memcpy(p, &v, 4); }
}

void vita_write16_slow(uint32_t a, uint16_t v) {
    void *p = vita_mem_ptr(a, 2);
    if (p) { watch(a, 2, v); memcpy(p, &v, 2); }
}

void vita_write8_slow(uint32_t a, uint8_t v) {
    void *p = vita_mem_ptr(a, 1);
    if (p) { watch(a, 1, v); *(uint8_t *)p = v; }
}
