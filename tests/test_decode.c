/* test_decode.c — ARMv7 decoding, on hand-built encodings only.
 *
 * Every encoding here is assembled in this file from the architecture's rules
 * and its expected result worked out by hand, so a wrong decoder disagrees with
 * arithmetic rather than with a blob somebody once observed.
 */

#include "decode.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "tests must be built with asserts enabled (NDEBUG is defined)"
#endif

/* Lay out little-endian halfwords at a notional base address. */
static uint8_t code[64];

static void put16(int off, uint16_t v) {
    code[off]     = (uint8_t)(v & 0xFF);
    code[off + 1] = (uint8_t)(v >> 8);
}

static void put32(int off, uint32_t v) {
    code[off]     = (uint8_t)(v);
    code[off + 1] = (uint8_t)(v >> 8);
    code[off + 2] = (uint8_t)(v >> 16);
    code[off + 3] = (uint8_t)(v >> 24);
}

#define BASE 0x1000u

static arm_insn dec(arm_mode mode, uint32_t addr) {
    arm_insn in;
    int n = arm_decode(code, BASE, sizeof(code), addr, mode, &in);
    assert(n == in.width);
    return in;
}

/* --- Thumb width: the thing that must never be wrong ------------------------ */
/*
 * A wrong width does not produce one wrong instruction, it desynchronises the
 * rest of the run and produces a long stretch of plausible nonsense. The rule
 * is that first halfwords 0xE800-0xFFFF introduce a 32-bit instruction.
 */

static void test_thumb_width(void) {
    memset(code, 0, sizeof(code));

    /* Just below the boundary: still 16-bit. */
    put16(0, 0xE7FE);                      /* b . (unconditional, 16-bit)    */
    assert(dec(ARM_T32, BASE).width == 2);

    /* At and above the boundary: 32-bit. All three prefixes. */
    put16(0, 0xE800); put16(2, 0x0000);
    assert(dec(ARM_T32, BASE).width == 4);
    put16(0, 0xF000); put16(2, 0xB800);
    assert(dec(ARM_T32, BASE).width == 4);
    put16(0, 0xF800); put16(2, 0x0000);
    assert(dec(ARM_T32, BASE).width == 4);

    /* An ordinary 16-bit data-processing instruction stays 2 bytes. */
    put16(0, 0x4478);                      /* add r0, pc                     */
    assert(dec(ARM_T32, BASE).width == 2);

    printf("  thumb width boundary           ok\n");
}

/* --- returns, in both instruction sets -------------------------------------- */

static void test_returns(void) {
    memset(code, 0, sizeof(code));

    put16(0, 0x4770);                      /* bx lr                          */
    assert(dec(ARM_T32, BASE).cls == A_RETURN);

    put16(0, 0xBD00);                      /* pop {pc}                       */
    assert(dec(ARM_T32, BASE).cls == A_RETURN);

    put16(0, 0xBC00);                      /* pop {} — no pc, not a return   */
    assert(dec(ARM_T32, BASE).cls == A_LOADM);

    put32(0, 0xE12FFF1E);                  /* bx lr (ARM)                    */
    assert(dec(ARM_A32, BASE).cls == A_RETURN);

    put32(0, 0xE8BD8000);                  /* ldm sp!, {pc} (ARM)            */
    assert(dec(ARM_A32, BASE).cls == A_RETURN);

    put32(0, 0xE1A0F00E);                  /* mov pc, lr (ARM)               */
    assert(dec(ARM_A32, BASE).cls == A_RETURN);

    printf("  returns, both modes            ok\n");
}

/* --- writing r15 is control flow, not arithmetic ---------------------------- */
/*
 * The high-register ADD/MOV group can name r15 as its destination, and doing so
 * is a BRANCH. Classifying it as an ALU write emits an assignment where a jump
 * belongs, and the recompiled function runs straight past a transfer it should
 * have taken. This was caught only because the runtime has no `pc` variable, so
 * the generated C refused to compile; with a `pc` present it would have built
 * cleanly and been silently wrong.
 */

static void test_pc_destination(void) {
    memset(code, 0, sizeof(code));

    /* rd is (DN << 3) | rd_low, split across bits 7 and 2:0. 0x46B7 is
     * rd = (1<<3)|7 = 15; 0x46B6 is rd = 14 and is an ordinary move to LR.
     * The two differ by one bit, which is why the split field is worth
     * spelling out here. */
    put16(0, 0x46B7);                      /* mov pc, r6  (rd=15, rm=6)      */
    arm_insn in = dec(ARM_T32, BASE);
    assert(in.cls == A_INDIRECT);
    assert(in.cls != A_ALU);

    put16(0, 0x46B6);                      /* mov lr, r6 — rd=14, still ALU  */
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_ALU && in.op == OP_MOV && in.rd == 14);

    put16(0, 0x46F7);                      /* mov pc, lr                     */
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_RETURN);

    put16(0, 0x4487);                      /* add pc, r0  (rd=15)            */
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_INDIRECT);

    /* An ordinary high-register move is still arithmetic. */
    put16(0, 0x4630);                      /* mov r0, r6                     */
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_ALU && in.op == OP_MOV && in.rd == 0 && in.rm == 6);

    /* CMP never writes its destination, so r15 there is a normal operand. */
    put16(0, 0x45B7);                      /* cmp r15, r6                    */
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_ALU && in.op == OP_CMP);

    printf("  r15 destination is a branch    ok\n");
}

/* --- ThumbExpandImm --------------------------------------------------------- */
/*
 * Two encodings share one 12-bit field, and the naive "it is just a 12-bit
 * integer" reading is correct for small constants and wrong for everything
 * else — which is exactly the shape of bug that survives casual testing. Each
 * case is checked against a value worked out from the definition.
 */

static void test_thumb_expand_imm(void) {
    memset(code, 0, sizeof(code));

    /* mov.w rd, #imm — 0xF04F is ORR with rn=15, which is MOV. */

    /* Pattern 00: the byte, unchanged. */
    put16(0, 0xF04F); put16(2, 0x0042);
    arm_insn in = dec(ARM_T32, BASE);
    assert(in.op == OP_MOV && in.has_imm && in.imm == 0x42);

    /* Pattern 01: byte replicated into halves -> 0x00XY00XY. */
    put16(0, 0xF04F); put16(2, 0x1042);
    in = dec(ARM_T32, BASE);
    assert(in.imm == 0x00420042u);

    /* Pattern 10: byte replicated, shifted up -> 0xXY00XY00. */
    put16(0, 0xF04F); put16(2, 0x2042);
    in = dec(ARM_T32, BASE);
    assert(in.imm == 0x42004200u);

    /* Pattern 11: byte in all four positions. */
    put16(0, 0xF04F); put16(2, 0x3042);
    in = dec(ARM_T32, BASE);
    assert(in.imm == 0x42424242u);

    /* The rotate form. The stored 7 bits carry an implicit leading 1, so the
     * value rotated is 0x80|imm7, not imm7 — the single easiest thing here to
     * get wrong, and it leaves every such constant short by 0x80.
     *
     * i:imm3:a = 01000 gives a rotate of 8 applied to 0x80: 0x80000000. */
    put16(0, 0xF04F); put16(2, 0x4000);
    in = dec(ARM_T32, BASE);
    assert(in.imm == 0x80000000u);

    printf("  thumb expand immediate         ok\n");
}

/* --- 32-bit Thumb operands -------------------------------------------------- */

