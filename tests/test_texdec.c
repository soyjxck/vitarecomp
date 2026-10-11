/* test_texdec.c — texel layouts, checked against blocks built by hand. */

#include "../src/texdec.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_morton(void) {
    /* a square: x and y interleave, y in the low bit */
    assert(tex_morton_index(0, 0, 4, 4) == 0);
    assert(tex_morton_index(0, 1, 4, 4) == 1);
    assert(tex_morton_index(1, 0, 4, 4) == 2);
    assert(tex_morton_index(1, 1, 4, 4) == 3);
    assert(tex_morton_index(2, 0, 4, 4) == 8);
    assert(tex_morton_index(3, 3, 4, 4) == 15);

    /* every texel of a square and of both kinds of oblong gets its own slot */
    static const uint32_t sizes[][2] = { {8, 8}, {16, 4}, {4, 16}, {1, 8}, {8, 1}, {32, 2} };
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        uint32_t w = sizes[s][0], h = sizes[s][1];
        uint8_t seen[64];
        memset(seen, 0, sizeof(seen));
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint32_t i = tex_morton_index(x, y, w, h);
                assert(i < w * h && !seen[i]);
                seen[i] = 1;
            }
    }
    /* an oblong is a row of squares: 16x4 is four 4x4 squares side by side */
    assert(tex_morton_index(4, 0, 16, 4) == 16);
    assert(tex_morton_index(5, 1, 16, 4) == 16 + 3);
    assert(tex_morton_index(0, 4, 4, 16) == 16);
    printf("  morton order                   ok\n");
}

static void test_bc1(void) {
    uint32_t px[16];
    /* red and blue, first the larger: four colours, two between */
    uint8_t opaque[8] = { 0x00, 0xF8, 0x1F, 0x00, 0x1B, 0x1B, 0x1B, 0x1B };     /* choices 3, 2, 1, 0 along each row */
    tex_decode_bc1(opaque, px);
    assert(px[3] == 0xFF0000FFu);                       /* choice 0: red, in the low byte */
    assert(px[2] == 0xFFFF0000u);                       /* choice 1: blue */
    assert(px[1] == 0xFF5500AAu);                       /* choice 2: two parts red, one blue */
    assert(px[0] == 0xFFAA0055u);                       /* choice 3: one part red, two blue */
    assert(px[4] == px[0] && px[15] == px[3]);

    /* the same two colours the other way round: three colours and a hole */
    uint8_t holed[8] = { 0x1F, 0x00, 0x00, 0xF8, 0x1B, 0x1B, 0x1B, 0x1B };
    tex_decode_bc1(holed, px);
    assert(px[3] == 0xFFFF0000u && px[2] == 0xFF0000FFu);
    assert(px[1] == 0xFF7F007Fu);                       /* half-way */
    assert(px[0] == 0);                                 /* nothing at all */
    printf("  bc1                            ok\n");
}

