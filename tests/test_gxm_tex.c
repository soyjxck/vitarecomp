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
static gxm_tex_cache cache;
static uint8_t store[1 << 16];

static void open_texture(const uint32_t w[4]) {
    assert(gxm_tex_decode(&t, w));
    assert(t.bytes <= sizeof(store));
    t.base = store;
    t.avail = sizeof(store);
    gxm_tex_cache_reset(&cache);
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
    gxm_tex_texel(&t, &cache, 0, 0, 4, 1, c);
    assert(c[0] == 1.0f);
    gxm_tex_texel(&t, &cache, 0, 0, 3, 1, c);
    assert(c[0] == 0.0f);
    gxm_tex_texel(&t, &cache, 0, 0, 5, 1, c);                   /* outside: nothing */
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
    gxm_tex_texel(&t, &cache, 0, 0, 0, 1, c);
    assert(c[0] == 1.0f);
    gxm_tex_texel(&t, &cache, 0, 0, 1, 0, c);
    assert(c[0] > 0.49f && c[0] < 0.51f);

    /* tiles of 32 by 32, row after row of them */
    memset(store, 0, sizeof(store));
    words(w, GXM_TEX_TILED, 0x00000000u, 64, 32, 1, 0, 2);
    open_texture(w);
    store[1024 + 5 * 32 + 3] = 255;                     /* the second tile, its texel (3, 5) */
    gxm_tex_texel(&t, &cache, 0, 0, 35, 5, c);
    assert(c[0] == 1.0f);

    /* a chain of levels, each straight after the last */
    memset(store, 0, sizeof(store));
    words(w, GXM_TEX_SWIZZLED, 0x00000000u, 8, 8, 4, 0, 2);
    open_texture(w);
    assert(t.levels == 4 && t.level_at[1] == 64 && t.level_at[2] == 80 && t.level_at[3] == 84 && t.bytes == 85);
    store[64] = 255;                                    /* level 1, texel (0, 0) */
    gxm_tex_texel(&t, &cache, 0, 1, 0, 0, c);
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
    gxm_tex_texel(&t, &cache, 0, 0, 2, 6, c);
    assert(c[0] == 1.0f && c[1] == 0.0f && c[2] == 0.0f && c[3] == 1.0f);
    gxm_tex_texel(&t, &cache, 0, 0, 5, 1, c);                   /* another block: black */
    assert(c[0] == 0.0f);
    gxm_tex_texel(&t, NULL, 0, 0, 2, 6, c);                     /* and the same without a cache */
    assert(c[0] == 1.0f && c[3] == 1.0f);

    /* a palette of 256 colours, the index a byte */
    static uint8_t palette[1024];
    memset(store, 0, sizeof(store));
    words(w, GXM_TEX_LINEAR, 0x95000000u, 8, 1, 1, 0, 2);
    open_texture(w);
    t.pal = palette;
    palette[4 * 7 + 1] = 255;                           /* entry 7 is green */
    store[3] = 7;
    gxm_tex_texel(&t, &cache, 0, 0, 3, 0, c);
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
    gxm_tex_sample(&t, &cache, at, 0.0f, c);
    assert(c[0] == 0.0f);
    at[0] = 0.51f;
    gxm_tex_sample(&t, &cache, at, 0.0f, c);
    assert(c[0] == 1.0f);

    /* linear: halfway between the two centres is halfway between the texels,
     * and at a centre the texel itself */
    words(w, GXM_TEX_LINEAR, 0x00000000u, 2, 1, 1, 1, 2);
    open_texture(w);
    at[0] = 0.5f;
    gxm_tex_sample(&t, &cache, at, 0.0f, c);
    assert(fabsf(c[0] - 0.5f) < 1e-6f);
    at[0] = 0.75f;
    gxm_tex_sample(&t, &cache, at, 0.0f, c);
    assert(c[0] == 1.0f);
    /* clamped: beyond the edge is the edge */
    at[0] = 3.0f;
    gxm_tex_sample(&t, &cache, at, 0.0f, c);
    assert(c[0] == 1.0f);
    /* repeated: beyond the edge is the other side */
    words(w, GXM_TEX_LINEAR, 0x00000000u, 2, 1, 1, 0, 0);
    open_texture(w);
    at[0] = 1.25f;
    gxm_tex_sample(&t, &cache, at, 0.0f, c);
    assert(c[0] == 0.0f);
    at[0] = -0.25f;
    gxm_tex_sample(&t, &cache, at, 0.0f, c);
    assert(c[0] == 1.0f);
    /* mirrored: the second period runs backwards */
    words(w, GXM_TEX_LINEAR, 0x00000000u, 2, 1, 1, 0, 1);
    open_texture(w);
    at[0] = 1.25f;
    gxm_tex_sample(&t, &cache, at, 0.0f, c);
    assert(c[0] == 1.0f);

    /* a footprint of two texels a pixel is one level down */
    words(w, GXM_TEX_SWIZZLED, 0x00000000u, 4, 4, 3, 1, 2);
    open_texture(w);
    assert(fabsf(gxm_tex_footprint(&t, 0.5f, 0.0f, 0.0f, 0.5f) - 1.0f) < 1e-5f);
    assert(gxm_tex_footprint(&t, 0.0f, 0.0f, 0.0f, 0.0f) < 0.0f);
    memset(store, 0, sizeof(store));
    memset(store + 16, 255, 4);                         /* level 1 is white */
    at[0] = at[1] = 0.5f;
    gxm_tex_sample(&t, &cache, at, 0.0f, c);
    assert(c[0] == 0.0f);
    gxm_tex_sample(&t, &cache, at, 1.0f, c);
    assert(c[0] == 1.0f);

    /* the four texels around a point, and how far the point is between them */
    float four[4], fraction[2];
    memset(store, 0, sizeof(store));
    words(w, GXM_TEX_LINEAR, 0x00000000u, 8, 2, 1, 0, 2);
    open_texture(w);
    store[0] = 10; store[1] = 20; store[8] = 30; store[9] = 40;
    at[0] = 1.0f / 8.0f; at[1] = 0.5f;                  /* on the corner the four texels share */
    gxm_tex_gather(&t, &cache, at, four, fraction);
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
        gxm_tex_sample(&t, &cache, dirs[face], 0.0f, c);
        assert(fabsf(c[0] * 255.0f - (float)(40 * (face + 1))) < 0.01f);
    }
    printf("  cube maps                      ok\n");
}

