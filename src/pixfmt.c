/* pixfmt.c — the console's pixel layouts, between bytes and colours. See pixfmt.h. */

#include "pixfmt.h"
#include "usse.h"
#include "vitarecomp/half.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* --- building a description ------------------------------------------------------ */

static void field(pixfmt *p, int shift, int bits, int kind) {
    p->f[p->fields].shift = (uint8_t)shift;
    p->f[p->fields].bits = (uint8_t)bits;
    p->f[p->fields].kind = (uint8_t)kind;
    p->fields++;
}

/* `n` fields of the same width and kind, the first in the lowest bits. */
static void even(pixfmt *p, int n, int bits, int kind) {
    for (int i = 0; i < n; i++) field(p, i * bits, bits, kind);
    p->bytes = (uint8_t)(n * bits / 8);
}

static void map(pixfmt *p, int r, int g, int b, int a) {
    p->from[0] = (int8_t)r; p->from[1] = (int8_t)g; p->from[2] = (int8_t)b; p->from[3] = (int8_t)a;
}

#define Z PIX_ZERO
#define O PIX_ONE

/* The channel orders of a texture, by how many fields the format has. A
 * name lists alpha, blue, green, red from the top bits down, so "ABGR" has
 * red in the lowest field. */
static void four(pixfmt *p, unsigned code) {
    static const int8_t M[8][4] = { { 0, 1, 2, 3 }, { 2, 1, 0, 3 }, { 3, 2, 1, 0 }, { 1, 2, 3, 0 },
                                    { 0, 1, 2, O }, { 2, 1, 0, O }, { 3, 2, 1, O }, { 1, 2, 3, O } };
    map(p, M[code & 7][0], M[code & 7][1], M[code & 7][2], M[code & 7][3]);
    p->components = 4;
}
static void three(pixfmt *p, unsigned code) {
    if (code & 1) map(p, 2, 1, 0, O); else map(p, 0, 1, 2, O);
    p->components = 4;
}
static void two(pixfmt *p, unsigned code) {
    static const int8_t M[8][4] = { { 0, 1, Z, O }, { 0, 1, Z, Z }, { 0, 0, 0, 1 }, { 1, 1, 1, 0 },
                                    { 0, 1, 0, 1 }, { 1, 0, Z, Z }, { 0, 1, Z, O }, { 0, 1, Z, O } };
    map(p, M[code & 7][0], M[code & 7][1], M[code & 7][2], M[code & 7][3]);
    p->components = (code & 7) ? 4 : 2;
}
static void one(pixfmt *p, unsigned code) {
    static const int8_t M[8][4] = { { 0, Z, Z, O }, { 0, Z, Z, Z }, { 0, O, O, O }, { 0, 0, 0, 0 },
                                    { 0, 0, 0, Z }, { 0, 0, 0, O }, { Z, Z, Z, 0 }, { O, O, O, 0 } };
    map(p, M[code & 7][0], M[code & 7][1], M[code & 7][2], M[code & 7][3]);
    p->components = (code & 7) ? 4 : 1;
}