static void test_t32_operands(void) {
    memset(code, 0, sizeof(code));

    /* add.w r0, r1, r2 */
    put16(0, 0xEB01); put16(2, 0x0002);
    arm_insn in = dec(ARM_T32, BASE);
    assert(in.width == 4 && in.op == OP_ADD);
    assert(in.rd == 0 && in.rn == 1 && in.rm == 2 && !in.sets_flags);

    /* adds.w r0, r1, r2 — the S bit is bit 4 of the first halfword. */
    put16(0, 0xEB11); put16(2, 0x0002);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_ADD && in.sets_flags);

    /* orr.w with rn == 15 is MOV, not a real ORR against r15. */
    put16(0, 0xEA4F); put16(2, 0x0001);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_MOV && in.rn == ARM_NO_REG && in.rm == 1);

    /* sub.w with rd == 15 and S set is CMP: flags only, no destination. */
    put16(0, 0xEBB1); put16(2, 0x0F02);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_CMP && in.rd == ARM_NO_REG);

    /* movw r0, #0x1234 — a plain 16-bit literal, not a modified immediate. */
    put16(0, 0xF241); put16(2, 0x2034);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_MOV && in.imm == 0x1234);

    /* ldr.w r0, [r1, #0x100] — 12-bit unsigned offset form. */
    put16(0, 0xF8D1); put16(2, 0x0100);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDR && in.rt == 0 && in.rn == 1 && in.imm == 0x100);

    /* str.w r0, [r1, #0x100] */
    put16(0, 0xF8C1); put16(2, 0x0100);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_STR && in.rt == 0 && in.rn == 1);

    /* movt r0, #0x8100 — writes the TOP half only.
     *
     * MOVW/MOVT is how ARM builds a 32-bit address. Leaving MOVT untranslated
     * costs every constructed pointer its high 16 bits, which a trace of
     * module_start showed as indirect branches to 0x00000000. */
    /* rd is bits 11:8 of the SECOND halfword, so 0x1000 means r0. The 16-bit
     * literal is assembled from imm4:i:imm3:imm8 spread across both halfwords:
     * 8:0:1:0x00 gives 0x8100. */
    put16(0, 0xF2C8); put16(2, 0x1000);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_MOVT && in.rd == 0 && in.imm == 0x8100);

    /* strd r1, r0, [sp] — bit 6 separates dual from multiple. An earlier mask
     * kept that bit and so excluded every LDRD/STRD from decoding at all. */
    put16(0, 0xE9CD); put16(2, 0x1000);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_STRD && in.rn == 13 && in.rt == 1 && in.rt2 == 0);

    put16(0, 0xE9DD); put16(2, 0x1200);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDRD && in.rn == 13 && in.rt == 1 && in.rt2 == 2);

    /* mul.w r0, r1, r2 — ra == 15 distinguishes it from MLA. */
    put16(0, 0xFB01); put16(2, 0xF002);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_MUL && in.rd == 0 && in.rn == 1 && in.rm == 2);

    /* push.w {r4,lr} / pop.w — only the SP-relative forms translate as stack
     * operations; an LDM against another base is the general form, with
     * its own direction and write-back. */
    put16(0, 0xE92D); put16(2, 0x4010);
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_STOREM && in.op == OP_PUSH && in.rn == 13);

    put16(0, 0xE8B1); put16(2, 0x0030);      /* ldm r1!, {r4,r5} — base r1 */
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_LOADM && in.op == OP_LDM && in.rn == 1);
    assert(in.writeback && in.mem_add && in.reglist == 0x0030);

    put16(0, 0xE913); put16(2, 0x0030);      /* ldmdb.w r3, {r4,r5} */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDM && in.rn == 3 && !in.writeback && !in.mem_add);

    put16(0, 0xE923); put16(2, 0x0030);      /* stmdb.w r3!, {r4,r5} */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_STM && in.rn == 3 && in.writeback && !in.mem_add);

    put16(0, 0xE93D); put16(2, 0x0030);      /* ldmdb.w sp!, {r4,r5}: not a pop */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDM && in.rn == 13 && !in.mem_add);

    put16(0, 0xE8BD); put16(2, 0x8010);      /* ldmia.w sp!, {r4,pc}: the return */
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_RETURN && in.reglist == 0x8010);

    /* The 16-bit forms. STMIA always writes back; LDMIA does unless the base
     * is in the list, in which case the loaded value wins. */
    put16(0, 0xC987);                        /* ldmia r1, {r0,r1,r2,r7} */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDM && in.rn == 1 && in.reglist == 0x87 && !in.writeback && in.mem_add);
    put16(0, 0xC90C);                        /* ldmia r1!, {r2,r3} */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDM && in.rn == 1 && in.reglist == 0x0C && in.writeback);
    put16(0, 0xC006);                        /* stmia r0!, {r1,r2} */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_STM && in.rn == 0 && in.reglist == 0x06 && in.writeback);

    /* The wide extends: a three-bit op, a rotate, and the add forms.
     * uxtb.w r8, r10, ror #8 / sxtb.w r0, r1 / uxth.w r0, r1 / uxtab r0, r1, r2 /
     * sxtb16 r0, r1 / sxtah r3, r4, r5, ror #16 */
    put16(0, 0xFA5F); put16(2, 0xF89A);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_UXTB && in.rd == 8 && in.rm == 10 && in.rn == ARM_NO_REG && in.shift_amt == 8 && in.shift_type == SH_ROR);
    put16(0, 0xFA4F); put16(2, 0xF081);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_SXTB && in.rd == 0 && in.rm == 1 && in.shift_amt == 0);
    put16(0, 0xFA1F); put16(2, 0xF081);
    assert(dec(ARM_T32, BASE).op == OP_UXTH);
    put16(0, 0xFA0F); put16(2, 0xF081);
    assert(dec(ARM_T32, BASE).op == OP_SXTH);
    put16(0, 0xFA51); put16(2, 0xF082);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_UXTB && in.rn == 1 && in.rm == 2 && in.rd == 0);
    put16(0, 0xFA2F); put16(2, 0xF081);
    assert(dec(ARM_T32, BASE).op == OP_SXTB16);
    put16(0, 0xFA3F); put16(2, 0xF081);
    assert(dec(ARM_T32, BASE).op == OP_UXTB16);
    put16(0, 0xFA04); put16(2, 0xF3A5);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_SXTH && in.rn == 4 && in.rd == 3 && in.rm == 5 && in.shift_amt == 16);

    /* rev r1, r0 / rev16 r1, r0 / revsh r1, r0: op bits 7:6 of 0xBAxx. */
    put16(0, 0xBA01);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_REV && in.rd == 1 && in.rm == 0);
    put16(0, 0xBA41);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_REV16 && in.rd == 1 && in.rm == 0);
    put16(0, 0xBAC1);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_REVSH && in.rd == 1 && in.rm == 0);
    put16(0, 0xBA81);                        /* unallocated */
    assert(dec(ARM_T32, BASE).op == OP_NONE);

    printf("  32-bit thumb operands          ok\n");
}

/* --- operand decoding ------------------------------------------------------- */

static void test_operands(void) {
    memset(code, 0, sizeof(code));

    /* 16-bit data processing sets flags implicitly — there is no S bit. */
    put16(0, 0x1C41);                      /* adds r1, r0, #1                */
    arm_insn in = dec(ARM_T32, BASE);
    assert(in.op == OP_ADD && in.rd == 1 && in.rn == 0);
    assert(in.has_imm && in.imm == 1 && in.sets_flags);

    put16(0, 0x2042);                      /* movs r0, #0x42                 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_MOV && in.rd == 0 && in.imm == 0x42 && in.sets_flags);

    /* Load/store immediates are scaled by the ACCESS SIZE, so the byte form is
     * not the word form with a smaller range. */
    put16(0, 0x6841);                      /* ldr r1, [r0, #4]               */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDR && in.rt == 1 && in.rn == 0 && in.imm == 4);

    put16(0, 0x7841);                      /* ldrb r1, [r0, #1]              */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDRB && in.imm == 1);

    put16(0, 0x8841);                      /* ldrh r1, [r0, #2]              */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDRH && in.imm == 2);

    /* PUSH/POP register lists, including the extra bit for LR and PC. */
    put16(0, 0xB570);                      /* push {r4,r5,r6,lr}             */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_PUSH && in.reglist == (0x70 | 0x4000));

    put16(0, 0xBD70);                      /* pop {r4,r5,r6,pc}              */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_POP && (in.reglist & 0x8000) && in.cls == A_RETURN);

    printf("  operands                       ok\n");
}

/* --- the J1/J2 branch encoding ---------------------------------------------- */
/*
 * The two J bits are stored XORed with the sign bit. A decoder that treats them
 * as plain address bits is correct for short forward branches and wrong for
 * long or backward ones, which is exactly the bug that survives casual testing.
 * Both directions are checked, with the encodings derived by hand.
 */

