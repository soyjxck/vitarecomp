/* usse.h — the console's shader bytecode, decoded and run.
 *
 * A compiled shader on the console is a list of 64-bit instructions for the
 * GPU's programmable cores ("USSE"). They work on banks of 32-bit registers:
 * primary attributes (what arrives per vertex or per fragment), secondary
 * attributes (uniforms and literals, the same for a whole draw), outputs,
 * temporaries, three or four wide internal registers, a table of constants,
 * four predicates and two index registers. An operand names a bank and a
 * register, says which components it takes and in what order, and may be
 * read as 32-bit floats, as 16-bit floats two to a register, as bytes four
 * to a register, or as integers.
 *
 * This is an interpreter of that: usse_decode turns an instruction word into
 * a record, usse_run executes records against a register file, and
 * usse_disassemble prints them. The disassembly follows the text the Vita3K
 * project's tools print for the same word, which is what makes it possible
 * to check this decoder against theirs line by line; the instruction set
 * itself is the hardware's, as their translator documents it.
 */

#ifndef VITARECOMP_USSE_H
#define VITARECOMP_USSE_H

#include <stddef.h>
#include <stdint.h>

#define USSE_WORDS 264              /* a bank: more than any operand number can reach */

/* Register banks. */
enum {
    USSE_TEMP = 0, USSE_PA, USSE_OUT, USSE_SA, USSE_INTERNAL, USSE_SPECIAL, USSE_GLOBAL, USSE_CONST,
    USSE_IMMEDIATE, USSE_INDEX, USSE_INDEXED1, USSE_INDEXED2, USSE_PREDICATE, USSE_INVALID
};

/* How a register's bits are read. */
enum {
    USSE_S8 = 0, USSE_S16, USSE_S32, USSE_C10, USSE_F16, USSE_F32, USSE_U8, USSE_U16, USSE_U32, USSE_O8
};

/* What feeds a component: one of the source's four, or a constant. */
enum { USSE_X = 0, USSE_Y, USSE_Z, USSE_W, USSE_ZERO, USSE_ONE, USSE_TWO, USSE_HALF, USSE_UNDEFINED };

enum { USSE_ABS = 1, USSE_NEG = 2, USSE_GPI = 4 };

/* Conditions on the predicate registers. */
enum { USSE_PRED_NONE = 0, USSE_PRED_P0, USSE_PRED_P1, USSE_PRED_P2, USSE_PRED_P3, USSE_PRED_NOT_P0, USSE_PRED_NOT_P1, USSE_PRED_PN };

typedef struct {
    uint8_t  bank, type, flags, role;       /* role: 0, 1, 2 for the sources, 3 for the destination */
    uint16_t num;
    uint8_t  sw[4];
} usse_operand;

/* What an instruction does, once its format's sub-fields are read. */
enum {
    USSE_OP_INVALID = 0,
    USSE_OP_MAD,            /* dest = src0 * src1 + src2 */
    USSE_OP_MUL, USSE_OP_ADD, USSE_OP_FRC, USSE_OP_DSX, USSE_OP_DSY, USSE_OP_MIN, USSE_OP_MAX, USSE_OP_DOT,
    USSE_OP_DOT_GPI,        /* the dot product with an internal register, which rotates its mask */
    USSE_OP_RCP, USSE_OP_RSQ, USSE_OP_LOG, USSE_OP_EXP,
    USSE_OP_MOV, USSE_OP_MOVC, USSE_OP_MOVCU8,
    USSE_OP_PCK,
    USSE_OP_AND, USSE_OP_OR, USSE_OP_XOR, USSE_OP_SHL, USSE_OP_SHR, USSE_OP_ASR, USSE_OP_ROL,
    USSE_OP_TEST,           /* compute, compare with zero, set a predicate */
    USSE_OP_IMAD16,
    USSE_OP_LDR, USSE_OP_STR,
    USSE_OP_SMP,
    USSE_OP_BR, USSE_OP_KILL, USSE_OP_PHAS, USSE_OP_NOP, USSE_OP_SMLSI, USSE_OP_SMBO, USSE_OP_SPEC, USSE_OP_LIMM,
    USSE_OP_DEPTHF,
    USSE_OP_TESTMASK        /* the test that writes one and zero instead of a predicate */
};

