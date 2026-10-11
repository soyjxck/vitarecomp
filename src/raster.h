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

    /* the viewport: window = offset + scale * (clip / w) */
    float  x_offset, x_scale, y_offset, y_scale, z_offset, z_scale;

    int    cull;                            /* RASTER_CULL_*, by winding in window space */
    int    clip_z;                          /* RASTER_CLIP_*: cut where depth leaves 0..1, as well as at w */
    int    clamp_depth;                     /* what is not cut is held to 0..1 */
    int    depth_func;                      /* RASTER_* */
    int    depth_write;
    int    want_derivatives;                /* fill ddx and ddy */
    int    varyings;                        /* floats per vertex in use */

    raster_shade shade;
    void  *user;

    /* counted for whoever asks */
    uint32_t triangles_in, triangles_drawn, pixels_shaded;
} raster;

/* Draw one triangle. The vertices are not modified. */
void raster_triangle(raster *r, const raster_vertex *a, const raster_vertex *b, const raster_vertex *c);

#endif /* VITARECOMP_RASTER_H */
