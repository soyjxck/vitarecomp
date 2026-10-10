/* hle_clib.c — SceLibKernel's sceClib* family.
 *
 * The firmware's own fast memcpy, memset, memmove and printf, which vitasdk's
 * newlib forwards the C functions to. The semantics are the standard ones;
 * what is not standard is the boundary, exactly as in hle_libc.c: every
 * pointer is a guest address and every length is checked against what is
 * backed before a host routine is allowed near it.
 */

#include "vitarecomp/hle.h"
#include "vitarecomp/recomp_rt.h"
#include "hle_format.h"

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

/* --- the guest printf -------------------------------------------------------
 *
 * Variadic over the AAPCS: r1-r3 then the stack, each argument a word, a
 * 64-bit one aligned to an even register or eight bytes on the stack; or a
 * va_list, which is a guest pointer into that stack. Floating-point
 * arguments arrive in core registers too: the variadic part of a call uses
 * the base standard whatever the ABI, so a %f is a 64-bit word. The format
 * is interpreted here because the arguments live in guest memory and a %s
 * is a guest address.
 */

uint32_t vita_varg32(vita_vargs *v) {
    if (v->mem) { uint32_t x = vita_read32(v->mem); v->mem += 4; return x; }
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
static uint64_t varg64(vita_vargs *v) {
    if (v->mem) {
        v->mem = (v->mem + 7) & ~7u;
        uint32_t lo = vita_read32(v->mem), hi = vita_read32(v->mem + 4);
        v->mem += 8;
        return ((uint64_t)hi << 32) | lo;
    }
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

size_t vita_guest_vformat(char *out, size_t cap, uint32_t fmt_addr, vita_vargs *v) {
    uint32_t avail = vita_mem_avail(fmt_addr);
    const char *fmt = avail ? (const char *)vita_mem_ptr(fmt_addr, 1) : NULL;
    size_t o = 0, total = 0;
    if (cap) out[0] = '\0';
    if (!fmt) return 0;

    for (uint32_t i = 0; i < avail && fmt[i]; i++) {
        char c = fmt[i];
        char piece[512];
        if (c != '%') { piece[0] = c; piece[1] = '\0'; }
        else {
            /* flags, width, precision, length */
            char spec[32]; size_t s = 0;
            spec[s++] = '%';
            i++;
            while (i < avail && fmt[i] && strchr("-+ #0", fmt[i]) && s < 8) spec[s++] = fmt[i++];
            if (i < avail && fmt[i] == '*') { snprintf(spec + s, sizeof(spec) - s, "%d", (int)vita_varg32(v)); s = strlen(spec); i++; }
            while (i < avail && fmt[i] >= '0' && fmt[i] <= '9' && s < 14) spec[s++] = fmt[i++];
            if (i < avail && fmt[i] == '.') {
                spec[s++] = fmt[i++];
                if (i < avail && fmt[i] == '*') { snprintf(spec + s, sizeof(spec) - s, "%d", (int)vita_varg32(v)); s = strlen(spec); i++; }
                while (i < avail && fmt[i] >= '0' && fmt[i] <= '9' && s < 20) spec[s++] = fmt[i++];
            }
            int is64 = 0;
            if (i < avail && fmt[i] == 'l') { i++; if (i < avail && fmt[i] == 'l') { is64 = 1; i++; } }
            else if (i < avail && (fmt[i] == 'h' || fmt[i] == 'z' || fmt[i] == 'j' || fmt[i] == 't' || fmt[i] == 'L')) {
                i++; if (i < avail && fmt[i] == 'h') i++;
            }
            if (i >= avail || !fmt[i]) break;
            char conv = fmt[i];

            piece[0] = '\0';
            switch (conv) {
            case 'd': case 'i':
                if (is64) { spec[s++] = 'l'; spec[s++] = 'l'; spec[s++] = 'd'; spec[s] = 0;
                            snprintf(piece, sizeof(piece), spec, (long long)varg64(v)); }
                else      { spec[s++] = 'd'; spec[s] = 0;
                            snprintf(piece, sizeof(piece), spec, (int)vita_varg32(v)); }
                break;
            case 'u': case 'x': case 'X': case 'o':
                if (is64) { spec[s++] = 'l'; spec[s++] = 'l'; spec[s++] = conv; spec[s] = 0;
                            snprintf(piece, sizeof(piece), spec, (unsigned long long)varg64(v)); }
                else      { spec[s++] = conv; spec[s] = 0;
                            snprintf(piece, sizeof(piece), spec, (unsigned)vita_varg32(v)); }
                break;
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
                uint64_t bits = varg64(v);
                double d;
                memcpy(&d, &bits, sizeof(d));
                spec[s++] = conv; spec[s] = 0;
                snprintf(piece, sizeof(piece), spec, d);
                break;
            }
            case 'p':
                snprintf(piece, sizeof(piece), "0x%08X", vita_varg32(v));
                break;
            case 'c':
                piece[0] = (char)vita_varg32(v); piece[1] = '\0';
                break;
            case 's': {
                uint32_t a = vita_varg32(v);
                uint32_t n = vita_mem_avail(a);
                const char *gs = n ? (const char *)vita_mem_ptr(a, 1) : NULL;
                if (!gs) { snprintf(piece, sizeof(piece), a ? "(bad string 0x%08X)" : "(null)", a); break; }
                size_t len = 0;
                while (len < n && gs[len] && len < sizeof(piece) - 1) len++;
                spec[s++] = '.'; spec[s++] = '*'; spec[s++] = 's'; spec[s] = 0;
                snprintf(piece, sizeof(piece), spec, (int)len, gs);
                break;
            }
            case '%':
                piece[0] = '%'; piece[1] = '\0';
                break;
            case 'n':
                (void)vita_varg32(v);
                break;
            default:
                snprintf(piece, sizeof(piece), "%%%c", conv);
                break;
            }
        }
        size_t pl = strlen(piece);
        total += pl;
        if (cap && o + pl >= cap) pl = cap - 1 - o;
        if (pl) { memcpy(out + o, piece, pl); o += pl; }
    }
    if (cap) out[o] = '\0';
    return total;
}

/* The formatted result to the console, or to guest memory with a bound. */
static void format_to_console(uint32_t fmt, vita_vargs *v) {
    char out[2048];
    size_t n = vita_guest_vformat(out, sizeof(out), fmt, v);
    fputs(out, stdout);
    fflush(stdout);
    /* Echoed to the trace too, so the program's own words line up with the
     * calls around them. */
    size_t len = strlen(out);
    while (len && (out[len - 1] == 10 || out[len - 1] == 13)) out[--len] = 0;
    if (len) vita_hle_trace("printf at 0x%08X: %s", vita_last_pc, out);
    r0 = (uint32_t)n;
}

static void format_to_guest(uint32_t dst, uint32_t cap, uint32_t fmt, vita_vargs *v) {
    char out[4096];
    size_t n = vita_guest_vformat(out, sizeof(out), fmt, v);
    if (cap) {
        uint32_t take = (uint32_t)(n < cap - 1 ? n : cap - 1);
        if (take > sizeof(out) - 1) take = sizeof(out) - 1;
        void *d = gp(dst, take + 1, "snprintf dst");
        if (d) { memcpy(d, out, take); ((char *)d)[take] = '\0'; }
    }
    r0 = (uint32_t)n;
}

/* int sceClibPrintf(const char *fmt, ...) */
void vita_hle_sceClibPrintf(void) {
    vita_vargs v = { 1, 0, 0 };
    format_to_console(r0, &v);
}

/* int sceClibVprintf(const char *fmt, va_list ap) */
void vita_hle_sceClibVprintf(void) {
    vita_vargs v = { 0, 0, r1 };
    format_to_console(r0, &v);
}

/* int sceClibSnprintf(char *dst, SceSize n, const char *fmt, ...) */
void vita_hle_sceClibSnprintf(void) {
    vita_vargs v = { 3, 0, 0 };
    format_to_guest(r0, r1, r2, &v);
}

/* int sceClibVsnprintf(char *dst, SceSize n, const char *fmt, va_list ap) */
void vita_hle_sceClibVsnprintf(void) {
    vita_vargs v = { 0, 0, r3 };
    format_to_guest(r0, r1, r2, &v);
}
