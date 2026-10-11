/* raster.h — triangles to pixels, in software.
 *
 * The reference renderer's rasteriser: it knows nothing about the console.
 * It takes vertices in clip space with a row of varyings each, clips them,
 * divides, applies a viewport, culls, and walks every pixel whose centre a
 * triangle covers, handing the caller the pixel's depth and its varyings
 * interpolated with perspective. What a pixel becomes — the fragment
 * program, the texture reads, the blend, the store — is the caller's, in
 * the `shade` callback, so the same walk serves any pipeline put on top.
 *
 * Conventions: window coordinates have y down and pixel centres at half
 * integers; a pixel exactly on an edge belongs to the triangle whose top or
 * left edge it is; depth is whatever the viewport maps clip z/w to.
 */

#ifndef VITARECOMP_RASTER_H
#define VITARECOMP_RASTER_H

#include <stddef.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#define RASTER_MAX_VARYINGS 64

typedef struct {
    float pos[4];                           /* clip space: x, y, z, w */
    float var[RASTER_MAX_VARYINGS];
} raster_vertex;

enum { RASTER_CULL_NONE = 0, RASTER_CULL_CW, RASTER_CULL_CCW };

/* Which ends of the depth range triangles are cut at. */
enum { RASTER_CLIP_FAR = 1, RASTER_CLIP_NEAR = 2 };

/* Comparisons of the incoming depth against the stored one. */
enum {
    RASTER_NEVER = 0, RASTER_LESS, RASTER_EQUAL, RASTER_LESS_EQUAL,
    RASTER_GREATER, RASTER_NOT_EQUAL, RASTER_GREATER_EQUAL, RASTER_ALWAYS
};

/* One pixel a triangle covers, after the depth test passed. `clockwise` says
 * which way the triangle turns on the screen; `var` holds the varyings at
 * the pixel's centre; `ddx` and `ddy` their change per pixel across and
 * down, for choosing a mip level. Return 0 to discard the pixel: its depth
 * is then left as it was. */
typedef int (*raster_shade)(void *user, int x, int y, float depth, int clockwise,
                            const float *var, const float *ddx, const float *ddy);

typedef struct {
    /* the target */
    int    width, height;                   /* in pixels */
    int    clip_x0, clip_y0, clip_x1, clip_y1;   /* the rectangle drawn into: x0 <= x < x1 */
    float *depth;                           /* width * height, or NULL for no depth buffer */
    uint32_t depth_bits;                    /* the buffer keeps integers of so many bits: 0 for floats as they are */
    /* A byte a pixel, or NULL, that a pixel is turned away by before
     * anything else is asked of it: the pixel goes on when `stencil_ref`
     * compared with its byte, both under `stencil_mask`, satisfies
     * `stencil_func` (RASTER_*, the reference on the left). The plane is
     * only read. */
    const uint8_t *stencil;
    uint8_t stencil_func, stencil_ref, stencil_mask;

    /* the viewport: window = offset + scale * (clip / w) */
    float  x_offset, x_scale, y_offset, y_scale, z_offset, z_scale;

    int    cull;                            /* RASTER_CULL_*, by winding in window space */
    int    clip_z;                          /* RASTER_CLIP_*: cut where depth leaves 0..1, as well as at w */
    int    clamp_depth;                     /* what is not cut is held to 0..1 */
    int    depth_func;                      /* RASTER_* */
    int    depth_write;
    uint64_t derivatives;                   /* a bit for each varying whose ddx and ddy are wanted, in fours:
                                             * one of a four asked for, all of the four are given */
    int    varyings;                        /* floats per vertex in use */

    raster_shade shade;
    void  *user;

    /* counted for whoever asks */
    uint32_t triangles_in, triangles_drawn, pixels_shaded;
} raster;

/* A depth as a buffer of `bits`-bit integers keeps it: the nearest step,
 * as a float again. A surface of integers compares and keeps integers;
 * without this a surface stored and loaded again would never equal the
 * depth of the same geometry drawn a second time. */
static inline float raster_whole_depth(uint32_t bits, float depth) {
    if (!bits) return depth;
    /* The steps below depth * top + 1/2, held to those there are. Said so
     * that nothing is called: a number between nothing and the top is
     * rounded down by being made a whole number, and this is done for
     * every pixel a triangle covers. */
    double top = (double)((1u << bits) - 1), scaled = (double)depth * top + 0.5;
    if (!(scaled > 0.0)) return 0.0f;                   /* behind the nearest step, or no number at all */
    if (scaled >= top + 1.0) return 1.0f;
    return (float)((double)(uint32_t)scaled / top);
}

/* Draw one triangle. The vertices are not modified. */
void raster_triangle(raster *r, const raster_vertex *a, const raster_vertex *b, const raster_vertex *c);

/* The same in two steps, for a caller that walks a triangle more than once:
 * in bands of rows, on several threads. A triangle is set up once into as
 * many as RASTER_MAX_PIECES pieces (clipping can leave a polygon), each
 * with raster_plane_floats(r) floats of its own in `planes`; a piece is
 * then walked in any range of rows, and the walk changes nothing of the
 * raster's, so walks of different rows may run together. */
#define RASTER_MAX_PIECES 14

typedef struct {
    int      x0, y0, x1, y1;                /* the pixels it may cover, inclusive */
    int64_t  ax, ay, bx, by, cx, cy;        /* its corners, in sixteenths of a pixel, clockwise */
    uint8_t  clockwise;                     /* as it was given, before being turned */
    uint8_t  bias_ab, bias_bc, bias_ca;
    float    inv_area, za, zb, zc, qa, qb, qc, q_dx, q_dy;
} raster_piece;

static inline size_t raster_plane_floats(const raster *r) { return 5u * (size_t)r->varyings; }

/* Returns the number of pieces written, 0 when nothing of it is drawn. */
int raster_setup(raster *r, const raster_vertex *a, const raster_vertex *b, const raster_vertex *c,
                 raster_piece *pieces, float *planes);

/* Walk the pixels of a piece in rows row0 <= y < row1, calling `shade` for
 * each as raster_triangle does. Returns how many were handed to it. */
uint32_t raster_walk(const raster *r, const raster_piece *piece, const float *planes, int row0, int row1,
                     raster_shade shade, void *user);

/* The same kept to columns col0 <= x < col1 as well: a piece walked tile by
 * tile hands over each of its pixels once, with the values it would have
 * had in one walk. */
uint32_t raster_walk_rect(const raster *r, const raster_piece *piece, const float *planes, int row0, int row1,
                          int col0, int col1, raster_shade shade, void *user);

#endif /* VITARECOMP_RASTER_H */
