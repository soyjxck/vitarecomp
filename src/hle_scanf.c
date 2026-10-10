/* hle_scanf.c — the guest scanf, for the strings a program parses in place.
 *
 * The same reasons as the formatter: the arguments are guest pointers to
 * write through, the input is guest memory, and the C library's scanf is the
 * standard's, so the standard's rules are implemented here rather than
 * guessed from the host's. A retail title reads every gesture's SVG and every
 * CSV of its input tables with sscanf, "%f,%f" and "%[^,]" among them. */

#include "vitarecomp/hle.h"
#include "vitarecomp/recomp_rt.h"
#include "hle_format.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *s;     /* the input, in host memory */
    uint32_t    base;  /* its guest address, for %n and the trace */
    size_t      n;     /* bytes available */
    size_t      i;     /* position */
} scan_in;

static int in_peek(const scan_in *in) { return in->i < in->n && in->s[in->i] ? (unsigned char)in->s[in->i] : -1; }

static void skip_space(scan_in *in) {
    while (in->i < in->n && in->s[in->i] && isspace((unsigned char)in->s[in->i])) in->i++;
}

/* Reads up to `width` characters of a number in `base` (0 for %i's own
 * choice, 10, 8, 16), or a floating-point number when base is -1, into buf. */
static size_t take_number(scan_in *in, size_t width, int base, char *buf, size_t cap) {
    size_t k = 0;
    size_t lim = width ? width : (size_t)-1;
    #define TAKE(cond) while (k < lim && k + 1 < cap && in->i < in->n && (cond)) { buf[k++] = in->s[in->i++]; }
    if (base == -1) {
        TAKE(in->s[in->i] == '+' || in->s[in->i] == '-');
        TAKE(isdigit((unsigned char)in->s[in->i]));
        TAKE(in->s[in->i] == '.');
        TAKE(isdigit((unsigned char)in->s[in->i]));
        if (k < lim && in->i < in->n && (in->s[in->i] == 'e' || in->s[in->i] == 'E')) {
            size_t save_k = k, save_i = in->i;
            buf[k++] = in->s[in->i++];
            TAKE(in->s[in->i] == '+' || in->s[in->i] == '-');
            size_t before = k;
            TAKE(isdigit((unsigned char)in->s[in->i]));
            if (k == before) { k = save_k; in->i = save_i; }
        }
    } else {
        TAKE(in->s[in->i] == '+' || in->s[in->i] == '-');
        if (base == 0 || base == 16) {
            if (k + 1 < lim && in->i + 1 < in->n && in->s[in->i] == '0' && (in->s[in->i + 1] == 'x' || in->s[in->i + 1] == 'X')) {
                buf[k++] = in->s[in->i++]; buf[k++] = in->s[in->i++];
                base = 16;
            } else if (base == 0 && in->i < in->n && in->s[in->i] == '0') base = 8;
            else if (base == 0) base = 10;
        }
        if (base == 16)      { TAKE(isxdigit((unsigned char)in->s[in->i])); }
        else if (base == 8)  { TAKE(in->s[in->i] >= '0' && in->s[in->i] <= '7'); }
        else                 { TAKE(isdigit((unsigned char)in->s[in->i])); }
    }
    #undef TAKE
    buf[k] = '\0';
    return k;
}