int pixfmt_texture(uint32_t format, pixfmt *p) {
    unsigned code = (format >> 12) & 7;
    int small_low = code == 2 || code == 3 || code == 6 || code == 7;   /* the odd field is the lowest */
    memset(p, 0, sizeof(*p));
    p->type = USSE_U8;
    switch (format & 0x9F000000u) {
    case 0x00000000u: even(p, 1, 8, PIX_UNORM); one(p, code); break;
    case 0x01000000u: even(p, 1, 8, PIX_SNORM); one(p, code); p->type = USSE_S8; break;
    case 0x02000000u: even(p, 4, 4, PIX_UNORM); four(p, code); break;
    case 0x03000000u:                               /* alpha 8, red 3, green 3, blue 2 */
        field(p, 0, 2, PIX_UNORM); field(p, 2, 3, PIX_UNORM); field(p, 5, 3, PIX_UNORM); field(p, 8, 8, PIX_UNORM);
        p->bytes = 2; map(p, 2, 1, 0, 3); p->components = 4;
        break;
    case 0x04000000u:
        if (small_low) { field(p, 0, 1, PIX_UNORM); field(p, 1, 5, PIX_UNORM); field(p, 6, 5, PIX_UNORM); field(p, 11, 5, PIX_UNORM); }
        else           { field(p, 0, 5, PIX_UNORM); field(p, 5, 5, PIX_UNORM); field(p, 10, 5, PIX_UNORM); field(p, 15, 1, PIX_UNORM); }
        p->bytes = 2; four(p, code);
        break;
    case 0x05000000u:
        field(p, 0, 5, PIX_UNORM); field(p, 5, 6, PIX_UNORM); field(p, 11, 5, PIX_UNORM);
        p->bytes = 2; three(p, code);
        break;
    case 0x06000000u:                               /* two signed fives and an unsigned six, which is blue */
        if (code & 1) { field(p, 0, 6, PIX_UNORM); field(p, 6, 5, PIX_SNORM); field(p, 11, 5, PIX_SNORM); map(p, 2, 1, 0, O); }
        else          { field(p, 0, 5, PIX_SNORM); field(p, 5, 5, PIX_SNORM); field(p, 10, 6, PIX_UNORM); map(p, 0, 1, 2, O); }
        p->bytes = 2; p->components = 4; p->type = USSE_S8;
        break;
    case 0x07000000u: even(p, 2, 8, PIX_UNORM); two(p, code); break;
    case 0x08000000u: even(p, 2, 8, PIX_SNORM); two(p, code); p->type = USSE_S8; break;
    case 0x09000000u: even(p, 1, 16, PIX_UNORM); one(p, code); p->type = USSE_U16; break;
    case 0x0A000000u: even(p, 1, 16, PIX_SNORM); one(p, code); p->type = USSE_S16; break;
    case 0x0B000000u: even(p, 1, 16, PIX_F16); one(p, code); p->type = USSE_F16; break;
    case 0x0C000000u: even(p, 4, 8, PIX_UNORM); four(p, code); break;
    case 0x0D000000u: even(p, 4, 8, PIX_SNORM); four(p, code); p->type = USSE_S8; break;
    case 0x0E000000u:
    case 0x9A000000u: {
        int kind = (format & 0x9F000000u) == 0x9A000000u ? PIX_UF10 : PIX_UNORM;
        if (small_low) { field(p, 0, 2, PIX_UNORM); field(p, 2, 10, kind); field(p, 12, 10, kind); field(p, 22, 10, kind); }
        else           { field(p, 0, 10, kind); field(p, 10, 10, kind); field(p, 20, 10, kind); field(p, 30, 2, PIX_UNORM); }
        p->bytes = 4; four(p, code);
        p->type = kind == PIX_UF10 ? USSE_F16 : USSE_U16;
        break;
    }
    case 0x0F000000u: even(p, 2, 16, PIX_UNORM); two(p, code); p->type = USSE_U16; break;
    case 0x10000000u: even(p, 2, 16, PIX_SNORM); two(p, code); p->type = USSE_S16; break;
    case 0x11000000u: even(p, 2, 16, PIX_F16); two(p, code); p->type = USSE_F16; break;
    case 0x12000000u: even(p, 1, 32, PIX_F32); one(p, code); p->type = USSE_F32; break;
    case 0x13000000u: even(p, 1, 32, PIX_F32M); one(p, code); p->type = USSE_F32; break;
    case 0x14000000u:                               /* red unsigned, green and blue signed, the top byte unused */
        if (code & 1) { field(p, 0, 8, PIX_SNORM); field(p, 8, 8, PIX_SNORM); field(p, 16, 8, PIX_UNORM); map(p, 2, 1, 0, O); }
        else          { field(p, 0, 8, PIX_UNORM); field(p, 8, 8, PIX_SNORM); field(p, 16, 8, PIX_SNORM); map(p, 0, 1, 2, O); }
        p->bytes = 4; p->components = 4; p->type = USSE_S8;
        break;
    case 0x15000000u:                               /* 24 bits of depth beside 8 of something else */
        field(p, (code & 1) ? 8 : 0, 24, PIX_UNORM);
        p->bytes = 4; map(p, 0, Z, Z, O); p->components = 1; p->type = USSE_F32;
        break;
    case 0x17000000u:
    case 0x18000000u: even(p, 1, 32, PIX_RAW); one(p, code); p->raw = 1; p->type = USSE_F32; break;
    case 0x19000000u:
        field(p, 0, 9, PIX_SE9); field(p, 9, 9, PIX_SE9); field(p, 18, 9, PIX_SE9);
        p->bytes = 4; three(p, code); p->type = USSE_F16;
        break;
    case 0x1A000000u:
        if (code & 1) { field(p, 0, 10, PIX_UF10); field(p, 10, 11, PIX_UF11); field(p, 21, 11, PIX_UF11); map(p, 2, 1, 0, O); }
        else          { field(p, 0, 11, PIX_UF11); field(p, 11, 11, PIX_UF11); field(p, 22, 10, PIX_UF10); map(p, 0, 1, 2, O); }
        p->bytes = 4; p->components = 4; p->type = USSE_F16;
        break;
    case 0x1B000000u: even(p, 4, 16, PIX_F16); four(p, code); p->type = USSE_F16; break;
    case 0x1C000000u: even(p, 4, 16, PIX_UNORM); four(p, code); p->type = USSE_U16; break;
    case 0x1D000000u: even(p, 4, 16, PIX_SNORM); four(p, code); p->type = USSE_S16; break;
    case 0x1E000000u: even(p, 2, 32, PIX_F32); two(p, code); p->type = USSE_F32; break;
    case 0x1F000000u: even(p, 2, 32, PIX_RAW); two(p, code); p->raw = 1; p->type = USSE_F32; break;
    case 0x98000000u: even(p, 3, 8, PIX_UNORM); three(p, code); break;
    case 0x99000000u: even(p, 3, 8, PIX_SNORM); three(p, code); p->type = USSE_S8; break;
    default: return 0;
    }
    return 1;
}

