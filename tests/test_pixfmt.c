/* test_pixfmt.c — pixel layouts, on pixels written here byte by byte. */

#include "../src/pixfmt.h"
#include "vitarecomp/half.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#error "the tests check with assert(): build them without NDEBUG"
#endif

static int near(float a, float b) { return fabsf(a - b) < 1.0f / 512.0f; }

static void test_bytes(void) {
    pixfmt f;
    float c[4];
    /* four bytes, named from the top down: ABGR is red first in memory */
    static const uint8_t px[4] = { 0x00, 0x33, 0x66, 0xFF };
    assert(pixfmt_texture(0x0C000000u, &f) && f.bytes == 4 && f.components == 4);
    pixfmt_decode(&f, px, c);
    assert(c[0] == 0.0f && near(c[1], 0.2f) && near(c[2], 0.4f) && c[3] == 1.0f);
    assert(pixfmt_texture(0x0C001000u, &f));                    /* ARGB: blue first */
    pixfmt_decode(&f, px, c);
    assert(near(c[0], 0.4f) && near(c[1], 0.2f) && c[2] == 0.0f && c[3] == 1.0f);
    assert(pixfmt_texture(0x0C002000u, &f));                    /* RGBA: alpha first */
    pixfmt_decode(&f, px, c);
    assert(c[0] == 1.0f && near(c[1], 0.4f) && near(c[2], 0.2f) && c[3] == 0.0f);
    assert(pixfmt_texture(0x0C004000u, &f));                    /* 1BGR: the alpha byte is ignored */
    pixfmt_decode(&f, px, c);
    assert(c[0] == 0.0f && c[3] == 1.0f);

    /* one byte, as luminance, as alpha, as red alone */
    static const uint8_t one[1] = { 0x80 };
    assert(pixfmt_texture(0x00005000u, &f) && f.bytes == 1);    /* 1RRR */
    pixfmt_decode(&f, one, c);
    assert(near(c[0], 0.502f) && c[0] == c[1] && c[1] == c[2] && c[3] == 1.0f);
    assert(pixfmt_texture(0x00006000u, &f));                    /* R000 */
    pixfmt_decode(&f, one, c);
    assert(c[0] == 0.0f && near(c[3], 0.502f));
    assert(pixfmt_texture(0x00000000u, &f) && f.components == 1);
    pixfmt_decode(&f, one, c);
    assert(near(c[0], 0.502f) && c[1] == 0.0f && c[3] == 1.0f);

    /* signed bytes: nothing below minus one */
    static const uint8_t sgn[4] = { 0x7F, 0x81, 0x80, 0x00 };
    assert(pixfmt_texture(0x0D000000u, &f));
    pixfmt_decode(&f, sgn, c);
    assert(c[0] == 1.0f && c[1] == -1.0f && c[2] == -1.0f && c[3] == 0.0f);
    printf("  bytes and their orders         ok\n");
}

static void test_packed(void) {
    pixfmt f;
    float c[4];
    /* five, six and five bits with red on top */
    static const uint8_t rgb565[2] = { 0x1F, 0xF8 };            /* 0xF81F: red and blue full, green none */
    assert(pixfmt_texture(0x05001000u, &f) && f.bytes == 2);
    pixfmt_decode(&f, rgb565, c);
    assert(c[0] == 1.0f && c[1] == 0.0f && c[2] == 1.0f && c[3] == 1.0f);
    /* four fours as ARGB: blue in the low nibble */
    static const uint8_t argb4[2] = { 0x0F, 0xF0 };             /* 0xF00F */
    assert(pixfmt_texture(0x02001000u, &f));
    pixfmt_decode(&f, argb4, c);
    assert(c[0] == 0.0f && c[1] == 0.0f && c[2] == 1.0f && c[3] == 1.0f);
    /* 24 bits of depth under 8 of stencil */
    static const uint8_t depth[4] = { 0xFF, 0xFF, 0xFF, 0x5A };
    assert(pixfmt_texture(0x15000000u, &f) && f.components == 1);
    pixfmt_decode(&f, depth, c);
    assert(c[0] == 1.0f && c[3] == 1.0f);
    printf("  packed fields                  ok\n");
}

