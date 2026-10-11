/* test_gxm_tex.c — textures, on control words and texels made here. */

#include "../src/gxm_tex.h"
#include "../src/texdec.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "the tests check with assert(): build them without NDEBUG"
#endif

/* Control words as the library writes them: clamped, nearest, with the
 * given filters, type, format, size and level count. */
static void words(uint32_t w[4], uint32_t type, uint32_t format, uint32_t width, uint32_t height, uint32_t levels,
                  uint32_t filter, uint32_t address) {
    int logs = type == GXM_TEX_SWIZZLED || type == GXM_TEX_CUBE;
    uint32_t lw = 0, lh = 0;
    while ((1u << lw) < width) lw++;
    while ((1u << lh) < height) lh++;
    w[0] = (address << 6) | (address << 3) | (filter << 10) | (filter << 12) | (((levels - 1) & 15) << 17) | (31u << 21) | (format & 0x80000000u);
    w[1] = (type << 29) | (format & 0x1F000000u) | (logs ? (lh | lw << 16) : ((height - 1) | (width - 1) << 12));
    w[2] = 0x1000;                                      /* any address that is not nothing */
    w[3] = 0x80000000u | (((format >> 12) & 7) << 28);
}

static gxm_tex t;
static uint8_t store[1 << 16];

static void open_texture(const uint32_t w[4]) {
    assert(gxm_tex_decode(&t, w));
    assert(t.bytes <= sizeof(store));
    t.base = store;
    t.avail = sizeof(store);
}

static void test_layouts(void) {
    uint32_t w[4];
    float c[4];

    /* rows: a row is the width rounded up to eight texels */
    memset(store, 0, sizeof(store));
    words(w, GXM_TEX_LINEAR, 0x00000000u, 5, 3, 1, 0, 2);
    open_texture(w);
    /* declared with a chain of one level, it takes the room of its sides rounded up to powers of two */
    assert(t.width == 5 && t.height == 3 && t.levels == 1 && t.bytes == 8 * 4);
    store[1 * 8 + 4] = 255;
    gxm_tex_texel(&t, 0, 0, 4, 1, c);
    assert(c[0] == 1.0f);
    gxm_tex_texel(&t, 0, 0, 3, 1, c);
    assert(c[0] == 0.0f);
    gxm_tex_texel(&t, 0, 0, 5, 1, c);                   /* outside: nothing */
    assert(c[0] == 0.0f && c[3] == 0.0f);

    /* declared with no chain at all, only what its rows need */
    words(w, GXM_TEX_LINEAR, 0x00000000u, 5, 3, 16, 0, 2);
    open_texture(w);
    assert(t.levels == 1 && t.bytes == 8 * 3);

    /* Morton order: down before across */
    memset(store, 0, sizeof(store));
    words(w, GXM_TEX_SWIZZLED, 0x00000000u, 4, 4, 1, 0, 2);
    open_texture(w);
    store[1] = 255;                                     /* (0, 1) */
    store[2] = 128;                                     /* (1, 0) */
    gxm_tex_texel(&t, 0, 0, 0, 1, c);
    assert(c[0] == 1.0f);
    gxm_tex_texel(&t, 0, 0, 1, 0, c);
    assert(c[0] > 0.49f && c[0] < 0.51f);

    /* tiles of 32 by 32, row after row of them */
    memset(store, 0, sizeof(store));
    words(w, GXM_TEX_TILED, 0x00000000u, 64, 32, 1, 0, 2);
    open_texture(w);
    store[1024 + 5 * 32 + 3] = 255;                     /* the second tile, its texel (3, 5) */
    gxm_tex_texel(&t, 0, 0, 35, 5, c);
    assert(c[0] == 1.0f);

    /* a chain of levels, each straight after the last */
    memset(store, 0, sizeof(store));
    words(w, GXM_TEX_SWIZZLED, 0x00000000u, 8, 8, 4, 0, 2);
    open_texture(w);
    assert(t.levels == 4 && t.level_at[1] == 64 && t.level_at[2] == 80 && t.level_at[3] == 84 && t.bytes == 85);
    store[64] = 255;                                    /* level 1, texel (0, 0) */
    gxm_tex_texel(&t, 0, 1, 0, 0, c);
    assert(c[0] == 1.0f);
    assert(t.level_w[3] == 1);

    /* no more levels than the shorter side allows */
    words(w, GXM_TEX_SWIZZLED, 0x00000000u, 16, 2, 9, 0, 2);
    open_texture(w);
    assert(t.levels == 2);
    printf("  rows, Morton order, tiles, levels ok\n");
}

static void test_blocks_and_palettes(void) {
    uint32_t w[4];
    float c[4];
    /* an 8 by 8 texture of four BC1 blocks in Morton order: the block at
     * (0, 1) is all of its first colour, pure red */
    memset(store, 0, sizeof(store));
    words(w, GXM_TEX_SWIZZLED, 0x85000000u, 8, 8, 1, 0, 2);
    open_texture(w);
    assert(t.kind == GXM_TEX_BC1 && t.bytes == 32);
    store[8 + 0] = 0x00; store[8 + 1] = 0xF8;           /* colour 0 = 0xF800 */
    gxm_tex_texel(&t, 0, 0, 2, 6, c);
    assert(c[0] == 1.0f && c[1] == 0.0f && c[2] == 0.0f && c[3] == 1.0f);
    gxm_tex_texel(&t, 0, 0, 5, 1, c);                   /* another block: black */
    assert(c[0] == 0.0f);

    /* a palette of 256 colours, the index a byte */
    static uint8_t palette[1024];
    memset(store, 0, sizeof(store));
    words(w, GXM_TEX_LINEAR, 0x95000000u, 8, 1, 1, 0, 2);
    open_texture(w);
    t.pal = palette;
    palette[4 * 7 + 1] = 255;                           /* entry 7 is green */
    store[3] = 7;
    gxm_tex_texel(&t, 0, 0, 3, 0, c);
    assert(c[0] == 0.0f && c[1] == 1.0f);
    printf("  blocks and palettes            ok\n");
}