/* A colour surface's formats are the same layouts under other numbers, and
 * with their own channel orders where there are one or two channels. */
static const struct { uint32_t colour, texture; uint8_t family; } COLOURS[] = {
    { 0x00000000u, 0x0C000000u, 4 }, { 0x10000000u, 0x98000000u, 3 }, { 0x30000000u, 0x05000000u, 3 },
    { 0x40000000u, 0x04000000u, 4 }, { 0x50000000u, 0x02000000u, 4 }, { 0x60000000u, 0x03000000u, 0 },
    { 0xF0000000u, 0x0B000000u, 1 }, { 0x00800000u, 0x11000000u, 2 }, { 0x10800000u, 0x12000000u, 1 },
    { 0x20800000u, 0x0A000000u, 1 }, { 0x30800000u, 0x10000000u, 2 }, { 0x40800000u, 0x09000000u, 1 },
    { 0x50800000u, 0x0F000000u, 2 }, { 0x60800000u, 0x0E000000u, 4 }, { 0x80800000u, 0x00000000u, 1 },
    { 0x90800000u, 0x01000000u, 1 }, { 0xA0800000u, 0x06000000u, 3 }, { 0xB0800000u, 0x07000000u, 2 },
    { 0xC0800000u, 0x08000000u, 2 }, { 0xE0800000u, 0x0D000000u, 4 }, { 0x01000000u, 0x1B000000u, 4 },
    { 0x11000000u, 0x1E000000u, 2 }, { 0x21000000u, 0x1A000000u, 3 }, { 0x31000000u, 0x19000000u, 3 },
    { 0x41000000u, 0x9A000000u, 4 },
};
#define COLOUR_COUNT (sizeof(COLOURS) / sizeof(COLOURS[0]))

uint32_t pixfmt_colour_texture(uint32_t format) {
    uint32_t base = format & 0xF1800000u;
    for (size_t i = 0; i < COLOUR_COUNT; i++)
        if (COLOURS[i].colour == base)
            return COLOURS[i].texture | (COLOURS[i].family >= 3 ? ((format >> 20) & 3) << 12 : 0u);
    return 0x0C000000u;
}

int pixfmt_colour(uint32_t format, pixfmt *p) {
    uint32_t base = format & 0xF1800000u;
    unsigned code = (format >> 20) & 3;
    if (base == 0xD0800000u) {                      /* unsigned, signed, signed, unsigned */
        memset(p, 0, sizeof(*p));
        field(p, 0, 8, PIX_UNORM); field(p, 8, 8, PIX_SNORM); field(p, 16, 8, PIX_SNORM); field(p, 24, 8, PIX_UNORM);
        p->bytes = 4; four(p, code); p->type = USSE_S8;
        return 1;
    }
    for (size_t i = 0; i < COLOUR_COUNT; i++) {
        if (COLOURS[i].colour != base) continue;
        int family = COLOURS[i].family;
        if (!pixfmt_texture(COLOURS[i].texture | ((family >= 3 ? code : 0u) << 12), p)) return 0;
        if (family == 2) {
            /* "GR" keeps red first; "RA" and "AR" are red beside alpha */
            if (code == 0) map(p, 0, 1, Z, O); else if (code == 1) map(p, 1, 0, Z, O);
            else if (code == 2) map(p, 1, Z, Z, 0); else map(p, 0, Z, Z, 1);
        } else if (family == 1) {
            if (code == 0) map(p, 0, Z, Z, O); else map(p, Z, Z, Z, 0);
        }
        return 1;
    }
    return 0;
}