/* --- the fast ways against the general way ------------------------------------------------
 * A sample has a general way, which asks for each texel by itself and works
 * each field out, and faster ways for what a scene mostly reads: a field
 * looked up in a table, a level read as rows of words. They have to give
 * the same bits. So: textures of the kinds a title samples, filled at
 * random; samples at random places and levels of detail, wrapped every way;
 * each taken both ways and compared. */

static uint64_t g_seed = 0x243F6A8885A308D3ull;
static uint32_t rnd(void) {
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17;
    return (uint32_t)(g_seed >> 16);
}
static float rnd_between(float lo, float hi) { return lo + (hi - lo) * (float)(rnd() & 0xFFFFFF) / 16777215.0f; }

static uint32_t plain_store[1 << 16];

/* Every level of a compressed texture decoded, as a draw keeps them. */
static void decode_levels(void) {
    uint32_t *at = plain_store;
    unsigned faces = t.cube ? 6 : 1;
    for (unsigned level = 0; level < t.levels; level++) {
        t.plain[level] = at;
        for (unsigned face = 0; face < faces; face++) {
            assert((size_t)(at - plain_store) + t.level_w[level] * t.level_h[level] <= sizeof(plain_store) / 4);
            gxm_tex_unpack(&t, face, level, 0, t.store_h[level], at);
            at += t.level_w[level] * t.level_h[level];
        }
    }
}

