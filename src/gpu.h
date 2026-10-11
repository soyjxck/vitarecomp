/* gpu.h — a graphics card, as far as the renderer needs one.
 *
 * gxm_draw.c is the renderer: it draws a scene's list of draws on the
 * processor, and what it draws is the definition. This is the other way to
 * draw the same list: hand it to the host's card. Everything about the
 * console stays above this file (gxm_gpu.c: which programs, which textures,
 * what the registers hold); what is below knows shaders as text, textures
 * as rows of texels, and a draw as buffers and state.
 *
 * A surface is something the card draws to and keeps: colour, or depth and
 * stencil. It is filled from rows of memory and read back into them when
 * that is asked for and not otherwise, so what is drawn in one list is
 * there for the next without a journey through the title's memory. A list
 * is drawn between gpu_begin and gpu_end, to a colour surface, or none,
 * and a depth surface.
 *
 * One implementation: gpu_d3d11.c (Direct3D 11, Windows). gpu_none.c is
 * what is linked elsewhere: there is no card, and gpu_open says so.
 */

#ifndef VITARECOMP_GPU_H
#define VITARECOMP_GPU_H

#include <stddef.h>
#include <stdint.h>

/* How a texel or a pixel lies in memory. */
enum {
    GPU_NONE = 0,
    GPU_RGBA8, GPU_RGBA8_SRGB,      /* a byte each, red first */
    GPU_BGRA8, GPU_BGRA8_SRGB,      /* blue first */
    GPU_RGBA16F,                    /* four halves */
    GPU_RGBA32F,                    /* four floats */
    GPU_R32UI, GPU_RG32UI, GPU_RGBA32UI,    /* words, as they are */
    GPU_D16,                        /* depth, sixteen bits */
    GPU_D24S8,                      /* depth in the low twenty-four bits of a word, stencil above */
    GPU_D32FS8                      /* a float of depth, then a word whose low byte is stencil */
};

typedef struct gpu_shader gpu_shader;
typedef struct gpu_texture gpu_texture;

/* Find a card and get ready. 0, or -1 when there is none to draw with. */
int gpu_open(void);

/* What is being drawn with, for the log. */
const char *gpu_name(void);

/* Compile a program (gxp_hlsl.h). A vertex program says how many words of
 * primary attributes a vertex has. NULL with the compiler's complaint in
 * `log` when it does not compile. */
gpu_shader *gpu_shader_make(int fragment, const char *hlsl, unsigned pa_words, char *log, size_t log_size);
void gpu_shader_free(gpu_shader *s);

/* A texture of `levels` levels, each half the last and at least one texel
 * a side; six faces of each when it is a cube, which a program samples as
 * an array of six by the face's number. `images` are the levels in order,
 * and within a level the faces in order: rows of texels, no gaps. */
gpu_texture *gpu_texture_make(unsigned format, unsigned width, unsigned height, unsigned levels, int cube,
                              const void *const *images);
void gpu_texture_free(gpu_texture *t);

typedef struct {
    uint8_t min_linear, mag_linear;
    uint8_t mip;                    /* 0 the first level only, 1 the nearest level, 2 between two */
    uint8_t address_u, address_v;   /* 0 repeat, 1 mirror, 2 clamp, 3 mirror once and clamp */
    float   lod_bias, lod_min, lod_max;
} gpu_sampler;

/* What a face's stencil test does, in the console's numbering: a
 * comparison 0 never .. 7 always with the reference on the left; an
 * operation 0 keep, 1 zero, 2 replace, 3 and 4 up and down and stay, 5
 * invert, 6 and 7 up and down and wrap. */
typedef struct { uint8_t func, fail, depth_fail, pass; } gpu_stencil_face;

