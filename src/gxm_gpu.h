/* gxm_gpu.h — a scene's list of draws, drawn by the host's graphics card.
 *
 * The renderer of gxm_draw.c is the definition and runs anywhere; it is
 * also a fifth of the speed the console has. This takes the same list and
 * draws it with the card: each program translated once to the card's
 * language (gxp_hlsl.h), each draw's vertices, uniforms and textures handed
 * over as they are, the console's state mapped to the card's. The target is
 * given to the card as the title's memory holds it and read back into it,
 * so that a list drawn this way leaves what a list drawn the other way
 * leaves, to within what two rasterisers and two sets of floating point
 * differ by, and the next list can be drawn either way.
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
 * has no equal for, a mask, a draw whose two faces test depth differently.
 */

#ifndef VITARECOMP_GXM_GPU_H
#define VITARECOMP_GXM_GPU_H

#include "gxm_draw_list.h"

enum { GXM_GPU_OFF = 0, GXM_GPU_DRAW, GXM_GPU_CHECK };

/* What was asked for, GXM_GPU_OFF when there is no card to do it with. */
int gxm_gpu_mode(void);

/* Draw a list. With `aside` clear, the target's memory and the scene's
 * planes are left as the list makes them, each counted draw's `pixels` is
 * what passed its tests, and 1 is returned; or nothing is touched and 0 is
 * returned. With `aside` set nothing of the title's is touched either way
 * and the result is kept for gxm_gpu_compare. */
int gxm_gpu_list(queued **list, uint32_t count, int aside);

/* After the same list has been drawn on the processor: say where what was
 * kept aside differs from what is there now. */
void gxm_gpu_compare(queued **list, uint32_t count);

#endif /* VITARECOMP_GXM_GPU_H */
