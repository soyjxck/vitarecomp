/* symbols.c — names for addresses, from an `nm -n` listing.
 *
 * A trace that says "0x810036C0" is a trace that needs a disassembly open
 * beside it. One that says "__getreent_for_thread" does not. The toolkit
 * never needs symbols — a retail module has none — but a sample built with
 * vitasdk has them, and bring-up goes faster when the host can show them.
 *
 * The format is what `arm-vita-eabi-nm -n module.elf` prints:
 *   81003890 T _init_vita_reent
 * sorted by address. Lines that do not look like that are skipped.
 */

#include "vitarecomp/host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t addr; char *name; } sym;

static sym     *g_syms;
static uint32_t g_count, g_cap;

static int by_addr(const void *a, const void *b) {
    uint32_t x = ((const sym *)a)->addr, y = ((const sym *)b)->addr;
    return x < y ? -1 : x > y;
}

int vita_symbols_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 1;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        unsigned addr;
        char type;
        char name[400];
        if (sscanf(line, "%x %c %399s", &addr, &type, name) != 3) continue;
        if (type != 'T' && type != 't' && type != 'W' && type != 'w') continue;
        if (g_count == g_cap) {
            g_cap = g_cap ? g_cap * 2 : 1024;
            g_syms = (sym *)realloc(g_syms, g_cap * sizeof(sym));
        }
        size_t len = strlen(name) + 1;
        char *copy = (char *)malloc(len);
        if (!copy) break;
        memcpy(copy, name, len);
        g_syms[g_count].addr = addr & ~1u;
        g_syms[g_count].name = copy;
        g_count++;
    }
    fclose(f);
    qsort(g_syms, g_count, sizeof(sym), by_addr);
    return 0;
}

uint32_t vita_symbols_count(void) { return g_count; }

/* The symbol at or just below `addr`; `off` receives the distance. */
const char *vita_symbol_name(uint32_t addr, uint32_t *off) {
    if (!g_count) return NULL;
    uint32_t key = addr & ~1u;
    uint32_t lo = 0, hi = g_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (g_syms[mid].addr <= key) lo = mid + 1; else hi = mid;
    }
    if (lo == 0) return NULL;
    const sym *s = &g_syms[lo - 1];
    if (off) *off = key - s->addr;
    return s->name;
}
