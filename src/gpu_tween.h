/* gpu_tween.h — the frame between two frames.
 *
 * A title that counts its time in frames and draws thirty of them a second
 * cannot be made to show sixty by hurrying it: it would run at twice its
 * speed. What can be shown between two of its frames is the frame that
 * lies half way: the later one's draws, with whatever moves put half way
 * back to where the earlier frame had it. Shown a sixtieth of a second
 * before the later frame, it makes sixty pictures a second of a title that
 * still takes thirty steps.
 *
 * This stands between gxm_gpu.c and the card. Whatever gxm_gpu.c asks of
 * the card that changes a surface is done as it is asked, and written
 * down. When the title has finished a frame, what was written down is
 * gone through again for a second set of surfaces, one beside each of the
 * card's: the same fillings, copies and lists, but each draw that is the
 * same thing as a draw of the frame before is given constants and
 * vertices half way between the two. The surface beside the one the title
 * shows then holds the frame between, and is shown first; the title's own
 * frame follows at the next vertical blank. Nothing the title can read is
 * touched: its own surfaces are drawn exactly as they are without this.
 *
 * What moves is told from what does not by what it is, not by how it
 * changes: a vertex program's matrices (a uniform of twelve floats or
 * more), whatever its secondary program works out, and a vertex's
 * position. A colour, a texture's offset or a texture coordinate stays
 * the later frame's: a sprite that changes its picture changes it whole.
 * And a draw is the same thing as a draw of the frame before when both
 * are in the same place in the same list with the same programs and the
 * same number of vertices, and nothing of them has moved further than
 * something moves in a thirtieth of a second; one that has no such draw,
 * or has jumped, is drawn where the later frame has it.
 *
 * VITARECOMP_TWEEN=1 turns it on. VITARECOMP_TWEEN_DUMP=<directory> writes
 * each picture as it is shown, tween-NNNNNN-t.ppm for a frame between and
 * -r for a frame of the title's, NNNNNN the turn of the display it was
 * shown at (one a vertical blank); <directory>@FROM-TO writes those of the
 * turns FROM to TO alone. It is also how the frames are looked at with no
 * window. VITARECOMP_TWEEN_LOG=1 says once a second how many draws found
 * their draw of the frame before, and how many of those moved.
 */

#ifndef VITARECOMP_GPU_TWEEN_H
#define VITARECOMP_GPU_TWEEN_H

#include "gpu.h"
#include "gxp.h"

/* Whether frames between are made. */
int gpu_tween_on(void);

/* What the draw about to be made is: its vertex program, the buffers bound
 * to its vertex stage, and the ranges of them its constants say a load
 * instruction reaches (each an address, its bytes, and its first word
 * among the draw's memory words). The vertex stage's constants are taken
 * to begin with its secondary attributes, a word each. A draw nothing was
 * said about is drawn in a frame between as it is in the later frame. */
void gpu_tween_about(const gxp_program *vertex, const uint32_t buffers[GXP_BUFFERS], const uint32_t memory[8][4]);

/* The title has set the frame it shows: what was drawn since the last time
 * is a frame, whole. */
void gpu_tween_flip(void);

/* The card's functions (gpu.h), by way of what writes them down. */
gpu_surface *tween_surface_make(unsigned format, uint32_t width, uint32_t height);
void tween_surface_free(gpu_surface *s);
int  tween_surface_put(gpu_surface *s, const void *rows, uint32_t pitch);
void tween_surface_clear(gpu_surface *s, float depth, uint8_t stencil);
int  tween_surface_copy(gpu_surface *into, gpu_surface *from);
int  tween_surface_copy_corner(gpu_surface *into, gpu_surface *from, uint32_t width, uint32_t height);
int  tween_surface_depth_corner(gpu_surface *into, gpu_surface *from, uint8_t stencil);
gpu_texture *tween_texture_blank(unsigned width, unsigned height);
void tween_texture_free(gpu_texture *t);
void tween_mesh_free(gpu_mesh *m);
int  tween_convert(gpu_texture *into, gpu_surface *from, gpu_shader *program, const void *constants, uint32_t constants_bytes);
int  tween_begin(gpu_surface *colour, gpu_surface *depth);
int  tween_draw(const gpu_draw_call *d);
int  tween_end(uint64_t *counts, unsigned max_counts);
int  tween_show(void *window, gpu_surface *s, int fresh);

/* gxm_gpu.c defines this before it includes the file, and from there on
 * its calls of the card are calls of the functions above. */
#ifdef GPU_TWEEN_CALLER
#define gpu_surface_make         tween_surface_make
#define gpu_surface_free         tween_surface_free
#define gpu_surface_put          tween_surface_put
#define gpu_surface_clear        tween_surface_clear
#define gpu_surface_copy         tween_surface_copy
#define gpu_surface_copy_corner  tween_surface_copy_corner
#define gpu_surface_depth_corner tween_surface_depth_corner
#define gpu_texture_blank        tween_texture_blank
#define gpu_texture_free         tween_texture_free
#define gpu_mesh_free            tween_mesh_free
#define gpu_convert              tween_convert
#define gpu_begin                tween_begin
#define gpu_draw                 tween_draw
#define gpu_end                  tween_end
#define gpu_show                 tween_show
#endif

#endif /* VITARECOMP_GPU_TWEEN_H */
