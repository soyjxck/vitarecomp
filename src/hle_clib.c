/* hle_clib.c — SceLibKernel's sceClib* family.
 *
 * The firmware's own fast memcpy, memset, memmove and printf, which vitasdk's
 * newlib forwards the C functions to. The semantics are the standard ones;
 * what is not standard is the boundary, exactly as in hle_libc.c: every
 * pointer is a guest address and every length is checked against what is
 * backed before a host routine is allowed near it.
 */

#include "vitarecomp/recomp_rt.h"

#include <stdio.h>
#include <string.h>

static void *gp(uint32_t addr, uint32_t len, const char *who) {
    if (len == 0) return NULL;
    void *p = vita_mem_ptr(addr, len);
    if (!p) vita_trap_unimpl(addr, len, who);
    return p;
}

/* void *sceClibMemset(void *dst, int ch, SceSize len) */
void vita_hle_sceClibMemset(void) {
    void *d = gp(r0, r2, "sceClibMemset");
    if (d) memset(d, (int)(r1 & 0xFF), r2);
    /* r0 is already the destination */
}

/* void *sceClibMemcpy(void *dst, const void *src, SceSize len) */
void vita_hle_sceClibMemcpy(void) {
    void *d = gp(r0, r2, "sceClibMemcpy");
    const void *s = gp(r1, r2, "sceClibMemcpy");
    if (d && s) memmove(d, s, r2);       /* the firmware's copes with overlap; so does this */
}

/* void *sceClibMemmove(void *dst, const void *src, SceSize len) */
void vita_hle_sceClibMemmove(void) {
    void *d = gp(r0, r2, "sceClibMemmove");
    const void *s = gp(r1, r2, "sceClibMemmove");
    if (d && s) memmove(d, s, r2);
}

/* int sceClibMemcmp(const void *a, const void *b, SceSize len) */
void vita_hle_sceClibMemcmp(void) {
    const void *a = gp(r0, r2, "sceClibMemcmp");
    const void *b = gp(r1, r2, "sceClibMemcmp");
    r0 = (a && b) ? (uint32_t)memcmp(a, b, r2) : 0;
}

/* --- sceClibPrintf ---------------------------------------------------------------
 *
 * Variadic over the AAPCS: r1-r3 then the stack, each argument a word, a
 * 64-bit one aligned to an even register or eight bytes on the stack. The
 * format is interpreted here rather than handed to the host's printf,
 * because the arguments live in guest registers and guest memory and a %s
 * is a guest address.
 */

typedef struct { uint32_t next_reg; uint32_t sp_off; } vargs;

static uint32_t varg32(vargs *v) {
    if (v->next_reg <= 3) {
        uint32_t x = v->next_reg == 1 ? r1 : v->next_reg == 2 ? r2 : r3;
        v->next_reg++;
        return x;
    }
    uint32_t x = vita_read32(sp + v->sp_off);
    v->sp_off += 4;
    return x;
}

/* A 64-bit argument takes an even-aligned register pair. With r0 holding the
 * format, the only pair left is r2:r3; from r3 on, it goes to the stack,
 * eight-byte aligned, and no later argument returns to the registers. */
static uint64_t varg64(vargs *v) {
    if (v->next_reg <= 2) {
        v->next_reg = 4;
        return ((uint64_t)r3 << 32) | r2;
    }
    v->next_reg = 4;
    v->sp_off = (v->sp_off + 7) & ~7u;
    uint32_t lo = vita_read32(sp + v->sp_off), hi = vita_read32(sp + v->sp_off + 4);
    v->sp_off += 8;
    return ((uint64_t)hi << 32) | lo;
}

/* int sceClibPrintf(const char *fmt, ...) */
void vita_hle_sceClibPrintf(void) {
    uint32_t avail = vita_mem_avail(r0);
    const char *fmt = avail ? (const char *)vita_mem_ptr(r0, 1) : NULL;
    if (!fmt) { r0 = 0; return; }

    char out[1024];
    size_t o = 0;
    vargs v = { 1, 0 };

    for (uint32_t i = 0; i < avail && fmt[i] && o < sizeof(out) - 1; i++) {
        char c = fmt[i];
        if (c != '%') { out[o++] = c; continue; }

        /* flags, width, precision, length */
        char spec[32]; size_t s = 0;
        spec[s++] = '%';
        i++;
        while (i < avail && fmt[i] && strchr("-+ #0", fmt[i]) && s < 8) spec[s++] = fmt[i++];
        while (i < avail && fmt[i] >= '0' && fmt[i] <= '9' && s < 14) spec[s++] = fmt[i++];
        if (i < avail && fmt[i] == '.') {
            spec[s++] = fmt[i++];
            while (i < avail && fmt[i] >= '0' && fmt[i] <= '9' && s < 20) spec[s++] = fmt[i++];
        }
        int is64 = 0;
        if (i < avail && fmt[i] == 'l') { i++; if (i < avail && fmt[i] == 'l') { is64 = 1; i++; } }
        else if (i < avail && (fmt[i] == 'h' || fmt[i] == 'z')) { i++; }
        if (i >= avail || !fmt[i]) break;
        char conv = fmt[i];

        char piece[256];
        piece[0] = '\0';
        switch (conv) {
        case 'd': case 'i':
            if (is64) { spec[s++] = 'l'; spec[s++] = 'l'; spec[s++] = 'd'; spec[s] = 0;
                        snprintf(piece, sizeof(piece), spec, (long long)varg64(&v)); }
            else      { spec[s++] = 'd'; spec[s] = 0;
                        snprintf(piece, sizeof(piece), spec, (int)varg32(&v)); }
            break;
        case 'u': case 'x': case 'X': case 'o':
            if (is64) { spec[s++] = 'l'; spec[s++] = 'l'; spec[s++] = conv; spec[s] = 0;
                        snprintf(piece, sizeof(piece), spec, (unsigned long long)varg64(&v)); }
            else      { spec[s++] = conv; spec[s] = 0;
                        snprintf(piece, sizeof(piece), spec, (unsigned)varg32(&v)); }
            break;
        case 'p':
            snprintf(piece, sizeof(piece), "0x%08X", varg32(&v));
            break;
        case 'c':
            piece[0] = (char)varg32(&v); piece[1] = '\0';
            break;
        case 's': {
            uint32_t a = varg32(&v);
            uint32_t n = vita_mem_avail(a);
            const char *gs = n ? (const char *)vita_mem_ptr(a, 1) : NULL;
            if (!gs) { snprintf(piece, sizeof(piece), "(bad string 0x%08X)", a); break; }
            size_t len = 0;
            while (len < n && gs[len] && len < sizeof(piece) - 1) len++;
            spec[s++] = '.'; spec[s++] = '*'; spec[s++] = 's'; spec[s] = 0;
            snprintf(piece, sizeof(piece), spec, (int)len, gs);
            break;
        }
        case '%':
            piece[0] = '%'; piece[1] = '\0';
            break;
        default:
            /* %f and friends need the VFP argument convention; name it. */
            snprintf(piece, sizeof(piece), "%%%c", conv);
            break;
        }
        size_t pl = strlen(piece);
        if (o + pl >= sizeof(out)) pl = sizeof(out) - 1 - o;
        memcpy(out + o, piece, pl);
        o += pl;
    }
    out[o] = '\0';
    fputs(out, stdout);
    fflush(stdout);
    r0 = (uint32_t)o;
}