static void test_bc2_bc3(void) {
    uint32_t px[16];
    uint8_t bc2[16] = { 0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE,     /* alphas 0, 1, 2 ... 15 */
                        0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };   /* white everywhere */
    tex_decode_bc2(bc2, px);
    for (uint32_t i = 0; i < 16; i++) assert(px[i] == (0x00FFFFFFu | ((i * 17u) << 24)));

    /* alpha 255 and 0, the first larger: eight steps; texel i chooses step i & 7 */
    uint8_t bc3[16] = { 0xFF, 0x00, 0x88, 0xC6, 0xFA, 0x88, 0xC6, 0xFA,
                        0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    tex_decode_bc3(bc3, px);
    assert((px[0] >> 24) == 255 && (px[1] >> 24) == 0);
    assert((px[2] >> 24) == 218 && (px[7] >> 24) == 36);    /* 6/7 and 1/7 of the way down */
    assert((px[8] >> 24) == 255 && (px[10] >> 24) == 218);
    assert((px[5] & 0x00FFFFFFu) == 0x00FFFFFFu);

    /* the first not larger: four steps between, then 0 and 255 */
    uint8_t six[16] = { 0x00, 0xFF, 0x88, 0xC6, 0xFA, 0x88, 0xC6, 0xFA,
                        0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    tex_decode_bc3(six, px);
    assert((px[0] >> 24) == 0 && (px[1] >> 24) == 255);
    assert((px[2] >> 24) == 51 && (px[5] >> 24) == 204);
    assert((px[6] >> 24) == 0 && (px[7] >> 24) == 255);

    /* one channel and two, the same ramp without a colour */
    tex_decode_bc4(bc3, px);
    assert(px[0] == 0xFF0000FFu && px[1] == 0xFF000000u && px[2] == 0xFF0000DAu);
    uint8_t two[16];
    memcpy(two, bc3, 8);
    memcpy(two + 8, six, 8);
    tex_decode_bc5(two, px);
    assert(px[0] == 0xFF0000FFu && px[1] == 0xFF00FF00u && px[2] == 0xFF0033DAu);
    printf("  bc2, bc3, bc4, bc5             ok\n");
}

/* PVRTC. A level whose words are all the same has nothing to interpolate, so
 * its texels can be worked out by hand; and words from a fixed generator,
 * decoded whole, must give the image the format's definition gives, held
 * here as a hash of it. */
static uint32_t pvrtc_image_hash(int version, int bits, uint32_t w, uint32_t h, const uint8_t *words, uint32_t nx, uint32_t ny) {
    static uint8_t image[64 * 64 * 4];
    uint32_t bw = bits == 2 ? 8 : 4, block[32], hash = 2166136261u;
    for (uint32_t by = 0; by < ny; by++)
        for (uint32_t bx = 0; bx < nx; bx++) {
            tex_decode_pvrtc(words, nx, ny, bx, by, version, bits, block);
            for (uint32_t j = 0; j < 4; j++)
                for (uint32_t i = 0; i < bw; i++) {
                    uint32_t x = bx * bw + i, y = by * 4 + j;
                    if (x < w && y < h) memcpy(&image[(y * w + x) * 4], &block[j * bw + i], 4);
                }
        }
    for (uint32_t i = 0; i < w * h * 4; i++) hash = (hash ^ image[i]) * 16777619u;
    return hash;
}

static void test_pvrtc(void) {
    static uint8_t words[64 * 8];
    uint32_t px[32];

    /* first generation, four bits: opaque red and opaque blue; modulation
     * all first colour, then all second, then the two steps between */
    static const uint8_t red_blue[4] = { 0x00, 0xFC, 0x1F, 0x80 };
    for (int i = 0; i < 4; i++) { memset(words + 8 * i, 0, 4); memcpy(words + 8 * i + 4, red_blue, 4); }
    tex_decode_pvrtc(words, 2, 2, 0, 0, 1, 4, px);
    assert(px[0] == 0xFF0000FFu && px[15] == 0xFF0000FFu);
    for (int i = 0; i < 4; i++) memset(words + 8 * i, 0xFF, 4);
    tex_decode_pvrtc(words, 2, 2, 1, 1, 1, 4, px);
    assert(px[0] == 0xFFFF0000u && px[15] == 0xFFFF0000u);
    for (int i = 0; i < 4; i++) { memset(words + 8 * i, 0, 4); words[8 * i] = 0x09; }   /* texel 0: three eighths, texel 1: five */
    tex_decode_pvrtc(words, 2, 2, 0, 0, 1, 4, px);
    assert(px[0] == 0xFF5F009Fu && px[1] == 0xFF9F005Fu && px[2] == 0xFF0000FFu);

    /* with the modulation flag set, the third code is a hole: no alpha in
     * the first generation, nothing at all in the second */
    for (int i = 0; i < 4; i++) { words[8 * i] = 0x02; words[8 * i + 4] |= 1; }
    tex_decode_pvrtc(words, 2, 2, 0, 0, 1, 4, px);
    assert(px[0] == 0x007F007Fu);
    for (int i = 0; i < 4; i++) words[8 * i + 5] &= 0x7F;        /* the second generation's hard flag off */
    tex_decode_pvrtc(words, 2, 2, 0, 0, 2, 4, px);
    assert(px[0] == 0x00000000u);

    /* second generation, translucent colours: its second colour's alpha has
     * a low bit the first generation's has not */
    static const uint8_t word[8] = { 0xDA, 0x7E, 0x4F, 0xD2, 0x8E, 0x18, 0x7F, 0x28 };
    for (int i = 0; i < 4; i++) memcpy(words + 8 * i, word, 8);
    tex_decode_pvrtc(words, 2, 2, 0, 0, 2, 4, px);
    assert(px[0] == 0x41FF7C8Cu);                               /* five eighths of the way from (140,140,255,34) to (140,115,255,85) */

    /* two bits: one bit a texel picks a colour outright */
    for (int i = 0; i < 4; i++) { memset(words + 8 * i, 0, 4); words[8 * i] = 0x02; memcpy(words + 8 * i + 4, red_blue, 4); }
    tex_decode_pvrtc(words, 2, 2, 0, 0, 1, 2, px);
    assert(px[0] == 0xFF0000FFu && px[1] == 0xFFFF0000u && px[8] == 0xFF0000FFu);

    static const struct { int version, bits; uint32_t w, h, hash; } CASES[] = {
        { 1, 4, 32, 32, 0xCF63DBF5u }, { 1, 4, 64, 16, 0xA89F0504u }, { 1, 4, 16, 64, 0x30E391A7u },
        { 1, 2, 32, 32, 0xF35CE18Bu }, { 1, 2, 64, 16, 0x9DDDB0D2u }, { 1, 2, 16, 64, 0x7C25D0CDu },
        { 2, 4, 32, 32, 0x6171EB5Cu }, { 2, 4, 64, 16, 0x949188F1u }, { 2, 4, 16, 64, 0x4ADE8CEFu },
        { 2, 2, 32, 32, 0x96871FFFu }, { 2, 2, 64, 16, 0x6B3E6C4Cu }, { 2, 2, 16, 64, 0x99D3EDB0u },
    };
    for (size_t c = 0; c < sizeof(CASES) / sizeof(CASES[0]); c++) {
        uint32_t bw = CASES[c].bits == 2 ? 8 : 4, least = CASES[c].version == 1 ? 2 : 1;
        uint32_t nx = CASES[c].w / bw, ny = CASES[c].h / 4;
        if (nx < least) nx = least;
        if (ny < least) ny = least;
        uint32_t x = (uint32_t)(CASES[c].version * 1000 + CASES[c].bits * 100) + CASES[c].w;
        assert(nx * ny * 8 <= sizeof(words));
        for (uint32_t i = 0; i < nx * ny * 8; i++) {
            x = x * 1664525u + 1013904223u;
            words[i] = (uint8_t)(x >> 24);
        }
        uint32_t got = pvrtc_image_hash(CASES[c].version, CASES[c].bits, CASES[c].w, CASES[c].h, words, nx, ny);
        if (got != CASES[c].hash) {
            fprintf(stderr, "pvrtc generation %d, %d bits, %ux%u: 0x%08X, wanted 0x%08X\n",
                    CASES[c].version, CASES[c].bits, CASES[c].w, CASES[c].h, got, CASES[c].hash);
            assert(0);
        }
    }
    printf("  pvrtc, both generations        ok\n");
}

int main(void) {
    printf("texdec:\n");
    test_morton();
    test_bc1();
    test_bc2_bc3();
    test_pvrtc();
    printf("all texdec tests passed\n");
    return 0;
}
