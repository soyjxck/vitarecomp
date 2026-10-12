/* gxm_surfaces.h — what the rest of the runtime says to the graphics card's
 * path (gxm_gpu.c) about the title's surfaces, and the one thing asked back.
 * Apart from gxm_gpu.h so that the graphics library and the display can
 * include it without the renderer's own records.
 */

#ifndef VITARECOMP_GXM_SURFACES_H
#define VITARECOMP_GXM_SURFACES_H

#include "gxm_draw.h"

#include <stddef.h>
#include <stdint.h>

/* While the card draws, what it has drawn stays on it, and the title's
 * memory does not hold a surface the card has drawn to until something off
 * the card asks. These are the askings.
 *
 * gxm_gpu_memory_wanted: these bytes are about to be read, or written, by
 * something that is not a list of the card's: the display, a dump. Any
 * surface the card holds newer than them goes back into them first.
 *
 * gxm_gpu_for_processor: the list in hand is the processor's to draw after
 * all. Every surface goes back, and the scene's depth into the planes.
 *
 * gxm_gpu_scene_begin: a scene starts, its planes made `depth` and
 * `stencil` throughout. `control` is its depth surface's five words, or
 * NULL for none, which the scene loads its depth from if `load` and keeps
 * its depth in if `store`. Returns 1 when the scene loads and the card
 * holds that surface: the planes are then left as they are and nothing is
 * read from memory.
 *
 * gxm_gpu_scene_end: it ends. Returns 1 when the scene keeps its depth and
 * the card holds it: nothing is to be written to memory. Otherwise the
 * planes are what the scene's depth is, fetched from the card if they
 * were not. */
void gxm_gpu_memory_wanted(const void *bytes, size_t count);

/* And one asking that is not for memory at all. The display's frame is
 * these bytes, `height` rows of `pitch` pixels of four bytes, red first,
 * `width` of each row to be seen: show it in the host's window (gpu.h says
 * what a window is). The card shows the surface it holds for those bytes
 * if it holds one newer than they are, and the bytes themselves if not;
 * nothing is sent back to memory on the frame's account. With no bytes the
 * window is made black. Returns 1 when the card showed it, and 0 when the
 * card is not drawing or shows nothing in windows: the host then shows
 * what memory holds, as it would have. */
int gxm_gpu_show(void *window, const void *bytes, uint32_t width, uint32_t height, uint32_t pitch);
void gxm_gpu_for_processor(const gxm_draw_state *s);
int gxm_gpu_scene_begin(const uint32_t control[5], uint32_t width, uint32_t height, uint32_t depth_bits, int load, int store,
                        float depth, uint8_t stencil);
int gxm_gpu_scene_end(int store, float *depth, uint8_t *stencil);

/* And one thing asked of the graphics library, which knows how a depth
 * surface of the title's is laid out: planes of depth and stencil,
 * `width` by `height`, into the surface these five words describe. */
void gxm_depth_surface_store(const uint32_t control[5], uint32_t width, uint32_t height, const float *depth, const uint8_t *stencil);

/* And the other way: the surface those five words describe, into planes of
 * depth, stencil and mask. For a scene whose depth the card's path left in
 * memory until a list looked at it. */
void gxm_depth_surface_load(const uint32_t control[5], uint32_t width, uint32_t height, float *depth, uint8_t *stencil, uint8_t *mask);

#endif /* VITARECOMP_GXM_SURFACES_H */