/* The bitfield group shares its 5-bit op field with MOVW/MOVT/ADDW, and the
 * position of the field is encoded three different ways across four
 * instructions: SBFX/UBFX give a width, BFI gives an end bit, BFC gives an end
 * bit and no source. Reading any of them as another silently shifts data. */
static void test_bitfield(void) {
    memset(code, 0, sizeof(code));

    /* ubfx r0, r1, #4, #8 */
    put16(0, 0xF3C1); put16(2, 0x1007);
    arm_insn in = dec(ARM_T32, BASE);
    assert(in.width == 4 && in.op == OP_UBFX);
    assert(in.rd == 0 && in.rn == 1 && in.bf_lsb == 4 && in.bf_width == 8);

    /* sbfx r0, r1, #4, #8 — same operands, one bit of op apart. */
    put16(0, 0xF341); put16(2, 0x1007);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_SBFX && in.bf_lsb == 4 && in.bf_width == 8);

    /* bfi r0, r1, #4, #8 — encoded as msb = 11, so width must be derived. */
    put16(0, 0xF361); put16(2, 0x100B);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_BFI && in.rn == 1 && in.bf_lsb == 4 && in.bf_width == 8);

    /* bfc r0, #4, #8 — the same encoding with rn == 15, which is "no source"
     * and not a reference to the PC. */
    put16(0, 0xF36F); put16(2, 0x100B);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_BFC && in.rn == ARM_NO_REG && in.bf_width == 8);

    /* ubfx running off the end of the register (lsb 28, width 8) is
     * UNPREDICTABLE, and a linear sweep over a literal pool produces it. It
     * must trap rather than emit a clamped extract. */
    put16(0, 0xF3C1); put16(2, 0x7007);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && strcmp(in.mnemonic, "ubfx") == 0);

    /* ssat lives in the same group and is not bitfield work; it is named
     * separately so the trap report does not overstate what is left. */
    put16(0, 0xF300); put16(2, 0x0000);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && strcmp(in.mnemonic, "sat") == 0);

    /* movw still matches: it sets bit 6, which the group's op field shares. */
    put16(0, 0xF241); put16(2, 0x2034);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_MOV && in.imm == 0x1234);

    printf("  bitfield                       ok\n");
}

/* The four encodings sharing the 32-bit branch group are selected by two bits
 * of the second halfword: bit 14 (call or branch) and bit 12 (which of the
 * pair). Masking bits 14:12 together and testing `& 5` conflates them, which
 * decoded BLX-immediate as B.W and sent real B.W down the CONDITIONAL path,
 * where it read a cond field that does not exist in its encoding.
 *
 * Encodings below are assembled from the architecture's rules, and the
 * expected targets worked out by hand.
 */
static void test_wide_branch_group(void) {
    memset(code, 0, sizeof(code));

    /* BLX immediate. h2: bit15=1, bit14=1, bit13=J1, bit12=0, bit11=J2,
     * bits10:1=imm10L, bit0=H(must be 0). With S=0, J1=J2=1 gives I1=I2=0, so
     * the offset is imm10L*4 = 4 here.
     *
     * The property that matters: the base is Align(PC,4), NOT PC, because the
     * destination is ARM code. Placed at an ODD halfword the two differ, and
     * using PC lands two bytes high — which is how a tail call into an import
     * stub arrived two bytes past the stub. */
    put16(2, 0xF000); put16(4, 0xE802);
    arm_insn in = dec(ARM_T32, BASE + 2);
    assert(in.width == 4);
    assert(in.cls == A_CALL);          /* a call, not a branch */
    assert(in.switches_mode);          /* and it changes instruction set */
    assert(in.has_target);
    /* pc = 0x1006, Align(pc,4) = 0x1004, +4 => 0x1008. Using pc would give
     * 0x100A, and an ARM target is never at an odd halfword. */
    assert(in.target == BASE + 8);
    assert((in.target & 3) == 0);

    /* The SAME encoding one halfword later, where PC is already aligned. The
     * offset is identical, so the target must move by 4 — not by 2. That is
     * the assertion the old code could not pass. */
    put16(4, 0xF000); put16(6, 0xE802);
    in = dec(ARM_T32, BASE + 4);
    assert(in.cls == A_CALL && in.target == BASE + 12);

    /* B.W: bit14=0, bit12=1. Unconditional, a branch, and NOT a call. */
    put16(8, 0xF000); put16(10, 0xB802);
    in = dec(ARM_T32, BASE + 8);
    assert(in.cls == A_BRANCH && in.op == OP_B);
    assert(!in.conditional && in.cond == ARM_COND_AL);
    assert(!in.switches_mode);
    assert(in.target == BASE + 16);    /* pc(0x100C) + 4 */

    /* BL: bit14=1, bit12=1. A call that does not change instruction set, and
     * unlike BLX its offset is a multiple of two, not four. */
    put16(16, 0xF000); put16(18, 0xF802);
    in = dec(ARM_T32, BASE + 16);
    assert(in.cls == A_CALL && !in.switches_mode);
    assert(in.target == BASE + 24);    /* pc(0x1014) + 4 */

    /* B<cond>.W: bit14=0, bit12=0. Keeps T3's own layout and a real
     * condition, which is exactly what B.W must NOT be given. */
    put16(24, 0xF000); put16(26, 0x8000);
    in = dec(ARM_T32, BASE + 24);
    assert(in.cls == A_BRANCH && in.conditional);

    printf("  wide branch group, all four    ok\n");
}

static void test_bl_target(void) {
    memset(code, 0, sizeof(code));

    /* BL at 0x1000 -> 0x1100. offset = 0x1100 - 0x1004 = +0xFC.
     * S=0, so I1=I2=0, which means J1=J2=1. imm11 = 0xFC>>1 = 0x7E. */
    put16(0, 0xF000);
    put16(2, 0xF87E);
    arm_insn in = dec(ARM_T32, BASE);
    assert(in.width == 4);
    assert(in.cls == A_CALL);
    assert(in.has_target);
    assert(in.target == 0x1100);

    /* BL at 0x1000 -> 0x0F00. offset = 0x0F00 - 0x1004 = -0x104.
     * S=1, I1=I2=1 so J1=J2=1; imm10=0x3FF, imm11=0x77E. */
    put16(0, 0xF7FF);
    put16(2, 0xFF7E);
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_CALL);
    assert(in.target == 0x0F00);

    printf("  bl target, both directions     ok\n");
}

static void test_branch_targets(void) {
    memset(code, 0, sizeof(code));

    /* 16-bit conditional branch: b<cond> +4 -> 0x1000+4+4 = 0x1008. */
    put16(0, 0xD002);                      /* cond=0 (eq), imm8=2            */
    arm_insn in = dec(ARM_T32, BASE);
    assert(in.cls == A_BRANCH && in.conditional && in.target == 0x1008);

    /* 16-bit unconditional: b . -> 0x1000+4-2 = 0x1000 (a self-loop). */
    put16(0, 0xE7FE);
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_BRANCH && !in.conditional && in.target == BASE);

    /* ARM bl: pc is addr+8 on ARM, not addr+4. bl +0 -> 0x1008. */
    put32(0, 0xEB000000);
    in = dec(ARM_A32, BASE);
    assert(in.cls == A_CALL && in.target == 0x1008);

    /* ARM b backwards: imm24 = -2 -> 0x1000+8-8 = 0x1000. */
    put32(0, 0xEAFFFFFE);
    in = dec(ARM_A32, BASE);
    assert(in.cls == A_BRANCH && in.target == BASE);

    printf("  branch targets                 ok\n");
}

/* --- interworking ----------------------------------------------------------- */

