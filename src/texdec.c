/* texdec.c — the texel layouts a renderer has to undo. See texdec.h. */

#include "texdec.h"

/* --- Morton order ----------------------------------------------------------------- */

uint32_t tex_morton_index(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    uint32_t index = 0, shift = 0;
    /* interleave while both axes have bits left: x in the odd place, y in the even */
    while (width > 1 && height > 1) {
        index |= (y & 1u) << shift++;
        index |= (x & 1u) << shift++;
        x >>= 1; y >>= 1;
        width >>= 1; height >>= 1;
    }
    /* what is left of the longer axis follows whole */
    if (width > 1)  index |= x << shift;
    if (height > 1) index |= y << shift;
    return index;
}

/* --- block compression ------------------------------------------------------------- */

static uint32_t rgb565(uint32_t c) {
    uint32_t r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return r | (g << 8) | (b << 16);
}

static uint32_t mix(uint32_t a, uint32_t b, uint32_t wa, uint32_t wb, uint32_t div) {
    uint32_t out = 0;
    for (int s = 0; s < 24; s += 8)
        out |= ((((a >> s) & 255u) * wa + ((b >> s) & 255u) * wb) / div) << s;
    return out;
}

/* The colour half of every one of these formats: two 5:6:5 colours and
 * sixteen two-bit choices among them and what lies between. With
 * `punch_through` the block may instead hold three colours and a hole. */
static void colour_block(const uint8_t *b, uint32_t out[16], int punch_through) {
    uint32_t c0 = (uint32_t)b[0] | ((uint32_t)b[1] << 8), c1 = (uint32_t)b[2] | ((uint32_t)b[3] << 8);
    uint32_t pal[4];
    pal[0] = rgb565(c0) | 0xFF000000u;
    pal[1] = rgb565(c1) | 0xFF000000u;
    if (c0 > c1 || !punch_through) {
        pal[2] = mix(pal[0], pal[1], 2, 1, 3) | 0xFF000000u;
        pal[3] = mix(pal[0], pal[1], 1, 2, 3) | 0xFF000000u;
    } else {
        pal[2] = mix(pal[0], pal[1], 1, 1, 2) | 0xFF000000u;
        pal[3] = 0;
    }
    uint32_t bits = (uint32_t)b[4] | ((uint32_t)b[5] << 8) | ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
    for (int i = 0; i < 16; i++) out[i] = pal[(bits >> (2 * i)) & 3u];
}

void tex_decode_bc1(const uint8_t block[8], uint32_t out[16]) {
    colour_block(block, out, 1);
}

void tex_decode_bc2(const uint8_t block[16], uint32_t out[16]) {
    colour_block(block + 8, out, 0);
    for (int i = 0; i < 16; i++) {
        uint32_t a = (block[i / 2] >> (4 * (i & 1))) & 15u;
        out[i] = (out[i] & 0x00FFFFFFu) | ((a * 17u) << 24);
    }
}

/* Eight alphas from two: the ends and what lies between, or, when the first
 * is not the larger, four between and then nothing and everything. */
static void alpha_ramp(uint32_t a0, uint32_t a1, uint32_t ramp[8]) {
    ramp[0] = a0;
    ramp[1] = a1;
    if (a0 > a1) {
        for (uint32_t i = 1; i < 7; i++) ramp[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    } else {
        for (uint32_t i = 1; i < 5; i++) ramp[i + 1] = ((5 - i) * a0 + i * a1) / 5;
        ramp[6] = 0;
        ramp[7] = 255;
    }
}

static void alpha_block(const uint8_t *b, uint32_t values[16]) {
    uint32_t ramp[8];
    alpha_ramp(b[0], b[1], ramp);
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) values[i] = ramp[(bits >> (3 * i)) & 7u];
}

void tex_decode_bc3(const uint8_t block[16], uint32_t out[16]) {
    uint32_t alpha[16];
    colour_block(block + 8, out, 0);
    alpha_block(block, alpha);
    for (int i = 0; i < 16; i++) out[i] = (out[i] & 0x00FFFFFFu) | (alpha[i] << 24);
}

void tex_decode_bc4(const uint8_t block[8], uint32_t out[16]) {
    uint32_t v[16];
    alpha_block(block, v);
    for (int i = 0; i < 16; i++) out[i] = v[i] | 0xFF000000u;
}

void tex_decode_bc5(const uint8_t block[16], uint32_t out[16]) {
    uint32_t r[16], g[16];
    alpha_block(block, r);
    alpha_block(block + 8, g);
    for (int i = 0; i < 16; i++) out[i] = r[i] | (g[i] << 8) | 0xFF000000u;
}