static void compare_ways(const char *what, unsigned rounds, int cube) {
    for (unsigned i = 0; i < rounds; i++) {
        float coord[3] = { rnd_between(-2.5f, 3.5f), rnd_between(-2.5f, 3.5f), rnd_between(-1.0f, 1.0f) };
        float lod = (i & 3) ? rnd_between(-2.0f, 9.0f) : 0.0f, fast[4], slow[4];
        if (cube) { coord[0] -= 0.5f; coord[1] -= 0.5f; }
        gxm_tex_plain_paths = 0; pixfmt_plain = 0;
        gxm_tex_sample(&t, &cache, coord, lod, fast);
        gxm_tex_plain_paths = 1; pixfmt_plain = 1;
        gxm_tex_cache_reset(&cache);
        gxm_tex_sample(&t, &cache, coord, lod, slow);
        gxm_tex_plain_paths = 0; pixfmt_plain = 0;
        if (memcmp(fast, slow, sizeof(fast))) {
            fprintf(stderr, "%s: at (%g, %g, %g) level %g the fast way gives %g %g %g %g, the general way %g %g %g %g\n", what,
                    (double)coord[0], (double)coord[1], (double)coord[2], (double)lod,
                    (double)fast[0], (double)fast[1], (double)fast[2], (double)fast[3],
                    (double)slow[0], (double)slow[1], (double)slow[2], (double)slow[3]);
            assert(0);
        }
    }
}

/* A row of samples is the same samples taken one at a time: for the
 * texture now open, sixty-four points at a time, with a level of detail
 * each or with none, a few of them not numbers at all. */
static void compare_row(const char *what, unsigned rounds, int cube) {
    enum { N = 64 };
    static float u[N], v[N], s[N], lod[N], out[4][N];
    const float *coord[3] = { u, v, s };
    float *rows[4] = { out[0], out[1], out[2], out[3] };
    for (unsigned round = 0; round < rounds; round++) {
        unsigned n = 1 + rnd() % N;
        int with_lod = (round & 1) != 0;
        for (unsigned i = 0; i < n; i++) {
            u[i] = rnd_between(-2.5f, 3.5f); v[i] = rnd_between(-2.5f, 3.5f); s[i] = rnd_between(-1.0f, 1.0f);
            if (cube) { u[i] -= 0.5f; v[i] -= 0.5f; }
            if (rnd() % 97 == 0) u[i] = v[i] - v[i] + (float)(1e30 * 1e30) * 0.0f;      /* not a number */
            lod[i] = rnd_between(-2.0f, 9.0f);
        }
        gxm_tex_cache_reset(&cache);
        gxm_tex_sample_row(&t, &cache, n, coord, with_lod ? lod : NULL, rows);
        for (unsigned i = 0; i < n; i++) {
            float point[3] = { u[i], v[i], s[i] }, one[4], row[4] = { out[0][i], out[1][i], out[2][i], out[3][i] };
            gxm_tex_sample(&t, &cache, point, with_lod ? lod[i] : 0.0f, one);
            if (memcmp(one, row, sizeof(one))) {
                fprintf(stderr, "%s: point %u of %u at (%g, %g, %g): in a row %g %g %g %g, by itself %g %g %g %g\n", what, i, n,
                        (double)u[i], (double)v[i], (double)s[i], (double)row[0], (double)row[1], (double)row[2], (double)row[3],
                        (double)one[0], (double)one[1], (double)one[2], (double)one[3]);
                assert(0);
            }
        }
    }
}

static void test_rows(void) {
    uint32_t w[4];
    for (size_t i = 0; i < sizeof(store); i++) store[i] = (uint8_t)rnd();
    /* bytes in rows, filtered, in levels blended; halves in tiles, one level; a cube in levels; blocks decoded */
    words(w, GXM_TEX_LINEAR, 0x0C000000u, 64, 32, 5, 1, 0);
    w[0] |= 1u << 9;
    open_texture(w);
    compare_row("bytes in rows, in levels", 300, 0);
    words(w, GXM_TEX_TILED, 0x1B000000u, 64, 32, 1, 0, 2);
    open_texture(w);
    compare_row("halves in tiles", 300, 0);
    words(w, GXM_TEX_LINEAR_STRIDED, 0x15000000u, 40, 12, 1, 1, 2);
    w[0] = (w[0] & ~((7u << 9) | (0x3FFu << 17) | (3u << 1))) | ((39u & 7) << 9) | ((39u >> 3) << 17);
    open_texture(w);
    compare_row("depth in rows of a given length", 300, 0);
    words(w, GXM_TEX_CUBE, 0x0C000000u, 16, 16, 3, 1, 2);
    w[0] |= 1u << 9;
    open_texture(w);
    compare_row("bytes on a cube, in levels", 300, 1);
    memset(&t, 0, sizeof(t));
    words(w, GXM_TEX_SWIZZLED, 0x87000000u, 64, 32, 6, 1, 0);
    w[0] |= 1u << 9;
    open_texture(w);
    decode_levels();
    compare_row("decoded blocks", 300, 0);
    memset(&t, 0, sizeof(t));
    printf("  a row and one at a time        ok\n");
}

