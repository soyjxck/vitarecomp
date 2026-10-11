/* gxm_tex.c — a texture, from its four control words to a filtered sample. See gxm_tex.h. */

#include "gxm_tex.h"
#include "texdec.h"

#include <math.h>
#include <string.h>

static uint32_t pow2_ceil(uint32_t v) { uint32_t p = 1; while (p < v) p <<= 1; return p; }
static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
static uint32_t at_least_1(uint32_t v) { return v ? v : 1; }

void gxm_tex_cache_reset(gxm_tex_cache *c) { memset(c->key, 0xFF, sizeof(c->key)); }

/* A byte as a fraction, and as light when it was kept on the display's
 * curve: the two conversions nearly every texel goes through, looked up. */
static float g_unorm8[256], g_srgb8[256], g_zero8[256], g_one8[256];

int gxm_tex_plain_paths;
int gxm_tex_one_level;

static void tables(void) {
    static int made;
    if (made) return;
    for (int i = 0; i < 256; i++) {
        g_unorm8[i] = (float)i / 255.0f;
        g_srgb8[i] = pix_srgb_to_linear(g_unorm8[i]);
        g_zero8[i] = 0.0f;
        g_one8[i] = 1.0f;
    }
    made = 1;
}

/* For a texel whose fields are whole bytes: which byte each channel is, and
 * the table that says what the byte stands for. A channel the format lacks
 * gets a table that says the same thing whatever the byte. */
static void channels(gxm_tex *t) {
    for (int c = 0; c < 4; c++) {
        int from = t->pf.from[c];
        /* light, for the colour channels of a texture kept on the curve: all three, or the two a two-channel format has */
        const float *table = t->gamma && (c < 2 || (c == 2 && t->gamma != 3)) ? g_srgb8 : g_unorm8;
        t->chan_table[c] = from == PIX_ZERO ? g_zero8 : from == PIX_ONE ? g_one8 : table;
        t->chan_shift[c] = from < 0 ? 0 : (uint8_t)(8 * from);
    }
}

/* --- a plain texel, quickly ---------------------------------------------------------------
 * Half the samples a deferred renderer takes are of its own buffers: depth
 * beside a byte, small floats, halves, signed bytes, in rows, in tiles, in
 * a rectangle of a larger surface. Each texel of those went through the
 * general reader, which asks what kind of texture it has every time, and
 * then through the general decoder, which asks what kind of field. For a
 * texture those answers do not change; they are worked out once, when its
 * control words are read. What comes out is what the general way gives. */

enum { Q_TABLE = 0, Q_SCALE, Q_BITS };
static const float g_zero1[1] = { 0.0f }, g_one1[1] = { 1.0f };

static void quick(gxm_tex *t) {
    t->quick = 0;
    if (t->kind != GXM_TEX_PLAIN || t->pf.bytes > 8 || t->pf.raw) return;
    if (t->gamma && !t->bytes8) return;                 /* the curve is in the byte tables and nowhere else */
    uint8_t all_tables = 1;
    for (int c = 0; c < 4; c++) {
        int from = t->pf.from[c];
        t->q[c].how = Q_TABLE; t->q[c].shift = 0; t->q[c].mask = 0; t->q[c].scale = 0.0f;
        if (from == PIX_ZERO) { t->q[c].table = g_zero1; continue; }
        if (from == PIX_ONE)  { t->q[c].table = g_one1; continue; }
        unsigned kind = t->pf.f[from].kind, bits = t->pf.f[from].bits;
        t->q[c].shift = t->pf.f[from].shift;
        t->q[c].mask = bits >= 32 ? 0xFFFFFFFFu : (1u << bits) - 1;
        if (t->bytes8) { t->q[c].table = t->chan_table[c]; continue; }      /* on the curve or off it, as channels() chose */
        t->q[c].table = pixfmt_table(kind, bits);
        if (t->q[c].table) continue;
        all_tables = 0;
        if (kind == PIX_UNORM && bits <= 32) {
            t->q[c].how = Q_SCALE;
            t->q[c].scale = bits >= 32 ? (float)0xFFFFFFFFull : (float)(uint64_t)t->q[c].mask;
        } else if (kind == PIX_F32 && bits == 32) {
            t->q[c].how = Q_BITS;
        } else {
            return;
        }
    }
    t->quick = all_tables ? 1 : 2;
}

/* Where texel (x, y) of a level of a plain texture lies, in bytes from its
 * first; the texel is within the level. */
