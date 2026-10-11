/* gxm_draw_list.h — a scene's list of draws, as the renderer keeps it.
 *
 * gxm_draw.c makes the list and draws it on the processor. gxm_gpu.c draws
 * the same list on the host's card, from the same records: a draw's state
 * as it stood, its two stages with their secondary attributes already
 * worked out and their textures read, where its vertices and indices are.
 * These are the records, and nothing else of gxm_draw.c is anyone's.
 */

#ifndef VITARECOMP_GXM_DRAW_LIST_H
#define VITARECOMP_GXM_DRAW_LIST_H

#include "gxm_draw.h"
#include "gxm_tex.h"
#include "pixfmt.h"
#include "raster.h"
#include "usse.h"

/* One stage of a draw: a program, its uniforms, its textures. */
typedef struct {
    const gxp_program *p;
    uint32_t  sa[USSE_WORDS];
    const gxm_tex *tex[GXP_TEXTURE_UNITS];  /* by unit; NULL where nothing usable is bound */
    gxm_tex_cache *cache;                   /* one a unit, belonging to whichever thread runs this */
    gxm_draw_counters *counters;            /* likewise */
    usse_env  env;
    usse_state st;
} stage;

/* A draw: its two stages, and what was worked out when it was made. */
typedef struct {
    gxm_draw_state *s;
    stage  v, f;                            /* f is the pattern each thread's fragment stage is copied from */
    uint32_t serial;                        /* which draw this is, counting from the first */

    /* a vertex program's inputs, resolved */
    struct { uint32_t base, stride; uint16_t reg; uint8_t format, components, per_instance, type, wanted, raw; } in[16];
    uint32_t inputs;

    /* which of the vertex program's outputs travel to the fragment program,
     * four floats each, and where in a vertex's row of varyings */
    int slot_at[GXP_SLOTS];
    int w_at;                               /* the clip-space w, for programs that ask for the position */
    int varyings;

    /* how the pixels go through the fragment program */
    int wide;                               /* a row of them at a time */
    int wide_vertices;                      /* and the vertices through theirs the same way */
    int late;                               /* the program can discard: the tests come after it */
    int each;                               /* the program reads the pixel: no triangle waits on another */
    const pixfmt_halves *stored;            /* its results go into pixels by lookup (pixfmt.h); NULL: write_colour */
} draw;

/* A draw on the list. */
typedef struct {
    gxm_draw_state s;                       /* as it stood when the draw was made */
    unsigned primitive, index_size;
    uint32_t indices, count, instances;
    uint32_t tally;                         /* the title's word that counts what the draw put on the screen, or 0 */
    int      tally_set;                     /* the word is set to 1 rather than added to */
    draw     d;
    gxm_tex  tex[2][GXP_TEXTURE_UNITS];     /* the textures of its two stages */
    raster   r;
    raster_piece *pieces;                   /* its triangles, set up: each piece, and its planes at the same place */
    float   *planes;
    uint32_t piece_count, pieces_cap;
    size_t   planes_cap;
    uint32_t pixels;
    float    first[4][4];                   /* where its first vertices came out */
    unsigned first_count;
} queued;

/* A vertex's attributes into the primary attribute registers `pa`, which
 * hold nothing yet: each read from its stream in the stream's form and
 * packed as the program's parameter says. */
void gxm_draw_vertex_words(const draw *d, uint32_t index, uint32_t instance, uint32_t *pa);

#endif /* VITARECOMP_GXM_DRAW_LIST_H */