static void test_mode_switches(void) {
    memset(code, 0, sizeof(code));

    put16(0, 0x4778);                      /* bx pc — switches to ARM        */
    /* A branch with a known destination, the word-aligned address four bytes
     * on, in ARM. The linker's interworking veneers start with it. */
    arm_insn in = dec(ARM_T32, BASE);
    assert(in.cls == A_BRANCH && in.switches_mode && in.has_target);
    assert(in.target == ((BASE + 4) & ~3u));

    /* The PIC long-branch veneer the Vita linker writes for a Thumb tail call
     * to an ARM symbol:  bx pc; nop; ldr ip, [pc]; add pc, ip, pc; .word off
     * The destination is off + (veneer + 16), and the decoder resolves it so
     * the ARM body is never walked. */
    put16(0, 0x4778);
    put16(2, 0x46C0);                      /* nop (mov r8, r8)               */
    put32(4, 0xE59FC000);                  /* ldr ip, [pc, #0]               */
    put32(8, 0xE08CF00F);                  /* add pc, ip, pc                 */
    put32(12, 0x00000210);                 /* -> BASE + 16 + 0x210           */
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_BRANCH && in.switches_mode && in.has_target);
    assert(in.target == BASE + 16 + 0x210);
    memset(code, 0, sizeof(code));

    put16(0, 0x4780);                      /* blx r0                         */
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_CALL && in.switches_mode);

    put32(0, 0xE12FFF30);                  /* blx r0 (ARM)                   */
    in = dec(ARM_A32, BASE);
    assert(in.cls == A_CALL && in.switches_mode);

    /* bx lr must NOT be flagged as a mode switch — it is a return, and
     * treating it as interworking would corrupt the decoder's mode state for
     * everything after it. */
    put16(0, 0x4770);
    assert(dec(ARM_T32, BASE).switches_mode == 0);

    printf("  interworking                   ok\n");
}

/* --- the ARM unconditional encoding space ----------------------------------- */

static void test_arm_cond_f(void) {
    memset(code, 0, sizeof(code));

    /* cond == 1111 is not "never" — it is a separate encoding space. Reading it
     * as a normal conditional instruction misdecodes all of it. */
    put32(0, 0xFA000000);                  /* blx imm                        */
    arm_insn in = dec(ARM_A32, BASE);
    assert(in.cls == A_CALL && in.switches_mode && !in.conditional);

    /* A genuinely conditional instruction still reports as conditional. */
    put32(0, 0x0A000000);                  /* beq                            */
    in = dec(ARM_A32, BASE);
    assert(in.cls == A_BRANCH && in.conditional);

    /* AL (1110) is unconditional in effect and must not be flagged. */
    put32(0, 0xEA000000);                  /* b                              */
    assert(dec(ARM_A32, BASE).conditional == 0);

    printf("  arm unconditional space        ok\n");
}

/* --- SIMD is identified, not decoded ---------------------------------------- */

/* --- scalar VFP ------------------------------------------------------------- */
/*
 * Register numbering is fiddly in opposite directions for the two precisions: a
 * single-precision register keeps its LOW bit in the separate D flag and its
 * high four in the main field, while a double-precision register does the
 * reverse. Getting it backwards yields a valid register number that is simply
 * the wrong one — no error, just a program reading floats it never wrote.
 */

static void test_vfp(void) {
    memset(code, 0, sizeof(code));

    /* vldr s0, [r1, #8] */
    put16(0, 0xED91); put16(2, 0x0A02);
    arm_insn in = dec(ARM_T32, BASE);
    assert(in.op == OP_VLDR && in.rn == 1 && in.vd == 0);
    assert(in.imm == 8 && in.mem_add && !in.vfp_dp);

    /* vldr d0, [r1, #8] — coproc 11 selects double precision */
    put16(0, 0xED91); put16(2, 0x0B02);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VLDR && in.vfp_dp && in.vd == 0);

    /* The D bit is the LOW bit of a single-precision register number. With
     * D set, Vd=0 means s1 rather than s0. */
    put16(0, 0xEDD1); put16(2, 0x0A02);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VLDR && in.vd == 1 && !in.vfp_dp);

    /* ...but the HIGH bit of a double-precision one: D set, Vd=0 means d16. */
    put16(0, 0xEDD1); put16(2, 0x0B02);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VLDR && in.vfp_dp && in.vd == 16);

    /* vadd.f32 s0, s1, s2 / vsub.f32 — the op bit of the second halfword
     * separates them within one encoding. */
    put16(0, 0xEE30); put16(2, 0x0A81);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VADD && !in.vfp_dp);

    put16(0, 0xEE30); put16(2, 0x0AC1);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VSUB);

    /* vmul.f32 and vdiv.f32; the op bit of the multiply group is VNMUL, and
     * opc1 01 is the pair of negated accumulates. */
    put16(0, 0xEE20); put16(2, 0x0A81);
    assert(dec(ARM_T32, BASE).op == OP_VMUL);
    put16(0, 0xEE20); put16(2, 0x0AC1);      /* vnmul.f32 s0, s1, s2 */
    assert(dec(ARM_T32, BASE).op == OP_VNMUL);
    put16(0, 0xEE10); put16(2, 0x0A81);      /* vnmls.f32 s0, s1, s2 */
    assert(dec(ARM_T32, BASE).op == OP_VNMLS);
    put16(0, 0xEE10); put16(2, 0x0AC1);      /* vnmla.f32 s0, s1, s2 */
    assert(dec(ARM_T32, BASE).op == OP_VNMLA);
    put16(0, 0xEE80); put16(2, 0x0A81);
    assert(dec(ARM_T32, BASE).op == OP_VDIV);

    /* vcmp.f32 s0, s1 */
    put16(0, 0xEEB4); put16(2, 0x0A41);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VCMP && in.vm != ARM_NO_REG);

    /* vcmp.f32 s0, #0 — compares against zero, so there is no second
     * register at all. */
    put16(0, 0xEEB5); put16(2, 0x0A40);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VCMP && in.vm == ARM_NO_REG && in.has_imm);

    /* vmrs APSR_nzcv, fpscr — VFP comparisons write FPSCR, and only this
     * moves them where a conditional branch can see them. */
    put16(0, 0xEEF1); put16(2, 0xFA10);
    assert(dec(ARM_T32, BASE).op == OP_VMRS);

    /* vabs / vneg / vsqrt share an encoding, split by opc2 and opc3. */
    put16(0, 0xEEB0); put16(2, 0x0AC0);
    assert(dec(ARM_T32, BASE).op == OP_VABS);
    put16(0, 0xEEB1); put16(2, 0x0A40);
    assert(dec(ARM_T32, BASE).op == OP_VNEG);
    put16(0, 0xEEB1); put16(2, 0x0AC0);
    assert(dec(ARM_T32, BASE).op == OP_VSQRT);

    /* Advanced SIMD proper is still reported as SIMD, not claimed. */
    put16(0, 0xEF00); put16(2, 0x0000);
    assert(dec(ARM_T32, BASE).cls == A_SIMD);

    /* vmov.f32 s2, #0.5 and vmov.f64 d0, #1.0: eight bits of constant. */
    put16(0, 0xEEB6); put16(2, 0x1A00);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VMOV_IMM && !in.vfp_dp && in.vd == 2 && in.imm == 0x3F000000u);
    put16(0, 0xEEB7); put16(2, 0x0B00);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VMOV_IMM && in.vfp_dp && in.vd == 0 && in.imm64 == 0x3FF0000000000000ull);
    put16(0, 0xEEBF); put16(2, 0x8A08);      /* vmov.f32 s16, #-1.5: imm8 0xF8 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VMOV_IMM && in.vd == 16 && in.imm == 0xBFC00000u);

    /* vldmia r0, {d0-d1} and vstmia sp, {d0-d1}: list forms without
     * write-back, P=0 U=1 W=0, which an earlier revision left unclaimed. */
    put16(0, 0xEC90); put16(2, 0x0B04);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VLDM && in.rn == 0 && in.vd == 0 && in.imm == 2 && !in.writeback && in.mem_add && in.vfp_dp);
    put16(0, 0xEC8D); put16(2, 0x0B04);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VSTM && in.rn == 13 && !in.writeback);

    /* vmov r0, r1, d0 / vmov d0, r0, r1 / vmov r0, r1, s0, s1 */
    put16(0, 0xEC51); put16(2, 0x0B10);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VMOV_D_TO_C && in.vfp_dp && in.rt == 0 && in.rt2 == 1 && in.vm == 0);
    put16(0, 0xEC41); put16(2, 0x0B10);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VMOV_C_TO_D && in.vfp_dp && in.rt == 0 && in.rt2 == 1 && in.vm == 0);
    put16(0, 0xEC51); put16(2, 0x0A10);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VMOV_D_TO_C && !in.vfp_dp && in.vm == 0);

    /* vcvtb.f16.f32 s0, s0 / vcvtb.f16.f32 s1, s1 / vcvtb.f32.f16 s0, s0 /
     * vcvtt.f32.f16 s0, s0: half precision, T in bit 7 of the low halfword. */
    put16(0, 0xEEB3); put16(2, 0x0A40);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VCVT_F32_F16 && in.vd == 0 && in.vm == 0 && in.imm == 0);
    put16(0, 0xEEF3); put16(2, 0x0A60);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VCVT_F32_F16 && in.vd == 1 && in.vm == 1 && in.imm == 0);
    put16(0, 0xEEB2); put16(2, 0x0A40);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VCVT_F16_F32 && in.vd == 0 && in.vm == 0 && in.imm == 0);
    put16(0, 0xEEB2); put16(2, 0x0AC0);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VCVT_F16_F32 && in.imm == 1);

    /* vcvt.f64.u32 d1, s0 and vcvt.f64.s32 d1, s0: the signedness is bit 7
     * of the second halfword. vcvt.f32.u32 s0, s1 likewise. */
    put16(0, 0xEEB8); put16(2, 0x1B40);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VCVT_I2F && in.vfp_dp && in.vfp_unsigned && in.vd == 1 && in.vm == 0);
    put16(0, 0xEEB8); put16(2, 0x1BC0);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VCVT_I2F && in.vfp_dp && !in.vfp_unsigned);
    put16(0, 0xEEB8); put16(2, 0x0A60);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VCVT_I2F && !in.vfp_dp && in.vfp_unsigned && in.vm == 1);
    put16(0, 0xEEB8); put16(2, 0x0AE0);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VCVT_I2F && !in.vfp_dp && !in.vfp_unsigned);

    /* vcvt.s32.f64 s0, d1: the integer side is an S register whichever the
     * float side is. vcvt.f64.f32 d2, s3 changes precision. */
    put16(0, 0xEEBD); put16(2, 0x0BC1);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VCVT_F2I && in.vfp_dp && in.vd == 0 && in.vm == 1 && !in.vfp_unsigned);
    put16(0, 0xEEB7); put16(2, 0x2AE1);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VCVT_FF && !in.vfp_dp && in.vd == 2 && in.vm == 3);

    printf("  scalar vfp                     ok\n");
}