static uint64_t plain_offset(const gxm_tex *t, unsigned face, unsigned level, uint32_t x, uint32_t y) {
    uint64_t at = (uint64_t)face * t->face_bytes + t->level_at[level];
    switch (t->type) {
    case GXM_TEX_LINEAR:         return at + ((uint64_t)y * t->store_w[level] + x) * t->unit_bytes;
    case GXM_TEX_LINEAR_STRIDED: return at + (uint64_t)y * t->stride + (uint64_t)x * t->unit_bytes;
    case GXM_TEX_TILED:
        return at + (uint64_t)(((y >> 5) * (t->store_w[level] / 32) + (x >> 5)) * 1024 + (y & 31) * 32 + (x & 31)) * t->unit_bytes;
    default:                     return at + (uint64_t)tex_morton_index(x, y, t->store_w[level], t->store_h[level]) * t->unit_bytes;
    }
}

static void quick_texel(const gxm_tex *t, unsigned face, unsigned level, int x, int y, float rgba[4]) {
    uint64_t at = plain_offset(t, face, level, (uint32_t)x, (uint32_t)y), word = 0;
    if (at + t->unit_bytes > t->avail) { rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.0f; return; }
    if (t->pf.bytes == 4) { uint32_t four; memcpy(&four, t->base + at, 4); word = four; }
    else if (t->pf.bytes == 8) memcpy(&word, t->base + at, 8);
    else memcpy(&word, t->base + at, t->pf.bytes);
    if (t->quick == 1) {
        for (int c = 0; c < 4; c++) rgba[c] = t->q[c].table[(uint32_t)(word >> t->q[c].shift) & t->q[c].mask];
        return;
    }
    for (int c = 0; c < 4; c++) {
        uint32_t field = (uint32_t)(word >> t->q[c].shift) & t->q[c].mask;
        if (t->q[c].how == Q_TABLE) rgba[c] = t->q[c].table[field];
        else if (t->q[c].how == Q_SCALE) rgba[c] = (float)field / t->q[c].scale;
        else memcpy(&rgba[c], &field, 4);
    }
}

/* A texel whose fields are whole bytes, through the tables. */
static void bytes_texel(const gxm_tex *t, uint32_t px, float rgba[4]) {
    for (int c = 0; c < 4; c++) rgba[c] = t->chan_table[c][(px >> t->chan_shift[c]) & 255];
}

/* --- the control words ------------------------------------------------------------ */