static void test_floats(void) {
    pixfmt f;
    float c[4], in[4] = { 0.5f, 2.0f, 1000.0f, 1.0f };
    uint8_t px[8];
    /* four halves: exact for these */
    memset(px, 0, sizeof(px));
    assert(pixfmt_texture(0x1B000000u, &f) && f.bytes == 8);
    pixfmt_encode(&f, px, in, 15);
    pixfmt_decode(&f, px, c);
    assert(c[0] == 0.5f && c[1] == 2.0f && c[2] == 1000.0f && c[3] == 1.0f);

    /* three small unsigned floats and two bits: brighter than one survives, below zero does not */
    float hdr[4] = { 0.25f, 6.0f, -3.0f, 1.0f };
    memset(px, 0, sizeof(px));
    assert(pixfmt_texture(0x9A000000u, &f) && f.bytes == 4);
    pixfmt_encode(&f, px, hdr, 15);
    pixfmt_decode(&f, px, c);
    assert(c[0] == 0.25f && c[1] == 6.0f && c[2] == 0.0f && c[3] == 1.0f);
    /* five bits of mantissa: the nearest value, within a thirty-second */
    hdr[0] = 0.7f;
    pixfmt_encode(&f, px, hdr, 1);
    pixfmt_decode(&f, px, c);
    assert(fabsf(c[0] - 0.7f) < 0.7f / 32.0f && c[1] == 6.0f);

    /* a shared exponent: the largest channel sets it */
    float shared[4] = { 1.0f, 0.5f, 100.0f, 1.0f };
    assert(pixfmt_texture(0x19000000u, &f));
    pixfmt_encode(&f, px, shared, 15);
    pixfmt_decode(&f, px, c);
    assert(fabsf(c[2] - 100.0f) < 0.5f && fabsf(c[0] - 1.0f) < 0.5f && fabsf(c[1] - 0.5f) < 0.5f);

    /* whole words are not numbers to convert: every bit comes back */
    uint32_t words[2] = { 0x7FC12345u, 0xFFFFFFFFu }, back[2];
    assert(pixfmt_texture(0x1F000000u, &f) && f.raw && f.components == 2);
    pixfmt_decode(&f, (const uint8_t *)words, c);
    memcpy(back, c, 8);
    assert(back[0] == words[0] && back[1] == words[1]);
    printf("  floats, small floats, raw words ok\n");
}

static void test_colour(void) {
    pixfmt f;
    uint8_t px[8];
    float c[4], in[4] = { 1.0f, 0.5f, 0.25f, 0.0f };
    /* the display's own format: red in the first byte */
    assert(pixfmt_colour(0x00000000u, &f) && f.bytes == 4);
    memset(px, 0xAA, sizeof(px));
    pixfmt_encode(&f, px, in, 15);
    assert(px[0] == 0xFF && px[1] == 0x80 && px[2] == 0x40 && px[3] == 0x00);
    /* a masked write leaves the other channels alone */
    in[1] = 0.0f;
    pixfmt_encode(&f, px, in, 2);
    assert(px[0] == 0xFF && px[1] == 0x00 && px[2] == 0x40);
    /* out of range is held */
    float wild[4] = { 7.0f, -7.0f, 0.5f, 1.0f };
    pixfmt_encode(&f, px, wild, 15);
    assert(px[0] == 0xFF && px[1] == 0x00);
    /* a single channel that is alpha */
    assert(pixfmt_colour(0x80900000u, &f) && f.bytes == 1);
    px[0] = 0x40;
    pixfmt_decode(&f, px, c);
    assert(c[0] == 0.0f && near(c[3], 0.251f));
    /* and the texture that reads a colour format has the same bytes */
    assert(pixfmt_colour_texture(0x00100000u) == 0x0C001000u);
    assert(pixfmt_colour_texture(0x41000000u) == 0x9A000000u);
    assert(!pixfmt_colour(0x70000000u, &f));

    /* the display curve and its inverse */
    for (int i = 0; i <= 255; i++) {
        float v = (float)i / 255.0f, there = pix_srgb_to_linear(v), back = pix_linear_to_srgb(there);
        assert(fabsf(back - v) < 1e-4f && there <= v + 1e-6f);
    }
    printf("  colour surfaces                ok\n");
}