typedef struct {
    uint64_t     raw;
    uint8_t      format;                    /* USSE_F_* of usse_fields.h */
    uint8_t      op;                        /* USSE_OP_* */
    uint8_t      pred;                      /* USSE_PRED_* */
    uint8_t      mask, src_mask;            /* components written, components read */
    uint8_t      repeat, repeat_mode;       /* executions - 1, and how operands advance */
    uint8_t      mult[4];                   /* register steps per repeat, by role */
    uint8_t      a, b, c, d;                /* what the op needs beyond its operands */
    uint32_t     imm;
    usse_operand dest, src0, src1, src2;
} usse_insn;

/* What a program needs from outside while it runs. */
typedef struct usse_env {
    void *user;
    /* a word of the memory a load instruction reads */
    uint32_t (*read32)(void *user, uint32_t address);
    /* A texture sample. The sampler is named by the secondary-attribute
     * register the instruction gives; `dimensions` is 1, 2, or 3 for a cube
     * map's direction; `lod_mode` is 0 for the level the hardware would
     * choose, 1 when `lod` is added to it, 2 when `lod` replaces it. Returns
     * how many components the texture has, 1 to 4. */
    int (*sample)(void *user, uint32_t sa_register, const float *coord, int dimensions,
                  int lod_mode, float lod, float rgba[4]);
    /* The four texels a bilinear sample would blend, first component only,
     * in the order (x0,y1) (x1,y1) (x1,y0) (x0,y0), and how far the point
     * lies into that footprint. */
    void (*gather)(void *user, uint32_t sa_register, const float *coord, float texels[4], float uv[2]);
} usse_env;

typedef struct {
    uint32_t  temp[USSE_WORDS], pa[USSE_WORDS], out[USSE_WORDS];
    uint32_t *sa;                           /* USSE_WORDS of them, shared by a whole draw */
    uint32_t  internal[16];
    int32_t   index[2];
    uint8_t   pred[4];
    int32_t   inc[4][17];                   /* the repeat increments, by role, which SMLSI sets */
    uint8_t   killed;                       /* the fragment was discarded */
    uint8_t   depth_written;
    float     depth;
    const usse_env *env;
} usse_state;

/* Decode one instruction word. `secondary` is set for a program's secondary
 * part, whose operands all address the secondary attributes. Returns 0 when
 * the word is not an instruction this decoder knows. */
int usse_decode(uint64_t word, int secondary, usse_insn *out);

/* Put the repeat increments back to their start: at the top of a program. */
void usse_reset(usse_state *s);

/* Whether usse_run has the means to execute an instruction. */
int usse_can_run(const usse_insn *in);

/* Run `count` instructions. Returns 0, or the 1-based index of the first
 * instruction it could not execute. */
uint32_t usse_run(const usse_insn *code, uint32_t count, usse_state *s);

/* --- many at once ---------------------------------------------------------------------
 * The same programs, run for a row of vertices or pixels in one pass: every
 * register is a row of words, one a lane, and an instruction is done for
 * all the lanes before the next is looked at. What each lane ends with is
 * what usse_run would have left it with; the gain is that the instruction
 * is taken apart once for the row, not once a pixel, and that the work on a
 * row is the kind a processor does several of at a time. */

#define USSE_LANES 64

typedef struct {
    uint32_t  temp[USSE_WORDS][USSE_LANES], pa[USSE_WORDS][USSE_LANES], out[USSE_WORDS][USSE_LANES];
    uint32_t  internal[16][USSE_LANES];
    const uint32_t *sa;                     /* USSE_WORDS of them, the same for every lane */
    uint8_t   pred[4][USSE_LANES];
    uint8_t   killed[USSE_LANES];           /* set where the fragment was discarded */
    int32_t   inc[4][17];
    unsigned  lanes;                        /* how many are in use */
    const usse_env *env;
} usse_wide;

/* Whether a program is one usse_run_wide executes: every instruction of it
 * and every operand. One that is not is run a lane at a time by usse_run. */
int usse_wide_can_run(const usse_insn *code, uint32_t count);

/* Run `count` instructions for lanes 0 to w->lanes - 1. The registers are
 * the caller's to fill and to read; `killed` is cleared first. */
void usse_run_wide(const usse_insn *code, uint32_t count, usse_wide *w);

/* Print one instruction as text into `out`, one line per execution it
 * stands for, each ending in a newline. The repeat increments in `s` are
 * consulted and, for the instructions that set them, changed, as when
 * running. Returns the number of characters written. */
size_t usse_disassemble(const usse_insn *in, usse_state *s, char *out, size_t cap);

#endif /* VITARECOMP_USSE_H */