static void test_fast_ways(void) {
    uint32_t w[4];
    for (size_t i = 0; i < sizeof(store); i++) store[i] = (uint8_t)rnd();

    /* four bytes a texel in rows, as a title's own colour buffers are: nearest
     * and filtered, every way of wrapping, plain and on the display's curve */
    for (uint32_t address = 0; address < 4; address++)
        for (uint32_t filter = 0; filter < 2; filter++)
            for (uint32_t gamma = 0; gamma < 2; gamma++) {
                words(w, GXM_TEX_LINEAR, 0x0C000000u | (address << 12), 37, 21, 1, filter, address);
                w[0] |= gamma << 27;
                open_texture(w);
                assert(t.bytes8 && t.kind == GXM_TEX_PLAIN);
                compare_ways("four bytes in rows", 4000, 0);
            }
    /* the same when not all of it can be read: the general way answers zero past the end, and so must the fast */
    words(w, GXM_TEX_LINEAR, 0x0C000000u, 64, 64, 1, 1, 0);
    open_texture(w);
    t.avail = 64 * 4 * 40;
    compare_ways("four bytes in rows, cut short", 4000, 0);

    /* a compressed texture with its levels decoded, as most of a scene's
     * materials are: all three block kinds that matter and a PVRTC, with
     * and without blending between levels */
    static const uint32_t PACKED[] = { 0x85000000u, 0x87000000u, 0x86000000u, 0x83000000u };
    for (size_t k = 0; k < sizeof(PACKED) / sizeof(PACKED[0]); k++)
        for (uint32_t mip = 0; mip < 2; mip++)
            for (uint32_t address = 0; address < 3; address++) {
                memset(&t, 0, sizeof(t));
                words(w, GXM_TEX_SWIZZLED, PACKED[k], 64, 32, 6, 1, address);
                w[0] |= mip << 9;
                open_texture(w);
                assert(gxm_tex_is_packed(&t) && t.levels == 6);
                decode_levels();
                compare_ways("decoded blocks", 4000, 0);
            }
    /* a cube of them */
    memset(&t, 0, sizeof(t));
    words(w, GXM_TEX_CUBE, 0x87000000u, 16, 16, 3, 1, 2);
    open_texture(w);
    decode_levels();
    compare_ways("a decoded cube", 4000, 1);
    memset(&t, 0, sizeof(t));

    /* and the formats whose fields are looked up: halves in tiles, the small
     * floats, signed bytes, depth beside a byte of something else, in rows of a given length */
    words(w, GXM_TEX_TILED, 0x1B000000u, 64, 32, 1, 1, 2);
    open_texture(w);
    compare_ways("halves in tiles", 4000, 0);
    words(w, GXM_TEX_LINEAR, 0x1A000000u, 48, 16, 1, 1, 2);
    open_texture(w);
    compare_ways("small floats", 4000, 0);
    words(w, GXM_TEX_LINEAR, 0x0D000000u, 48, 16, 1, 1, 0);
    open_texture(w);
    compare_ways("signed bytes", 4000, 0);
    words(w, GXM_TEX_LINEAR_STRIDED, 0x15000000u, 40, 12, 1, 1, 2);
    w[0] = (w[0] & ~((7u << 9) | (0x3FFu << 17) | (3u << 1))) | ((39u & 7) << 9) | ((39u >> 3) << 17);
    open_texture(w);
    assert(t.stride == 160);
    compare_ways("depth in rows of a given length", 4000, 0);
    /* And a scene's own buffers read as textures, each in the layouts they
     * come in: small floats beside two bits, nearest and filtered; halves
     * on the faces of a cube; bytes swizzled and on a cube; one float; two
     * sixteen-bit fractions, which are scaled and not looked up; depth
     * beside a byte, in tiles. */
    for (uint32_t filter = 0; filter < 2; filter++) {
        words(w, GXM_TEX_LINEAR, 0x9A000000u, 45, 26, 1, filter, 2);
        open_texture(w);
        assert(t.quick == 1);
        compare_ways("small floats beside two bits", 4000, 0);
        words(w, GXM_TEX_LINEAR, 0x9A001000u, 45, 26, 1, filter, 0);
        open_texture(w);
        compare_ways("two bits beside small floats", 4000, 0);
        words(w, GXM_TEX_SWIZZLED, 0x0C000000u, 32, 16, 4, filter, 0);
        w[0] |= filter << 9;
        open_texture(w);
        assert(t.quick == 1 && t.levels == 4);
        compare_ways("bytes, swizzled, in levels", 4000, 0);
        words(w, GXM_TEX_SWIZZLED, 0x0C000000u, 32, 16, 1, filter, 0);
        w[0] |= 1u << 27;
        open_texture(w);
        assert(t.quick == 1 && t.gamma);
        compare_ways("bytes on the display's curve, swizzled", 4000, 0);
        words(w, GXM_TEX_TILED, 0x15000000u, 64, 64, 1, filter, 2);
        open_texture(w);
        assert(t.quick == 2);
        compare_ways("depth beside a byte, in tiles", 4000, 0);
        words(w, GXM_TEX_LINEAR, 0x0F000000u, 40, 24, 1, filter, 1);
        open_texture(w);
        assert(t.quick == 2);
        compare_ways("two sixteen-bit fractions", 4000, 0);
        words(w, GXM_TEX_LINEAR, 0x12000000u, 40, 24, 1, 0, 2);
        open_texture(w);
        compare_ways("one float", 4000, 0);
    }
    memset(&t, 0, sizeof(t));
    words(w, GXM_TEX_CUBE, 0x1B000000u, 16, 16, 1, 1, 2);
    open_texture(w);
    assert(t.quick == 1 && t.cube);
    compare_ways("halves on a cube", 4000, 1);
    words(w, GXM_TEX_CUBE, 0x0C000000u, 16, 16, 3, 1, 2);
    w[0] |= 1u << 9;
    open_texture(w);
    compare_ways("bytes on a cube, in levels", 4000, 1);
    /* cut short: the general way answers nothing past the end, and so must this */
    words(w, GXM_TEX_TILED, 0x1B000000u, 64, 64, 1, 1, 0);
    open_texture(w);
    t.avail = 64 * 8 * 40;
    compare_ways("halves in tiles, cut short", 4000, 0);
    memset(&t, 0, sizeof(t));
    printf("  the fast ways and the general  ok\n");
}

