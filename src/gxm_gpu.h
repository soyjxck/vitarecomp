/* gxm_gpu.h — a scene's list of draws, drawn by the host's graphics card.
 *
 * The renderer of gxm_draw.c is the definition and runs anywhere; it is
 * also a fifth of the speed the console has. This takes the same list and
 * draws it with the card: each program translated once to the card's
 * language (gxp_hlsl.h), each draw's vertices, uniforms and textures handed
 * over as they are, the console's state mapped to the card's. What a list
 * draws is what a list drawn the other way draws, to within what two
 * rasterisers and two sets of floating point differ by; but it is left on
 * the card, and reaches the title's memory when something off the card
 * asks for it (gxm_surfaces.h), so the next list can still be drawn either
 * way.
 *
 * VITARECOMP_GXM_GPU says which:
 *   unset, 0   every list on the processor
 *   1          a list on the card when everything in it is something the
 *              card's path can do; otherwise on the processor, and the
 *              reason said once
 *   check      every list on the processor, as when unset; and those the
 *              card can do drawn there too, aside, and the two results
 *              compared: where they differ is reported, with the draws
 *
 * What the card's path does not do yet is refused a list at a time: lines
 * and points (nor does the other), instanced draws, a surface format it
 * has no equal for, a mask, a draw whose two faces test depth differently,
 * a stencil test beside sixteen bits of depth.
 */

#ifndef VITARECOMP_GXM_GPU_H
#define VITARECOMP_GXM_GPU_H

#include "gxm_draw_list.h"
#include "gxm_surfaces.h"

#include <stddef.h>

enum { GXM_GPU_OFF = 0, GXM_GPU_DRAW, GXM_GPU_CHECK };

/* What was asked for, GXM_GPU_OFF when there is no card to do it with. */
int gxm_gpu_mode(void);

/* Draw a list. With `aside` clear, the list is drawn to the card's own
 * copies of its target and its scene's depth, each counted draw's `pixels`
 * is what passed its tests, and 1 is returned; or 0 is returned and the
 * list is the processor's to draw, after gxm_gpu_for_processor. With
 * `aside` set nothing of the title's is touched either way: the list is
 * drawn from what memory and the planes hold, and the result kept for
 * gxm_gpu_compare. */
int gxm_gpu_list(queued **list, uint32_t count, int aside);

/* After the same list has been drawn on the processor: say where what was
 * kept aside differs from what is there now. */
void gxm_gpu_compare(queued **list, uint32_t count);

/* What the path cost since it was last asked, a line for the stage profile
 * (VITARECOMP_GXM_PROFILE), over so many frames. Nothing when no list went
 * to the card. */
void gxm_gpu_profile(uint32_t frames);

#endif /* VITARECOMP_GXM_GPU_H */