/* A pixel from four halves by lookup is the pixel the long way gives: for
 * surfaces of bytes, of halves, of small floats beside two bits and of
 * five, six and five bits, on the display's curve and off it, clamped
 * first and not, every half in every channel once and then halves at
 * random, into pixels that are not empty. A format with a whole float in
 * it has no tables. */
static void test_halves(void) {
    static const uint32_t FORMATS[] = { 0x00000000u, 0x00001000u, 0x01000000u, 0x41000000u, 0x30000000u, 0xE0800000u };
    uint64_t seed = 0x2545F4914F6CDD1Dull;
    unsigned tried = 0;
    for (size_t n = 0; n < sizeof(FORMATS) / sizeof(FORMATS[0]); n++) {
        pixfmt f;
        if (!pixfmt_colour(FORMATS[n], &f)) continue;
        for (int on_curve = 0; on_curve < 2; on_curve++)
            for (int clamped = 0; clamped < 2; clamped++) {
                const pixfmt_halves *t = pixfmt_halves_for(&f, on_curve, clamped);
                assert(t && t == pixfmt_halves_for(&f, on_curve, clamped));
                for (uint32_t round = 0; round < 65536 + 20000; round++) {
                    uint16_t h[4];
                    for (int c = 0; c < 4; c++) {
                        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                        h[c] = round < 65536 ? (uint16_t)(round + (uint32_t)c * 21845u) : (uint16_t)(seed >> 20);
                    }
                    uint32_t words[2] = { (uint32_t)h[0] | (uint32_t)h[1] << 16, (uint32_t)h[2] | (uint32_t)h[3] << 16 };
                    uint8_t by_table[8], long_way[8];
                    float c[4];
                    memset(by_table, (int)(round * 37u), sizeof(by_table));
                    memcpy(long_way, by_table, sizeof(long_way));
                    for (int k = 0; k < 4; k++) {
                        c[k] = vita_f16_to_f32(h[k]);
                        if (clamped) c[k] = !(c[k] > 0.0f) ? 0.0f : c[k] > 1.0f ? 1.0f : c[k];
                        if (on_curve && k < 3) c[k] = pix_linear_to_srgb(c[k]);
                    }
                    pixfmt_encode(&f, long_way, c, 15);
                    pixfmt_halves_store(t, by_table, words);
                    if (memcmp(by_table, long_way, sizeof(by_table))) {
                        fprintf(stderr, "format %08X curve %d clamp %d halves %04X %04X %04X %04X\n", FORMATS[n], on_curve, clamped,
                                h[0], h[1], h[2], h[3]);
                        assert(0);
                    }
                }
                tried++;
            }
    }
    assert(tried >= 16);
    pixfmt whole;
    assert(pixfmt_texture(0x12000000u, &whole) && !pixfmt_halves_for(&whole, 0, 0));       /* one whole float */
    printf("  four halves by lookup          ok\n");
}

/* A picture's pixel from its brightness and two colour differences, by the
 * two standards and over the two ranges. */
static int within(uint32_t word, int r, int g, int b, int by) {
    int dr = (int)(word & 255) - r, dg = (int)((word >> 8) & 255) - g, db = (int)((word >> 16) & 255) - b;
    return abs(dr) <= by && abs(dg) <= by && abs(db) <= by && (word >> 24) == 255;
}

