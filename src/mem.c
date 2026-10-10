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

uint32_t vita_mem_bad_access;

static uint8_t *g_mem;
static uint32_t g_base;
static size_t   g_size;

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

int vita_mem_init(uint32_t base, size_t size) {
    vita_mem_free();
    g_mem = (uint8_t *)calloc(size, 1);
    if (!g_mem) return 1;
    g_base = base;
    g_size = size;
    return 0;
}

void vita_mem_free(void) {
    free(g_mem);
    g_mem = NULL;
    g_base = 0;
    g_size = 0;
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

/* Unaligned access is legal on ARMv7 for ordinary loads and stores, so these
 * go through memcpy rather than a cast. A cast would be undefined behaviour on
 * an unaligned address and would fault outright on some hosts. */

uint32_t vita_read32(uint32_t a) {
    void *p = vita_mem_ptr(a, 4);
    uint32_t v = 0;
    if (p) memcpy(&v, p, 4);
    return v;
}

uint16_t vita_read16(uint32_t a) {
    void *p = vita_mem_ptr(a, 2);
    uint16_t v = 0;
    if (p) memcpy(&v, p, 2);
    return v;
}

uint8_t vita_read8(uint32_t a) {
    void *p = vita_mem_ptr(a, 1);
    return p ? *(uint8_t *)p : 0;
}

/* A write watchpoint: VITARECOMP_WATCH=<hex address> names a guest address,
 * and every store that covers it is logged with the value and the PC (when
 * the C was emitted with VITARECOMP_EMIT_PC). The question it answers is
 * "who overwrote this stack slot", which no other trace can. */
static uint32_t g_watch, g_watch_set = (uint32_t)-1;

static void watch(uint32_t a, uint32_t len, uint32_t v) {
    if (g_watch_set == (uint32_t)-1) {
        const char *e = getenv("VITARECOMP_WATCH");
        g_watch_set = (e && *e) ? 1 : 0;
        if (g_watch_set) g_watch = (uint32_t)strtoul(e, NULL, 16);
    }
    if (g_watch_set && g_watch >= a && g_watch < a + len)
        fprintf(stderr, "TRACE watch   write 0x%08X (%u bytes) = 0x%08X  at pc 0x%08X\n",
                a, len, v, vita_last_pc);
}

void vita_write32(uint32_t a, uint32_t v) {
    void *p = vita_mem_ptr(a, 4);
    if (p) { watch(a, 4, v); memcpy(p, &v, 4); }
}

void vita_write16(uint32_t a, uint16_t v) {
    void *p = vita_mem_ptr(a, 2);
    if (p) { watch(a, 2, v); memcpy(p, &v, 2); }
}

void vita_write8(uint32_t a, uint8_t v) {
    void *p = vita_mem_ptr(a, 1);
    if (p) { watch(a, 1, v); *(uint8_t *)p = v; }
}