int vita_guest_vscanf(uint32_t str_addr, uint32_t fmt_addr, vita_vargs *v) {
    uint32_t favail = vita_mem_avail(fmt_addr), savail = vita_mem_avail(str_addr);
    const char *fmt = favail ? (const char *)vita_mem_ptr(fmt_addr, 1) : NULL;
    const char *src = savail ? (const char *)vita_mem_ptr(str_addr, 1) : NULL;
    if (!fmt || !src) return -1;
    scan_in in = { src, str_addr, savail, 0 };
    int assigned = 0, converted = 0;

    for (size_t f = 0; f < favail && fmt[f]; f++) {
        char c = fmt[f];
        if (isspace((unsigned char)c)) { skip_space(&in); continue; }
        if (c != '%') {
            if (in_peek(&in) != (unsigned char)c) break;
            in.i++;
            continue;
        }
        f++;
        if (f >= favail || !fmt[f]) break;
        int suppress = 0;
        size_t width = 0;
        int len = 0;     /* 'h', 'H' (hh), 'l', 'L' (ll) */
        if (fmt[f] == '*') { suppress = 1; f++; }
        while (f < favail && isdigit((unsigned char)fmt[f])) { width = width * 10 + (size_t)(fmt[f] - '0'); f++; }
        if (fmt[f] == 'h') { len = 'h'; f++; if (fmt[f] == 'h') { len = 'H'; f++; } }
        else if (fmt[f] == 'l') { len = 'l'; f++; if (fmt[f] == 'l') { len = 'L'; f++; } }
        else if (fmt[f] == 'L') { len = 'L'; f++; }
        char conv = fmt[f];
        if (conv == '%') { skip_space(&in); if (in_peek(&in) != '%') break; in.i++; continue; }
        if (conv == 'n') {
            if (!suppress) vita_write32(vita_varg32(v), (uint32_t)in.i);
            continue;
        }
        if (conv == 'c') {
            size_t w = width ? width : 1;
            if (in.i + w > in.n || !in.s[in.i]) break;
            uint32_t dst = suppress ? 0 : vita_varg32(v);
            for (size_t k = 0; k < w; k++) { if (dst) vita_write8(dst + (uint32_t)k, (uint8_t)in.s[in.i]); in.i++; }
            converted++;
            if (!suppress) assigned++;
            continue;
        }
        if (conv == 's' || conv == '[') {
            char set[256]; int negate = 0;
            memset(set, 0, sizeof(set));
            if (conv == '[') {
                f++;
                if (fmt[f] == '^') { negate = 1; f++; }
                if (fmt[f] == ']') { set[(unsigned char)']'] = 1; f++; }
                while (f < favail && fmt[f] && fmt[f] != ']') {
                    unsigned char a = (unsigned char)fmt[f];
                    if (fmt[f + 1] == '-' && fmt[f + 2] && fmt[f + 2] != ']') {
                        unsigned char b = (unsigned char)fmt[f + 2];
                        for (unsigned ch = a; ch <= b; ch++) set[ch] = 1;
                        f += 3;
                    } else { set[a] = 1; f++; }
                }
                if (fmt[f] != ']') break;
            } else {
                skip_space(&in);
            }
            uint32_t dst = suppress ? 0 : vita_varg32(v);
            size_t k = 0, lim = width ? width : (size_t)-1;
            while (k < lim && in.i < in.n && in.s[in.i]) {
                unsigned char ch = (unsigned char)in.s[in.i];
                int ok = conv == 's' ? !isspace(ch) : (set[ch] != 0) != negate;
                if (!ok) break;
                if (dst) vita_write8(dst + (uint32_t)k, ch);
                in.i++; k++;
            }
            if (k == 0) break;
            if (dst) vita_write8(dst + (uint32_t)k, 0);
            converted++;
            if (!suppress) assigned++;
            continue;
        }
        int base;
        switch (conv) {
        case 'd': case 'u': base = 10; break;
        case 'i':           base = 0;  break;
        case 'o':           base = 8;  break;
        case 'x': case 'X': case 'p': base = 16; break;
        case 'f': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': base = -1; break;
        default: goto done;                 /* an unknown conversion ends the scan */
        }
        skip_space(&in);
        if (in_peek(&in) < 0) break;
        char buf[128];
        size_t got = take_number(&in, width, base, buf, sizeof(buf));
        if (!got) break;
        converted++;
        if (suppress) continue;
        uint32_t dst = vita_varg32(v);
        if (base == -1) {
            double d = strtod(buf, NULL);
            if (len == 'l' || len == 'L') {
                union { double d; uint64_t u; } u; u.d = d;
                vita_write32(dst, (uint32_t)u.u);
                vita_write32(dst + 4, (uint32_t)(u.u >> 32));
            } else {
                union { float f; uint32_t u; } u; u.f = (float)d;
                vita_write32(dst, u.u);
            }
        } else {
            int b = base == 0 ? 0 : base;
            uint64_t val = conv == 'd' || conv == 'i'
                         ? (uint64_t)strtoll(buf, NULL, b) : strtoull(buf, NULL, b);
            if (len == 'H')      vita_write8(dst, (uint8_t)val);
            else if (len == 'h') vita_write16(dst, (uint16_t)val);
            else if (len == 'L') { vita_write32(dst, (uint32_t)val); vita_write32(dst + 4, (uint32_t)(val >> 32)); }
            else                 vita_write32(dst, (uint32_t)val);
        }
        assigned++;
    }
done:
    if (converted == 0 && in_peek(&in) < 0) return -1;
    return assigned;
}

static void scan_trace(uint32_t str, uint32_t fmt, int n) {
    char a[72], b[48];
    uint32_t av = vita_mem_avail(str), fv = vita_mem_avail(fmt);
    const char *s = av ? (const char *)vita_mem_ptr(str, 1) : "";
    const char *f = fv ? (const char *)vita_mem_ptr(fmt, 1) : "";
    size_t i = 0;
    for (; i < sizeof(a) - 1 && i < av && s[i] && s[i] != '\n'; i++) a[i] = s[i];
    a[i] = '\0';
    for (i = 0; i < sizeof(b) - 1 && i < fv && f[i]; i++) b[i] = f[i];
    b[i] = '\0';
    vita_hle_trace("sscanf(\"%s\", \"%s\") -> %d", a, b, n);
}

/* int sscanf(const char *s, const char *fmt, ...) */
void vita_hle_sscanf(void)  { vita_vargs v = { 2, 0, 0 };  int n = vita_guest_vscanf(r0, r1, &v); scan_trace(r0, r1, n); r0 = (uint32_t)n; }
/* int vsscanf(const char *s, const char *fmt, va_list ap) */
void vita_hle_vsscanf(void) { vita_vargs v = { 0, 0, r2 }; int n = vita_guest_vscanf(r0, r1, &v); scan_trace(r0, r1, n); r0 = (uint32_t)n; }
/* int fscanf(FILE *f, const char *fmt, ...): no stream here reads */
void vita_hle_fscanf(void)  { r0 = (uint32_t)-1; }