typedef struct {
    gpu_shader *vertex, *fragment;          /* no fragment program: depth and stencil only */
    const void *vertices;                   /* each a vertex's primary attributes, words */
    uint32_t    vertex_count, vertex_stride;
    const void *indices;
    uint32_t    index_count;
    uint8_t     index_size;                 /* 2 or 4 */
    uint8_t     strip;                      /* a strip of triangles; otherwise three indices a triangle */

    /* by stage, vertex then fragment: the constants (gxp_hlsl.h's cbuffer),
     * the words a load instruction can reach, the textures */
    const void *constants[2];
    uint32_t    constants_bytes[2];
    const uint32_t *memory[2];
    uint32_t    memory_words[2];
    gpu_texture *textures[2][16];
    gpu_sampler  samplers[2][16];
    uint8_t     reads_pixel;                /* the fragment program is given the target: 1 as it stood before
                                             * the draw, 2 itself, to read and write in order (gpu_ordered) */

    uint8_t     cull;                       /* 0 nothing, 1 what turns clockwise on the screen, 2 the other way */
    int32_t     scissor[4];                 /* x0, y0, and one past x1, y1 */
    uint8_t     depth_func, depth_write;    /* 0 never .. 7 always, the incoming depth on the left */
    uint8_t     stencil_read, stencil_write, stencil_ref;
    gpu_stencil_face stencil[2];            /* counter-clockwise faces, clockwise faces */

    /* blending, when `blend`: an operation for colour and one for alpha (1
     * source plus destination, 2 source less destination, 3 destination
     * less source, 4 the smaller, 5 the larger, 0 the source as it is), and
     * the factors of each (0 nothing, 1 one, 2 and 3 the source and what is
     * left of one after it, 4 and 5 the source's alpha likewise, 6 to 9 the
     * same of the destination, 10 the source's alpha held to the room left) */
    uint8_t     blend, colour_op, alpha_op, colour_src, colour_dst, alpha_src, alpha_dst;
    uint8_t     write_mask;                 /* red 1, green 2, blue 4, alpha 8 */
    uint8_t     count;                      /* the pixels that pass the tests are wanted */
} gpu_draw_call;

typedef struct gpu_surface gpu_surface;

/* A surface of one of the forms above, a colour's or a depth's, holding
 * nothing in particular yet. NULL when the card will not have one. */
gpu_surface *gpu_surface_make(unsigned format, uint32_t width, uint32_t height);
void gpu_surface_free(gpu_surface *s);

/* What it holds, from rows of memory and into them: rows from the top,
 * `pitch` bytes from one to the next, a pixel as the form lays it out.
 * 0 when it was done. Reading waits for the card to have drawn. */
int gpu_surface_put(gpu_surface *s, const void *rows, uint32_t pitch);
int gpu_surface_get(gpu_surface *s, void *rows, uint32_t pitch);

/* A depth surface made one depth and one stencil throughout. */
void gpu_surface_clear(gpu_surface *s, float depth, uint8_t stencil);

/* One surface's contents into another of the same form and size; or the
 * top left `width` by `height` of one into the top left of another of the
 * same form. 0 when done. */
int gpu_surface_copy(gpu_surface *into, gpu_surface *from);
int gpu_surface_copy_corner(gpu_surface *into, gpu_surface *from, uint32_t width, uint32_t height);

/* The same of depth, into a smaller depth surface from the top left of a
 * larger one of the same form: the depths as they are, and one stencil
 * throughout, a card having no way to hand stencil from one surface to
 * part of another. 0 when done. */
int gpu_surface_depth_corner(gpu_surface *into, gpu_surface *from, uint8_t stencil);

/* A texture of four floats a texel that holds nothing yet, for gpu_convert
 * to fill. */
gpu_texture *gpu_texture_blank(unsigned width, unsigned height);

/* Fill such a texture from a surface, on the card: `program` is a fragment
 * program, `float4 main(float4 pos : SV_Position) : SV_Target`, run once
 * for each texel of `into`. It finds the surface at t0 as the card keeps
 * it: a colour's four numbers, bytes as fractions of 255 with red first
 * whatever order the form has them in, and never on the display's curve;
 * words as whole numbers; a depth as one number from nothing to one, with
 * its stencil at t1, a whole number in the second place. `constants`, at
 * most 64 bytes, are its b0. This is how a surface one scene drew is read
 * by the next in another form than it was drawn in without leaving the
 * card. 0 when it was done. */
int gpu_convert(gpu_texture *into, gpu_surface *from, gpu_shader *program, const void *constants, uint32_t constants_bytes);

/* Whether a fragment program can be handed a target of this form to read
 * and to write for itself, a pixel at a time in the order the triangles
 * come. A program that blends for itself needs that when triangles of one
 * draw lie over each other: each has to find what the last one left. Not
 * every card has the means; without, such a program reads the target as
 * it stood before its draw. */
int gpu_ordered(unsigned format);

/* Start a list: what follows is drawn to these two, of one size. 0, or -1
 * when the card will not have it and nothing has been changed. */
int gpu_begin(gpu_surface *colour, gpu_surface *depth);

/* One draw. Returns its place among the draws of the list that asked for a
 * count, or -1. */
int gpu_draw(const gpu_draw_call *d);

/* Finish the list: the counts asked for, in order, which waits for the
 * card when there are any. 0 when every draw was taken. */
int gpu_end(uint64_t *counts, unsigned max_counts);

#endif /* VITARECOMP_GPU_H */