int gxm_tex_decode(gxm_tex *t, const uint32_t w[4]) {
    memset(t, 0, sizeof(*t));
    tables();

    t->type = w[1] >> 29;
    t->format = (w[1] & 0x1F000000u) | (w[0] & 0x80000000u) | (((w[3] >> 28) & 7) << 12);
    int power_of_two = t->type == GXM_TEX_SWIZZLED || t->type == GXM_TEX_CUBE;
    t->width  = power_of_two ? 1u << ((w[1] >> 16) & 0xF) : ((w[1] >> 12) & 0xFFF) + 1;
    t->height = power_of_two ? 1u << (w[1] & 0xF) : (w[1] & 0xFFF) + 1;
    t->data = w[2] & ~3u;
    t->palette = (w[3] & 0x03FFFFFFu) << 6;
    t->lod_min = (uint8_t)((w[2] & 3) | (((w[3] >> 26) & 3) << 2));
    t->v_mode = (w[0] >> 3) & 7;
    t->u_mode = (w[0] >> 6) & 7;
    t->mag_filter = (w[0] >> 12) & 3;
    t->gamma = (w[0] >> 27) & 3;
    t->cube = t->type == GXM_TEX_CUBE || t->type == GXM_TEX_CUBE_ARBITRARY;
    if (!t->data) return 0;

    uint32_t levels = 1;
    if (t->type == GXM_TEX_LINEAR_STRIDED) {
        /* the row length takes the bits the mip settings have elsewhere */
        uint32_t s = ((w[0] >> 9) & 7) | (((w[0] >> 17) & 0x3FF) << 3) | (((w[0] >> 1) & 3) << 13);
        t->stride = (s + 1) * 4;
        t->min_filter = t->mag_filter;
        t->no_chain = 1;
    } else {
        uint32_t field = (w[0] >> 17) & 0xF;
        t->mip_filter = (w[0] >> 9) & 1;
        t->min_filter = (w[0] >> 10) & 3;
        t->no_chain = field == 0xF;
        levels = (field + 1) & 15;
        if (!levels) levels = 1;
        t->lod_bias = (float)((int)((w[0] >> 21) & 0x3F) - 31) / 8.0f;
    }

    /* what a unit of storage is, and what it decodes to */
    unsigned swizzle = t->format & 0x7000u;
    t->unit_w = t->unit_h = 1;
    switch (t->format & 0x9F000000u) {
    case 0x85000000u: t->kind = GXM_TEX_BC1; t->unit_bytes = 8;  pixfmt_texture(0x0C000000u | swizzle, &t->pf); break;
    case 0x86000000u: t->kind = GXM_TEX_BC2; t->unit_bytes = 16; pixfmt_texture(0x0C000000u | swizzle, &t->pf); break;
    case 0x87000000u: t->kind = GXM_TEX_BC3; t->unit_bytes = 16; pixfmt_texture(0x0C000000u | swizzle, &t->pf); break;
    case 0x88000000u:
    case 0x89000000u: t->kind = GXM_TEX_BC4; t->unit_bytes = 8;  pixfmt_texture(0x00000000u | swizzle, &t->pf); break;
    case 0x8A000000u:
    case 0x8B000000u: t->kind = GXM_TEX_BC5; t->unit_bytes = 16; pixfmt_texture(0x07000000u | swizzle, &t->pf); break;
    case 0x80000000u: case 0x81000000u: case 0x82000000u: case 0x83000000u:
        t->kind = GXM_TEX_PVRTC; t->unit_bytes = 8;
        t->pvrtc_version = (t->format & 0x02000000u) ? 2 : 1;
        t->pvrtc_bits = (t->format & 0x01000000u) ? 4 : 2;
        pixfmt_texture(0x0C000000u | swizzle, &t->pf);
        break;
    case 0x94000000u: t->kind = GXM_TEX_PALETTE4; t->unit_bytes = 1; pixfmt_texture(0x0C000000u | swizzle, &t->pf); break;
    case 0x95000000u: t->kind = GXM_TEX_PALETTE8; t->unit_bytes = 1; pixfmt_texture(0x0C000000u | swizzle, &t->pf); break;
    default:
        if (pixfmt_texture(t->format, &t->pf)) { t->kind = GXM_TEX_PLAIN; t->unit_bytes = t->pf.bytes; }
        else { t->kind = GXM_TEX_UNKNOWN; t->unit_bytes = 1; levels = 1; }
        break;
    }
    if (t->kind >= GXM_TEX_BC1 && t->kind <= GXM_TEX_BC5) t->unit_w = t->unit_h = 4;
    t->bytes8 = t->pf.bytes <= 4;
    for (int i = 0; i < t->pf.fields; i++)
        if (t->pf.f[i].kind != PIX_UNORM || t->pf.f[i].bits != 8 || t->pf.f[i].shift != 8 * i) t->bytes8 = 0;
    if (t->kind == GXM_TEX_PVRTC) { t->unit_w = t->pvrtc_bits == 2 ? 8 : 4; t->unit_h = 4; }
    channels(t);
    quick(t);

    /* no more levels than the shorter side can be halved */
    uint32_t shorter = t->width < t->height ? t->width : t->height, most = 1;
    while ((shorter >> most) && most < GXM_TEX_LEVELS) most++;
    t->levels = levels < most ? levels : most;
    if (gxm_tex_one_level) t->levels = 1;

    /* Where each level lies. The room a level takes comes from the sides
     * rounded up to powers of two and halved; within it the texels are
     * addressed by the layout's own rule. */
    int rows = t->type == GXM_TEX_LINEAR || t->type == GXM_TEX_LINEAR_STRIDED, tiles = t->type == GXM_TEX_TILED;
    uint32_t layout_w = (rows && t->no_chain) ? t->width : pow2_ceil(t->width);
    uint32_t layout_h = (rows && t->no_chain) ? t->height : pow2_ceil(t->height);
    uint32_t aw = t->unit_w, ah = t->unit_h, unit_texels = (uint32_t)t->unit_w * t->unit_h, offset = 0, full = 0;
    if (rows && aw < 8) aw = 8;
    if (tiles) { aw = 32; ah = 32; }
    for (uint32_t n = 0; n < GXM_TEX_LEVELS; n++) {
        uint32_t lw = at_least_1(layout_w >> n), lh = at_least_1(layout_h >> n);
        uint32_t size = align_up(lw, aw) * align_up(lh, ah) / unit_texels * t->unit_bytes;
        if (t->kind == GXM_TEX_PALETTE4) size = (size + 1) / 2;
        /* a PVRTC level is a grid of words, never fewer than two each way in the first generation */
        uint32_t least = t->pvrtc_version == 1 ? 2 : 1;
        uint32_t gx = (lw + t->unit_w - 1) / t->unit_w, gy = (lh + t->unit_h - 1) / t->unit_h;
        if (gx < least) gx = least;
        if (gy < least) gy = least;
        if (t->kind == GXM_TEX_PVRTC) size = gx * gy * 8;
        if (n < t->levels) {
            t->level_w[n] = at_least_1(t->width >> n);
            t->level_h[n] = at_least_1(t->height >> n);
            t->level_at[n] = offset;
            if (rows)       { t->store_w[n] = align_up(t->level_w[n], 8) / t->unit_w; t->store_h[n] = align_up(t->level_h[n], t->unit_h) / t->unit_h; }
            else if (tiles) { t->store_w[n] = align_up(t->level_w[n], 32) / t->unit_w; t->store_h[n] = align_up(t->level_h[n], 32) / t->unit_h; }
            else            { t->store_w[n] = at_least_1(lw / t->unit_w); t->store_h[n] = at_least_1(lh / t->unit_h); }
            if (t->kind == GXM_TEX_PVRTC) { t->store_w[n] = gx; t->store_h[n] = gy; }
            offset += size;
        }
        /* a cube's faces are a whole chain apart, down to a side of one */
        if ((layout_w < layout_h ? layout_w : layout_h) >> n) full += size;
    }
    t->bytes = offset;
    if (t->type == GXM_TEX_LINEAR_STRIDED) t->bytes = t->stride * t->height;
    if (t->cube) {
        uint32_t bits = (uint32_t)t->unit_bytes * 8 / unit_texels;
        int block = unit_texels > 1;
        int large = (t->width >= 32 && t->height >= 32 && (bits <= 8 || block)) ||
                    (t->width >= 16 && t->height >= 16 && (bits == 16 || bits == 32)) ||
                    (t->width >= 8 && t->height >= 8 && bits == 64);
        t->face_bytes = t->no_chain ? align_up(offset, 4) : align_up(full, large ? 2048 : 4);
        t->bytes = 5 * t->face_bytes + offset;
    }
    return t->width && t->height;
}

