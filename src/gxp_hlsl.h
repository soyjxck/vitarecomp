/* gxp_hlsl.h — a shader program in a host GPU's language.
 *
 * usse.c runs the console's bytecode; this writes it out again as HLSL, so
 * that a graphics card runs it. It is a translation of the interpreter and
 * not of the bytecode's intent: the registers stay what they are there,
 * banks of 32-bit words, and every instruction becomes the statements the
 * interpreter's `execute` performs for it, a load of each operand in the
 * form the operand names, the arithmetic, a store. A register written as
 * two halves and read back as a word, or as four bytes, comes out as it
 * does on the console because nothing here ever decides what a register
 * "is". What the card's compiler makes of the bit traffic is its business.
 *
 * A program is translated for what is bound when it is used, because the
 * bytecode leaves two things to the hardware that depend on it: a sample
 * arrives in the registers in a form the texture's own format chooses, and
 * a fragment program's result leaves in the form the colour surface takes.
 *
 * The text expects (the back end that compiles it supplies them):
 *
 *   cbuffer, slot 0     uint4 SAv[66]   the secondary attributes after the
 *                                       program's secondary part has run,
 *                                       which is done once a draw, on the
 *                                       processor, by the interpreter
 *                       float4 VP[3]    scale, offset, (2/width, 2/height)
 *                       uint4 MEMR[8]   memory a load instruction can
 *                                       reach: first address, bytes, and
 *                                       where its words start in MEM
 *   Buffer<uint> MEM, t16               those words
 *   Texture2D or TextureCube TEXn, tn; SamplerState SMPn, sn
 *   Texture2D NOW, t17                  the colour surface as it stands,
 *                                       for a program that reads the pixel
 *                                       it writes: as four floats, or as
 *                                       four words (Texture2D<uint4>)
 *
 * A vertex program takes its primary attributes as they would lie in the
 * registers, four words to an input (semantic PA, index n), and leaves the
 * position for a viewport of the whole target and every interpolated
 * quantity it writes as four floats (TEXCOORDn, n the slot of gxp.h). A
 * fragment program takes the same quantities and returns its colour, or
 * the words of its result.
 *
 * A branch forward is an `if` round what it passes over. Instructions the
 * interpreter has and this does not: a branch backward, and a sample with
 * explicit gradients. A program with one is refused, with the reason, and
 * drawn by the interpreter.
 */

#ifndef VITARECOMP_GXP_HLSL_H
#define VITARECOMP_GXP_HLSL_H

#include "gxp.h"

#include <stddef.h>

typedef struct {
    /* Textures, by unit: how many components a sample of the one bound has
     * (0 for nothing bound), the kind its texels are (USSE_F32, USSE_U8,
     * USSE_S8; anything else is halves), and whether it is a cube. */
    uint8_t  tex_components[GXP_TEXTURE_UNITS], tex_type[GXP_TEXTURE_UNITS], tex_cube[GXP_TEXTURE_UNITS];
    /* How a flat texture's coordinate wraps, the console's mode across in
     * the low four bits and down in the high; and whether it is read as
     * the one texel a point falls in, with no filter and no second level.
     * Such a texel is chosen by the program's own arithmetic and not by the
     * card's sampler: a title that draws a target half a texture's size
     * samples exactly on the line between two texels, and cards differ on
     * which side of the line that is. */
    uint8_t  tex_wrap[GXP_TEXTURE_UNITS], tex_point[GXP_TEXTURE_UNITS];
    /* The interpolated quantities the vertex program hands on, a bit a
     * slot: the same for the two programs of a draw. */
    uint32_t slots;
    /* A vertex program: how many words of primary attributes it is fed, a
     * multiple of four; and how many of each slot it writes. */
    uint32_t pa_words;
    /* A fragment program: 0 for the colour as four floats, else how many
     * words of the result go out as they are, 1 to 4; and whether a colour
     * is held to 0..1 before it leaves. */
    uint8_t  result_words, clamp;
    /* And for one that reads the pixel it writes, what NOW holds: 1 the
     * pixel as light, four floats; 2 its words, of which `now_words` are
     * the pixel's and the rest of the four nothing. */
    uint8_t  now, now_words;
    /* Set, the pixel is not handed over but read from the target itself,
     * and the result is written there by the program, in the order the
     * triangles come (gpu.h's gpu_ordered): the draw binds the target at
     * u0 and nothing as its output, and VP[2].z holds, as a word, which of
     * red, green, blue and alpha are written (bits 0 to 3). For a colour
     * of four floats. */
    uint8_t  ordered;
} gxp_hlsl_bind;

/* The program as HLSL, in memory the caller frees. NULL when it cannot be
 * translated, with the reason in `why`. */
char *gxp_hlsl(const gxp_program *p, const gxp_hlsl_bind *bind, char *why, size_t why_size);

#endif /* VITARECOMP_GXP_HLSL_H */