/* --- a movie's picture: brightness in a plane, the colour differences at half the size -------- */

/* A texel as the word of four bytes it was made from, red lowest. */
static uint32_t texel_word(int x, int y) {
    float c[4];
    uint32_t word = 0;
    gxm_tex_texel(&t, &cache, 0, 0, x, y, c);
    for (int k = 0; k < 4; k++) word |= (uint32_t)(c[k] * 255.0f + 0.5f) << (8 * k);
    return word;
}

static void test_planes(void) {
    static uint32_t unpacked[64 * 64];
    uint32_t w[4];
    float c[4];
    const pix_yuv *sd = pix_yuv_for(PIX_YUV_BT601), *hd = pix_yuv_for(PIX_YUV_BT709);

    /* Two planes, as a decoder leaves a picture and the console's player
     * hands it on, 12 by 6 and declared without a chain: a row is the width
     * rounded up to eight, and the differences begin where the six rows end. */
    memset(store, 0, sizeof(store));
    words(w, GXM_TEX_LINEAR, 0x90003000u, 12, 6, 16, 0, 2);                    /* "YVU", the second conversion */
    open_texture(w);
    assert(t.kind == GXM_TEX_YUV420 && t.levels == 1 && gxm_tex_is_packed(&t));
    assert(t.yuv_row == 16 && t.yuv_at == 16 * 6 && t.yuv_step == 1 && t.bytes == 16 * 6 + 16 * 3);
    /* texel (5, 3) has its own brightness and the differences of block (2, 1), the blue one first: BT.709's red */
    store[3 * 16 + 5] = 63;
    store[96 + 1 * 16 + 4] = 102;
    store[96 + 1 * 16 + 5] = 240;
    assert(texel_word(5, 3) == pix_yuv_word(hd, 63, 102, 240));
    gxm_tex_texel(&t, &cache, 0, 0, 5, 3, c);
    assert(c[0] > 0.99f && c[1] < 0.01f && c[2] < 0.01f && c[3] == 1.0f);
    /* the other three of the block share the differences and have their own brightness; the next block has its own of both */
    assert(texel_word(4, 3) == pix_yuv_word(hd, 0, 102, 240) && texel_word(5, 2) == pix_yuv_word(hd, 0, 102, 240));
    assert(texel_word(6, 3) == pix_yuv_word(hd, 0, 0, 0) && texel_word(5, 4) == pix_yuv_word(hd, 0, 0, 0));
    /* named the other way round, the same bytes are the red difference first: BT.709's blue, nearly */
    words(w, GXM_TEX_LINEAR, 0x90002000u, 12, 6, 16, 0, 2);                    /* "YUV" */
    open_texture(w);
    assert(texel_word(5, 3) == pix_yuv_word(hd, 63, 240, 102));
    /* and by the first conversion, BT.601, the same numbers are another colour */
    words(w, GXM_TEX_LINEAR, 0x90001000u, 12, 6, 16, 0, 2);
    open_texture(w);
    assert(texel_word(5, 3) == pix_yuv_word(sd, 63, 102, 240) && texel_word(5, 3) != pix_yuv_word(hd, 63, 102, 240));
    /* which a title may point elsewhere: the whole range, where 255 is white and 63 a dark grey */
    gxm_tex_yuv_profile[0] = PIX_YUV_BT601_FULL;
    open_texture(w);
    store[0] = 255; store[1] = 63; store[96] = store[97] = 128;
    assert(texel_word(0, 0) == 0xFFFFFFFFu && texel_word(1, 0) == 0xFF3F3F3Fu);
    gxm_tex_yuv_profile[0] = PIX_YUV_BT601;

    /* Three planes: the blue differences in a plane of half-length rows, then the red, or named the other way the red first */
    memset(store, 0, sizeof(store));
    words(w, GXM_TEX_LINEAR, 0x91002000u, 12, 6, 16, 0, 2);                    /* "YUV", three planes */
    open_texture(w);
    assert(t.kind == GXM_TEX_YUV420 && t.yuv_row == 16 && t.yuv_at == 96 && t.yuv_step == 8 * 3 && t.bytes == 96 + 2 * 24);
    store[3 * 16 + 5] = 63;
    store[96 + 1 * 8 + 2] = 102;
    store[96 + 24 + 1 * 8 + 2] = 240;
    assert(texel_word(5, 3) == pix_yuv_word(hd, 63, 102, 240) && texel_word(4, 2) == pix_yuv_word(hd, 0, 102, 240));
    words(w, GXM_TEX_LINEAR, 0x91003000u, 12, 6, 16, 0, 2);                    /* "YVU" */
    open_texture(w);
    assert(texel_word(5, 3) == pix_yuv_word(hd, 63, 240, 102));

    /* declared with a chain, the planes are as far apart as the sides rounded up to powers of two make them */
    words(w, GXM_TEX_LINEAR, 0x90003000u, 12, 6, 1, 0, 2);
    open_texture(w);
    assert(t.levels == 1 && t.yuv_row == 16 && t.yuv_at == 16 * 8 && t.bytes == 16 * 8 + 16 * 4);
    /* and in any layout but rows it is nothing this reads */
    words(w, GXM_TEX_SWIZZLED, 0x90003000u, 16, 8, 1, 0, 2);
    open_texture(w);
    assert(t.kind == GXM_TEX_UNKNOWN);

    /* A whole picture of noise, 37 by 21: decoded in rows it is what the
     * texels are one at a time, a filtered sample is the blend of four of
     * them, and cut short it is nothing past the cut. */
    for (size_t i = 0; i < sizeof(store); i++) store[i] = (uint8_t)rnd();
    for (uint32_t format = 0x90000000u; format <= 0x91003000u; format += format == 0x90003000u ? 0x00FFD000u : 0x1000u) {
        const int two = !(format & 0x01000000u), blue_first = two == (int)((format >> 12) & 1);
        const pix_yuv *m = pix_yuv_for(gxm_tex_yuv_profile[(format >> 13) & 1]);
        words(w, GXM_TEX_LINEAR, format, 37, 21, 16, 1, 2);
        open_texture(w);
        assert(t.kind == GXM_TEX_YUV420 && t.yuv_row == 40 && t.bytes == 40 * 21 + 40 * 11);
        gxm_tex_unpack(&t, 0, 0, 0, 11, unpacked);
        gxm_tex_unpack(&t, 0, 0, 11, 21, unpacked);
        for (int y = 0; y < 21; y++)
            for (int x = 0; x < 37; x++) {
                uint32_t first = two ? 840u + (uint32_t)(y / 2) * 40 + ((uint32_t)x & ~1u) : 840u + (uint32_t)(y / 2) * 20 + (uint32_t)x / 2;
                uint32_t second = first + (two ? 1u : 220u);
                uint32_t want = pix_yuv_word(m, store[y * 40 + x], store[blue_first ? first : second], store[blue_first ? second : first]);
                assert(texel_word(x, y) == want && unpacked[y * 37 + x] == want);
            }
        float uv[2] = { 10.0f / 37.0f, 10.0f / 21.0f }, blend[4], corner[4][4];
        gxm_tex_sample(&t, &cache, uv, 0.0f, blend);                    /* on the corner where four texels meet */
        gxm_tex_texel(&t, &cache, 0, 0, 9, 9, corner[0]);
        gxm_tex_texel(&t, &cache, 0, 0, 10, 9, corner[1]);
        gxm_tex_texel(&t, &cache, 0, 0, 9, 10, corner[2]);
        gxm_tex_texel(&t, &cache, 0, 0, 10, 10, corner[3]);
        for (int k = 0; k < 4; k++)
            assert(fabsf(blend[k] - (corner[0][k] + corner[1][k] + corner[2][k] + corner[3][k]) / 4.0f) < 1e-3f);
        /* with the decoded rows attached, as a renderer keeps them, the answers are the same */
        float direct[4], kept[4];
        gxm_tex_sample(&t, &cache, uv, 0.0f, direct);
        t.plain[0] = unpacked;
        gxm_tex_sample(&t, &cache, uv, 0.0f, kept);
        assert(!memcmp(direct, kept, sizeof(direct)));
        t.plain[0] = NULL;
        t.avail = two ? 840 + 40 * 5 : 840 + 220 + 20 * 5;              /* the differences of the first ten rows and no more */
        assert(texel_word(3, 9) != 0 && texel_word(3, 10) == 0);
        memset(unpacked, 0xAA, sizeof(unpacked));
        gxm_tex_unpack(&t, 0, 0, 0, 21, unpacked);
        assert(unpacked[9 * 37 + 3] == texel_word(3, 9) && unpacked[10 * 37 + 3] == 0);
    }
    memset(&t, 0, sizeof(t));
    printf("  a picture in planes            ok\n");
}

int main(void) {
    printf("gxm_tex:\n");
    test_layouts();
    test_blocks_and_palettes();
    test_planes();
    test_samples();
    test_cube();
    test_fast_ways();
    test_rows();
    printf("all gxm_tex tests passed\n");
    return 0;
}