static void test_yuv(void) {
    /* the studio's white and black, and what lies beyond them, by either set of weights */
    for (unsigned p = PIX_YUV_BT601; p <= PIX_YUV_BT709; p++) {
        const pix_yuv *t = pix_yuv_for(p);
        assert(pix_yuv_word(t, 235, 128, 128) == 0xFFFFFFFFu && pix_yuv_word(t, 255, 128, 128) == 0xFFFFFFFFu);
        assert(pix_yuv_word(t, 16, 128, 128) == 0xFF000000u && pix_yuv_word(t, 0, 128, 128) == 0xFF000000u);
        assert(within(pix_yuv_word(t, 126, 128, 128), 128, 128, 128, 0));          /* half way up is half grey */
    }
    /* over the whole range a byte of brightness is the byte of grey */
    for (unsigned p = PIX_YUV_BT601_FULL; p <= PIX_YUV_BT709_FULL; p++) {
        const pix_yuv *t = pix_yuv_for(p);
        for (unsigned y = 0; y < 256; y++) assert(within(pix_yuv_word(t, y, 128, 128), (int)y, (int)y, (int)y, 0));
    }
    /* the primaries as each standard's colour bars code them */
    const pix_yuv *sd = pix_yuv_for(PIX_YUV_BT601), *hd = pix_yuv_for(PIX_YUV_BT709);
    assert(within(pix_yuv_word(sd, 81, 90, 240), 255, 0, 0, 2) && within(pix_yuv_word(hd, 63, 102, 240), 255, 0, 0, 2));
    assert(within(pix_yuv_word(sd, 145, 54, 34), 0, 255, 0, 2) && within(pix_yuv_word(hd, 173, 42, 26), 0, 255, 0, 2));
    assert(within(pix_yuv_word(sd, 41, 240, 110), 0, 0, 255, 2) && within(pix_yuv_word(hd, 32, 240, 118), 0, 0, 255, 2));
    /* and one standard's red read by the other's weights is not red: too little of it, and green in it */
    assert(!within(pix_yuv_word(hd, 81, 90, 240), 255, 0, 0, 8) && !within(pix_yuv_word(sd, 63, 102, 240), 255, 0, 0, 8));

    /* every profile against the arithmetic written out, on a grid of all three bytes */
    static const double RED[2] = { 0.299, 0.2126 }, BLUE[2] = { 0.114, 0.0722 };
    for (unsigned p = 0; p < 4; p++) {
        const pix_yuv *t = pix_yuv_for(p);
        const double kr = RED[p & 1], kb = BLUE[p & 1], kg = 1.0 - kr - kb;
        for (unsigned y = 0; y < 256; y += 5)
            for (unsigned u = 0; u < 256; u += 15)
                for (unsigned v = 0; v < 256; v += 17) {
                    double l = p >= 2 ? (double)y : ((double)y - 16.0) * 255.0 / 219.0;
                    double cb = ((double)u - 128.0) * (p >= 2 ? 1.0 : 255.0 / 224.0), cr = ((double)v - 128.0) * (p >= 2 ? 1.0 : 255.0 / 224.0);
                    /* red and blue are the brightness and their difference; green is what the brightness has left */
                    double r = l + 2.0 * (1.0 - kr) * cr, b = l + 2.0 * (1.0 - kb) * cb, g = l - (kr * (r - l) + kb * (b - l)) / kg;
                    int want[3] = { (int)floor(r + 0.5), (int)floor(g + 0.5), (int)floor(b + 0.5) };
                    for (int k = 0; k < 3; k++) want[k] = want[k] < 0 ? 0 : want[k] > 255 ? 255 : want[k];
                    assert(within(pix_yuv_word(t, y, u, v), want[0], want[1], want[2], 1));
                }
    }
    printf("  brightness and differences     ok\n");
}

int main(void) {
    printf("pixfmt:\n");
    test_bytes();
    test_packed();
    test_floats();
    test_colour();
    test_halves();
    test_yuv();
    printf("all pixfmt tests passed\n");
    return 0;
}