/* --- one texel -------------------------------------------------------------------- */

static void decode_block(const gxm_tex *t, const uint8_t *block, uint32_t out[16]) {
    switch (t->kind) {
    case GXM_TEX_BC1: tex_decode_bc1(block, out); break;
    case GXM_TEX_BC2: tex_decode_bc2(block, out); break;
    case GXM_TEX_BC3: tex_decode_bc3(block, out); break;
    case GXM_TEX_BC4: tex_decode_bc4(block, out); break;
    default:          tex_decode_bc5(block, out); break;
    }
}

void gxm_tex_texel(const gxm_tex *t, gxm_tex_cache *cache, unsigned face, unsigned level, int x, int y, float rgba[4]) {
    uint32_t scratch[32], *block = scratch;
    rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.0f;
    if (!t->base || level >= t->levels || x < 0 || y < 0 ||
        (uint32_t)x >= t->level_w[level] || (uint32_t)y >= t->level_h[level]) return;
    if (t->kind == GXM_TEX_UNKNOWN) { rgba[0] = rgba[2] = rgba[3] = 1.0f; return; }    /* a format not read yet: shown, not hidden */
    if (t->plain[level]) {
        size_t w = t->level_w[level], h = t->level_h[level];
        bytes_texel(t, t->plain[level][((size_t)face * h + (size_t)y) * w + (size_t)x], rgba);
        return;
    }

    uint32_t ux = (uint32_t)x / t->unit_w, uy = (uint32_t)y / t->unit_h, index;
    uint64_t at = (uint64_t)face * t->face_bytes + t->level_at[level];
    if (t->kind == GXM_TEX_PVRTC) {
        /* a block is decoded from its level as a whole, which must all be there */
        uint32_t nx = t->store_w[level], ny = t->store_h[level], texel;
        uint64_t word = at + 8ull * tex_morton_index(ux, uy, nx, ny);
        if (at + 8ull * nx * ny > t->avail) return;
        uint32_t slot = (uint32_t)((word / 8) * 2654435761u) >> 26;
        if (cache) block = cache->texels[slot];
        if (!cache || cache->key[slot] != (uint32_t)word) {
            tex_decode_pvrtc(t->base + at, nx, ny, ux, uy, t->pvrtc_version, t->pvrtc_bits, block);
            if (cache) cache->key[slot] = (uint32_t)word;
        }
        texel = block[((uint32_t)y & 3) * t->unit_w + ((uint32_t)x & (t->unit_w - 1u))];
        bytes_texel(t, texel, rgba);
        return;
    }
    switch (t->type) {
    case GXM_TEX_LINEAR:
        index = uy * t->store_w[level] + ux;
        break;
    case GXM_TEX_LINEAR_STRIDED:
        index = 0;
        at += (uint64_t)uy * t->stride + (uint64_t)ux * t->unit_bytes;
        break;
    case GXM_TEX_TILED:
        index = ((uy >> 5) * (t->store_w[level] / 32) + (ux >> 5)) * 1024 + (uy & 31) * 32 + (ux & 31);
        break;
    default:
        index = tex_morton_index(ux, uy, t->store_w[level], t->store_h[level]);
        break;
    }

    uint32_t texel;
    const uint8_t *bytes;
    if (t->kind == GXM_TEX_PALETTE4 || t->kind == GXM_TEX_PALETTE8) {
        uint32_t entry;
        at += t->kind == GXM_TEX_PALETTE4 ? index / 2 : index;
        if (!t->pal || at >= t->avail) return;
        entry = t->base[at];
        if (t->kind == GXM_TEX_PALETTE4) entry = (index & 1) ? entry >> 4 : entry & 15;
        bytes = t->pal + 4 * entry;
    } else if (t->kind == GXM_TEX_PLAIN) {
        if (t->type != GXM_TEX_LINEAR_STRIDED) at += (uint64_t)index * t->unit_bytes;
        if (at + t->unit_bytes > t->avail) return;
        bytes = t->base + at;
    } else {
        at += (uint64_t)index * t->unit_bytes;
        if (at + t->unit_bytes > t->avail) return;
        uint32_t slot = (uint32_t)((at / t->unit_bytes) * 2654435761u) >> 26;
        if (cache) block = cache->texels[slot];
        if (!cache || cache->key[slot] != (uint32_t)at) {
            decode_block(t, t->base + at, block);
            if (cache) cache->key[slot] = (uint32_t)at;
        }
        bytes_texel(t, block[(y & 3) * 4 + (x & 3)], rgba);
        return;
    }
    if (t->bytes8) {
        texel = 0;
        memcpy(&texel, bytes, t->pf.bytes);
        bytes_texel(t, texel, rgba);
        return;
    }
    pixfmt_decode(&t->pf, bytes, rgba);

    /* colours kept on the display's curve are handed over as light */
    if (t->gamma) {
        rgba[0] = pix_srgb_to_linear(rgba[0]);
        rgba[1] = pix_srgb_to_linear(rgba[1]);
        if (t->gamma != 3) rgba[2] = pix_srgb_to_linear(rgba[2]);
    }
}

