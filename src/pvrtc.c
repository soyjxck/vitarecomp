/* pvrtc.c — PowerVR's texture compression, both generations. See texdec.h.
 *
 * A PVRTC image is a grid of 64-bit words. Each word holds two colours and
 * the modulation of its own block of texels (four by four, or eight by four
 * at two bits a texel). The colours are not the block's: they are samples of
 * two low-resolution images, taken at the block's centre, and between the
 * samples the two images are interpolated, so a texel's two colours come
 * from the four words around it and the edges of the image wrap around to
 * each other. The modulation then says how far from the first colour to the
 * second each texel lies. The second generation keeps all of that and adds a
 * flag that turns the interpolation off around a corner where four words
 * meet, and a palette mode.
 *
 * Written from the Khronos Data Format Specification's chapter on the
 * format, and checked texel for texel against what Imagination's own codec
 * gives for the same words.
 */

#include "texdec.h"

#include <stddef.h>

typedef struct {
    uint32_t mod;                   /* the modulation data */
    uint8_t  m, h;                  /* the modulation flag; the hard transition flag */
    uint8_t  a5[4], b5[4];          /* the two colours: five bits a channel, four of alpha */
    uint8_t  a8[4], b8[4];          /* the same, eight bits each */
} pvr_word;

static const uint8_t WEIGHT[4] = { 0, 3, 5, 8 }, PUNCH[4] = { 0, 4, 4, 8 };

static void widen(const uint8_t c5[4], uint8_t c8[4]) {
    for (int i = 0; i < 3; i++) c8[i] = (uint8_t)((c5[i] << 3) | (c5[i] >> 2));
    c8[3] = (uint8_t)((c5[3] << 4) | c5[3]);
}

static void unpack(const uint8_t *p, int version, pvr_word *w) {
    uint32_t col = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    w->mod = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    w->m = col & 1;
    /* the first generation has an opacity flag for each colour; the second
     * one for both, and uses the other's place for the hard transition */
    uint32_t opaque_b = col >> 31, opaque_a = version == 2 ? opaque_b : (col >> 15) & 1;
    w->h = version == 2 ? (col >> 15) & 1 : 0;

    if (opaque_a) {
        uint32_t b = (col >> 1) & 15;
        w->a5[0] = (col >> 10) & 31; w->a5[1] = (col >> 5) & 31; w->a5[2] = (uint8_t)((b << 1) | (b >> 3)); w->a5[3] = 15;
    } else {
        uint32_t r = (col >> 8) & 15, g = (col >> 4) & 15, b = (col >> 1) & 7;
        w->a5[0] = (uint8_t)((r << 1) | (r >> 3)); w->a5[1] = (uint8_t)((g << 1) | (g >> 3));
        w->a5[2] = (uint8_t)((b << 2) | (b >> 1)); w->a5[3] = (uint8_t)(((col >> 12) & 7) << 1);
    }
    if (opaque_b) {
        w->b5[0] = (col >> 26) & 31; w->b5[1] = (col >> 21) & 31; w->b5[2] = (col >> 16) & 31; w->b5[3] = 15;
    } else {
        uint32_t r = (col >> 24) & 15, g = (col >> 20) & 15, b = (col >> 16) & 15;
        w->b5[0] = (uint8_t)((r << 1) | (r >> 3)); w->b5[1] = (uint8_t)((g << 1) | (g >> 3));
        w->b5[2] = (uint8_t)((b << 1) | (b >> 3));
        /* the second generation's second colour is never quite transparent */
        w->b5[3] = (uint8_t)((((col >> 28) & 7) << 1) | (version == 2 ? 1u : 0u));
    }
    widen(w->a5, w->a8);
    widen(w->b5, w->b8);
}

/* --- two bits a texel: the weights ------------------------------------------------------
 * A word either gives every texel one bit, or gives every other texel two
 * bits in a checkerboard and leaves the rest to be the average of their
 * neighbours: all four, or only those beside, or only those above and
 * below. Two of the stored texels give up a bit to say which. */

/* The weight a word stores for one of its own texels: any texel of a word
 * in the one-bit mode, a texel on the checkerboard otherwise. */
static int stored(const pvr_word *w, unsigned xo, unsigned yo) {
    if (!w->m) return ((w->mod >> (8 * yo + xo)) & 1) ? 8 : 0;
    unsigned k = 4 * yo + (xo >> 1);
    if (k == 0) return (w->mod & 2) ? 8 : 0;
    if (k == 10 && (w->mod & 1)) return ((w->mod >> 21) & 1) ? 8 : 0;
    return WEIGHT[(w->mod >> (2 * k)) & 3];
}

/* The nine words around a block, the block's own in the middle, and the
 * weight of a texel anywhere in them by its offset from the middle block's
 * corner. */
static int around(const pvr_word n[3][3], int x, int y) {
    const pvr_word *w = &n[(y + 4) / 4][(x + 8) / 8];
    return stored(w, (unsigned)(x + 8) & 7, (unsigned)(y + 4) & 3);
}

static int weight2(const pvr_word n[3][3], int xo, int yo) {
    const pvr_word *own = &n[1][1];
    if (!own->m || !((xo + yo) & 1)) return stored(own, (unsigned)xo, (unsigned)yo);
    if (!(own->mod & 1))
        return (around(n, xo - 1, yo) + around(n, xo + 1, yo) + around(n, xo, yo - 1) + around(n, xo, yo + 1) + 2) >> 2;
    if (!((own->mod >> 20) & 1)) return (around(n, xo - 1, yo) + around(n, xo + 1, yo) + 1) >> 1;
    return (around(n, xo, yo - 1) + around(n, xo, yo + 1) + 1) >> 1;
}