#undef Z
#undef O

/* --- the small floats -------------------------------------------------------------- */

static float bits_to_float(uint32_t u) { float f; memcpy(&f, &u, sizeof(f)); return f; }
static uint32_t float_to_bits(float f) { uint32_t u; memcpy(&u, &f, sizeof(u)); return u; }

/* An unsigned float: five bits of exponent above `mb` bits of mantissa. */
static float small_to_float(uint32_t x, int mb) {
    uint32_t e = x >> mb, m = x & ((1u << mb) - 1);
    if (e == 0) return m ? ldexpf((float)m, -14 - mb) : 0.0f;
    if (e == 31) return m ? NAN : INFINITY;
    return ldexpf((float)((1u << mb) + m), (int)e - 15 - mb);
}

static uint32_t float_to_small(float v, int mb) {
    uint32_t top = (30u << mb) | ((1u << mb) - 1);          /* the largest finite value */
    if (v != v) return (31u << mb) | 1u;
    if (v <= 0.0f) return 0;
    if (v > small_to_float(top, mb)) return top;
    int ex;
    float fr = frexpf(v, &ex);                              /* v = fr * 2^ex, fr in [0.5, 1) */
    int e = ex - 1 + 15;
    if (e <= 0) {
        uint32_t m = (uint32_t)lroundf(ldexpf(v, 14 + mb));
        return m >= (1u << mb) ? (1u << mb) : m;
    }
    uint32_t m = (uint32_t)lroundf((fr * 2.0f - 1.0f) * (float)(1u << mb));
    if (m >= (1u << mb)) { m = 0; e++; }
    if (e >= 31) return top;
    return ((uint32_t)e << mb) | m;
}

