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
    pixfmt   pf;                            /* a texel; for blocks and palettes, what they decode to */
    uint32_t level_w[GXM_TEX_LEVELS], level_h[GXM_TEX_LEVELS];      /* texels */
    uint32_t store_w[GXM_TEX_LEVELS], store_h[GXM_TEX_LEVELS];      /* units, as laid out */
    uint32_t level_at[GXM_TEX_LEVELS];      /* bytes from the start of a face */
    uint32_t face_bytes;                    /* from one cube face to the next */
    uint32_t bytes;                         /* all of it */

    /* supplied by the caller before any texel is read */
    const uint8_t *base;                    /* the bytes at `data` */
    uint32_t avail;                         /* how many of them may be read */
    const uint8_t *pal;                     /* 16 or 256 words at `palette`, or NULL */

    /* the compressed block decoded last, per slot */
    uint32_t cache_key[64];
    uint32_t cache[64][32];
} gxm_tex;

/* Read the control words. Returns 0 when they do not describe a texture
 * (a zero size); the texture is otherwise ready for `base` to be set. */
int gxm_tex_decode(gxm_tex *t, const uint32_t words[4]);

/* One texel, unfiltered, in the channel order the format's swizzle gives.
 * Coordinates outside the level, or bytes outside `avail`, read as zero. */
void gxm_tex_texel(gxm_tex *t, unsigned face, unsigned level, int x, int y, float rgba[4]);

/* The level a sample would come from, from how fast the coordinate moves
 * per pixel across and down: log2 of the footprint in texels. */
float gxm_tex_footprint(const gxm_tex *t, float dudx, float dvdx, float dudy, float dvdy);

/* A sample: `coord` is (u, v), or a direction for a cube map; `lod` is the
 * footprint level before the texture's own bias. */
void gxm_tex_sample(gxm_tex *t, const float *coord, float lod, float rgba[4]);

/* The four texels a bilinear sample at (u, v) of level 0 would blend, first
 * channel only, in the order (x0,y1) (x1,y1) (x1,y0) (x0,y0), and the
 * fractions of the way from x0 to x1 and from y0 to y1. */
void gxm_tex_gather(gxm_tex *t, const float *coord, float texels[4], float fraction[2]);

#endif /* VITARECOMP_GXM_TEX_H */
