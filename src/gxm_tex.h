/* gxm_tex.h — a texture, from its four control words to a filtered sample.
 *
 * The console describes a texture in sixteen bytes: where the texels are,
 * how they are laid out (rows, 32-texel tiles, Morton order, six faces of a
 * cube), their format and channel order, and how to filter and wrap. This
 * reads that description and answers the two questions a renderer asks:
 * what is texel (x, y) of level n, and what does a sample at (u, v) return.
 *
 * It knows nothing of the guest's memory: the caller says where the bytes
 * are and how many may be read, and every read is checked against that.
 */

#ifndef VITARECOMP_GXM_TEX_H
#define VITARECOMP_GXM_TEX_H

#include "pixfmt.h"

#include <stdint.h>

#define GXM_TEX_LEVELS 13

/* The type field of the second control word. */
enum {
    GXM_TEX_SWIZZLED = 0, GXM_TEX_CUBE = 2, GXM_TEX_LINEAR = 3, GXM_TEX_TILED = 4,
    GXM_TEX_SWIZZLED_ARBITRARY = 5, GXM_TEX_LINEAR_STRIDED = 6, GXM_TEX_CUBE_ARBITRARY = 7
};

/* How the texels of a level are stored. */
enum { GXM_TEX_PLAIN = 0, GXM_TEX_BC1, GXM_TEX_BC2, GXM_TEX_BC3, GXM_TEX_BC4, GXM_TEX_BC5,
       GXM_TEX_PALETTE4, GXM_TEX_PALETTE8, GXM_TEX_PVRTC, GXM_TEX_UNKNOWN };

typedef struct {
    /* read from the control words */
    uint32_t type;                          /* GXM_TEX_SWIZZLED .. */
    uint32_t format;                        /* base and swizzle, as the SDK numbers them */
    uint32_t width, height;
    uint32_t levels;                        /* that may be read: 1 or more */
    uint32_t data, palette;                 /* guest addresses */
    uint32_t stride;                        /* bytes a row, for the strided kind */
    uint8_t  u_mode, v_mode, min_filter, mag_filter, mip_filter, gamma, lod_min, cube;
    float    lod_bias;                      /* in levels */

    /* worked out from them */
    uint8_t  kind;                          /* GXM_TEX_PLAIN .. */
    uint8_t  unit_w, unit_h, unit_bytes;    /* a unit is a texel, or a compressed block */
    uint8_t  no_chain;                      /* declared without a mip chain */
    uint8_t  pvrtc_version, pvrtc_bits;     /* which PVRTC: generation 1 or 2, 2 or 4 bits a texel */
    uint8_t  bytes8;                        /* every field of a texel is one unsigned byte */
    uint8_t  chan_shift[4];                 /* then, for red, green, blue and alpha: which byte of the texel, */
    const float *chan_table[4];             /* and the 256 numbers that byte can stand for */
    pixfmt   pf;                            /* a texel; for blocks and palettes, what they decode to */
    /* For a texel that is a plain run of bytes whose every channel is a
     * field with a table, a field scaled, a float as it stands or a
     * constant: how each channel is had from the texel read as one word.
     * `quick` is 1 when all four are lookups (a constant is a table of
     * one), 2 when some need their own arithmetic, 0 when the texel goes
     * through pixfmt_decode. */
    uint8_t  quick;
    struct { uint8_t shift, how; uint32_t mask; const float *table; float scale; } q[4];
    uint32_t level_w[GXM_TEX_LEVELS], level_h[GXM_TEX_LEVELS];      /* texels */
    uint32_t store_w[GXM_TEX_LEVELS], store_h[GXM_TEX_LEVELS];      /* units, as laid out */
    uint32_t level_at[GXM_TEX_LEVELS];      /* bytes from the start of a face */
    uint32_t face_bytes;                    /* from one cube face to the next */
    uint32_t bytes;                         /* all of it */

    /* supplied by the caller before any texel is read */
    const uint8_t *base;                    /* the bytes at `data` */
    uint32_t avail;                         /* how many of them may be read */
    const uint8_t *pal;                     /* 16 or 256 words at `palette`, or NULL */
    /* Optionally, for a compressed texture: its levels decoded, a word a
     * texel with red in the low byte, row by row, a cube's six faces one
     * after another (gxm_tex_unpack fills them). Texels are then read from
     * here and the compressed bytes are not looked at again. */
    const uint32_t *plain[GXM_TEX_LEVELS];
} gxm_tex;

/* The compressed blocks decoded lately, for whoever is reading a texture:
 * one of these to a reader, since several threads may read one texture. */
typedef struct {
    uint32_t key[64];                       /* which block each slot holds */
    uint32_t texels[64][32];
} gxm_tex_cache;

/* Set, every texel is fetched and converted by itself, through the general
 * path; clear, the common layouts are read as rows of words. For a test to
 * hold the second way to the first. */
extern int gxm_tex_plain_paths;

/* Forget what a cache holds: before it is used for another texture, or
 * when the texture's bytes may have changed. */
void gxm_tex_cache_reset(gxm_tex_cache *c);

/* Read the control words. Returns 0 when they do not describe a texture
 * (a zero size); the texture is otherwise ready for `base` to be set. */
int gxm_tex_decode(gxm_tex *t, const uint32_t words[4]);

/* Whether the format is one gxm_tex_unpack decodes: blocks of some kind. */
int gxm_tex_is_packed(const gxm_tex *t);

/* Decode rows of blocks row0 <= row < row1 of one face of one level into
 * `out`, which holds that face of the level whole: level_w by level_h
 * words, row by row. The rows of blocks of a level are store_h[level].
 * Different rows may be decoded by different threads. */
void gxm_tex_unpack(const gxm_tex *t, unsigned face, unsigned level, uint32_t row0, uint32_t row1, uint32_t *out);

/* One texel, unfiltered, in the channel order the format's swizzle gives.
 * Coordinates outside the level, or bytes outside `avail`, read as zero.
 * `cache` may be NULL, at the cost of decoding a compressed block for every
 * texel taken from it; here and below. */
void gxm_tex_texel(const gxm_tex *t, gxm_tex_cache *cache, unsigned face, unsigned level, int x, int y, float rgba[4]);

/* The level a sample would come from, from how fast the coordinate moves
 * per pixel across and down: log2 of the footprint in texels. */
float gxm_tex_footprint(const gxm_tex *t, float dudx, float dvdx, float dudy, float dvdy);

/* A sample: `coord` is (u, v), or a direction for a cube map; `lod` is the
 * footprint level before the texture's own bias. */
void gxm_tex_sample(const gxm_tex *t, gxm_tex_cache *cache, const float *coord, float lod, float rgba[4]);

/* The four texels a bilinear sample at (u, v) of level 0 would blend, first
 * channel only, in the order (x0,y1) (x1,y1) (x1,y0) (x0,y0), and the
 * fractions of the way from x0 to x1 and from y0 to y1. */
/* gxm_tex_sample for `n` points at once. Coordinate k of point i is
 * coord[k][i]: two of them, or the three of a direction for a cube. lod[i]
 * is its level of detail, or `lod` is NULL for none. Component c of point
 * i's sample is left in out[c][i]. Each point gets what gxm_tex_sample
 * gives it; what does not depend on the point is worked out once. */
void gxm_tex_sample_row(const gxm_tex *t, gxm_tex_cache *cache, unsigned n, const float *const coord[3],
                        const float *lod, float *const out[4]);

void gxm_tex_gather(const gxm_tex *t, gxm_tex_cache *cache, const float *coord, float texels[4], float fraction[2]);

#endif /* VITARECOMP_GXM_TEX_H */