int gxm_tex_is_packed(const gxm_tex *t) {
    return (t->kind >= GXM_TEX_BC1 && t->kind <= GXM_TEX_BC5) || t->kind == GXM_TEX_PVRTC;
}

void gxm_tex_unpack(const gxm_tex *t, unsigned face, unsigned level, uint32_t row0, uint32_t row1, uint32_t *out) {
    uint32_t w = t->level_w[level], h = t->level_h[level], nx = t->store_w[level], ny = t->store_h[level];
    uint64_t at = (uint64_t)face * t->face_bytes + t->level_at[level];
    uint32_t block[32];
    if (!gxm_tex_is_packed(t) || !t->base || level >= t->levels) return;
    if (row1 > ny) row1 = ny;
    int whole = at + (uint64_t)nx * ny * t->unit_bytes <= t->avail;        /* PVRTC reads around each block */
    for (uint32_t by = row0; by < row1; by++)
        for (uint32_t bx = 0; bx < nx; bx++) {
            uint32_t index = t->type == GXM_TEX_LINEAR ? by * nx + bx
                           : t->type == GXM_TEX_TILED ? ((by >> 5) * (nx / 32) + (bx >> 5)) * 1024 + (by & 31) * 32 + (bx & 31)
                           : tex_morton_index(bx, by, nx, ny);
            uint64_t unit = at + (uint64_t)index * t->unit_bytes;
            if (t->kind == GXM_TEX_PVRTC) {
                if (whole) tex_decode_pvrtc(t->base + at, nx, ny, bx, by, t->pvrtc_version, t->pvrtc_bits, block);
                else memset(block, 0, sizeof(block));
            } else if (unit + t->unit_bytes <= t->avail) {
                decode_block(t, t->base + unit, block);
            } else {
                memset(block, 0, sizeof(block));
            }
            for (uint32_t j = 0; j < t->unit_h; j++)
                for (uint32_t i = 0; i < t->unit_w; i++) {
                    uint32_t x = bx * t->unit_w + i, y = by * t->unit_h + j;
                    if (x < w && y < h) out[(size_t)y * w + x] = block[j * t->unit_w + i];
                }
        }
}

/* --- samples ---------------------------------------------------------------------- */

