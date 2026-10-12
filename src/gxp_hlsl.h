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
 *
 * The same program is written in GLSL for a card that is reached through
 * OpenGL (gxp_text). It is the same text: the statements of an instruction
 * are spelt once, in HLSL's words, and the GLSL begins by saying what those
 * words are in its own (gxp_glsl_names). What differs is what goes round
 * the statements, where the two languages have different things and not
 * different names: how a texture is declared and sampled, how a program is
 * given its inputs and hands on its results, and that GLSL's variables
 * begin holding nothing in particular where HLSL's begin at nothing. The
 * GLSL expects, of the back end that compiles it:
 *
 *   a first line of its own               `#version 430 core` or later; the
 *                                         text has none. After it the back
 *                                         end may define CLIP_AS_GL (below)
 *   uniform block `draw`, std140          the same three arrays, at binding
 *                                         0 for a vertex program and 1 for
 *                                         a fragment program: the two share
 *                                         one set of numbers
 *   usamplerBuffer MEM                    at binding 16 for a fragment
 *                                         program, 34 for a vertex program
 *   sampler2D or sampler2DArray TEXn      at binding n for a fragment
 *                                         program, 18 + n for a vertex
 *                                         program: a texture and how it is
 *                                         sampled are one thing here, and a
 *                                         cube is an array of six
 *   sampler2D or usampler2D NOW           at binding 17
 *
 * A vertex program's inputs are `uvec4`s at locations 0 upward, and what it
 * hands on is a `vec4` at the location of its slot, which is where a
 * fragment program looks for it; each is a program by itself, linked alone
 * (a separable program), and so declares the position it writes. A
 * fragment program's result is location 0, four floats or four words. The
 * position is left as it is for Direct3D: up is up, depth runs from nothing
 * to the fourth number, and the first row of a target is its top. A back
 * end whose card can be told to read it so (glClipControl) tells it; one
 * whose card cannot defines CLIP_AS_GL, and the program turns the position
 * over and stretches its depth as it leaves, the target's rows still top
 * first. A pixel's place is given to a fragment program counted from the
 * top row either way.
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
    /* A fragment program drawn to a target `larger` times the size the
     * title made it, when that is more than once; and by unit, whether the
     * texture there is one whose texels are parts of the pixels of a
     * surface: a title keeps two words a pixel and reads them as a texture
     * twice as wide, a word a texel. On a larger target the point a
     * fragment samples at moves by parts of a pixel, and here a part of a
     * pixel is a whole texel and another word. Such a texture's texel is
     * the one the title's own pixel would have read, found from the
     * coordinate taken back to that pixel's middle; and when the texture
     * is itself kept larger, which the bit of its unit in VP[2].w says
     * draw by draw, the texel's part that this fragment is. */
    uint8_t  larger, tex_locked[GXP_TEXTURE_UNITS];
} gxp_hlsl_bind;

/* The program as HLSL, in memory the caller frees. NULL when it cannot be
 * translated, with the reason in `why`. */
char *gxp_hlsl(const gxp_program *p, const gxp_hlsl_bind *bind, char *why, size_t why_size);

/* The same in the language asked for. A program refused in one is refused
 * in the other, for the same reason. */
enum { GXP_HLSL = 0, GXP_GLSL };
char *gxp_text(const gxp_program *p, const gxp_hlsl_bind *bind, int language, char *why, size_t why_size);

/* What a GLSL text written in HLSL's words begins with: those words, as
 * names GLSL has for the same things. For whoever else writes such a text
 * (gxm_gpu.c's readings of a surface). */
extern const char gxp_glsl_names[];

#endif /* VITARECOMP_GXP_HLSL_H */
