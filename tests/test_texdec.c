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

int main(void) {
    printf("texdec:\n");
    test_morton();
    test_bc1();
    test_bc2_bc3();
    printf("all texdec tests passed\n");
    return 0;
}
