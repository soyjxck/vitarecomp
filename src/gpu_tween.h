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
 *
 * And a draw is the same thing as a draw of the frame before when both
 * are near the same place in the same list, with the same programs and
 * the same form of geometry, and something says they are one thing:
 * either both draw the same geometry on the card, or the vertices of both
 * are at hand and lie within half their own size of each other. Vertices
 * are at hand when the title's geometry was not kept on the card, and for
 * a small mesh that was (the vertices of those are kept here as they are
 * made): a title that writes what moves into two buffers turn and turn
 * about draws a sprite from another mesh each frame, and it is the same
 * sprite. Nearness in a list is not enough to go by. A town draws a row
 * of people with one program, each a rectangle with a shadow under it;
 * taken for one another they are drawn half way between where two of them
 * stand, and so is a wall that comes into view beside its like. So the
 * draws that are surest of theirs choose first, and one that has no such
 * draw, or has jumped, is drawn where the later frame has it. A single
 * sprite that shows another picture of itself is put half way all the
 * same, as the later frame shapes it.
 *
 * VITARECOMP_TWEEN=1 turns it on. VITARECOMP_TWEEN_DUMP=<directory> writes
 * each picture as it is shown, tween-NNNNNN-t.ppm for a frame between and
 * -r for a frame of the title's, NNNNNN the turn of the display it was
 * shown at (one a vertical blank); <directory>@FROM-TO writes those of the
 * turns FROM to TO alone. It is also how the frames are looked at with no
 * window. VITARECOMP_TWEEN_LOG=1 says once a second how many draws found
 * their draw of the frame before, and how many of those moved; 2 also
 * goes through a frame now and then draw by draw: what each was taken
 * for, what of it differed, and why one found nothing; 3 does so for four
 * frames running and names each draw's mesh, programs and texture, and
 * the numbers of it that may move as they were and as they are.
 *
 * A fragment program's matrices move as a vertex program's do. A title
 * that lights its picture in passes of its own keeps the camera there: a
 * light's volume is placed by a vertex program, and what makes a place of
 * a depth or a shadow of a place may be a fragment program's.
 * VITARECOMP_TWEEN_FRAGMENT=0 leaves those the later frame's.
 *
 * A frame between is the title's frame drawn a second time. Where the
 * card has not the time for both, the title would be made to wait and
 * run slow; so when the title's frames slow down, a second's worth are
 * drawn without, and if the title is faster for it none are made for a
 * while, and then they are tried again (gpu_tween.c, "giving way").
 *
 * And two things for finding what is wrong with a frame between.
 * VITARECOMP_TWEEN_STILL=1 puts nothing half way: a frame gone through
 * again is then the frame, byte for byte, or the going through is at
 * fault and not the moving. VITARECOMP_TWEEN_LISTS=A-B moves only the
 * draws of a frame's lists A to B, counted from 0 as the log's second
 * level names them, which finds the list whose moving does the harm.
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
 * to begin with its secondary attributes, a word each. `shape` is a number
 * for the form of its geometry as the title asked for it, how many indices
 * of what primitive, and never 0: two draws are the same thing only with
 * the same one, wherever their vertices lie. A draw nothing was said about
 * is drawn in a frame between as it is in the later frame. */
void gpu_tween_about(const gxp_program *vertex, const uint32_t buffers[GXP_BUFFERS], const uint32_t memory[8][4], uint32_t shape);

/* And its fragment program, said after the above and before the draw: its
 * buffers and the ranges of them its load instructions reach, as for the
 * vertex stage. A title that lights its picture in a pass of its own
 * keeps the camera there too: the matrices that make a place of a depth,
 * and a shadow's of a place, are a fragment program's, and a frame
 * between whose geometry is half way and whose lighting is not has its
 * shadows where the later frame's camera would see them. */
void gpu_tween_about_fragment(const gxp_program *fragment, const uint32_t buffers[GXP_BUFFERS], const uint32_t memory[8][4]);

/* The title has set the frame it shows: what was drawn since the last time
 * is a frame, whole. */
void gpu_tween_flip(void);

/* The pictures shown at the next `count` turns of the display, written
 * into a directory as VITARECOMP_TWEEN_DUMP writes them: asked for while a
 * program runs, by whoever is looking at it. */
void gpu_tween_pictures(const char *directory, unsigned count);

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
gpu_mesh *tween_mesh_make(const void *vertices, uint32_t vertex_count, uint32_t stride, const uint32_t *indices, uint32_t index_count);
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
#define gpu_mesh_make            tween_mesh_make
#define gpu_mesh_free            tween_mesh_free
#define gpu_convert              tween_convert
#define gpu_begin                tween_begin
#define gpu_draw                 tween_draw
#define gpu_end                  tween_end
#define gpu_show                 tween_show
#endif

#endif /* VITARECOMP_GPU_TWEEN_H */
