/* hle_mspace.c — SceLibKernel's sceClibMspace*: a heap inside a region the
 * program owns.
 *
 * A retail title does not malloc from the C library. It allocates memory
 * blocks (hle_sysmem.c), hands each to sceClibMspaceCreate, and carves its
 * heaps out of them itself; the GPU maps some of those blocks, so what comes
 * back from an mspace has to lie inside the region it was created on. A
 * first-fit list of chunks per mspace does that, with splitting on
 * allocation and coalescing on free; it is not the console's dlmalloc, and
 * nothing a program can observe depends on which it is except the addresses.
 *
 * The handle is the region's base address, which is what the console returns
 * too (its mspace header lives there); the bookkeeping is host-side.
 *
 * Calling convention: see hle.h. Arguments in r0-r3, result in r0, every
 * pointer a guest address.
 */

#include "vitarecomp/hle.h"
#include "vitarecomp/recomp_rt.h"

#include <stdlib.h>
#include <string.h>

#define MAX_MSPACES 32

typedef struct { uint32_t addr, size; uint8_t used; } chunk;

typedef struct {
    int      live;
    uint32_t base, size;
    chunk   *c;
    uint32_t n, cap;
} mspace;

static mspace g_ms[MAX_MSPACES];

static mspace *find(uint32_t handle) {
    for (int i = 0; i < MAX_MSPACES; i++)
        if (g_ms[i].live && g_ms[i].base == handle) return &g_ms[i];
    return NULL;
}

/* The mspace that contains an address, for the calls that take only a pointer. */
static mspace *find_by_addr(uint32_t a) {
    for (int i = 0; i < MAX_MSPACES; i++)
        if (g_ms[i].live && a >= g_ms[i].base && a < g_ms[i].base + g_ms[i].size) return &g_ms[i];
    return NULL;
}

static int insert(mspace *m, uint32_t at, chunk v) {
    if (m->n == m->cap) {
        uint32_t cap = m->cap ? m->cap * 2 : 256;
        chunk *c = (chunk *)realloc(m->c, cap * sizeof(chunk));
        if (!c) return 0;
        m->c = c; m->cap = cap;
    }
    memmove(m->c + at + 1, m->c + at, (m->n - at) * sizeof(chunk));
    m->c[at] = v;
    m->n++;
    return 1;
}

static void erase(mspace *m, uint32_t at) {
    memmove(m->c + at, m->c + at + 1, (m->n - at - 1) * sizeof(chunk));
    m->n--;
}

/* First fit: the lowest free chunk that can hold `size` bytes at `align`.
 * The part before the aligned start stays free, the part after is split
 * off, so the chunk list stays a partition of the region. */
static uint32_t ms_alloc(mspace *m, uint32_t align, uint32_t size) {
    if (align < 8) align = 8;
    size = (size + 7u) & ~7u;
    if (size == 0) size = 8;
    for (uint32_t i = 0; i < m->n; i++) {
        chunk *c = &m->c[i];
        if (c->used) continue;
        uint32_t start = (c->addr + align - 1) & ~(align - 1);
        if (start < c->addr || start - c->addr > c->size) continue;
        if (c->size - (start - c->addr) < size) continue;
        uint32_t before = start - c->addr, after = c->size - before - size;
        if (before) {
            chunk head = { c->addr, before, 0 };
            if (!insert(m, i, head)) return 0;
            c = &m->c[i + 1];
            i++;
        }
        if (after) {
            chunk tail = { start + size, after, 0 };
            if (!insert(m, i + 1, tail)) return 0;
            c = &m->c[i];
        }
        c->addr = start; c->size = size; c->used = 1;
        return start;
    }
    return 0;
}

static int ms_free(mspace *m, uint32_t a) {
    for (uint32_t i = 0; i < m->n; i++) {
        chunk *c = &m->c[i];
        if (c->addr != a) continue;
        if (!c->used) return 0;                     /* a double free */
        c->used = 0;
        if (i + 1 < m->n && !m->c[i + 1].used) { c->size += m->c[i + 1].size; erase(m, i + 1); }
        if (i > 0 && !m->c[i - 1].used) { m->c[i - 1].size += c->size; erase(m, i); }
        return 1;
    }
    return 0;
}

static uint32_t ms_size_of(mspace *m, uint32_t a) {
    for (uint32_t i = 0; i < m->n; i++)
        if (m->c[i].addr == a && m->c[i].used) return m->c[i].size;
    return 0;
}