/* An integer coordinate brought into a level by the texture's address mode. */
static int wrap(int i, int n, int mode) {
    switch (mode) {
    case 0: case 4:                             /* repeat */
        if (!(n & (n - 1))) return i & (n - 1); /* the same, when the side is a power of two: no division */
        i %= n;
        return i < 0 ? i + n : i;
    case 1:                                     /* mirror: every other period runs backwards */
        i %= 2 * n;
        if (i < 0) i += 2 * n;
        return i < n ? i : 2 * n - 1 - i;
    case 3:                                     /* mirror once about zero, then clamp */
        if (i < 0) i = -1 - i;
        return i >= n ? n - 1 : i;
    default:                                    /* clamp */
        return i < 0 ? 0 : i >= n ? n - 1 : i;
    }
}

/* Where a level's texels lie, when they can be read as rows of four-byte
 * words with nothing asked of each: a compressed level that has been
 * decoded, or rows of four whole bytes a texel all of which are readable.
 * That is most of what a scene samples, its materials and its own colour
 * buffers. NULL for anything else, which goes texel by texel through
 * gxm_tex_texel. `stride` is in words. */
static const uint8_t *word_rows(const gxm_tex *t, unsigned face, unsigned level, uint32_t *stride) {
    uint32_t w = t->level_w[level], h = t->level_h[level];
    if (gxm_tex_plain_paths || level >= t->levels || !t->base || t->kind == GXM_TEX_UNKNOWN) return NULL;
    if (t->plain[level]) {
        *stride = w;
        return (const uint8_t *)(t->plain[level] + (size_t)face * h * w);
    }
    if (t->kind == GXM_TEX_PLAIN && t->type == GXM_TEX_LINEAR && t->bytes8 && t->pf.bytes == 4) {
        uint64_t at = (uint64_t)face * t->face_bytes + t->level_at[level];
        if (at + ((uint64_t)(h - 1) * t->store_w[level] + w) * 4 > t->avail) return NULL;
        *stride = t->store_w[level];
        return t->base + at;
    }
    return NULL;
}

static uint32_t word_at(const uint8_t *rows, uint32_t stride, int x, int y) {
    uint32_t word;
    memcpy(&word, rows + ((size_t)y * stride + (size_t)x) * 4, 4);
    return word;
}

/* A level as a sample sees it: everything about reading it that does not
 * depend on where the sample falls. A row of samples of one texture looks at
 * each level once. */
typedef struct {
    const gxm_tex *t;
    gxm_tex_cache *cache;
    unsigned face, level;
    int w, h, um, vm, quickly;
    const uint8_t *rows;
    uint32_t stride;
} level_view;

static void view_level(level_view *lv, const gxm_tex *t, gxm_tex_cache *cache, unsigned face, unsigned level) {
    lv->t = t; lv->cache = cache; lv->face = face; lv->level = level;
    lv->w = (int)t->level_w[level]; lv->h = (int)t->level_h[level];
    lv->um = t->cube ? 2 : t->u_mode; lv->vm = t->cube ? 2 : t->v_mode;
    lv->stride = 0;
    lv->rows = word_rows(t, face, level, &lv->stride);
    lv->quickly = !lv->rows && t->quick && !gxm_tex_plain_paths && t->base && level < t->levels;
}

/* Where a point lies among a level's texels has 256 steps to a texel and
 * no more, as it has in a graphics processor. A title that draws a target
 * of half a texture's size samples exactly on the line between two texels,
 * and which side of the line a float comes out on is the last bit of an
 * interpolation: with steps, the line is the line, and the point belongs
 * to the texel beyond it, here and on a host's card alike. */
#define TEXEL_STEPS 256.0f

static inline float in_steps(float texels) { return floorf(texels * TEXEL_STEPS + 0.5f) * (1.0f / TEXEL_STEPS); }