static void test_samples(void) {
    uint32_t w[4];
    float c[4], at[2];
    /* two texels, black and white */
    memset(store, 0, sizeof(store));
    store[1] = 255;

    /* nearest: each half of the texture is one texel */
    words(w, GXM_TEX_LINEAR, 0x00000000u, 2, 1, 1, 0, 2);
    open_texture(w);
    at[0] = 0.49f; at[1] = 0.5f;
    gxm_tex_sample(&t, at, 0.0f, c);
    assert(c[0] == 0.0f);
    at[0] = 0.51f;
    gxm_tex_sample(&t, at, 0.0f, c);
    assert(c[0] == 1.0f);

    /* linear: halfway between the two centres is halfway between the texels,
     * and at a centre the texel itself */
    words(w, GXM_TEX_LINEAR, 0x00000000u, 2, 1, 1, 1, 2);
    open_texture(w);
    at[0] = 0.5f;
    gxm_tex_sample(&t, at, 0.0f, c);
    assert(fabsf(c[0] - 0.5f) < 1e-6f);
    at[0] = 0.75f;
    gxm_tex_sample(&t, at, 0.0f, c);
    assert(c[0] == 1.0f);
    /* clamped: beyond the edge is the edge */
    at[0] = 3.0f;
    gxm_tex_sample(&t, at, 0.0f, c);
    assert(c[0] == 1.0f);
    /* repeated: beyond the edge is the other side */
    words(w, GXM_TEX_LINEAR, 0x00000000u, 2, 1, 1, 0, 0);
    open_texture(w);
    at[0] = 1.25f;
    gxm_tex_sample(&t, at, 0.0f, c);
    assert(c[0] == 0.0f);
    at[0] = -0.25f;
    gxm_tex_sample(&t, at, 0.0f, c);
    assert(c[0] == 1.0f);
    /* mirrored: the second period runs backwards */
    words(w, GXM_TEX_LINEAR, 0x00000000u, 2, 1, 1, 0, 1);
    open_texture(w);
    at[0] = 1.25f;
    gxm_tex_sample(&t, at, 0.0f, c);
    assert(c[0] == 1.0f);

    /* a footprint of two texels a pixel is one level down */
    words(w, GXM_TEX_SWIZZLED, 0x00000000u, 4, 4, 3, 1, 2);
    open_texture(w);
    assert(fabsf(gxm_tex_footprint(&t, 0.5f, 0.0f, 0.0f, 0.5f) - 1.0f) < 1e-5f);
    assert(gxm_tex_footprint(&t, 0.0f, 0.0f, 0.0f, 0.0f) < 0.0f);
    memset(store, 0, sizeof(store));
    memset(store + 16, 255, 4);                         /* level 1 is white */
    at[0] = at[1] = 0.5f;
    gxm_tex_sample(&t, at, 0.0f, c);
    assert(c[0] == 0.0f);
    gxm_tex_sample(&t, at, 1.0f, c);
    assert(c[0] == 1.0f);

    /* the four texels around a point, and how far the point is between them */
    float four[4], fraction[2];
    memset(store, 0, sizeof(store));
    words(w, GXM_TEX_LINEAR, 0x00000000u, 8, 2, 1, 0, 2);
    open_texture(w);
    store[0] = 10; store[1] = 20; store[8] = 30; store[9] = 40;
    at[0] = 1.0f / 8.0f; at[1] = 0.5f;                  /* on the corner the four texels share */
    gxm_tex_gather(&t, at, four, fraction);
    assert(fabsf(four[0] * 255.0f - 30.0f) < 0.01f && fabsf(four[1] * 255.0f - 40.0f) < 0.01f);
    assert(fabsf(four[2] * 255.0f - 20.0f) < 0.01f && fabsf(four[3] * 255.0f - 10.0f) < 0.01f);
    assert(fabsf(fraction[0] - 0.5f) < 1e-6f && fabsf(fraction[1] - 0.5f) < 1e-6f);
    printf("  filters, wrapping, levels      ok\n");
}

static void test_cube(void) {
    uint32_t w[4];
    float c[4];
    /* six faces of one texel each; the face a direction points at */
    memset(store, 0, sizeof(store));
    words(w, GXM_TEX_CUBE, 0x00000000u, 1, 1, 1, 0, 2);
    open_texture(w);
    assert(t.cube && t.face_bytes == 4);
    for (int face = 0; face < 6; face++) store[face * 4] = (uint8_t)(40 * (face + 1));
    static const float dirs[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    for (int face = 0; face < 6; face++) {
        gxm_tex_sample(&t, dirs[face], 0.0f, c);
        assert(fabsf(c[0] * 255.0f - (float)(40 * (face + 1))) < 0.01f);
    }
    printf("  cube maps                      ok\n");
}

int main(void) {
    printf("gxm_tex:\n");
    test_layouts();
    test_blocks_and_palettes();
    test_samples();
    test_cube();
    printf("all gxm_tex tests passed\n");
    return 0;
}
