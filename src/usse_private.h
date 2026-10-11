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

/* How far an operand has moved on the r-th execution of its instruction:
 * in words for the banks of words, in registers for the internal ones. Only
 * the increments of `s` are read. */
int32_t usse_repeat_offset(const usse_state *s, const usse_insn *in, const usse_operand *op, int r);

/* What an SMLSI instruction does to the increments of `s`. */
void usse_apply_smlsi(usse_state *s, const usse_insn *in);

/* The two words of constant register n. */
const uint32_t *usse_constant_words(unsigned n);

#endif /* VITARECOMP_USSE_PRIVATE_H */
