/* gxp.h — a compiled shader program, read into what a renderer needs.
 *
 * A GXP is the console's shader container: a header, a table of named
 * parameters, tables that say which register every input arrives in, and
 * two pieces of USSE bytecode. Running the bytecode is usse.c's business;
 * this is everything around it:
 *
 *   - where the vertex attributes go (primary attributes, by register);
 *   - how the secondary attributes are laid out: which uniform buffers are
 *     copied into registers and where, which registers hold the addresses
 *     of buffers the program reads from memory, which hold literals, and
 *     which stand for a texture the bytecode samples itself;
 *   - for a vertex program, how its output registers are packed: position,
 *     colours, fog, texture coordinates, point size, in that order;
 *   - for a fragment program, the list that fills its primary attributes
 *     before it runs: interpolated vertex outputs, and the texture reads
 *     the hardware performs ahead of the program at an interpolated
 *     coordinate; and where its colour is afterwards, and in what form.
 *
 * The layouts are the console's. They were worked out against 200 real
 * programs and against the Vita3K project's translator, which documents
 * the same tables.
 */

#ifndef VITARECOMP_GXP_H
#define VITARECOMP_GXP_H

#include "usse.h"

#include <stdint.h>

/* Program flags (header word 0x14). */
#define GXP_FLAG_FRAGMENT     0x0001u
#define GXP_FLAG_DISCARD      0x0008u       /* contains a KILL */
#define GXP_FLAG_DEPTH        0x0010u       /* replaces the fragment's depth */
#define GXP_FLAG_SPRITECOORD  0x0020u
#define GXP_FLAG_NATIVECOLOR  0x0040u       /* the result is in the output registers */
#define GXP_FLAG_FRAGCOLOR    0x0080u       /* reads the pixel's present colour from them */

/* Parameter categories and types. */
enum { GXP_ATTRIBUTE = 0, GXP_UNIFORM, GXP_SAMPLER, GXP_AUXILIARY_SURFACE, GXP_UNIFORM_BUFFER };
enum { GXP_F32 = 0, GXP_F16, GXP_C10, GXP_U32, GXP_S32, GXP_U16, GXP_S16, GXP_U8, GXP_S8, GXP_AGGREGATE };

#define GXP_DEFAULT_BUFFER 14               /* the container index of the default uniform buffer */
#define GXP_BUFFERS        15               /* 14 the title sets, and the default one */
#define GXP_TEXTURE_UNITS  16

/* The interpolated quantities a fragment program can ask for: the ten
 * texture coordinates, then these. */
enum { GXP_SLOT_COLOR0 = 10, GXP_SLOT_COLOR1, GXP_SLOT_FOG, GXP_SLOT_POSITION, GXP_SLOT_SPRITE, GXP_SLOTS };

typedef struct { uint16_t buffer, reg, words; } gxp_block;      /* a uniform buffer copied into registers */
typedef struct { uint16_t buffer, reg; int32_t bias; } gxp_base; /* a register holding a buffer's address */
typedef struct { uint16_t reg; uint32_t value; } gxp_literal;
typedef struct { uint16_t reg, unit; } gxp_sampler;             /* the register a sample instruction names */

/* One entry of a fragment program's input list. Either half may be absent. */
typedef struct {
    uint8_t  has_iterator, source, type, components;    /* slot, USSE type, 1..4 */
    uint16_t iterator_reg;
    uint8_t  has_texture, coord, projection, unit;      /* slot; 0 xy, 1 xy/w, 2 xy/z; texture unit */
    uint8_t  result, single;                            /* 0 the texture's own form, 2 halves, 3 floats */
    uint16_t texture_reg;
} gxp_input;

typedef struct {
    const uint8_t *data;                    /* the container, wherever the caller keeps it */
    uint32_t size;
    uint32_t flags;
    uint32_t pa_count, sa_count;
    uint32_t default_buffer_words;

    /* parameters, in place: 16 bytes each */
    const uint8_t *params;
    uint32_t param_count;

    /* the secondary attributes */
    gxp_block   blocks[GXP_BUFFERS];
    uint32_t    block_count;
    gxp_base    bases[GXP_BUFFERS + 4];
    uint32_t    base_count;
    gxp_literal *literals;
    uint32_t    literal_count;
    gxp_sampler samplers[GXP_TEXTURE_UNITS];
    uint32_t    sampler_count;
    uint32_t    buffers_used;               /* a bit per buffer the program reads, either way */
    uint32_t    textures_used;              /* a bit per texture unit */

    /* a vertex program's outputs: the first register of each slot it
     * writes and how many, 0 for none. Position is registers 0..3. */
    uint8_t  out_reg[GXP_SLOTS], out_count[GXP_SLOTS];
    uint8_t  psize_reg, has_psize, out_words;
    uint64_t attribute_regs, untyped_regs;

    /* a fragment program's inputs and result */
    gxp_input *inputs;
    uint32_t  input_count;
    uint8_t   result_type, result_components;   /* GXP_* type */
    uint32_t  slots_used;                   /* a bit per slot some input reads */

    /* the code */
    usse_insn *secondary, *primary;
    uint32_t  secondary_count, primary_count;
    uint32_t  undecoded;                    /* instructions usse_decode refused */
} gxp_program;

/* Read a container. The bytes must stay where they are for as long as the
 * program is used. Returns 0, or a negative number when it is not a GXP or
 * a table points outside it. */
int gxp_parse(gxp_program *p, const uint8_t *data, uint32_t size);
void gxp_free(gxp_program *p);

/* The parameter whose resource index is `reg` in the given category, or
 * NULL. The record is 16 bytes: name offset, category and type, component
 * count and container, semantic, semantic index, array size, resource index. */
const uint8_t *gxp_find_param(const gxp_program *p, int category, uint32_t resource_index);

static inline int gxp_param_category(const uint8_t *param)   { return param[4] & 15; }
static inline int gxp_param_type(const uint8_t *param)       { return param[4] >> 4; }
static inline int gxp_param_components(const uint8_t *param) { return param[5] & 15; }
static inline int gxp_param_container(const uint8_t *param)  { return param[5] >> 4; }

#endif /* VITARECOMP_GXP_H */