/* --- Advanced SIMD operands ------------------------------------------------- */

static void test_neon_operands(void) {
    memset(code, 0, sizeof(code));
    arm_insn in;

    /* vdup.32 d0, d0[0] / q0, d0[0] / d2, d1[1] / vdup.16 d0, d1[0] / vdup.8 d0, d1[1] */
    put16(0, 0xFFB4); put16(2, 0x0C00);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_DUP_LANE && in.vd == 0 && in.vm == 0 && in.neon_size == 32 && in.imm == 0 && !in.neon_q);
    put16(0, 0xFFB4); put16(2, 0x0C40);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_DUP_LANE && in.neon_q);
    put16(0, 0xFFBC); put16(2, 0x2C01);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_DUP_LANE && in.vd == 2 && in.vm == 1 && in.neon_size == 32 && in.imm == 1);
    put16(0, 0xFFB2); put16(2, 0x0C01);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_DUP_LANE && in.neon_size == 16 && in.imm == 0);
    put16(0, 0xFFB3); put16(2, 0x0C01);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_DUP_LANE && in.neon_size == 8 && in.imm == 1);

    /* The f32 three-register forms. */
    put16(0, 0xFF04); put16(2, 0x6D54);      /* vmul.f32 q3, q2, q2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FMUL && in.vd == 6 && in.vn == 4 && in.vm == 4 && in.neon_q && in.neon_f);
    put16(0, 0xEF01); put16(2, 0x0D00);      /* vadd.f32 d0, d1, d0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FADD && in.vd == 0 && in.vn == 1 && in.vm == 0 && !in.neon_q);
    put16(0, 0xEF21); put16(2, 0x0D00);      /* vsub.f32 d0, d1, d0 */
    assert(dec(ARM_T32, BASE).op == OP_NEON_FSUB);
    put16(0, 0xFF02); put16(2, 0x1D02);      /* vpadd.f32 d1, d2, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FPADD && in.vd == 1 && in.vn == 2 && in.vm == 2);
    put16(0, 0xFF02); put16(2, 0x1D42);      /* ...has no Q form */
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_SIMD && in.op == OP_NONE);
    put16(0, 0xEF26); put16(2, 0x6F52);      /* vrsqrts.f32 q3, q3, q1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FRSQRTS && in.vd == 6 && in.vn == 6 && in.vm == 2 && in.neon_q);
    put16(0, 0xEF06); put16(2, 0x6F52);      /* vrecps.f32 q3, q3, q1 */
    assert(dec(ARM_T32, BASE).op == OP_NEON_FRECPS);
    put16(0, 0xEF00); put16(2, 0x0E00);      /* vceq.f32 d0, d0, d0 */
    assert(dec(ARM_T32, BASE).op == OP_NEON_FCEQ);
    put16(0, 0xFF00); put16(2, 0x0E00);      /* vcge.f32 */
    assert(dec(ARM_T32, BASE).op == OP_NEON_FCGE);
    put16(0, 0xFF20); put16(2, 0x0E00);      /* vcgt.f32 */
    assert(dec(ARM_T32, BASE).op == OP_NEON_FCGT);
    put16(0, 0xEF00); put16(2, 0x0F00);      /* vmax.f32 */
    assert(dec(ARM_T32, BASE).op == OP_NEON_FMAX);
    put16(0, 0xEF20); put16(2, 0x0F00);      /* vmin.f32 */
    assert(dec(ARM_T32, BASE).op == OP_NEON_FMIN);
    put16(0, 0xEF00); put16(2, 0x0D10);      /* vmla.f32 */
    assert(dec(ARM_T32, BASE).op == OP_NEON_FMLA);
    put16(0, 0xEF20); put16(2, 0x0D10);      /* vmls.f32 */
    assert(dec(ARM_T32, BASE).op == OP_NEON_FMLS);
    put16(0, 0xFF20); put16(2, 0x0D00);      /* vabd.f32 */
    assert(dec(ARM_T32, BASE).op == OP_NEON_FABD);
    put16(0, 0xEF10); put16(2, 0x0D00);      /* size<0> set: not f32 */
    assert(dec(ARM_T32, BASE).op == OP_NONE);

    /* Integer three-register forms. */
    put16(0, 0xEF00); put16(2, 0x0910);      /* vmul.i8 d0, d0, d0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MUL && in.neon_size == 8);
    put16(0, 0xEF10); put16(2, 0x0900);      /* vmla.i16 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MLA && in.neon_size == 16);
    put16(0, 0xFF20); put16(2, 0x0600);      /* vmax.u32 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MAX && in.vfp_unsigned && in.neon_size == 32);
    put16(0, 0xEF20); put16(2, 0x0610);      /* vmin.s32 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MIN && !in.vfp_unsigned);
    put16(0, 0xEF00); put16(2, 0x0300);      /* vcgt.s8 */
    assert(dec(ARM_T32, BASE).op == OP_NEON_CGT);
    put16(0, 0xFF00); put16(2, 0x0310);      /* vcge.u8 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CGE && in.vfp_unsigned);

    /* Two registers, miscellaneous. */
    put16(0, 0xFFBB); put16(2, 0x45C2);      /* vrsqrte.f32 q2, q1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FRSQRTE && in.vd == 4 && in.vm == 2 && in.neon_q);
    put16(0, 0xFFBB); put16(2, 0x2542);      /* vrecpe.f32 q1, q1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FRECPE && in.vd == 2 && in.vm == 2);
    put16(0, 0xFFBB); put16(2, 0x0442);      /* vrecpe.u32: the integer estimate, unclaimed */
    assert(dec(ARM_T32, BASE).op == OP_NONE);
    put16(0, 0xFFB9); put16(2, 0x07C0);      /* vneg.f32 q0, q0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FNEG && in.neon_q);
    put16(0, 0xFFB9); put16(2, 0x0740);      /* vabs.f32 q0, q0 */
    assert(dec(ARM_T32, BASE).op == OP_NEON_FABS);
    put16(0, 0xFFB5); put16(2, 0x0380);      /* vneg.s16 d0, d0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_NEG && in.neon_size == 16 && !in.neon_f);
    put16(0, 0xFFB0); put16(2, 0x0580);      /* vmvn d0, d0 */
    assert(dec(ARM_T32, BASE).op == OP_NEON_MVN);
    put16(0, 0xFFB1); put16(2, 0x0000);      /* vcgt.s8 d0, d0, #0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CMP0 && in.imm == 0 && in.neon_size == 8 && !in.neon_f);
    put16(0, 0xFFB9); put16(2, 0x0480);      /* vcge.f32 d0, d0, #0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CMP0 && in.imm == 1 && in.neon_f);
    put16(0, 0xFFB0); put16(2, 0x0000);      /* vrev64.8 d0, d0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_REV && in.imm == 64 && in.neon_size == 8);
    put16(0, 0xFFB4); put16(2, 0x0080);      /* vrev32.16 d0, d0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_REV && in.imm == 32 && in.neon_size == 16);
    put16(0, 0xFFB4); put16(2, 0x0100);      /* vrev16.16: no such thing */
    assert(dec(ARM_T32, BASE).op == OP_NONE);
    put16(0, 0xFFB0); put16(2, 0x0500);      /* vcnt.8 d0, d0 */
    assert(dec(ARM_T32, BASE).op == OP_NEON_CNT);
    put16(0, 0xFFB8); put16(2, 0x0480);      /* vclz.i32 d0, d0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CLZ && in.neon_size == 32);
    put16(0, 0xFFB2); put16(2, 0x0000);      /* vswp d0, d0 */
    assert(dec(ARM_T32, BASE).op == OP_NEON_SWP);
    put16(0, 0xFFBB); put16(2, 0x0740);      /* vcvt.s32.f32 q0, q0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CVT && in.imm == 2 && in.neon_q);
    put16(0, 0xFFBB); put16(2, 0x0640);      /* vcvt.f32.s32 q0, q0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CVT && in.imm == 0);

    /* vext.8 q3, q3, q3, #8 and vext.8 d0, d1, d2, #3 */
    put16(0, 0xEFB6); put16(2, 0x6846);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_EXT && in.vd == 6 && in.vn == 6 && in.vm == 6 && in.imm == 8 && in.neon_q);
    put16(0, 0xEFB1); put16(2, 0x0302);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_EXT && in.vd == 0 && in.vn == 1 && in.vm == 2 && in.imm == 3 && !in.neon_q);
    put16(0, 0xEFB1); put16(2, 0x0902);      /* #9 on a D register: no */
    assert(dec(ARM_T32, BASE).op == OP_NONE);

    /* Shifts by an immediate: the lane width is where the top bit of
     * L:imm6 sits, the amount is the rest. */
    put16(0, 0xFFA0); put16(2, 0x3490);      /* vsri.64 d3, d0, #32 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SRI && in.vd == 3 && in.vm == 0 && in.neon_size == 64 && in.imm == 32);
    put16(0, 0xFFA0); put16(2, 0x2593);      /* vsli.64 d2, d3, #32 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SLI && in.vd == 2 && in.vm == 3 && in.neon_size == 64 && in.imm == 32);
    put16(0, 0xEFB4); put16(2, 0x0050);      /* vshr.s32 d0, d0, #12 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHR && in.neon_size == 32 && in.imm == 12 && !in.vfp_unsigned);
    put16(0, 0xFFB4); put16(2, 0x0050);      /* vshr.u32 d0, d0, #12 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHR && in.vfp_unsigned);
    put16(0, 0xEF9C); put16(2, 0x0110);      /* vsra.s16 d0, d0, #4 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SRA && in.neon_size == 16 && in.imm == 4);
    put16(0, 0xEF8B); put16(2, 0x0510);      /* vshl.i8 d0, d0, #3 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHL && in.neon_size == 8 && in.imm == 3);
    put16(0, 0xEF80); put16(2, 0x0590);      /* vshl.i64 d0, d0, #0 (L set) */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHL && in.neon_size == 64 && in.imm == 0);
    put16(0, 0xEFA0); put16(2, 0x0250);      /* vrshr.s32 d0, d0, #32 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_RSHR && in.neon_size == 32 && in.imm == 32);
    put16(0, 0xEF90); put16(2, 0x0A10);      /* vmovl.s16 q0, d0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MOVL && in.neon_size == 16 && in.imm == 0 && in.neon_q && !in.vfp_unsigned);
    put16(0, 0xFF88); put16(2, 0x0A10);      /* vmovl.u8 q0, d0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MOVL && in.neon_size == 8 && in.vfp_unsigned);
    put16(0, 0xEF9C); put16(2, 0x0A10);      /* vshll.s16 q0, d0, #12 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHLL && in.neon_size == 16 && in.imm == 12);
    put16(0, 0xEFA8); put16(2, 0x0810);      /* vshrn.i64 d0, q0, #24 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHRN && in.neon_size == 64 && in.imm == 24 && !in.neon_q);
    put16(0, 0xEF90); put16(2, 0x0850);      /* vrshrn.i32 d0, q0, #16 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_RSHRN && in.neon_size == 32 && in.imm == 16);
    put16(0, 0xFFB6); put16(2, 0x0200);      /* vmovn.i32 d0, q0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MOVN && in.neon_size == 32 && in.imm == 0);
    put16(0, 0xEF88); put16(2, 0x0710);      /* vqshl.s8 #0: saturating, unclaimed */
    assert(dec(ARM_T32, BASE).op == OP_NONE);
    put16(0, 0xEF81); put16(2, 0x0710);      /* imm6 000xxx with L clear is the immediate group: vorr.i32 #0x10000000 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_ORR_IMM && in.imm64 == 0x1000000010000000ull);

    /* By a scalar: vmla.f32 q1, q2, d1[0]; vmul.f32 q0, q0, d3[1]; vmul.i16 d0, d0, d1[2] */
    put16(0, 0xFFA4); put16(2, 0x2141);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MLA_SCALAR && in.neon_f && in.neon_q && in.vd == 2 && in.vn == 4 && in.vm == 1 && in.imm == 0);
    put16(0, 0xFFA0); put16(2, 0x0963);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MUL_SCALAR && in.neon_f && in.neon_q && in.vm == 3 && in.imm == 1);
    put16(0, 0xEF90); put16(2, 0x0841);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MUL_SCALAR && !in.neon_f && !in.neon_q && in.neon_size == 16 && in.vm == 1 && in.imm == 0);
    put16(0, 0xEF90); put16(2, 0x0861);      /* d1[2]: the lane is M:Vm<3>, M high */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MUL_SCALAR && in.vm == 1 && in.imm == 2);
    put16(0, 0xEF90); put16(2, 0x0869);      /* d1[3] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MUL_SCALAR && in.vm == 1 && in.imm == 3);
    put16(0, 0xEFA0); put16(2, 0x0440);      /* vmls.i32 d0, d0, d0[0] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MLS_SCALAR && !in.neon_f && in.neon_size == 32);
    put16(0, 0xEFA0); put16(2, 0x0A40);      /* vmull: long, unclaimed */
    assert(dec(ARM_T32, BASE).op == OP_NONE);

    printf("  neon operands                  ok\n");
}