/* SceClibMspace sceClibMspaceCreate(void *base, SceSize size) */
void vita_hle_sceClibMspaceCreate(void) {
    uint32_t base = r0, size = r1;
    if (!size || !vita_mem_ptr(base, size)) { r0 = 0; return; }
    int i;
    for (i = 0; i < MAX_MSPACES && g_ms[i].live; i++) {}
    if (i == MAX_MSPACES) { vita_trap_unimpl(base, size, "sceClibMspaceCreate: too many mspaces"); r0 = 0; return; }
    mspace *m = &g_ms[i];
    memset(m, 0, sizeof(*m));
    m->live = 1; m->base = base; m->size = size;
    chunk all = { base, size, 0 };
    if (!insert(m, 0, all)) { m->live = 0; r0 = 0; return; }
    vita_hle_trace("sceClibMspaceCreate(0x%08X, 0x%X) -> 0x%08X", base, size, base);
    r0 = base;
}

/* int sceClibMspaceDestroy(SceClibMspace msp) */
void vita_hle_sceClibMspaceDestroy(void) {
    mspace *m = find(r0);
    if (!m) { r0 = (uint32_t)-1; return; }
    free(m->c);
    memset(m, 0, sizeof(*m));
    r0 = 0;
}

/* void *sceClibMspaceMalloc(SceClibMspace msp, SceSize size) */
void vita_hle_sceClibMspaceMalloc(void) {
    mspace *m = find(r0);
    if (!m) { vita_trap_unimpl(r0, r1, "sceClibMspaceMalloc: no such mspace"); r0 = 0; return; }
    uint32_t a = ms_alloc(m, 8, r1);
    if (!a) vita_hle_trace("sceClibMspaceMalloc(0x%08X, 0x%X): out of space", m->base, r1);
    r0 = a;
}

/* void *sceClibMspaceCalloc(SceClibMspace msp, SceSize num, SceSize size) */
void vita_hle_sceClibMspaceCalloc(void) {
    mspace *m = find(r0);
    uint32_t n = r1 * r2;
    if (!m) { vita_trap_unimpl(r0, n, "sceClibMspaceCalloc: no such mspace"); r0 = 0; return; }
    uint32_t a = ms_alloc(m, 8, n);
    if (a) { void *p = vita_mem_ptr(a, n); if (p) memset(p, 0, n); }
    r0 = a;
}

/* void *sceClibMspaceMemalign(SceClibMspace msp, SceSize align, SceSize size) */
void vita_hle_sceClibMspaceMemalign(void) {
    mspace *m = find(r0);
    if (!m) { vita_trap_unimpl(r0, r2, "sceClibMspaceMemalign: no such mspace"); r0 = 0; return; }
    uint32_t align = r1;
    if (align == 0 || (align & (align - 1))) align = 8;
    uint32_t a = ms_alloc(m, align, r2);
    if (!a) vita_hle_trace("sceClibMspaceMemalign(0x%08X, %u, 0x%X): out of space", m->base, align, r2);
    r0 = a;
}

/* void *sceClibMspaceRealloc(SceClibMspace msp, void *ptr, SceSize size) */
void vita_hle_sceClibMspaceRealloc(void) {
    mspace *m = find(r0);
    uint32_t old = r1, n = r2;
    if (!m) { vita_trap_unimpl(r0, n, "sceClibMspaceRealloc: no such mspace"); r0 = 0; return; }
    if (!old) { r0 = ms_alloc(m, 8, n); return; }
    uint32_t have = ms_size_of(m, old);
    if (have >= n) { r0 = old; return; }
    uint32_t a = ms_alloc(m, 8, n);
    if (a) {
        void *d = vita_mem_ptr(a, have), *s = vita_mem_ptr(old, have);
        if (d && s) memcpy(d, s, have);
        ms_free(m, old);
    }
    r0 = a;
}

/* void sceClibMspaceFree(SceClibMspace msp, void *ptr) */
void vita_hle_sceClibMspaceFree(void) {
    mspace *m = find(r0);
    if (!m || !r1) return;
    if (!ms_free(m, r1)) vita_hle_trace("sceClibMspaceFree(0x%08X, 0x%08X): not an allocation", m->base, r1);
}

/* SceSize sceClibMspaceMallocUsableSize(void *ptr) */
void vita_hle_sceClibMspaceMallocUsableSize(void) {
    mspace *m = find_by_addr(r0);
    r0 = m ? ms_size_of(m, r0) : 0;
}

/* int sceClibMspaceIsHeapEmpty(SceClibMspace msp) */
void vita_hle_sceClibMspaceIsHeapEmpty(void) {
    mspace *m = find(r0);
    if (!m) { r0 = 0; return; }
    for (uint32_t i = 0; i < m->n; i++) if (m->c[i].used) { r0 = 0; return; }
    r0 = 1;
}