static inline void sample_view(const level_view *lv, float u, float v, int linear, float rgba[4]) {
    const gxm_tex *t = lv->t;
    int w = lv->w, h = lv->h, um = lv->um, vm = lv->vm;
    const uint8_t *rows = lv->rows;
    uint32_t stride = lv->stride;
    if (!linear) {
        int px = wrap((int)floorf(in_steps(u * (float)w)), w, um), py = wrap((int)floorf(in_steps(v * (float)h)), h, vm);
        if (rows) bytes_texel(t, word_at(rows, stride, px, py), rgba);
        else if (lv->quickly) quick_texel(t, lv->face, lv->level, px, py, rgba);
        else gxm_tex_texel(t, lv->cache, lv->face, lv->level, px, py, rgba);
        return;
    }
    float x = u * (float)w - 0.5f, y = v * (float)h - 0.5f;
    float fx0 = floorf(x), fy0 = floorf(y), fx = x - fx0, fy = y - fy0;
    int x0 = wrap((int)fx0, w, um), x1 = wrap((int)fx0 + 1, w, um);
    int y0 = wrap((int)fy0, h, vm), y1 = wrap((int)fy0 + 1, h, vm);
    if (rows) {
        /* the four words, and for each channel its four numbers and their blend */
        uint32_t wa = word_at(rows, stride, x0, y0), wb = word_at(rows, stride, x1, y0);
        uint32_t wc = word_at(rows, stride, x0, y1), wd = word_at(rows, stride, x1, y1);
        for (int i = 0; i < 4; i++) {
            const float *table = t->chan_table[i];
            unsigned shift = t->chan_shift[i];
            float ca = table[(wa >> shift) & 255], cb = table[(wb >> shift) & 255];
            float cc = table[(wc >> shift) & 255], cd = table[(wd >> shift) & 255];
            float top = ca + (cb - ca) * fx, bottom = cc + (cd - cc) * fx;
            rgba[i] = top + (bottom - top) * fy;
        }
        return;
    }
    float a[4], b[4], c[4], d[4];
    if (lv->quickly) {
        quick_texel(t, lv->face, lv->level, x0, y0, a);
        quick_texel(t, lv->face, lv->level, x1, y0, b);
        quick_texel(t, lv->face, lv->level, x0, y1, c);
        quick_texel(t, lv->face, lv->level, x1, y1, d);
    } else {
        gxm_tex_texel(t, lv->cache, lv->face, lv->level, x0, y0, a);
        gxm_tex_texel(t, lv->cache, lv->face, lv->level, x1, y0, b);
        gxm_tex_texel(t, lv->cache, lv->face, lv->level, x0, y1, c);
        gxm_tex_texel(t, lv->cache, lv->face, lv->level, x1, y1, d);
    }
    for (int i = 0; i < 4; i++) {
        float top = a[i] + (b[i] - a[i]) * fx, bottom = c[i] + (d[i] - c[i]) * fx;
        rgba[i] = top + (bottom - top) * fy;
    }
}

/* The view of level `n` of the face in hand: made the first time it is asked for. */
static inline const level_view *view_of(const gxm_tex *t, gxm_tex_cache *cache, unsigned face, level_view *views,
                                        uint32_t *made, unsigned n) {
    if (!((*made >> n) & 1)) { view_level(&views[n], t, cache, face, n); *made |= 1u << n; }
    return &views[n];
}

/* One sample of one face at (u, v): the level or the two levels it falls
 * on, the filter, the blend. */
static inline void sample_point(const gxm_tex *t, gxm_tex_cache *cache, unsigned face, level_view *views, uint32_t *made,
                                float u, float v, float lod, float rgba[4]) {
    if (u != u || v != v) { rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.0f; return; }
    /* keep the arithmetic of wrapping within what an int holds */
    if (u > 32768.0f || u < -32768.0f) u = 0.0f;
    if (v > 32768.0f || v < -32768.0f) v = 0.0f;

    float last = (float)(t->levels - 1), level = lod + t->lod_bias;
    if (!(level > (float)t->lod_min)) level = (float)t->lod_min;
    if (level > last) level = last;
    /* at or below the first level the texture is being enlarged */
    int filter = level > 0.0f ? t->min_filter : t->mag_filter;
    int linear = (filter == 1 || filter == 2) && !t->pf.raw;
    if (t->mip_filter && !t->pf.raw && level < last) {
        unsigned lower = (unsigned)level;
        float between = level - (float)lower, a[4], b[4];
        if (between > 0.0f) {
            sample_view(view_of(t, cache, face, views, made, lower), u, v, linear, a);
            sample_view(view_of(t, cache, face, views, made, lower + 1), u, v, linear, b);
            for (int i = 0; i < 4; i++) rgba[i] = a[i] + (b[i] - a[i]) * between;
            return;
        }
    }
    unsigned whole = (unsigned)(level + 0.5f);
    if (whole >= GXM_TEX_LEVELS) whole = GXM_TEX_LEVELS - 1;
    sample_view(view_of(t, cache, face, views, made, whole), u, v, linear, rgba);
}