/* --- exclusive access and barriers ----------------------------------------- */

static void test_exclusive(void) {
    memset(code, 0, sizeof(code));
    arm_insn in;

    put16(0, 0xE850); put16(2, 0x2F00);      /* ldrex r2, [r0] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDREX && in.cls == A_LOAD && in.rt == 2 && in.rn == 0 && in.imm == 0 && in.mem_size == 32);
    put16(0, 0xE840); put16(2, 0x2301);      /* strex r3, r2, [r0, #4] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_STREX && in.cls == A_STORE && in.rd == 3 && in.rt == 2 && in.rn == 0 && in.imm == 4);
    put16(0, 0xE8D1); put16(2, 0x4F4F);      /* ldrexb r4, [r1] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDREX && in.rt == 4 && in.rn == 1 && in.mem_size == 8);
    put16(0, 0xE8C1); put16(2, 0x4F52);      /* strexh r2, r4, [r1] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_STREX && in.rd == 2 && in.rt == 4 && in.rn == 1 && in.mem_size == 16);

    /* mrc p15, 0, r0, c13, c0, 3: the TLS pointer; any other MRC is not claimed. */
    put16(0, 0xEE1D); put16(2, 0x0F70);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_MRC_TLS && in.rt == 0);
    put16(0, 0xEE1D); put16(2, 0x5F70);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_MRC_TLS && in.rt == 5);
    put16(0, 0xEE1D); put16(2, 0x0F50);      /* c13, c0, 2: TPIDRURW */
    assert(dec(ARM_T32, BASE).op == OP_NONE);

    /* tbb [pc, r0] shares the 0xE8D0 space and must stay a table branch. */
    put16(0, 0xE8DF); put16(2, 0xF000);
    assert(dec(ARM_T32, BASE).op == OP_TBB);

    /* The guarding compare may sit several loads and stores before the table
     * branch; a flag-setting instruction or a write to the index between them
     * means the compare is not this table's. cmp r3, #6 / ldr r5, [r1, #12] /
     * str r5, [r4, #4] / str.w r12, [r1, #12] / bhi / tbh [pc, r3, lsl #1] */
    {
        arm_insn prev[6], tb;
        put16(0, 0x2B06); prev[0] = dec(ARM_T32, BASE);
        put16(0, 0x68CD); prev[1] = dec(ARM_T32, BASE);
        put16(0, 0x6065); prev[2] = dec(ARM_T32, BASE);
        put16(0, 0xF8C1); put16(2, 0xC00C); prev[3] = dec(ARM_T32, BASE);
        put16(0, 0xD814); prev[4] = dec(ARM_T32, BASE);
        put16(0, 0xE8DF); put16(2, 0xF013); tb = dec(ARM_T32, BASE);
        assert(tb.op == OP_TBH && tb.rm == 3);
        assert(arm_table_count(prev, 5, &tb) == 7);
        put16(0, 0x9300); prev[3] = dec(ARM_T32, BASE);     /* str r3, [sp]: reads the index, keeps it */
        assert(arm_table_count(prev, 5, &tb) == 7);
        put16(0, 0x3301); prev[5] = dec(ARM_T32, BASE);     /* adds r3, #1 */
        prev[4] = prev[5]; assert(arm_table_count(prev, 5, &tb) == 0);
        put16(0, 0x4613); prev[4] = dec(ARM_T32, BASE);     /* mov r3, r2 */
        assert(arm_table_count(prev, 5, &tb) == 0);
    }

    /* A switch on 40..58: cmp r0, #58 / bgt out / cmp r0, #39 / ble.w out /
     * subs r0, #40 / tbh [pc, r0, lsl #1]. The nearest compare is the lower
     * bound; the count comes from the upper one, less the subtract: 19. */
    {
        arm_insn prev[5], tb;
        put16(0, 0x283A); prev[0] = dec(ARM_T32, BASE);
        put16(0, 0xDC2A); prev[1] = dec(ARM_T32, BASE);
        put16(0, 0x2827); prev[2] = dec(ARM_T32, BASE);
        put16(0, 0xF340); put16(2, 0x8105); prev[3] = dec(ARM_T32, BASE);
        put16(0, 0x3828); prev[4] = dec(ARM_T32, BASE);
        put16(0, 0xE8DF); put16(2, 0xF010); tb = dec(ARM_T32, BASE);
        assert(prev[1].cls == A_BRANCH && prev[1].conditional && prev[1].cond == ARM_COND_GT);
        assert(prev[3].cond == ARM_COND_LE && prev[4].op == OP_SUB && prev[4].rd == 0 && prev[4].imm == 40);
        assert(arm_table_count(prev, 5, &tb) == 19);
        assert(arm_table_count(prev + 2, 3, &tb) == 0);     /* only the lower bound in view */
    }

    /* A guard that branches to its table: cmp r0, #37 / bls table ... table:
     * str r1, [sp, #44] / tbh [pc, r0, lsl #1]. The walk from `table` is
     * seeded with the compare and counts 38; bne is not a guard, blo is one
     * less. */
    {
        arm_insn prev[2], br, tb;
        int8_t reg = -1; uint32_t max = 0;
        put16(0, 0x2825); prev[0] = dec(ARM_T32, BASE);
        put16(0, 0xD90A); br = dec(ARM_T32, BASE);
        assert(arm_table_guard(prev, 1, &br, &reg, &max) && reg == 0 && max == 37);
        put16(0, 0xD10A); br = dec(ARM_T32, BASE);
        assert(!arm_table_guard(prev, 1, &br, &reg, &max));
        put16(0, 0xD30A); br = dec(ARM_T32, BASE);
        assert(arm_table_guard(prev, 1, &br, &reg, &max) && max == 36);
        arm_table_seed(&prev[0], 0, 37);
        put16(0, 0x910B); prev[1] = dec(ARM_T32, BASE);
        put16(0, 0xE8DF); put16(2, 0xF010); tb = dec(ARM_T32, BASE);
        assert(arm_table_count(prev, 2, &tb) == 38);
    }

    /* The index moved between registers and offset on the way.
     * cmp r7, #13 / bgt / adds r0, r7, #1 / tbh [pc, r0]: 15.
     * adds r5, r1, #0 / cmp r1, #4 / bhi / tbh [pc, r5]: the compare comes
     * after the move: 5.
     * movw r1, #283 / cmp r0, r1 / bgt / subs r0, #1 / subs.w r0, r0, #256 /
     * tbh [pc, r0]: 27. */
    {
        arm_insn prev[5], tb;
        put16(0, 0x2F0D); prev[0] = dec(ARM_T32, BASE);
        put16(0, 0xDCD8); prev[1] = dec(ARM_T32, BASE);
        put16(0, 0x1C78); prev[2] = dec(ARM_T32, BASE);
        put16(0, 0xE8DF); put16(2, 0xF010); tb = dec(ARM_T32, BASE);
        assert(prev[2].op == OP_ADD && prev[2].rd == 0 && prev[2].rn == 7 && prev[2].imm == 1);
        assert(arm_table_count(prev, 3, &tb) == 15);

        put16(0, 0x1C0D); prev[0] = dec(ARM_T32, BASE);
        put16(0, 0x2904); prev[1] = dec(ARM_T32, BASE);
        put16(0, 0xD80A); prev[2] = dec(ARM_T32, BASE);
        put16(0, 0xE8DF); put16(2, 0xF015); tb = dec(ARM_T32, BASE);
        assert(arm_table_count(prev, 3, &tb) == 5);

        put16(0, 0xF240); put16(2, 0x111B); prev[0] = dec(ARM_T32, BASE);
        put16(0, 0x4288); prev[1] = dec(ARM_T32, BASE);
        put16(0, 0xDC21); prev[2] = dec(ARM_T32, BASE);
        put16(0, 0x3801); prev[3] = dec(ARM_T32, BASE);
        put16(0, 0xF5B0); put16(2, 0x7080); prev[4] = dec(ARM_T32, BASE);
        put16(0, 0xE8DF); put16(2, 0xF010); tb = dec(ARM_T32, BASE);
        assert(prev[0].op == OP_MOV && prev[0].has_imm && prev[0].imm == 283 && prev[0].rd == 1);
        assert(prev[1].op == OP_CMP && !prev[1].has_imm && prev[1].rn == 0 && prev[1].rm == 1);
        assert(prev[4].op == OP_SUB && prev[4].imm == 256);
        assert(arm_table_count(prev, 5, &tb) == 27);
    }

    /* dmb sy, dsb sy, isb sy, clrex: nothing to do on one host thread. */
    put16(0, 0xF3BF); put16(2, 0x8F5F);
    assert(dec(ARM_T32, BASE).op == OP_NOP);
    put16(0, 0xF3BF); put16(2, 0x8F4F);
    assert(dec(ARM_T32, BASE).op == OP_NOP);
    put16(0, 0xF3BF); put16(2, 0x8F6F);
    assert(dec(ARM_T32, BASE).op == OP_NOP);
    put16(0, 0xF3BF); put16(2, 0x8F2F);
    assert(dec(ARM_T32, BASE).op == OP_NOP);

    printf("  ldrex/strex, barriers          ok\n");
}