float pix_srgb_to_linear(float v) {
    if (!(v > 0.0f)) return 0.0f;
    if (v >= 1.0f) return 1.0f;
    return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

float pix_linear_to_srgb(float v) {
    if (!(v > 0.0f)) return 0.0f;
    if (v >= 1.0f) return 1.0f;
    return v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

/* --- converting -------------------------------------------------------------------- */

static uint64_t field_mask(int bits) { return bits >= 64 ? ~0ull : (1ull << bits) - 1; }

/* What one field is worth, worked out. */
static float field_value(unsigned kind, int n, uint32_t x, uint64_t bits) {
    switch (kind) {
    case PIX_UNORM: return (float)x / (float)field_mask(n);
    case PIX_SNORM: {
        int32_t s = (int32_t)(x << (32 - n)) >> (32 - n);
        float t = (float)s / (float)((1u << (n - 1)) - 1);
        return t < -1.0f ? -1.0f : t;
    }
    case PIX_RAW:  return bits_to_float(x);
    case PIX_F16:  return vita_f16_to_f32((uint16_t)x);
    case PIX_F32:  return bits_to_float(x);
    case PIX_F32M: return bits_to_float(x & 0x7FFFFFFFu);
    case PIX_UF11: return small_to_float(x, 6);
    case PIX_UF10: return small_to_float(x, 5);
    default:       return ldexpf((float)x, (int)((bits >> 27) & 31) - 24);                 /* the shared exponent */
    }
}

/* A field of few bits has few values, and what most of them cost is a
 * division or a scaling by a power of two, done again for every texel a
 * frame reads: nine million times a frame, in a title's cinematic. So the
 * values are worked out once, by the arithmetic above, and looked up
 * after: every fraction of up to eight bits, the signed bytes, every half,
 * and the two small unsigned floats. What a table gives is what the
 * arithmetic gave. */
int pixfmt_plain;
static float g_unorm[9][256], g_snorm8[256], g_half[65536], g_uf11[2048], g_uf10[1024];
static int g_tabled;

static void tables(void) {
    /* Safe from several threads at once: each writes the same values, and
     * none reads before it has seen the mark that all of them are written. */
    if (g_tabled) return;
    for (int n = 1; n <= 8; n++)
        for (uint32_t x = 0; x < (1u << n); x++) g_unorm[n][x] = field_value(PIX_UNORM, n, x, 0);
    for (uint32_t x = 0; x < 256; x++) g_snorm8[x] = field_value(PIX_SNORM, 8, x, 0);
    for (uint32_t x = 0; x < 65536; x++) g_half[x] = field_value(PIX_F16, 16, x, 0);
    for (uint32_t x = 0; x < 2048; x++) g_uf11[x] = field_value(PIX_UF11, 11, x, 0);
    for (uint32_t x = 0; x < 1024; x++) g_uf10[x] = field_value(PIX_UF10, 10, x, 0);
    g_tabled = 1;
}

const float *pixfmt_table(unsigned kind, unsigned bits) {
    if (!g_tabled) tables();
    if (kind == PIX_UNORM && bits >= 1 && bits <= 8) return g_unorm[bits];
    if (kind == PIX_F16 && bits == 16) return g_half;
    if (kind == PIX_SNORM && bits == 8) return g_snorm8;
    if (kind == PIX_UF10 && bits == 10) return g_uf10;
    if (kind == PIX_UF11 && bits == 11) return g_uf11;
    return NULL;
}

void pixfmt_decode(const pixfmt *f, const uint8_t *pixel, float rgba[4]) {
    uint64_t bits = 0;
    float v[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    memcpy(&bits, pixel, f->bytes);
    if (!g_tabled) tables();
    for (int i = 0; i < f->fields; i++) {
        int n = f->f[i].bits;
        unsigned kind = f->f[i].kind;
        uint32_t x = (uint32_t)((bits >> f->f[i].shift) & field_mask(n));
        if (pixfmt_plain)                          v[i] = field_value(kind, n, x, bits);
        else if (kind == PIX_UNORM && n <= 8)      v[i] = g_unorm[n][x];
        else if (kind == PIX_F16)                  v[i] = g_half[x];
        else if (kind == PIX_SNORM && n == 8)      v[i] = g_snorm8[x];
        else if (kind == PIX_UF10)                 v[i] = g_uf10[x];
        else if (kind == PIX_UF11)                 v[i] = g_uf11[x];
        else                                       v[i] = field_value(kind, n, x, bits);
    }
    for (int c = 0; c < 4; c++)
        rgba[c] = f->from[c] == PIX_ZERO ? 0.0f : f->from[c] == PIX_ONE ? 1.0f : v[f->from[c]];
}

/* Three mantissas under one exponent: the exponent is the largest
 * channel's, and the others lose what precision that costs them. */
static uint64_t shared_exponent(const float c[3]) {
    const float top = 511.0f * 128.0f;                      /* (2^9 - 1) * 2^(31 - 24) */
    float v[3], most = 0.0f;
    for (int i = 0; i < 3; i++) {
        v[i] = !(c[i] > 0.0f) ? 0.0f : c[i] > top ? top : c[i];
        if (v[i] > most) most = v[i];
    }
    int e = 0;
    if (most > 0.0f) {
        int ex;
        frexpf(most, &ex);                                  /* most < 2^ex */
        e = ex + 15;                                        /* so that most / 2^(e - 24) < 512 */
        if (e < 0) e = 0;
        if (lroundf(ldexpf(most, 24 - e)) >= 512) e++;
        if (e > 31) e = 31;
    }
    uint64_t out = (uint64_t)e << 27;
    for (int i = 0; i < 3; i++) {
        long m = lroundf(ldexpf(v[i], 24 - e));
        out |= (uint64_t)(m > 511 ? 511 : m) << (9 * i);
    }
    return out;
}

void pixfmt_encode(const pixfmt *f, uint8_t *pixel, const float rgba[4], unsigned channels) {
    uint64_t bits = 0;
    memcpy(&bits, pixel, f->bytes);

    if (f->fields == 3 && f->f[0].kind == PIX_SE9) {
        float now[4], c[3];
        pixfmt_decode(f, pixel, now);
        for (int ch = 0; ch < 3; ch++) if ((channels >> ch) & 1) now[ch] = rgba[ch];
        for (int ch = 0; ch < 3; ch++) if (f->from[ch] >= 0) c[f->from[ch]] = now[ch];
        bits = shared_exponent(c);
        memcpy(pixel, &bits, f->bytes);
        return;
    }

    for (int i = 0; i < f->fields; i++) {
        int n = f->f[i].bits, ch = -1;
        for (int c = 0; c < 4; c++) if (f->from[c] == i && ((channels >> c) & 1)) { ch = c; break; }
        if (ch < 0) continue;
        float v = rgba[ch];
        uint64_t x;
        switch (f->f[i].kind) {
        case PIX_UNORM:
            v = !(v > 0.0f) ? 0.0f : v > 1.0f ? 1.0f : v;
            x = (uint64_t)llroundf(v * (float)field_mask(n));
            break;
        case PIX_SNORM:
            v = v != v ? 0.0f : v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
            x = (uint64_t)(int64_t)lroundf(v * (float)((1u << (n - 1)) - 1));
            break;
        case PIX_F16:  x = vita_f32_to_f16(v); break;
        case PIX_F32M: x = (float_to_bits(v) & 0x7FFFFFFFu) | ((bits >> f->f[i].shift) & 0x80000000u); break;
        case PIX_UF11: x = float_to_small(v, 6); break;
        case PIX_UF10: x = float_to_small(v, 5); break;
        default:       x = float_to_bits(v); break;
        }
        bits = (bits & ~(field_mask(n) << f->f[i].shift)) | ((x & field_mask(n)) << f->f[i].shift);
    }
    memcpy(pixel, &bits, f->bytes);
}

/* --- a pixel from four halves, by lookup (pixfmt.h) ---------------------------------------- */

static pixfmt_halves g_halves[32];
static unsigned g_halves_count;

const pixfmt_halves *pixfmt_halves_for(const pixfmt *f, int on_curve, int clamped) {
    on_curve = on_curve != 0; clamped = clamped != 0;
    if (f->bytes > 8) return NULL;
    for (int i = 0; i < f->fields; i++) {
        unsigned kind = f->f[i].kind;
        if (f->f[i].bits > 16 || (kind != PIX_UNORM && kind != PIX_SNORM && kind != PIX_F16 && kind != PIX_UF10 && kind != PIX_UF11))
            return NULL;
    }
    for (unsigned i = 0; i < g_halves_count; i++)
        if (!memcmp(&g_halves[i].format, f, sizeof(*f)) && g_halves[i].on_curve == on_curve && g_halves[i].clamped == clamped)
            return &g_halves[i];
    if (g_halves_count == sizeof(g_halves) / sizeof(g_halves[0])) return NULL;

    pixfmt_halves *t = &g_halves[g_halves_count];
    uint16_t cut[4] = { 0, 0, 0, 0 };
    uint64_t taken = 0;
    memset(t, 0, sizeof(*t));
    t->format = *f;
    t->on_curve = (uint8_t)on_curve; t->clamped = (uint8_t)clamped; t->bytes = f->bytes;
    for (int i = 0; i < f->fields; i++) {
        int ch = -1;
        for (int c = 0; c < 4; c++) if (f->from[c] == i) { ch = c; break; }       /* the channel pixfmt_encode gives the field */
        if (ch < 0) continue;
        unsigned k = t->fields++;
        t->channel[k] = (uint8_t)ch;
        t->shift[k] = f->f[i].shift;
        cut[k] = (uint16_t)field_mask(f->f[i].bits);
        taken |= field_mask(f->f[i].bits) << f->f[i].shift;
        t->becomes[k] = malloc(65536 * sizeof(uint16_t));
        if (!t->becomes[k]) { while (t->fields) free(t->becomes[--t->fields]); return NULL; }
    }
    t->keep = field_mask(8 * f->bytes) & ~taken;
    /* every half the long way, off the curve and on it, into a pixel of nothing */
    for (uint32_t h = 0; h < 65536; h++) {
        float v = vita_f16_to_f32((uint16_t)h), straight[4], bent[4];
        uint64_t plain = 0, curved = 0;
        if (clamped) v = !(v > 0.0f) ? 0.0f : v > 1.0f ? 1.0f : v;
        for (int c = 0; c < 4; c++) { straight[c] = v; bent[c] = pix_linear_to_srgb(v); }
        pixfmt_encode(f, (uint8_t *)&plain, straight, 15);
        pixfmt_encode(f, (uint8_t *)&curved, bent, 15);
        for (unsigned k = 0; k < t->fields; k++)
            t->becomes[k][h] = (uint16_t)(((on_curve && t->channel[k] < 3 ? curved : plain) >> t->shift[k]) & cut[k]);
    }
    g_halves_count++;
    return t;
}
