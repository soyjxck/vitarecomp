/* gxm_draw.h — one draw call, from index buffer to pixels.
 *
 * The graphics library's state layer (hle_gxm.c) collects what a draw
 * consumes into a gxm_draw_state and calls gxm_draw. Everything between is
 * here: fetch the vertices, run the vertex program on each, assemble and
 * rasterise triangles, and for every pixel covered run the tests, the
 * fragment program and the blend, and store the result in the colour
 * surface's own format. It is a reference renderer: one sample a pixel,
 * every program interpreted. It is meant to be right before it is fast, and
 * what it does for speed changes no pixel: a draw's rows are shared out
 * among threads, and a fragment program is run for a row of pixels at a
 * time where it is one that can be.
 */

#ifndef VITARECOMP_GXM_DRAW_H
#define VITARECOMP_GXM_DRAW_H

#include "gxp.h"
#include "pixfmt.h"

#include <stdint.h>

typedef struct { uint16_t stream, offset; uint8_t format, components; uint16_t reg; } gxm_attribute;
typedef struct { uint16_t stride, index_source; } gxm_stream;

/* A vertex or fragment program as the shader patcher made it: the compiled
 * program, and what was fixed when it was created. */
typedef struct {
    uint32_t      header;                   /* the GXP, in guest memory */
    uint64_t      name;                     /* a hash of its bytes: what a dump of it is called */
    int           ok;                       /* read, and all of its code can be run */
    int           wide;                     /* and its main part for a row of pixels at once (usse.h) */
    gxp_program   gxp;
    gxm_attribute attributes[16];           /* a vertex program's inputs */
    uint32_t      attribute_count;
    gxm_stream    streams[16];
    uint32_t      stream_count;
    uint8_t       blend[4];                 /* a fragment program's: mask, functions, factors */
    uint32_t      output_format;
} gxm_program;

/* The state that exists once for front faces and once for back faces. */
typedef struct {
    uint8_t depth_func, depth_write;
    uint8_t stencil_func, stencil_fail, depth_fail, depth_pass;
    uint8_t compare_mask, write_mask, stencil_ref;
    uint8_t program_off;                    /* depth and stencil only */
} gxm_face;

enum { GXM_SURFACE_LINEAR = 0, GXM_SURFACE_TILED, GXM_SURFACE_SWIZZLED };
enum { GXM_REGION_NONE = 0, GXM_REGION_ALL, GXM_REGION_OUTSIDE, GXM_REGION_INSIDE };

typedef struct {
    /* the scene: where pixels go */
    uint32_t width, height;                 /* the render target, in pixels */
    int      has_colour;
    pixfmt   colour;
    uint32_t colour_layout;                 /* GXM_SURFACE_* */
    uint32_t colour_stride;                 /* pixels a row */
    uint8_t *colour_data;
    uint32_t colour_avail;                  /* bytes that may be written from colour_data */
    uint8_t  colour_gamma;
    float   *depth;                         /* width * height of each */
    uint32_t depth_bits;                    /* depth is kept as an integer of so many bits; 0 for a float */
    uint8_t *stencil, *mask;

    /* raster state */
    int      region_mode;                   /* GXM_REGION_* */
    uint32_t region[4];                     /* x and y of the first and of the last pixel */
    int      viewport_on;
    float    offset[3], scale[3];
    int      cull;                          /* 0 none, 1 clockwise, 2 the other way */
    int      two_sided;
    gxm_face face[2];                       /* front, back */

    /* bindings */
    const gxm_program *vertex, *fragment;
    uint32_t streams[16];
    uint32_t vertex_buffers[GXP_BUFFERS], fragment_buffers[GXP_BUFFERS];   /* the last is the default buffer */
    uint32_t vertex_textures[GXP_TEXTURE_UNITS][4], fragment_textures[GXP_TEXTURE_UNITS][4];
} gxm_draw_state;

/* What was drawn and what could not be, since the counters were last
 * cleared. */
typedef struct {
    uint32_t draws, triangles, pixels;
    uint32_t not_triangles;                 /* draws of lines or points, not drawn */
    uint32_t no_program;                    /* draws whose programs could not be run */
    uint32_t stopped;                       /* program runs that met an instruction they could not execute */
    uint32_t stopped_at;                    /* the last such instruction's position, from 1 */
    uint32_t no_texture;                    /* samples of a unit with nothing usable bound */
} gxm_draw_counters;
extern gxm_draw_counters gxm_counters;

enum { GXM_TRIANGLES = 0, GXM_LINES, GXM_POINTS, GXM_TRIANGLE_STRIP, GXM_TRIANGLE_FAN, GXM_TRIANGLE_EDGES };

/* Draw. `index_size` is 2 or 4; `count` indices are used for each of
 * `instances` instances. Returns how many pixels passed the depth and
 * stencil tests. The pixels are shaded on the threads of pool.h. */
uint32_t gxm_draw(gxm_draw_state *s, unsigned primitive, unsigned index_size, uint32_t indices,
                  uint32_t count, uint32_t instances);

/* A frame has been shown. Compressed textures are kept decoded from one
 * draw to the next, and looked at again for changes once a frame. */
void gxm_draw_frame(void);

/* Where pixel (x, y) of a surface lies, in bytes from its start. */
uint32_t gxm_pixel_offset(uint32_t layout, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                          uint32_t stride, uint32_t bytes);

#endif /* VITARECOMP_GXM_DRAW_H */