/* The structure loads and stores. These share their top bits with the core
 * load/store encodings, so an unclaimed one is not a trap but a halfword
 * store of a core register: `vld1.32 {d8[]}, [sl]` in a retail title's
 * record loop became `strh r8, [sl, #0xC8F]` and wrote the loop counter
 * over the records. */
static void test_neon_structure_loads(void) {
    memset(code, 0, sizeof(code));
    arm_insn in;

    /* vld1.32 {d8[]}, [sl]: one word to every lane of one register */
    put16(0, 0xF9AA); put16(2, 0x8C8F);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_LD_ALL && in.vd == 8 && in.rn == 10 && in.rm == 15 &&
           in.neon_size == 32 && in.imm == 1 && in.bf_width == 1 && in.bf_lsb == 1 && !in.writeback);

    /* vld1.8 {d1[3]}, [r2] */
    put16(0, 0xF9A2); put16(2, 0x106F);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_LD_LANE && in.vd == 1 && in.rn == 2 && in.neon_size == 8 && in.imm64 == 3 && in.imm == 1);

    /* vst1.32 {d0[1]}, [r0]! */
    put16(0, 0xF980); put16(2, 0x088D);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_ST_LANE && in.vd == 0 && in.rn == 0 && in.rm == 13 && in.neon_size == 32 && in.imm64 == 1 && in.writeback);

    /* vld2.16 {d0[2], d1[2]}, [r3] */
    put16(0, 0xF9A3); put16(2, 0x058F);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_LD_LANE && in.imm == 2 && in.neon_size == 16 && in.imm64 == 2 && in.bf_lsb == 1);

    /* vld2.8 {d0, d1}, [r0]: two interleaved structures, one group */
    put16(0, 0xF920); put16(2, 0x080F);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_LDN && in.imm == 2 && in.bf_lsb == 1 && in.bf_width == 1 && in.neon_size == 8);

    /* vst4.32 {d0, d2, d4, d6}, [r1]: four, spaced two apart */
    put16(0, 0xF901); put16(2, 0x018F);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_STN && in.imm == 4 && in.bf_lsb == 2 && in.bf_width == 1 && in.neon_size == 32);

    /* vld2.32 {d0, d1, d2, d3}, [r0]: two structures twice over */
    put16(0, 0xF920); put16(2, 0x038F);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_LDN && in.imm == 2 && in.bf_width == 2 && in.neon_size == 32);

    /* The space is claimed whole: a 64-bit lane form is SIMD, not a store. */
    put16(0, 0xF9A2); put16(2, 0x0C0F | 0x00C0);   /* vld1 all lanes, size 11 */
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_SIMD && in.op == OP_NONE);

    printf("  neon structure loads           ok\n");
}

