/* usse_private.h — what a translator of the bytecode shares with its interpreter.
 *
 * usse.c is the definition of what an instruction does. Anything that turns
 * the bytecode into another language, a host GPU's say, has to agree with it
 * about the things that are not in the instruction record: how far an
 * operand has moved on the r-th execution of a repeated instruction, what
 * the constant registers hold, and which operation a test computes. Those
 * are here so that there is one copy of each.
 */

#ifndef VITARECOMP_USSE_PRIVATE_H
#define VITARECOMP_USSE_PRIVATE_H

#include "usse.h"

/* The operations a test can compute before comparing: `a` of a TEST or a
 * TESTMASK instruction. */
enum {
    T_NONE = 0, T_ADD, T_FRC, T_RCP, T_RSQ, T_LOG, T_EXP, T_DOT, T_MIN, T_MAX, T_DSX, T_DSY, T_MUL, T_SUB,
    T_AND, T_OR, T_XOR, T_SHL, T_SHR, T_ROL, T_ASR
};

/* The operations a dual-issue instruction can carry: `a` and `b` of a DUAL.
 * The first six work on vectors, the rest on one component. */
enum {
    DUAL_NONE = 0, DUAL_VMAD, DUAL_VDP, DUAL_VSSQ, DUAL_VMUL, DUAL_VADD, DUAL_VMOV,
    DUAL_FRSQ, DUAL_FRCP, DUAL_FMAD, DUAL_FADD, DUAL_FMUL, DUAL_FSUBFLR, DUAL_FEXP, DUAL_FLOG
};

/* One of the two operations of a dual-issue instruction, as an instruction
 * of its own would be: what it computes, the components it writes and
 * reads, where the result goes and what it reads. */
typedef struct {
    uint8_t      op, sources, mask, src_mask;
    usse_operand dest, src[3];
} usse_half;

/* The two operations of a DUAL instruction, the primary one first. Both
 * read their operands before either writes. */
void usse_dual_halves(const usse_insn *in, usse_half half[2]);

/* What a factor of a sum of products is taken from: nothing, or a source's
 * channel, or a source's alpha for every channel. A SOP2, SOP2M or SOP3
 * computes, a channel at a time,
 *     op(factor1 * src1, factor2 * src2)
 * with the colour's two factors in `a`, the alpha's two in `b`, each four
 * bits: one of these, and SOP_COMPLEMENT for one minus it. `c` holds the
 * two operations, the colour's in its low two bits and the alpha's above:
 * add, subtract, the smaller, the larger. */
enum { SOP_ZERO = 0, SOP_SRC0, SOP_SRC0_ALPHA, SOP_SRC1, SOP_SRC1_ALPHA, SOP_SRC2, SOP_SRC2_ALPHA, SOP_UNKNOWN };
#define SOP_COMPLEMENT 8

/* Bits of `d` of a sum of products: source 1 is taken as one minus itself
 * throughout, and the instruction asks for something there is no account
 * of, so that it is listed but not run. */
enum { SOP_SRC1_INVERSE = 1, SOP_UNDESCRIBED = 2 };

/* Bits of `b` of an IMAD8: the addend is subtracted, the result is held to
 * a byte instead of cut to one. `a` says where the addend comes from: its
 * colour in the low two bits (source 0, source 1, source 0's alpha, source
 * 1's alpha), its alpha in the next (source 0's, source 1's). */
enum { IMAD8_SUBTRACT = 1, IMAD8_SATURATE = 2 };

/* How far an operand has moved on the r-th execution of its instruction:
 * in words for the banks of words, in registers for the internal ones. Only
 * the increments of `s` are read. */
int32_t usse_repeat_offset(const usse_state *s, const usse_insn *in, const usse_operand *op, int r);

/* What an SMLSI instruction does to the increments of `s`. */
void usse_apply_smlsi(usse_state *s, const usse_insn *in);

/* The two words of constant register n. */
const uint32_t *usse_constant_words(unsigned n);

#endif /* VITARECOMP_USSE_PRIVATE_H */