/* The face of a cube a direction points at, and where on it. */
static void cube_face(const float *d, unsigned *face, float *u, float *v) {
    float ax = fabsf(d[0]), ay = fabsf(d[1]), az = fabsf(d[2]), ma, sc, tc;
    if (ax >= ay && ax >= az) {
        ma = ax;
        if (d[0] >= 0) { *face = 0; sc = -d[2]; tc = -d[1]; } else { *face = 1; sc = d[2]; tc = -d[1]; }
    } else if (ay >= az) {
        ma = ay;
        if (d[1] >= 0) { *face = 2; sc = d[0]; tc = d[2]; } else { *face = 3; sc = d[0]; tc = -d[2]; }
    } else {
        ma = az;
        if (d[2] >= 0) { *face = 4; sc = d[0]; tc = -d[1]; } else { *face = 5; sc = -d[0]; tc = -d[1]; }
    }
    if (!(ma > 0.0f)) { *face = 0; *u = *v = 0.5f; return; }
    *u = (sc / ma + 1.0f) * 0.5f;
    *v = (tc / ma + 1.0f) * 0.5f;
}

float gxm_tex_footprint(const gxm_tex *t, float dudx, float dvdx, float dudy, float dvdy) {
    float ax = dudx * (float)t->width, ay = dvdx * (float)t->height;
    float bx = dudy * (float)t->width, by = dvdy * (float)t->height;
    float a = ax * ax + ay * ay, b = bx * bx + by * by, most = a > b ? a : b;
    return most > 0.0f ? 0.5f * log2f(most) : -64.0f;
}

void gxm_tex_sample(const gxm_tex *t, gxm_tex_cache *cache, const float *coord, float lod, float rgba[4]) {
    level_view views[GXM_TEX_LEVELS];
    uint32_t made = 0;
    unsigned face = 0;
    float u = coord[0], v = coord[1];
    if (t->cube) cube_face(coord, &face, &u, &v);
    sample_point(t, cache, face, views, &made, u, v, lod, rgba);
}

void gxm_tex_sample_row(const gxm_tex *t, gxm_tex_cache *cache, unsigned n, const float *const coord[3],
                        const float *lod, float *const out[4]) {
    level_view views[GXM_TEX_LEVELS];
    uint32_t made = 0;
    unsigned face = 0, views_of = 0;
    float rgba[4];
    for (unsigned i = 0; i < n; i++) {
        float u = coord[0][i], v = coord[1][i];
        if (t->cube) {
            float direction[3] = { u, v, coord[2][i] };
            cube_face(direction, &face, &u, &v);
            if (face != views_of) { made = 0; views_of = face; }       /* the views are of one face */
        }
        sample_point(t, cache, face, views, &made, u, v, lod ? lod[i] : 0.0f, rgba);
        out[0][i] = rgba[0]; out[1][i] = rgba[1]; out[2][i] = rgba[2]; out[3][i] = rgba[3];
    }
}

void gxm_tex_level_floats(const gxm_tex *t, gxm_tex_cache *cache, unsigned face, unsigned level, float *out) {
    level_view lv;
    view_level(&lv, t, cache, face, level);
    for (int y = 0; y < lv.h; y++)
        for (int x = 0; x < lv.w; x++, out += 4) {
            if (lv.rows) bytes_texel(t, word_at(lv.rows, lv.stride, x, y), out);
            else if (lv.quickly) quick_texel(t, face, level, x, y, out);
            else gxm_tex_texel(t, cache, face, level, x, y, out);
        }
}

void gxm_tex_gather(const gxm_tex *t, gxm_tex_cache *cache, const float *coord, float texels[4], float fraction[2]) {
    int w = (int)t->level_w[0], h = (int)t->level_h[0];
    float u = coord[0], v = coord[1], rgba[4];
    if (u != u || v != v || u > 32768.0f || u < -32768.0f || v > 32768.0f || v < -32768.0f) u = v = 0.0f;
    float x = in_steps(u * (float)w - 0.5f), y = in_steps(v * (float)h - 0.5f), fx0 = floorf(x), fy0 = floorf(y);
    int x0 = wrap((int)fx0, w, t->u_mode), x1 = wrap((int)fx0 + 1, w, t->u_mode);
    int y0 = wrap((int)fy0, h, t->v_mode), y1 = wrap((int)fy0 + 1, h, t->v_mode);
    gxm_tex_texel(t, cache, 0, 0, x0, y1, rgba); texels[0] = rgba[0];
    gxm_tex_texel(t, cache, 0, 0, x1, y1, rgba); texels[1] = rgba[0];
    gxm_tex_texel(t, cache, 0, 0, x1, y0, rgba); texels[2] = rgba[0];
    gxm_tex_texel(t, cache, 0, 0, x0, y0, rgba); texels[3] = rgba[0];
    fraction[0] = x - fx0;
    fraction[1] = y - fy0;
}