/* VSHL by register: the value is Vm and the counts are Vn. */
static void test_neon_shift_by_register(void) {
    memset(code, 0, sizeof(code));
    arm_insn in;

    put16(0, 0xEF21); put16(2, 0x1402);     /* vshl.s32 d1, d2, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHL_REG && in.vd == 1 && in.vn == 1 && in.vm == 2);
    assert(in.neon_size == 32 && !in.vfp_unsigned && !in.neon_q);

    put16(0, 0xFF6A); put16(2, 0x94A9);     /* vshl.u32 d25, d25, d26 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHL_REG && in.vd == 25 && in.vn == 26 && in.vm == 25);
    assert(in.neon_size == 32 && in.vfp_unsigned);

    printf("  neon shift by register         ok\n");
}

/* The 32-bit PC-relative loads. Rn = 15 is the literal form, with a 12-bit
 * offset whose sign is bit 7 of the first halfword: the bit the other forms
 * use to pick the 12-bit immediate. Read the ordinary way, a negative
 * offset becomes a register offset of zero, and the "literal" is the next
 * instruction's bytes: a retail title's vector capacity came out as 69
 * elements in a buffer of 30 that way. */
static void test_pc_relative_loads(void) {
    memset(code, 0, sizeof(code));
    arm_insn in;

    put16(0, 0xF85F); put16(2, 0x2020);     /* ldr.w r2, [pc, #-32] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDR && in.rn == 15 && in.rt == 2 && in.has_imm && in.imm == 32 && !in.mem_add);

    put16(0, 0xF8DF); put16(2, 0x2020);     /* ldr.w r2, [pc, #32] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDR && in.rn == 15 && in.has_imm && in.imm == 32 && in.mem_add);

    put16(0, 0xF83F); put16(2, 0x1004);     /* ldrh.w r1, [pc, #-4] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDRH && in.rn == 15 && in.rt == 1 && in.imm == 4 && !in.mem_add);

    put16(0, 0xF91F); put16(2, 0x3010);     /* ldrsb.w r3, [pc, #-16] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDRSB && in.rn == 15 && in.rt == 3 && in.imm == 16 && !in.mem_add);

    put16(0, 0xF81F); put16(2, 0xF020);     /* pld [pc, #-32]: a hint, nothing */
    assert(dec(ARM_T32, BASE).op == OP_NOP);

    /* The positive 16-bit form is unchanged: ldr r0, [pc, #8] */
    put16(0, 0x4802);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDR && in.rn == 15 && in.imm == 8 && in.mem_add);

    printf("  pc-relative loads              ok\n");
}

static void test_simd_identified(void) {
    memset(code, 0, sizeof(code));

    arm_insn in;
    put16(0, 0xEF00); put16(2, 0x0000);    /* advanced SIMD (Thumb)          */
    assert(dec(ARM_T32, BASE).cls == A_SIMD);

    /* VSTR shares the coprocessor space but is scalar VFP, not vector work, so
     * it is now claimed and translated rather than reported as opaque SIMD.
     * Most of this space is like that: 82% of it on a real module is scalar
     * floating point. */
    put16(0, 0xED00); put16(2, 0x0A00);    /* vstr s0, [r0]                  */
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_STORE && in.op == OP_VSTR);
    assert(in.rn == 0 && in.vd == 0 && !in.vfp_dp);

    put32(0, 0xEE000A10);                  /* vmov (ARM)                     */
    assert(dec(ARM_A32, BASE).cls == A_SIMD);

    printf("  simd identified as a class     ok\n");
}

/* --- IT blocks -------------------------------------------------------------- */

static void test_it_block(void) {
    memset(code, 0, sizeof(code));

    /* IT makes up to the next four instructions conditional. It must be
     * distinguishable from the NOP-family hints that share its encoding
     * space, because phase 5 has to honour it. */
    put16(0, 0xBF08);                      /* it eq — mask nonzero           */
    arm_insn in = dec(ARM_T32, BASE);
    assert(in.cls == A_SYS && strcmp(in.mnemonic, "it") == 0);

    put16(0, 0xBF00);                      /* nop — mask zero                */
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_NOP);

    printf("  it block vs nop                ok\n");
}

/* --- refusing to run off the end -------------------------------------------- */

static void test_bounds(void) {
    arm_insn in;
    /* A 32-bit Thumb instruction whose second halfword is past the end must
     * report "no instruction", not decode from uninitialised memory. */
    memset(code, 0, sizeof(code));
    put16(sizeof(code) - 2, 0xF000);
    assert(arm_decode(code, BASE, sizeof(code),
                      BASE + sizeof(code) - 2, ARM_T32, &in) == 0);

    /* An address before the start of the buffer. */
    assert(arm_decode(code, BASE, sizeof(code), BASE - 4, ARM_T32, &in) == 0);

    printf("  bounds                         ok\n");
}

int main(void) {
    printf("decode:\n");
    test_thumb_width();
    test_returns();
    test_pc_destination();
    test_operands();
    test_thumb_expand_imm();
    test_t32_operands();
    test_bitfield();
    test_wide_branch_group();
    test_bl_target();
    test_branch_targets();
    test_mode_switches();
    test_arm_cond_f();
    test_simd_identified();
    test_neon_structure_loads();
    test_pc_relative_loads();
    test_neon_shift_by_register();
    test_vfp();
    test_neon_operands();
    test_exclusive();
    test_it_block();
    test_bounds();
    printf("all decode tests passed\n");
    return 0;
}