/* --- the second generation's palette ------------------------------------------------------
 * Around a hard corner, a texel of a word in the palette mode picks one of
 * four of the eight colours the four words have; which four depends on
 * where in the region it is. Each entry: the word (P, Q, R, S as 0..3), and
 * its second colour or its first. */
#define E(word, second) (uint8_t)((word) << 1 | (second))
#define PA E(0, 0)
#define PB E(0, 1)
#define QA E(1, 0)
#define QB E(1, 1)
#define RA E(2, 0)
#define RB E(2, 1)
#define SA E(3, 0)
#define SB E(3, 1)
static const uint8_t PALETTE[4][4][4] = {
    { { PA, PB, PA, PB }, { PA, PB, QA, QB }, { PA, PB, QA, QB }, { PA, PB, QA, QB } },
    { { PA, PB, RA, RB }, { PA, PB, QA, RB }, { PA, PB, QA, QB }, { SA, PB, QA, QB } },
    { { PA, PB, RA, RB }, { PA, PB, RA, RB }, { PA, SB, RA, QB }, { SA, SB, QA, QB } },
    { { PA, PB, RA, RB }, { PA, SB, RA, RB }, { SA, SB, RA, RB }, { SA, SB, RA, QB } },
};

void tex_decode_pvrtc(const uint8_t *words, uint32_t nx, uint32_t ny, uint32_t bx, uint32_t by,
                      int version, int bits, uint32_t out[32]) {
    int bw = bits == 2 ? 8 : 4;
    pvr_word n[3][3];
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) {
            uint32_t x = (bx + nx + (uint32_t)i - 1) % nx, y = (by + ny + (uint32_t)j - 1) % ny;
            unpack(words + 8 * (size_t)tex_morton_index(x, y, nx, ny), version, &n[j][i]);
        }
    const pvr_word *own = &n[1][1];

    for (int yo = 0; yo < 4; yo++)
        for (int xo = 0; xo < bw; xo++) {
            /* the four words whose colour samples surround this texel, and
             * where it lies between them */
            int left = xo < bw / 2, up = yo < 2;
            int xr = left ? xo + bw / 2 : xo - bw / 2, yr = up ? yo + 2 : yo - 2;
            const pvr_word *q[4] = { &n[up ? 0 : 1][left ? 0 : 1], &n[up ? 0 : 1][left ? 1 : 2],
                                     &n[up ? 1 : 2][left ? 0 : 1], &n[up ? 1 : 2][left ? 1 : 2] };
            int hard = q[0]->h, punch = 0, w;
            unsigned code = 0;
            uint8_t a[4], b[4], c[4];
            if (bits == 4) {
                code = (own->mod >> (2 * (4 * yo + xo))) & 3;
                punch = own->m && code == 2;
                w = own->m ? PUNCH[code] : WEIGHT[code];
            } else {
                w = weight2(n, xo, yo);
            }

            if (hard && bits == 4 && own->m) {
                /* the palette; on the corner itself, a blend of the corner word's own two */
                if (xr == 0 && yr == 0) {
                    w = WEIGHT[code];
                    for (int k = 0; k < 4; k++) c[k] = (uint8_t)((q[0]->a8[k] * (8 - w) + q[0]->b8[k] * w) >> 3);
                } else {
                    uint8_t e = PALETTE[yr][xr][code];
                    const uint8_t *from = (e & 1) ? q[e >> 1]->b8 : q[e >> 1]->a8;
                    for (int k = 0; k < 4; k++) c[k] = from[k];
                }
            } else {
                if (hard) {
                    /* no interpolation: the block's own two colours */
                    for (int k = 0; k < 4; k++) { a[k] = own->a8[k]; b[k] = own->b8[k]; }
                } else {
                    int wp = (bw - xr) * (4 - yr), wq = xr * (4 - yr), wr = (bw - xr) * yr, ws = xr * yr;
                    for (int k = 0; k < 4; k++) {
                        int ua = q[0]->a5[k] * wp + q[1]->a5[k] * wq + q[2]->a5[k] * wr + q[3]->a5[k] * ws;
                        int ub = q[0]->b5[k] * wp + q[1]->b5[k] * wq + q[2]->b5[k] * wr + q[3]->b5[k] * ws;
                        if (bits == 4) {
                            a[k] = (uint8_t)(k < 3 ? (ua >> 1) + (ua >> 6) : ua + (ua >> 4));
                            b[k] = (uint8_t)(k < 3 ? (ub >> 1) + (ub >> 6) : ub + (ub >> 4));
                        } else {
                            a[k] = (uint8_t)(k < 3 ? (ua >> 2) + (ua >> 7) : (ua >> 1) + (ua >> 5));
                            b[k] = (uint8_t)(k < 3 ? (ub >> 2) + (ub >> 7) : (ub >> 1) + (ub >> 5));
                        }
                    }
                }
                for (int k = 0; k < 4; k++) c[k] = (uint8_t)((a[k] * (8 - w) + b[k] * w) >> 3);
                /* a hole: without alpha, and in the second generation without colour */
                if (punch) {
                    c[3] = 0;
                    if (version == 2) c[0] = c[1] = c[2] = 0;
                }
            }
            out[yo * bw + xo] = (uint32_t)c[0] | ((uint32_t)c[1] << 8) | ((uint32_t)c[2] << 16) | ((uint32_t)c[3] << 24);
        }
}
