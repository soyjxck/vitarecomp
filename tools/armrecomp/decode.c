/* decode.c — ARMv7-A instruction decoding.
 *
 * Two instruction sets sharing one address space. The single most important
 * output here is WIDTH: get it wrong once and every subsequent instruction in
 * the run is decoded from the wrong offset, producing a long stretch of
 * plausible-looking nonsense rather than an obvious failure.
 *
 * Thumb width is determined by the top five bits of the first halfword, which
 * is a rule rather than a table, so it is written as one.
 */

#include "decode.h"

#include <string.h>

const char *arm_class_name(arm_class c) {
    switch (c) {
        case A_UNKNOWN:  return "unknown";
        case A_UNDEF:    return "undefined";
        case A_ALU:      return "alu";
        case A_LOAD:     return "load";
        case A_STORE:    return "store";
        case A_LOADM:    return "loadm";
        case A_STOREM:   return "storem";
        case A_BRANCH:   return "branch";
        case A_CALL:     return "call";
        case A_RETURN:   return "return";
        case A_INDIRECT: return "indirect";
        case A_SIMD:     return "simd";
        case A_SYS:      return "sys";
        case A_NOP:      return "nop";
        default:         return "?";
    }
}

const char *arm_cond_name(uint8_t cond) {
    static const char *n[16] = {
        "eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
        "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"
    };
    return n[cond & 0xF];
}

static uint32_t sign_extend(uint32_t v, int bits) {
    uint32_t m = 1u << (bits - 1);
    return (v ^ m) - m;
}

static uint16_t rd16le(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void set(arm_insn *o, arm_class c, const char *m) {
    o->cls = c;
    o->mnemonic = m;
}

/* --- Thumb, 16-bit ---------------------------------------------------------- */

/* The 16 data-processing operations of the 010000 group, in encoding order. */
static const arm_op t16_dp[16] = {
    OP_AND, OP_EOR, OP_LSL, OP_LSR, OP_ASR, OP_ADC, OP_SBC, OP_ROR,
    OP_TST, OP_RSB, OP_CMP, OP_CMN, OP_ORR, OP_MUL, OP_BIC, OP_MVN
};

/* The eight register-offset load/store forms, in encoding order. */
static const arm_op t16_ldst_reg[8] = {
    OP_STR, OP_STRH, OP_STRB, OP_LDRSB, OP_LDR, OP_LDRH, OP_LDRB, OP_LDRSH
};

static void decode_t16(uint16_t h, arm_insn *o) {
    o->raw = h;

    /* --- fully decoded operand forms -------------------------------------- */
    /*
     * These are ordered by encoding, and every one sets `op` so the emitter can
     * translate it. Anything falling past them keeps its class but leaves
     * op == OP_NONE and becomes a trap rather than a guess.
     *
     * Flag setting is the detail most easily got wrong here: 16-bit Thumb data
     * processing sets NZCV *implicitly*, with no S bit to read. The exception
     * is inside an IT block, where the same encodings do not. The decoder
     * records the instruction's own behaviour; honouring IT is the emitter's
     * job, because only it knows the surrounding context.
     */

    /* 000 op(2) imm5 rm rd — shift by immediate; op==11 is add/sub register */
    if ((h & 0xE000) == 0x0000 && (h & 0x1800) != 0x1800) {
        static const arm_op sh[3] = { OP_LSL, OP_LSR, OP_ASR };
        o->op = sh[(h >> 11) & 3];
        o->rd = h & 7;
        o->rm = (h >> 3) & 7;
        o->imm = (h >> 6) & 0x1F;
        o->has_imm = 1;
        o->sets_flags = 1;
        set(o, A_ALU, "shift");
        return;
    }

    /* 00011 I op rm/imm3 rn rd */
    if ((h & 0xF800) == 0x1800) {
        o->op = (h & 0x0200) ? OP_SUB : OP_ADD;
        o->rd = h & 7;
        o->rn = (h >> 3) & 7;
        if (h & 0x0400) { o->imm = (h >> 6) & 7; o->has_imm = 1; }
        else            { o->rm  = (h >> 6) & 7; }
        o->sets_flags = 1;
        set(o, A_ALU, "add/sub");
        return;
    }

    /* 001 op(2) rd imm8 */
    if ((h & 0xE000) == 0x2000) {
        static const arm_op ops[4] = { OP_MOV, OP_CMP, OP_ADD, OP_SUB };
        o->op = ops[(h >> 11) & 3];
        o->rd = o->rn = (h >> 8) & 7;
        o->imm = h & 0xFF;
        o->has_imm = 1;
        o->sets_flags = 1;
        set(o, A_ALU, "alu imm8");
        return;
    }

    /* 010000 op(4) rm rd — register data processing */
    if ((h & 0xFC00) == 0x4000) {
        o->op = t16_dp[(h >> 6) & 0xF];
        o->rd = o->rn = h & 7;
        o->rm = (h >> 3) & 7;
        o->sets_flags = 1;
        /* RSB in this group is the "negate" form: rd = 0 - rm. */
        if (o->op == OP_RSB) { o->imm = 0; o->has_imm = 1; o->rn = o->rm; }
        set(o, A_ALU, "alu reg");
        return;
    }

    /* 01000111 L Rm — interworking branches. These are the only 16-bit
     * instructions that can change instruction set, so they are handled before
     * the general data-processing case that would otherwise swallow them.
     *
     * The mask is 0xFF00, NOT 0xFF80: bit 7 is the L bit that distinguishes BX
     * from BLX, so masking it out excludes every BLX from this case. It then
     * falls through to "add/cmp/mov hi" and decodes as an ALU operation —
     * which means every indirect *call* becomes invisible, and phase 4 would
     * silently never discover the functions reached through them. */
    if ((h & 0xFF00) == 0x4700) {
        int rm = (h >> 3) & 0xF;
        if (h & 0x0080) {
            set(o, A_CALL, "blx");
            o->switches_mode = 1;
        } else if (rm == 14) {
            set(o, A_RETURN, "bx lr");
        } else if (rm == 15) {
            /* `bx pc`: the Thumb-to-ARM switch the linker writes into a
             * veneer when Thumb code tail-calls an ARM symbol (every call
             * from newlib to a firmware stub goes this way). The target is
             * static — the word-aligned address four bytes on — so it is a
             * branch with a known destination in the other instruction set,
             * not a computed one. */
            set(o, A_BRANCH, "bx pc");
            o->switches_mode = 1;
            o->has_target = 1;
            o->target = (o->addr + 4) & ~3u;
            o->op = OP_BX;
            o->rm = rm;
            return;
        } else {
            set(o, A_INDIRECT, "bx");
            o->switches_mode = 1;
        }
        o->op = (h & 0x0080) ? OP_BLX : OP_BX;
        o->rm = rm;
        return;
    }

    /* 010001 op(2) DN rm(4) rd(3) — high-register forms. These do NOT set
     * flags, unlike almost everything else in 16-bit Thumb; CMP is the sole
     * exception because comparing is all it does. */
    if ((h & 0xFC00) == 0x4400) {
        static const arm_op ops[3] = { OP_ADD, OP_CMP, OP_MOV };
        int which = (h >> 8) & 3;
        if (which < 3) {
            int rd = ((h >> 4) & 8) | (h & 7);
            int rm = (h >> 3) & 0xF;

            /* Writing r15 is a BRANCH, not a register write. `MOV pc, rN` is
             * an indirect jump and `ADD pc, rN` a computed one; treating either
             * as arithmetic emits an assignment where control flow belongs, and
             * the recompiled function then runs straight on past a jump it
             * should have taken. `MOV pc, lr` is the plain return form. */
            if (rd == 15 && which != 1 /* CMP never writes rd */) {
                o->rm = rm;
                if (which == 2 && rm == 14) { o->op = OP_BX; set(o, A_RETURN, "mov pc,lr"); }
                else                        { o->op = OP_BX; set(o, A_INDIRECT, "mov/add pc"); }
                return;
            }

            o->op = ops[which];
            o->rd = o->rn = (int8_t)rd;
            o->rm = (int8_t)rm;
            o->sets_flags = (o->op == OP_CMP);
            set(o, A_ALU, "add/cmp/mov hi");
            return;
        }
    }

    /* 01001 rt imm8 — LDR from the literal pool. The address is PC-relative
     * with PC aligned down to a word boundary, which is why the emitter can
     * resolve these to a constant at translation time. */
    if ((h & 0xF800) == 0x4800) {
        o->op = OP_LDR;
        o->rt = (h >> 8) & 7;
        o->rn = 15;
        o->imm = (uint32_t)(h & 0xFF) * 4;
        o->has_imm = 1;
        o->mem_add = 1;
        set(o, A_LOAD, "ldr literal");
        return;
    }

    /* 0101 op(3) rm rn rt — register-offset load/store */
    if ((h & 0xF000) == 0x5000) {
        o->op = t16_ldst_reg[(h >> 9) & 7];
        o->rt = h & 7;
        o->rn = (h >> 3) & 7;
        o->rm = (h >> 6) & 7;
        o->mem_add = 1;
        set(o, (o->op >= OP_STR) ? A_STORE : A_LOAD, "ldr/str reg");
        return;
    }

    /* 011 B L imm5 rn rt — word/byte immediate. The immediate is scaled by the
     * access size, so a byte form is NOT the word form with a smaller range. */
    if ((h & 0xE000) == 0x6000) {
        int byte = (h & 0x1000) != 0;
        int load = (h & 0x0800) != 0;
        o->op = byte ? (load ? OP_LDRB : OP_STRB) : (load ? OP_LDR : OP_STR);
        o->rt = h & 7;
        o->rn = (h >> 3) & 7;
        o->imm = (uint32_t)((h >> 6) & 0x1F) * (byte ? 1u : 4u);
        o->has_imm = 1;
        o->mem_add = 1;
        set(o, load ? A_LOAD : A_STORE, "ldr/str imm");
        return;
    }

    /* 1000 L imm5 rn rt — halfword, scaled by 2 */
    if ((h & 0xF000) == 0x8000) {
        int load = (h & 0x0800) != 0;
        o->op = load ? OP_LDRH : OP_STRH;
        o->rt = h & 7;
        o->rn = (h >> 3) & 7;
        o->imm = (uint32_t)((h >> 6) & 0x1F) * 2u;
        o->has_imm = 1;
        o->mem_add = 1;
        set(o, load ? A_LOAD : A_STORE, "ldrh/strh");
        return;
    }

    /* 1001 L rt imm8 — SP-relative, scaled by 4 */
    if ((h & 0xF000) == 0x9000) {
        int load = (h & 0x0800) != 0;
        o->op = load ? OP_LDR : OP_STR;
        o->rt = (h >> 8) & 7;
        o->rn = 13;
        o->imm = (uint32_t)(h & 0xFF) * 4u;
        o->has_imm = 1;
        o->mem_add = 1;
        set(o, load ? A_LOAD : A_STORE, "ldr/str sp");
        return;
    }

    /* 1010 SP rd imm8 — ADR, or ADD rd, sp, #imm */
    if ((h & 0xF000) == 0xA000) {
        o->rd = (h >> 8) & 7;
        o->imm = (uint32_t)(h & 0xFF) * 4u;
        o->has_imm = 1;
        if (h & 0x0800) { o->op = OP_ADD; o->rn = 13; set(o, A_ALU, "add sp"); }
        else            { o->op = OP_ADR; o->rn = 15; set(o, A_ALU, "adr"); }
        return;
    }

    /* 1100 L rn rlist — LDM / STM */
    if ((h & 0xF000) == 0xC000) {
        int load = (h & 0x0800) != 0;
        o->rn = (h >> 8) & 7;
        o->reglist = h & 0xFF;
        /* STMIA always writes back; LDMIA writes back unless the base is
         * also in the list, in which case the loaded value wins. */
        o->writeback = load ? ((h & (1u << o->rn)) == 0) : 1;
        o->mem_add = 1;
        o->op = load ? OP_LDM : OP_STM;
        set(o, load ? A_LOADM : A_STOREM, "ldm/stm");
        return;
    }

    switch (h >> 12) {
        case 0x0: case 0x1:                     /* shift / add / sub          */
        case 0x2: case 0x3:                     /* mov / cmp / add / sub imm  */
            set(o, A_ALU, "alu");
            return;

        case 0x4:
            if ((h & 0xFC00) == 0x4000) { set(o, A_ALU, "alu"); return; }
            if ((h & 0xFC00) == 0x4400) { set(o, A_ALU, "add/cmp/mov hi"); return; }
            set(o, A_LOAD, "ldr literal");      /* 01001 — LDR Rt,[pc,#imm]   */
            return;

        case 0x5:                               /* load/store register offset */
            set(o, (h & 0x0800) ? A_LOAD : A_STORE, "ldr/str reg");
            return;

        case 0x6: case 0x7: case 0x8: case 0x9: /* load/store immediate       */
            set(o, (h & 0x0800) ? A_LOAD : A_STORE, "ldr/str imm");
            return;

        case 0xA:                               /* adr / add sp               */
            set(o, A_ALU, "adr/add sp");
            return;

        case 0xB:                               /* miscellaneous              */
            if ((h & 0xFF00) == 0xBF00) {
                /* IT and hints share this space. IT is the one that matters:
                 * it makes up to the next four instructions conditional, which
                 * the emitter has to honour. It is identified here so that it
                 * is never silently treated as a NOP. */
                if (h & 0x000F) {
                    o->op = OP_IT;
                    o->cond = (h >> 4) & 0xF;
                    o->it_mask = h & 0xF;
                    set(o, A_SYS, "it");
                } else {
                    o->op = OP_NOP;
                    set(o, A_NOP, "nop");
                }
                return;
            }
            if ((h & 0xFF00) == 0xB000) {       /* ADD/SUB SP, #imm7*4        */
                o->op = (h & 0x0080) ? OP_SUB : OP_ADD;
                o->rd = o->rn = 13;
                o->imm = (uint32_t)(h & 0x7F) * 4u;
                o->has_imm = 1;
                set(o, A_ALU, "add/sub sp");
                return;
            }
            if ((h & 0xFF00) == 0xB200) {       /* SXTH/SXTB/UXTH/UXTB        */
                static const arm_op ex[4] = { OP_SXTH, OP_SXTB, OP_UXTH, OP_UXTB };
                o->op = ex[(h >> 6) & 3];
                o->rd = h & 7;
                o->rm = (h >> 3) & 7;
                set(o, A_ALU, "extend");
                return;
            }
            if ((h & 0xFF00) == 0xBA00 && ((h >> 6) & 3) != 2) {
                /* 1011 1010 op rm rd: op 00 REV, 01 REV16, 11 REVSH; 10 is
                 * unallocated. An earlier revision read every op but 01 as
                 * REV, which turned REVSH into a full byte reverse. */
                static const arm_op rv16[4] = { OP_REV, OP_REV16, OP_NONE, OP_REVSH };
                o->op = rv16[(h >> 6) & 3];
                o->rd = h & 7;
                o->rm = (h >> 3) & 7;
                set(o, A_ALU, o->op == OP_REV ? "rev" : o->op == OP_REV16 ? "rev16" : "revsh");
                return;
            }
            if ((h & 0xF600) == 0xB400) {
                int load = (h & 0x0800) != 0;
                /* Bit 8 is the extra register: LR for PUSH, PC for POP. A POP
                 * that restores PC is a return, and must not also be emitted as
                 * an ordinary register load. */
                o->reglist = h & 0xFF;
                o->op = load ? OP_POP : OP_PUSH;
                if (load && (h & 0x0100)) {
                    o->reglist |= 0x8000;       /* PC */
                    set(o, A_RETURN, "pop {..,pc}");
                    return;
                }
                if (!load && (h & 0x0100)) o->reglist |= 0x4000;  /* LR */
                set(o, load ? A_LOADM : A_STOREM, load ? "pop" : "push");
                return;
            }
            if ((h & 0xF500) == 0xB100) {       /* CBZ / CBNZ                 */
                o->op = (h & 0x0800) ? OP_CBNZ : OP_CBZ;
                o->rn = h & 7;
                o->conditional = 1;
                o->has_target = 1;
                o->target = o->addr + 4
                          + ((((uint32_t)(h >> 9) & 1) << 6)
                          |  (((uint32_t)(h >> 3) & 0x1F) << 1));
                set(o, A_BRANCH, "cbz/cbnz");
                return;
            }
            set(o, A_ALU, "misc");
            return;

        case 0xC:                               /* stm / ldm                  */
            set(o, (h & 0x0800) ? A_LOADM : A_STOREM, "ldm/stm");
            return;

        case 0xD: {                             /* conditional branch, svc    */
            int cond = (h >> 8) & 0xF;
            if (cond == 0xF) { o->op = OP_SVC; set(o, A_SYS, "svc"); return; }
            if (cond == 0xE) { set(o, A_UNDEF, "udf"); return; }
            o->op = OP_B;
            o->cond = (uint8_t)cond;
            set(o, A_BRANCH, "b<cond>");
            o->conditional = 1;
            o->has_target = 1;
            o->target = o->addr + 4 + sign_extend(h & 0xFF, 8) * 2;
            return;
        }

        case 0xE:                               /* unconditional branch       */
            o->op = OP_B;
            o->cond = ARM_COND_AL;
            set(o, A_BRANCH, "b");
            o->has_target = 1;
            o->target = o->addr + 4 + sign_extend(h & 0x7FF, 11) * 2;
            return;

        default:
            set(o, A_UNKNOWN, "?");
            return;
    }
}

/* --- scalar VFP ------------------------------------------------------------- */
/*
 * The coprocessor space holds both Advanced SIMD and scalar VFP, and on this
 * platform the split is roughly 18/82 in favour of the scalar side. This
 * function claims the scalar part; anything it does not recognise falls back to
 * being reported as SIMD, so the boundary stays honest rather than optimistic.
 *
 * Register numbering is the fiddly part, and it is fiddly in opposite
 * directions for the two precisions. A single-precision register puts its LOW
 * bit in the separate D/N/M flag and its high four bits in the main field; a
 * double-precision register does the reverse. Getting it backwards yields a
 * valid register number that is simply the wrong one — no error, just a program
 * that reads floats it never wrote.
 */

static int vfp_sreg(int field, int extra) { return (field << 1) | extra; }
static int vfp_dreg(int field, int extra) { return (extra << 4) | field; }

/* VFPExpandImm: a sign, a three-bit exponent centred on 1.0 and four bits of
 * fraction. The exponent's top bit is stored inverted and then replicated,
 * which is what keeps the range symmetric around 1.0: 0.125 to 31.0. */
static uint32_t vfp_expand_imm32(uint32_t imm8) {
    uint32_t sign = (imm8 >> 7) & 1, b6 = (imm8 >> 6) & 1;
    uint32_t exp  = ((b6 ^ 1) << 7) | (b6 ? 0x7Cu : 0u) | ((imm8 >> 4) & 3);
    return (sign << 31) | (exp << 23) | ((imm8 & 0xF) << 19);
}

static uint64_t vfp_expand_imm64(uint32_t imm8) {
    uint64_t sign = (imm8 >> 7) & 1, b6 = (imm8 >> 6) & 1;
    uint64_t exp  = ((b6 ^ 1) << 10) | (b6 ? 0x3FCull : 0ull) | ((imm8 >> 4) & 3);
    return (sign << 63) | (exp << 52) | ((uint64_t)(imm8 & 0xF) << 48);
}

static int decode_vfp(uint16_t h1, uint16_t h2, arm_insn *o) {
    int coproc = (h2 >> 8) & 0xF;
    int dp     = (coproc == 11);
    if (coproc != 10 && coproc != 11) return 0;   /* not scalar VFP */

    o->vfp_dp = (uint8_t)dp;

    /* --- extension register load/store: VLDR / VSTR ----------------------- */
    if ((h1 & 0xFE00) == 0xEC00) {
        int P = (h1 >> 8) & 1, U = (h1 >> 7) & 1;
        int D = (h1 >> 6) & 1, W = (h1 >> 5) & 1, L = (h1 >> 4) & 1;
        int vd = (h2 >> 12) & 0xF;

        /* P=1, W=0 is the plain offset form: VLDR/VSTR. Everything else in
         * this encoding is a register-LIST transfer — VLDM/VSTM, and the
         * SP-relative VPUSH/VPOP that are by far the most common of them.
         *
         * These are worth claiming rather than skipping: they were 293 of the
         * instructions still reported as "SIMD", which is more than a fifth of
         * that bucket, and they do no arithmetic at all. imm8 counts the
         * registers transferred — one per register for single precision, two
         * for double. */
        /* The list forms are W == 1, and P=0 U=1 whatever W is:
         *   P=1 W=0  VLDR/VSTR, a single register at an offset
         *   P=0 W=1  VLDMIA
         *   P=1 W=1  VLDMDB / VPUSH / VPOP
         *   P=0 U=1 W=0  VLDMIA / VSTMIA without write-back
         *   P=0 U=0 W=0  the 64-bit core<->VFP transfers, claimed just below
         *
         * An earlier revision tested `P != W`, which is also true for P=1 W=0 —
         * so it captured the plain VLDR/VSTR forms that were already working
         * and re-decoded them as register lists. Translation went DOWN. Worth
         * recording: a change that claims more encodings can still be a
         * regression if it claims the wrong ones. */
        /* VMOV between two core registers and a D register or an S pair:
         * 1110 1100 010 L Rt2 | Rt 101 C 00 M 1 Vm. L=1 reads the register. */
        if (!P && !U && D && !W && (h2 & 0x00D0) == 0x0010) {
            o->op  = L ? OP_VMOV_D_TO_C : OP_VMOV_C_TO_D;
            o->rt  = (h2 >> 12) & 0xF;
            o->rt2 = h1 & 0xF;
            o->vm  = (int8_t)(dp ? vfp_dreg(h2 & 0xF, (h2 >> 5) & 1) : vfp_sreg(h2 & 0xF, (h2 >> 5) & 1));
            o->vd  = ARM_NO_REG;
            set(o, A_ALU, L ? "vmov r,r,d" : "vmov d,r,r");
            return 1;
        }
        if (W || (!P && U)) {
            int vd_first = dp ? vfp_dreg(vd, D) : vfp_sreg(vd, D);
            uint32_t n = (uint32_t)(h2 & 0xFF);
            if (dp) n /= 2;
            if (n == 0 || n > 32) return 0;

            o->op = L ? OP_VLDM : OP_VSTM;
            o->rn = h1 & 0xF;
            o->vd = (int8_t)vd_first;
            o->imm = n;                       /* register count */
            o->has_imm = 1;
            o->writeback = (uint8_t)W;
            o->mem_add = (uint8_t)U;          /* 0 = decrement before (VPUSH) */
            set(o, L ? A_LOADM : A_STOREM, L ? "vldm/vpop" : "vstm/vpush");
            return 1;
        }
        if (!P || W) return 0;

        o->op  = L ? OP_VLDR : OP_VSTR;
        o->rn  = h1 & 0xF;
        o->vd  = (int8_t)(dp ? vfp_dreg(vd, D) : vfp_sreg(vd, D));
        o->imm = (uint32_t)(h2 & 0xFF) * 4u;
        o->has_imm = 1;
        o->mem_add = (uint8_t)U;
        set(o, L ? A_LOAD : A_STORE, L ? "vldr" : "vstr");
        return 1;
    }

    if ((h1 & 0xEF00) != 0xEE00) return 0;        /* not VFP data processing */

    int D  = (h1 >> 6) & 1;
    int vn = h1 & 0xF;
    int vd = (h2 >> 12) & 0xF;
    int N  = (h2 >> 7) & 1;
    int M  = (h2 >> 5) & 1;
    int vm = h2 & 0xF;
    int op = (h2 >> 6) & 1;
    int opc1 = (h1 >> 4) & 0xB;                   /* bits 7, 5:4 */

    o->vd = (int8_t)(dp ? vfp_dreg(vd, D) : vfp_sreg(vd, D));
    o->vn = (int8_t)(dp ? vfp_dreg(vn, N) : vfp_sreg(vn, N));
    o->vm = (int8_t)(dp ? vfp_dreg(vm, M) : vfp_sreg(vm, M));

    /* --- VMRS: the float flags to the integer ones ------------------------ */
    if ((h1 & 0xFFF0) == 0xEEF0 && (h2 & 0x0F10) == 0x0A10) {
        o->op = OP_VMRS;
        o->rt = (h2 >> 12) & 0xF;
        set(o, A_SYS, "vmrs");
        return 1;
    }

    /* --- VMOV between a core register and a single-precision register ----- */
    if ((h1 & 0xFFE0) == 0xEE00 && (h2 & 0x0F7F) == 0x0A10) {
        int to_vfp = ((h1 >> 4) & 1) == 0;
        o->op = to_vfp ? OP_VMOV_TO_V : OP_VMOV_TO_C;
        o->rt = (h2 >> 12) & 0xF;
        o->vn = (int8_t)vfp_sreg(vn, N);          /* always single here */
        o->vfp_dp = 0;
        set(o, A_ALU, to_vfp ? "vmov s,r" : "vmov r,s");
        return 1;
    }

    switch (opc1) {
        case 0x0:
            /* Multiply-accumulate. Previously declined on the grounds that
             * translating it as a plain multiply would drop the accumulate —
             * which was right about the hazard and wrong about the conclusion,
             * exactly as with MOVT. It expresses directly as
             * `vd = vd +/- (vn * vm)`, and it was 404 instructions.
             *
             * The only genuine loss is that ARM may fuse the multiply and add
             * without an intermediate rounding step, where C rounds twice. That
             * is a last-bit difference in the mantissa, not a wrong answer, and
             * it is recorded rather than silently accepted. */
            o->op = op ? OP_VMLS : OP_VMLA;
            set(o, A_ALU, op ? "vmls" : "vmla");
            return 1;

        case 0x1:                                  /* VNMLS / VNMLA           */
            /* The negated accumulates: vd = -vd + vn*vm (op clear) and
             * vd = -vd - vn*vm (op set). An earlier revision read the op=1
             * form here as VNMUL, which drops the accumulator entirely. */
            o->op = op ? OP_VNMLA : OP_VNMLS;
            set(o, A_ALU, op ? "vnmla" : "vnmls");
            return 1;

        case 0x2:                                  /* VMUL / VNMUL            */
            o->op = op ? OP_VNMUL : OP_VMUL;
            set(o, A_ALU, op ? "vnmul" : "vmul");
            return 1;

        case 0x3:
            o->op = op ? OP_VSUB : OP_VADD;
            set(o, A_ALU, op ? "vsub" : "vadd");
            return 1;

        case 0x8:
            o->op = OP_VDIV;
            set(o, A_ALU, "vdiv");
            return 1;

        case 0xB: {                                /* the "other" group       */
            int opc2 = h1 & 0xF;
            int opc3 = (h2 >> 6) & 3;

            /* VMOV (immediate): bits 7:4 clear, the constant in imm4H:imm4L.
             * Eight bits name a sign, a small exponent and four bits of
             * fraction: 0.5, 1.0, 2.75 and the like. The retail census had
             * 45,000 of them, every one a trap. */
            if ((h2 & 0x00F0) == 0) {
                uint32_t imm8 = ((uint32_t)(h1 & 0xF) << 4) | (uint32_t)(h2 & 0xF);
                o->op = OP_VMOV_IMM;
                o->has_imm = 1;
                if (dp) o->imm64 = vfp_expand_imm64(imm8);
                else    o->imm   = vfp_expand_imm32(imm8);
                set(o, A_ALU, "vmov imm");
                return 1;
            }

            if (opc2 == 0x0 && opc3 == 1) { o->op = OP_VMOV;  set(o, A_ALU, "vmov"); return 1; }
            if (opc2 == 0x0 && opc3 == 3) { o->op = OP_VABS;  set(o, A_ALU, "vabs"); return 1; }
            if (opc2 == 0x1 && opc3 == 1) { o->op = OP_VNEG;  set(o, A_ALU, "vneg"); return 1; }
            if (opc2 == 0x1 && opc3 == 3) { o->op = OP_VSQRT; set(o, A_ALU, "vsqrt"); return 1; }

            /* Half precision: opc2 0010 widens a half to single, 0011 narrows
             * a single to half; bit 7 (T) names the top half of the S
             * register rather than the bottom. A retail title packs its
             * vertex data this way. Both sides are S registers. */
            if ((opc2 == 0x2 || opc2 == 0x3) && (opc3 & 1) && !dp) {
                o->op      = opc2 == 0x2 ? OP_VCVT_F16_F32 : OP_VCVT_F32_F16;
                o->imm     = (uint32_t)((h2 >> 7) & 1);
                o->has_imm = 1;
                set(o, A_ALU, opc2 == 0x2 ? "vcvt f32,f16" : "vcvt f16,f32");
                return 1;
            }

            /* VCMP and VCMPE. The E form differs only in whether a quiet NaN
             * raises an exception, which this runtime does not model. */
            if ((opc2 == 0x4 || opc2 == 0x5) && (opc3 & 1)) {
                o->op = OP_VCMP;
                /* opc2 == 5 compares against zero rather than a register. */
                if (opc2 == 0x5) { o->vm = ARM_NO_REG; o->has_imm = 1; o->imm = 0; }
                set(o, A_ALU, "vcmp");
                return 1;
            }

            /* Integer to float: opc2 1000, with the source signedness in the
             * op bit rather than a separate field. */
            if (opc2 == 0x8 && (opc3 & 1)) {
                o->op = OP_VCVT_I2F;
                /* The signedness is bit 7 here, not the group's op bit 6,
                 * which this form always has set. Reading bit 6 made every
                 * conversion signed, and the C library's unsigned divide,
                 * which goes through a double for large quotients, turned
                 * 0x81959988 / 3 into 0. */
                o->vfp_unsigned = (uint8_t)(((h2 >> 7) & 1) == 0);
                /* The integer side is always an S register, whatever the
                 * float side is. Numbering it as a D register, as this once
                 * did for the f64 form, read the wrong register silently. */
                o->vm = (int8_t)vfp_sreg(vm, M);
                set(o, A_ALU, "vcvt f,i");
                return 1;
            }

            /* Float to integer: opc2 110x (round to nearest) or 111x (toward
             * zero). Only the toward-zero forms are claimed, because that is
             * what C's conversion does and the other rounding modes would need
             * explicit modelling to be right. */
            /* Between precisions: opc2 0111, opc3 11. The coprocessor bit
             * names the SOURCE size, so the destination is the other kind. */
            if (opc2 == 0x7 && opc3 == 3) {
                o->op = OP_VCVT_FF;
                o->vd = (int8_t)(dp ? vfp_sreg(vd, D) : vfp_dreg(vd, D));
                set(o, A_ALU, dp ? "vcvt.f32.f64" : "vcvt.f64.f32");
                return 1;
            }

            if ((opc2 & 0xE) == 0xC && (opc3 & 1)) {
                o->op = OP_VCVT_F2I;
                o->vfp_unsigned = (uint8_t)((opc2 & 1) == 0);
                o->vd = (int8_t)vfp_sreg(vd, D);     /* the integer result */
                set(o, A_ALU, "vcvt i,f");
                return 1;
            }
            return 0;
        }

        default:
            return 0;
    }
}

/* --- Thumb, 32-bit ---------------------------------------------------------- */

/* ThumbExpandImm — the 12-bit field that encodes a 32-bit constant.
 *
 * Two entirely different encodings share the field, selected by its top two
 * bits. With them clear, the low byte is replicated into a pattern (once,
 * halfword-spaced, byte-spaced, or all four); otherwise the field is a
 * rotate-right of a value whose top bit is implicit and always set.
 *
 * Reading it as a plain 12-bit integer is the obvious mistake, and it is quiet:
 * small constants happen to be in the first case with pattern 00, where the
 * naive reading is correct. Everything above 0xFF is then wrong. */
static uint32_t thumb_expand_imm(uint32_t imm12) {
    if ((imm12 & 0xC00) == 0) {
        uint32_t b = imm12 & 0xFF;
        switch ((imm12 >> 8) & 3) {
            case 0:  return b;
            case 1:  return (b << 16) | b;
            case 2:  return (b << 24) | (b << 8);
            default: return (b << 24) | (b << 16) | (b << 8) | b;
        }
    }
    /* The 8-bit value always has bit 7 set implicitly, so only 7 bits are
     * stored. Forgetting that leaves every rotated constant short by 0x80. */
    uint32_t v = 0x80u | (imm12 & 0x7F);
    uint32_t r = (imm12 >> 7) & 0x1F;
    return r ? ((v >> r) | (v << (32 - r))) : v;
}

/* --- Advanced SIMD ------------------------------------------------------------ */
/*
 * The forms a running program has asked for, on D registers, lane by lane
 * (neon.h). GCC vectorises the C library's own code, so a newlib-linked
 * module reaches these before it reaches its own first line: VMOV immediate,
 * VDUP, VADD/VSUB/VCEQ/VPADD, the bitwise ops, VLD1/VST1, and the 32-bit lane
 * moves. Everything else in the space stays classified as SIMD and traps.
 */

static uint64_t neon_rep(uint64_t v, int bits) {
    uint64_t r = 0;
    for (int s = 0; s < 64; s += bits) r |= v << s;
    return r;
}

/* AdvSIMDExpandImm: the modified immediate of VMOV/VMVN/VORR/VBIC. */
static int neon_expand_imm(uint32_t cmode, uint32_t op, uint32_t imm8, uint64_t *out) {
    switch (cmode) {
    case 0x0: case 0x1: *out = neon_rep(imm8, 32);                        return 1;
    case 0x2: case 0x3: *out = neon_rep((uint64_t)imm8 << 8, 32);         return 1;
    case 0x4: case 0x5: *out = neon_rep((uint64_t)imm8 << 16, 32);        return 1;
    case 0x6: case 0x7: *out = neon_rep((uint64_t)imm8 << 24, 32);        return 1;
    case 0x8: case 0x9: *out = neon_rep(imm8, 16);                        return 1;
    case 0xA: case 0xB: *out = neon_rep((uint64_t)imm8 << 8, 16);         return 1;
    case 0xC:           *out = neon_rep(((uint64_t)imm8 << 8) | 0xFF, 32);     return 1;
    case 0xD:           *out = neon_rep(((uint64_t)imm8 << 16) | 0xFFFF, 32);  return 1;
    case 0xE:
        if (op == 0) { *out = neon_rep(imm8, 8); return 1; }
        {   /* each bit of imm8 becomes a byte: the 64-bit mask form */
            uint64_t r = 0;
            for (int i = 0; i < 8; i++) if (imm8 & (1u << i)) r |= 0xFFull << (8 * i);
            *out = r;
            return 1;
        }
    case 0xF:
        if (op != 0) return 0;
        {   /* VFPExpandImm, single precision, replicated */
            uint32_t a = (imm8 >> 7) & 1, b = (imm8 >> 6) & 1;
            uint32_t f = (a << 31) | ((b ? 0u : 1u) << 30) | (b ? (0x1Fu << 25) : 0u)
                       | (((imm8 >> 4) & 3) << 23) | ((imm8 & 0xF) << 19);
            *out = neon_rep(f, 32);
            return 1;
        }
    }
    return 0;
}

static int decode_neon(uint16_t h1, uint16_t h2, arm_insn *o) {
    /* Element or structure load/store, multiple single elements (VLD1/VST1):
     * 1111 1001 0 D L 0 Rn | Vd type size align Rm. type names the register
     * count; Rm = 15 is no write-back, 13 write-back by the bytes moved, else
     * post-index by the register. The lane size does not change the bytes a
     * little-endian machine moves, so only the count matters here. */
    if ((h1 & 0xFF90) == 0xF900) {
        int D = (h1 >> 6) & 1, L = (h1 >> 5) & 1;
        int type = (h2 >> 8) & 0xF;
        int regs = type == 0x7 ? 1 : type == 0xA ? 2 : type == 0x6 ? 3 : type == 0x2 ? 4 : 0;
        o->vd        = (int8_t)(((h2 >> 12) & 0xF) | (D << 4));
        o->rn        = h1 & 0xF;
        o->rm        = h2 & 0xF;
        o->has_imm   = 1;
        o->neon_size = (uint8_t)(8 << ((h2 >> 6) & 3));
        o->writeback = o->rm != 15;
        if (regs) {
            o->op  = L ? OP_NEON_LD1 : OP_NEON_ST1;
            o->imm = (uint32_t)regs;
            set(o, L ? A_LOAD : A_STORE, L ? "vld1" : "vst1");
            return 1;
        }
        /* VLD2-4 / VST2-4, multiple structures: n elements interleaved in
         * memory, one register per element. type 8/9: two, 3: two twice
         * over; 4/5: three; 0/1: four; the odd types space the registers
         * two apart. Anything else in the space is claimed as SIMD, never
         * handed on: the core load/store decoder shares the top bits and
         * would turn it into a halfword store. */
        int n = 0, spacing = 1, groups = 1;
        switch (type) {
        case 0x8: n = 2; break;
        case 0x9: n = 2; spacing = 2; break;
        case 0x3: n = 2; groups = 2; break;
        case 0x4: n = 3; break;
        case 0x5: n = 3; spacing = 2; break;
        case 0x0: n = 4; break;
        case 0x1: n = 4; spacing = 2; break;
        default: set(o, A_SIMD, "simd"); return 1;
        }
        if (o->neon_size == 64) { set(o, A_SIMD, "simd"); return 1; }
        o->op       = L ? OP_NEON_LDN : OP_NEON_STN;
        o->imm      = (uint32_t)n;
        o->bf_lsb   = (uint8_t)spacing;
        o->bf_width = (uint8_t)groups;
        set(o, L ? A_LOAD : A_STORE, L ? "vldn" : "vstn");
        return 1;
    }

    /* Element or structure load/store, single structure:
     * 1111 1001 1 D L 0 Rn | Vd B index_align Rm. B in bits 11:8: 11nn is
     * "to all lanes" of n+1 registers (size in 7:6, T in 5), otherwise
     * size in 11:10 and n in 9:8 for one lane of each of n+1 registers,
     * the lane and the register spacing packed into index_align. The
     * retail title keeps a loop's two loads in `vld1.32 {d8[]}, [sl]`; as
     * a halfword store it wrote the loop counter over the record. */
    if ((h1 & 0xFF90) == 0xF980) {
        int D = (h1 >> 6) & 1, L = (h1 >> 5) & 1;
        int B = (h2 >> 8) & 0xF;
        o->vd        = (int8_t)(((h2 >> 12) & 0xF) | (D << 4));
        o->rn        = h1 & 0xF;
        o->rm        = h2 & 0xF;
        o->has_imm   = 1;
        o->writeback = o->rm != 15;
        if ((B >> 2) == 3) {
            int n = (B & 3) + 1, sz = (h2 >> 6) & 3, T = (h2 >> 5) & 1;
            if (!L || sz == 3) { set(o, A_SIMD, "simd"); return 1; }
            o->op        = OP_NEON_LD_ALL;
            o->imm       = (uint32_t)n;
            o->neon_size = (uint8_t)(8 << sz);
            o->bf_lsb    = (uint8_t)(n == 1 ? 1 : T + 1);     /* spacing */
            o->bf_width  = (uint8_t)(n == 1 ? T + 1 : n);     /* registers */
            set(o, A_LOAD, "vld-all");
            return 1;
        }
        int sz = B >> 2, n = (B & 3) + 1, ia = (h2 >> 4) & 0xF;
        int index = 0, spacing = 1;
        if (sz == 0)      index = ia >> 1;
        else if (sz == 1) { index = ia >> 2; spacing = (ia & 2) ? 2 : 1; }
        else if (sz == 2) { index = ia >> 3; spacing = (ia & 4) ? 2 : 1; }
        else { set(o, A_SIMD, "simd"); return 1; }
        o->op        = L ? OP_NEON_LD_LANE : OP_NEON_ST_LANE;
        o->imm       = (uint32_t)n;
        o->imm64     = (uint64_t)index;
        o->neon_size = (uint8_t)(8 << sz);
        o->bf_lsb    = (uint8_t)spacing;
        o->bf_width  = 1;
        set(o, L ? A_LOAD : A_STORE, L ? "vld-lane" : "vst-lane");
        return 1;
    }

    /* VDUP from a core register: 1110 1110 1 B Q 0 Vd | Rt 1011 D 0 E 1 0000 */
    if ((h1 & 0xFF90) == 0xEE80 && (h2 & 0x0F5F) == 0x0B10) {
        int B = (h1 >> 6) & 1, Q = (h1 >> 5) & 1, E = (h2 >> 5) & 1, D = (h2 >> 7) & 1;
        int be = (B << 1) | E;
        if (be == 3) return 0;
        o->op        = OP_NEON_DUP;
        o->vd        = (int8_t)((h1 & 0xF) | (D << 4));
        o->rt        = (h2 >> 12) & 0xF;
        o->neon_q    = (uint8_t)Q;
        o->neon_size = be == 0 ? 32 : be == 1 ? 16 : 8;
        set(o, A_SIMD, "vdup");
        return 1;
    }

    /* VMOV core register to a 32-bit lane: 1110 1110 0 0 x 0 Vd | Rt 1011 D 00 1 0000 */
    if ((h1 & 0xFFD0) == 0xEE00 && (h2 & 0x0F7F) == 0x0B10) {
        o->op  = OP_NEON_SET32;
        o->vd  = (int8_t)((h1 & 0xF) | (((h2 >> 7) & 1) << 4));
        o->rt  = (h2 >> 12) & 0xF;
        o->imm = (h1 >> 5) & 1;
        o->has_imm = 1;
        set(o, A_SIMD, "vmov.32 d[x], r");
        return 1;
    }

    /* VMOV a 32-bit lane to a core register: 1110 1110 0 0 x 1 Vn | Rt 1011 N 00 1 0000 */
    if ((h1 & 0xFFD0) == 0xEE10 && (h2 & 0x0F7F) == 0x0B10) {
        o->op  = OP_NEON_GET32;
        o->vn  = (int8_t)((h1 & 0xF) | (((h2 >> 7) & 1) << 4));
        o->rt  = (h2 >> 12) & 0xF;
        o->imm = (h1 >> 5) & 1;
        o->has_imm = 1;
        set(o, A_SIMD, "vmov.32 r, d[x]");
        return 1;
    }

    /* Data processing: 111 U 1111 ... */
    if ((h1 & 0xEF00) != 0xEF00) return 0;

    int U = (h1 >> 12) & 1, D = (h1 >> 6) & 1;
    int N = (h2 >> 7) & 1, Q = (h2 >> 6) & 1, M = (h2 >> 5) & 1;
    o->vd     = (int8_t)(((h2 >> 12) & 0xF) | (D << 4));
    o->vn     = (int8_t)((h1 & 0xF) | (N << 4));
    o->vm     = (int8_t)((h2 & 0xF) | (M << 4));
    o->neon_q = (uint8_t)Q;

    /* One register and a modified immediate:
     * 111 i 1111 1 D 000 imm3 | Vd cmode 0 Q op 1 imm4 */
    if ((h1 & 0x00B8) == 0x0080 && (h2 & 0x0090) == 0x0010) {
        uint32_t imm8  = (uint32_t)(((h1 >> 12) & 1) << 7) | (uint32_t)((h1 & 7) << 4) | (uint32_t)(h2 & 0xF);
        uint32_t cmode = (h2 >> 8) & 0xF, op = (h2 >> 5) & 1;
        uint64_t v;
        if (!neon_expand_imm(cmode, op, imm8, &v)) return 0;
        int orr_form = (cmode & 1) && cmode < 0xC;          /* 0xx1, 10x1 */
        if (op == 0)           o->op = orr_form ? OP_NEON_ORR_IMM : OP_NEON_MOV_IMM;
        else if (cmode == 0xE) o->op = OP_NEON_MOV_IMM;      /* the byte-mask form */
        else if (orr_form)     o->op = OP_NEON_BIC_IMM;
        else                 { o->op = OP_NEON_MOV_IMM; v = ~v; }   /* VMVN */
        o->imm64 = v;
        set(o, A_SIMD, o->op == OP_NEON_MOV_IMM ? "vmov imm" : o->op == OP_NEON_ORR_IMM ? "vorr imm" : "vbic imm");
        return 1;
    }

    /* Two registers and a shift amount: 111 U 1111 1 D imm6 Vd | opc L Q M 1 Vm.
     * The lane width is the position of the top set bit of L:imm6 and the
     * shift sits in the bits below it, counted up from the lane's bottom
     * for left shifts and down from its top for right ones. The retail
     * title's favourite is `vsli.64 d, d, #32`, which is how the compiler
     * packs two 32-bit halves into one D register. */
    if ((h1 & 0x0080) && (h2 & 0x0010)) {
        int imm6 = h1 & 0x3F, L = (h2 >> 7) & 1, opc = (h2 >> 8) & 0xF;
        int size, left, right;
        if (L)                { size = 64; left = imm6;      right = 64 - imm6; }
        else if (imm6 & 0x20) { size = 32; left = imm6 - 32; right = 64 - imm6; }
        else if (imm6 & 0x10) { size = 16; left = imm6 - 16; right = 32 - imm6; }
        else if (imm6 & 0x08) { size = 8;  left = imm6 - 8;  right = 16 - imm6; }
        else return 0;                       /* the modified-immediate space */
        o->neon_size = (uint8_t)size;
        o->has_imm = 1;
        o->vfp_unsigned = (uint8_t)U;
        switch (opc) {
        case 0x0: o->op = OP_NEON_SHR;  o->imm = (uint32_t)right; set(o, A_SIMD, U ? "vshr.u" : "vshr.s");   return 1;
        case 0x1: o->op = OP_NEON_SRA;  o->imm = (uint32_t)right; set(o, A_SIMD, U ? "vsra.u" : "vsra.s");   return 1;
        case 0x2: o->op = OP_NEON_RSHR; o->imm = (uint32_t)right; set(o, A_SIMD, U ? "vrshr.u" : "vrshr.s"); return 1;
        case 0x3: o->op = OP_NEON_RSRA; o->imm = (uint32_t)right; set(o, A_SIMD, U ? "vrsra.u" : "vrsra.s"); return 1;
        case 0x4:
            if (!U) return 0;
            o->op = OP_NEON_SRI; o->imm = (uint32_t)right; set(o, A_SIMD, "vsri"); return 1;
        case 0x5:
            o->op = U ? OP_NEON_SLI : OP_NEON_SHL; o->imm = (uint32_t)left;
            set(o, A_SIMD, U ? "vsli" : "vshl.i"); return 1;
        case 0x8:
            /* VSHRN / VRSHRN: a Q source narrowed to a D, bit 6 the
             * rounding. The U form is the saturating VQSHRUN. */
            if (U || L) return 0;
            o->op = (h2 & 0x0040) ? OP_NEON_RSHRN : OP_NEON_SHRN;
            o->neon_size = (uint8_t)(size * 2);  /* the source lane */
            o->imm = (uint32_t)right;
            o->neon_q = 0;
            set(o, A_SIMD, (h2 & 0x0040) ? "vrshrn" : "vshrn");
            return 1;
        case 0xA:
            /* VMOVL / VSHLL: a D source widened to a Q, then shifted left
             * by the amount, which is zero for the plain move. */
            if (L || (h2 & 0x0040)) return 0;
            o->op = left ? OP_NEON_SHLL : OP_NEON_MOVL;
            o->imm = (uint32_t)left;
            o->neon_q = 1;
            set(o, A_SIMD, left ? (U ? "vshll.u" : "vshll.s") : (U ? "vmovl.u" : "vmovl.s"));
            return 1;
        default:
            return 0;                        /* saturating shifts, fixed-point conversions */
        }
    }

    /* Two registers and a scalar: 111 Q 1111 1 D size Vn Vd | opc N 1 M 0 Vm.
     * One operand is a single lane of a D register, broadcast. Bit 28 is Q
     * here rather than U, and for 16-bit lanes the lane index borrows the
     * top bit of Vm. */
    if ((h1 & 0x0080) && (h1 & 0x0030) != 0x0030 && (h2 & 0x0050) == 0x0040) {
        int Qf = (h1 >> 12) & 1, size = (h1 >> 4) & 3, opc = (h2 >> 8) & 0xF;
        int vm4 = h2 & 0xF, M = (h2 >> 5) & 1, F = opc & 1;
        if (size == 0 || (F && size != 2)) return 0;
        o->neon_q    = (uint8_t)Qf;
        o->neon_size = (uint8_t)(8 << size);
        o->neon_f    = (uint8_t)F;
        if (size == 2) { o->vm = (int8_t)vm4;       o->imm = (uint32_t)M; }
        else           { o->vm = (int8_t)(vm4 & 7); o->imm = (uint32_t)((M << 1) | (vm4 >> 3)); }
        o->has_imm = 1;
        switch (opc & 0xE) {
        case 0x0: o->op = OP_NEON_MLA_SCALAR; set(o, A_SIMD, F ? "vmla.f32 d[x]" : "vmla.i d[x]"); return 1;
        case 0x4: o->op = OP_NEON_MLS_SCALAR; set(o, A_SIMD, F ? "vmls.f32 d[x]" : "vmls.i d[x]"); return 1;
        case 0x8: o->op = OP_NEON_MUL_SCALAR; set(o, A_SIMD, F ? "vmul.f32 d[x]" : "vmul.i d[x]"); return 1;
        default:  return 0;                  /* the long and saturating forms */
        }
    }

    /* Three registers of the same length: 111 U 1111 0 D size Vn | Vd A N Q M B Vm.
     *
     * The floating-point forms are the same encoding with size<0> clear (the
     * Cortex-A9 has no half-precision arithmetic, so they are all f32) and
     * size<1> choosing between two operations. The retail census put these
     * first: VMUL.F32 alone was 30,000 instructions, VADD/VSUB/VPADD.F32 and
     * the two Newton-Raphson steps most of the rest. */
    if ((h1 & 0x0080) == 0) {
        int size = (h1 >> 4) & 3, A = (h2 >> 8) & 0xF, B = (h2 >> 4) & 1;
        int f32 = (size & 1) == 0, hi = (size >> 1) & 1;
        o->neon_size = (uint8_t)(8 << size);
        if (A == 0x1 && B == 1) {
            static const arm_op l0[4] = { OP_NEON_AND, OP_NEON_BIC, OP_NEON_ORR, OP_NEON_ORN };
            static const arm_op l1[4] = { OP_NEON_EOR, OP_NEON_BSL, OP_NEON_BIT, OP_NEON_BIF };
            static const char *n0[4] = { "vand", "vbic", "vorr", "vorn" };
            static const char *n1[4] = { "veor", "vbsl", "vbit", "vbif" };
            o->op = U ? l1[size] : l0[size];
            set(o, A_SIMD, U ? n1[size] : n0[size]);
            return 1;
        }
        if (A == 0x3) {
            o->op = B ? OP_NEON_CGE : OP_NEON_CGT;
            o->vfp_unsigned = (uint8_t)U;
            set(o, A_SIMD, B ? (U ? "vcge.u" : "vcge.s") : (U ? "vcgt.u" : "vcgt.s"));
            return 1;
        }
        /* VSHL by register: each lane of Vm shifted by the signed byte in
         * the same lane of Vn, left when it is positive and right when it
         * is negative. U chooses the kind of right shift. */
        if (A == 0x4 && B == 0) {
            o->op = OP_NEON_SHL_REG;
            o->vfp_unsigned = (uint8_t)U;
            set(o, A_SIMD, U ? "vshl.u reg" : "vshl.s reg");
            return 1;
        }
        if (A == 0x6) {
            o->op = B ? OP_NEON_MIN : OP_NEON_MAX;
            o->vfp_unsigned = (uint8_t)U;
            set(o, A_SIMD, B ? (U ? "vmin.u" : "vmin.s") : (U ? "vmax.u" : "vmax.s"));
            return 1;
        }
        if (A == 0x8 && B == 0) { o->op = U ? OP_NEON_SUB : OP_NEON_ADD; set(o, A_SIMD, U ? "vsub.i" : "vadd.i"); return 1; }
        if (A == 0x8 && B == 1) { o->op = U ? OP_NEON_CEQ : OP_NEON_TST; set(o, A_SIMD, U ? "vceq.i" : "vtst");  return 1; }
        if (A == 0x9 && B == 0) { o->op = U ? OP_NEON_MLS : OP_NEON_MLA; set(o, A_SIMD, U ? "vmls.i" : "vmla.i"); return 1; }
        if (A == 0x9 && B == 1 && !U) { o->op = OP_NEON_MUL; set(o, A_SIMD, "vmul.i"); return 1; }
        if (A == 0xB && B == 1 && !U && !Q) { o->op = OP_NEON_PADD; set(o, A_SIMD, "vpadd.i"); return 1; }

        if (!f32) return 0;
        o->neon_size = 32;
        o->neon_f = 1;
        if (A == 0xD && B == 0) {
            if (!U) { o->op = hi ? OP_NEON_FSUB : OP_NEON_FADD; set(o, A_SIMD, hi ? "vsub.f32" : "vadd.f32"); return 1; }
            if (hi) { o->op = OP_NEON_FABD; set(o, A_SIMD, "vabd.f32"); return 1; }
            if (Q) return 0;                          /* VPADD has no Q form */
            o->op = OP_NEON_FPADD; set(o, A_SIMD, "vpadd.f32"); return 1;
        }
        if (A == 0xD && B == 1) {
            if (!U) { o->op = hi ? OP_NEON_FMLS : OP_NEON_FMLA; set(o, A_SIMD, hi ? "vmls.f32" : "vmla.f32"); return 1; }
            if (hi) return 0;
            o->op = OP_NEON_FMUL; set(o, A_SIMD, "vmul.f32"); return 1;
        }
        if (A == 0xE && B == 0) {
            if (!U) { if (hi) return 0; o->op = OP_NEON_FCEQ; set(o, A_SIMD, "vceq.f32"); return 1; }
            o->op = hi ? OP_NEON_FCGT : OP_NEON_FCGE; set(o, A_SIMD, hi ? "vcgt.f32" : "vcge.f32"); return 1;
        }
        if (A == 0xF && B == 0 && !U) { o->op = hi ? OP_NEON_FMIN : OP_NEON_FMAX; set(o, A_SIMD, hi ? "vmin.f32" : "vmax.f32"); return 1; }
        if (A == 0xF && B == 1 && !U) { o->op = hi ? OP_NEON_FRSQRTS : OP_NEON_FRECPS; set(o, A_SIMD, hi ? "vrsqrts.f32" : "vrecps.f32"); return 1; }
        return 0;
    }

    /* Bits 23 and 21:20 all set: 111 U 1111 1 D 11 .... Four groups share
     * it, told apart by U and by bits 11:8 and 4 of the second halfword. */
    if ((h1 & 0x00B0) == 0x00B0) {
        if (!U) {
            /* VEXT: 1110 1111 1 D 11 Vn | Vd imm4 N Q M 0 Vm. Bytes imm4 and up
             * of the pair vm:vn, vn being the low half. The compiler's way of
             * swapping the halves of a Q register is `vext.8 q, q, q, #8`. */
            if (h2 & 0x0010) return 0;
            uint32_t pos = (h2 >> 8) & 0xF;
            if (!Q && pos > 7) return 0;
            o->op = OP_NEON_EXT;
            o->imm = pos;
            o->has_imm = 1;
            o->neon_size = 8;
            set(o, A_SIMD, "vext");
            return 1;
        }

        /* VDUP (scalar): 1111 1111 1 D 11 imm4 | Vd 1100 0 Q M 0 Vm. The lowest
         * set bit of imm4 is the lane width, the bits above it the lane. The
         * single most frequent untranslated instruction of the retail title:
         * every vector-by-scalar multiply the compiler emits starts with one. */
        if ((h2 & 0x0F90) == 0x0C00) {
            int imm4 = h1 & 0xF;
            if (imm4 & 1)      { o->neon_size = 8;  o->imm = (uint32_t)imm4 >> 1; }
            else if (imm4 & 2) { o->neon_size = 16; o->imm = (uint32_t)imm4 >> 2; }
            else if (imm4 & 4) { o->neon_size = 32; o->imm = (uint32_t)imm4 >> 3; }
            else return 0;
            o->op = OP_NEON_DUP_LANE;
            o->has_imm = 1;
            set(o, A_SIMD, "vdup lane");
            return 1;
        }

        /* VTBL and VTBX: 1111 1111 1 D 11 Vn | Vd 10 len N op M 0 Vm. Each
         * byte of vm is a place in a table made of len + 1 registers from
         * vn; a place beyond the table reads zero (VTBL) or leaves the byte
         * of vd alone (VTBX). What a hand-written routine shuffles bytes
         * with: a retail title's vertex code has 134 of them. */
        if ((h2 & 0x0C10) == 0x0800) {
            o->op = (h2 & 0x0040) ? OP_NEON_TBX : OP_NEON_TBL;
            o->imm = ((uint32_t)(h2 >> 8) & 3) + 1;
            o->has_imm = 1;
            o->neon_q = 0;                      /* that bit is the choice between the two */
            o->neon_size = 8;
            set(o, A_SIMD, (h2 & 0x0040) ? "vtbx" : "vtbl");
            return 1;
        }

        /* Two registers, miscellaneous:
         * 1111 1111 1 D 11 size opc1 | Vd 0 opc2 Q M 0 Vm */
        if ((h2 & 0x0810) == 0) {
            int size = (h1 >> 2) & 3, opc1 = h1 & 3, opc2 = (h2 >> 7) & 0xF;
            o->neon_size = (uint8_t)(8 << size);
            switch (opc1) {
            case 0:
                /* The architecture's table gives this column as five bits,
                 * opc2 then Q; opc2 alone is its first four. */
                switch (opc2) {
                case 0: case 1: case 2: {              /* VREV64 / VREV32 / VREV16 */
                    int group = 64 >> opc2;
                    if (o->neon_size >= group) return 0;
                    o->op = OP_NEON_REV;
                    o->imm = (uint32_t)group;
                    o->has_imm = 1;
                    set(o, A_SIMD, group == 64 ? "vrev64" : group == 32 ? "vrev32" : "vrev16");
                    return 1;
                }
                case 4: case 5:                        /* VPADDL, signed and unsigned */
                    if (size == 3) return 0;
                    o->op = OP_NEON_PADDL;
                    o->vfp_unsigned = (uint8_t)(opc2 & 1);
                    set(o, A_SIMD, "vpaddl");
                    return 1;
                case 9:  if (size == 3) return 0; o->op = OP_NEON_CLZ; set(o, A_SIMD, "vclz"); return 1;
                case 10: if (size != 0) return 0; o->op = OP_NEON_CNT; set(o, A_SIMD, "vcnt"); return 1;
                case 11: if (size != 0) return 0; o->op = OP_NEON_MVN; set(o, A_SIMD, "vmvn"); return 1;
                default: return 0;
                }
            case 1: {
                static const char *cmp0[5] = { "vcgt #0", "vcge #0", "vceq #0", "vcle #0", "vclt #0" };
                int F = (opc2 >> 3) & 1, sub = opc2 & 7;
                if (size == 3 || (F && size != 2)) return 0;
                o->neon_f = (uint8_t)F;
                if (sub <= 4) {
                    o->op = OP_NEON_CMP0;
                    o->imm = (uint32_t)sub;
                    o->has_imm = 1;
                    set(o, A_SIMD, cmp0[sub]);
                    return 1;
                }
                if (sub == 6) { o->op = F ? OP_NEON_FABS : OP_NEON_ABS; set(o, A_SIMD, F ? "vabs.f32" : "vabs"); return 1; }
                if (sub == 7) { o->op = F ? OP_NEON_FNEG : OP_NEON_NEG; set(o, A_SIMD, F ? "vneg.f32" : "vneg"); return 1; }
                return 0;
            }
            case 2:
                if (opc2 == 0 && size == 0) { o->op = OP_NEON_SWP; set(o, A_SIMD, "vswp"); return 1; }
                /* VTRN: a pair of registers read as a 2 x n matrix, transposed */
                if (opc2 == 1 && size != 3) { o->op = OP_NEON_TRN; set(o, A_SIMD, "vtrn"); return 1; }
                /* VMOVN: the low half of each lane of a Q, into a D. The
                 * size field names the RESULT lane; bit 6 set would be the
                 * saturating VQMOVUN. */
                if (opc2 == 4 && !(h2 & 0x0040) && size != 3) {
                    o->op = OP_NEON_MOVN;
                    o->neon_size = (uint8_t)(16 << size);  /* the source lane */
                    o->imm = 0;
                    o->has_imm = 1;
                    o->neon_q = 0;
                    set(o, A_SIMD, "vmovn");
                    return 1;
                }
                /* VCVT between half and single precision, four lanes:
                 * 1111 1111 1 D 11 01 10 | Vd 011 op 00 M 0 Vm. With op clear
                 * a Q of singles becomes a D of halves, which is how a title
                 * packs the vertices it builds at run time; with op set, the
                 * other way. */
                if ((opc2 & 0xD) == 0xC && size == 1 && !(h2 & 0x0040)) {
                    o->op = (opc2 & 2) ? OP_NEON_UNHALVE : OP_NEON_HALVE;
                    o->neon_q = 0;
                    o->neon_f = 1;
                    set(o, A_SIMD, (opc2 & 2) ? "vcvt.f32.f16" : "vcvt.f16.f32");
                    return 1;
                }
                return 0;                              /* VUZP/VZIP, the rest */
            case 3:
                if (size != 2) return 0;
                if ((opc2 & 0xC) == 0x8) {             /* VRECPE / VRSQRTE */
                    if (!((opc2 >> 1) & 1)) return 0;  /* the integer estimates */
                    o->neon_f = 1;
                    o->op = (opc2 & 1) ? OP_NEON_FRSQRTE : OP_NEON_FRECPE;
                    set(o, A_SIMD, (opc2 & 1) ? "vrsqrte.f32" : "vrecpe.f32");
                    return 1;
                }
                if ((opc2 & 0xC) == 0xC) {             /* VCVT, f32 and 32-bit integers */
                    o->op = OP_NEON_CVT;
                    o->imm = (uint32_t)(opc2 & 3);
                    o->has_imm = 1;
                    o->neon_f = 1;
                    set(o, A_SIMD, "vcvt");
                    return 1;
                }
                return 0;
            }
        }
        return 0;
    }
    return 0;
}

static void decode_t32(uint16_t h1, uint16_t h2, arm_insn *o) {
    o->raw = ((uint32_t)h1 << 16) | h2;

    /* The Advanced SIMD forms translated so far, claimed before anything else
     * because two of them (VDUP and the lane moves) share the VFP transfer
     * space and would otherwise be read as a double-precision operation. */
    if (decode_neon(h1, h2, o)) return;

    /* Advanced SIMD and VFP occupy the coprocessor space: bits 15:13 are 111
     * and bits 11:10 are 11, which is exactly `(h1 & 0xEC00) == 0xEC00`.
     *
     * An earlier mask of `(h1 & 0xEE00) == 0xEC00` matched only 0xEC and 0xED,
     * missing the whole of 0xEE and 0xEF — which is where VFP's CDP/MCR/MRC
     * forms live, and they are the bulk of it. Those ~2,800 instructions were
     * reported as UNKNOWN rather than SIMD, so every earlier SIMD percentage
     * this toolkit printed was an undercount and every "unknown" figure was
     * correspondingly inflated.
     *
     * Worth noting that this made the tool look better than it was on one axis
     * and worse on another, which is why the trap breakdown by kind was what
     * finally surfaced it: 2.16% of instructions decoding to "?" is a number
     * that demands an explanation, and there wasn't one. */
    /* MRC p15, 0, Rt, c13, c0, 3 reads TPIDRURO, the user-readable thread ID
     * register (claimed before the coprocessor space swallows it) that the kernel points at the running thread's TLS block. It
     * is how Sony's C runtime finds its per-thread state, and the only
     * system register user code on this console reads. */
    if (h1 == 0xEE1D && (h2 & 0x0FFF) == 0x0F70) {
        o->op = OP_MRC_TLS;
        o->rt = (h2 >> 12) & 0xF;
        set(o, A_SYS, "mrc tpidruro");
        return;
    }

    if ((h1 & 0xEC00) == 0xEC00) {
        if (decode_vfp(h1, h2, o)) return;
        set(o, A_SIMD, "simd/vfp");
        return;
    }

    /* Barriers and CLREX: 1111 0011 1011 1111 | 1000 1111 op option. DMB, DSB
     * and ISB order memory that this host's single thread already sees in
     * order; CLREX drops an exclusive monitor the runtime never arms (below).
     * All four do nothing here. */
    if (h1 == 0xF3BF && (h2 & 0xFF00) == 0x8F00) {
        int op = (h2 >> 4) & 0xF;
        if (op == 0x2 || op == 0x4 || op == 0x5 || op == 0x6) {
            o->op = OP_NOP;
            set(o, A_NOP, op == 0x2 ? "clrex" : op == 0x4 ? "dsb" : op == 0x5 ? "dmb" : "isb");
            return;
        }
    }

    /* 11110 ... with bit 15 of the second halfword set: branches and
     * miscellaneous control. */
    if ((h1 & 0xF800) == 0xF000 && (h2 & 0x8000)) {
        /* Four encodings share this group, and they are selected by exactly two
         * bits of h2: bit 14 says call-or-branch, bit 12 picks within the pair.
         *
         *   bit14 bit12
         *     0     0     B<cond>.W (T3), or a system instruction
         *     0     1     B.W       (T4)
         *     1     0     BLX imm   (T2)
         *     1     1     BL        (T1)
         *
         * Masking bits 14:12 together and testing `& 5` conflates them, which
         * is what was here: `== 5` caught only BL, `== 4` caught BLX imm and
         * called it B.W, and real B.W matched neither and fell through to the
         * CONDITIONAL path — where it read a cond field that does not exist in
         * T4 and assembled its offset with T3's 21-bit layout. Every
         * unconditional wide branch was getting a wrong target and a bogus
         * condition, and the one that surfaced it was a tail call into an
         * import stub landing two bytes past the stub. */
        int is_call = (h2 >> 14) & 1;
        int low     = (h2 >> 12) & 1;

        /* The J1/J2 encoding: the two J bits are stored XORed with the sign
         * bit, so they must be un-XORed before assembling the offset. Treating
         * them as plain address bits gives a target that is right for short
         * branches and wrong for long ones. */
        uint32_t s   = (h1 >> 10) & 1;
        uint32_t j1  = (h2 >> 13) & 1;
        uint32_t j2  = (h2 >> 11) & 1;
        uint32_t i1  = !(j1 ^ s);
        uint32_t i2  = !(j2 ^ s);
        uint32_t hi  = (s << 24) | (i1 << 23) | (i2 << 22)
                     | ((uint32_t)(h1 & 0x03FF) << 12);

        if (is_call && low) {                   /* BL */
            set(o, A_CALL, "bl");
            o->has_target = 1;
            o->target = o->addr + 4
                      + sign_extend(hi | ((uint32_t)(h2 & 0x07FF) << 1), 25);
            return;
        }

        if (is_call) {                          /* BLX immediate */
            /* Two things differ from BL and both matter. The offset is a
             * multiple of FOUR — the low encoding field is imm10L, bit 0 is
             * the H bit and is not part of it — and the base is Align(PC,4),
             * not PC, because the destination is ARM code and ARM
             * instructions are word-aligned. Using PC gives a target two bytes
             * high whenever the instruction sits at an odd halfword, which is
             * half the time. */
            set(o, A_CALL, "blx imm");
            o->switches_mode = 1;
            o->has_target = 1;
            o->target = ((o->addr + 4) & ~3u)
                      + sign_extend(hi | ((uint32_t)(h2 & 0x07FE) << 1), 25);
            return;
        }

        if (low) {                              /* B.W, unconditional */
            /* `op` matters as much as the class. The class routes the emitter's
             * fallback, but only `op` reaches the switch that actually
             * translates a branch — leaving it OP_NONE meant every wide branch
             * decoded perfectly, computed its target correctly, and then
             * trapped anyway. 2,359 instructions, purely for want of this
             * line. */
            o->op = OP_B;
            o->cond = ARM_COND_AL;
            set(o, A_BRANCH, "b.w");
            o->has_target = 1;
            o->target = o->addr + 4
                      + sign_extend(hi | ((uint32_t)(h2 & 0x07FF) << 1), 25);
            return;
        }

        /* Conditional B.W, or a system instruction when the condition field
         * (bits 9:6) is 111x. The test used to read bits 10:8, which takes in
         * the S bit: every backward `bgt.w` and `ble.w` (S = 1, cond = 110x)
         * was classed as a system instruction and trapped. */
        if (((h1 >> 6) & 0xE) == 0xE) { set(o, A_SYS, "sys"); return; }
        o->op = OP_B;
        o->cond = (uint8_t)((h1 >> 6) & 0xF);
        set(o, A_BRANCH, "b<cond>.w");
        o->conditional = 1;
        /* T3 has its own layout: a 6-bit high field rather than 10, and J1/J2
         * placed directly rather than un-XORed with S. Reusing the raw s/j1/j2
         * read above, since only the assembly differs. */
        {
            uint32_t imm  = (s << 20) | (j2 << 19) | (j1 << 18)
                          | ((uint32_t)(h1 & 0x003F) << 12)
                          | ((uint32_t)(h2 & 0x07FF) << 1);
            o->has_target = 1;
            o->target = o->addr + 4 + sign_extend(imm, 21);
        }
        return;
    }

    /* Exclusive loads and stores, the pair every lock and atomic counter is
     * built from:
     *   1110 1000 0101 Rn | Rt 1111 imm8      LDREX  rt, [rn, #imm8*4]
     *   1110 1000 0100 Rn | Rt Rd   imm8      STREX  rd, rt, [rn, #imm8*4]
     *   1110 1000 1101 Rn | Rt 1111 010x 1111 LDREXB/H
     *   1110 1000 1100 Rn | Rt 1111 010x Rd   STREXB/H
     * 17,000 of the retail title's traps. One guest thread runs at a time on
     * this host, so the monitor cannot be broken between the pair: the store
     * always succeeds and rd reads 0. */
    if ((h1 & 0xFFE0) == 0xE840 || ((h1 & 0xFFE0) == 0xE8C0 && (h2 & 0x00E0) == 0x0040)) {
        int load = (h1 & 0x0010) != 0, word = (h1 & 0x0080) == 0;
        o->op = load ? OP_LDREX : OP_STREX;
        o->rn = h1 & 0xF;
        o->rt = (h2 >> 12) & 0xF;
        if (word) {
            o->rd = load ? ARM_NO_REG : (h2 >> 8) & 0xF;
            o->imm = (uint32_t)(h2 & 0xFF) * 4u;
            o->mem_size = 32;
        } else {
            o->rd = load ? ARM_NO_REG : h2 & 0xF;
            o->imm = 0;
            o->mem_size = (h2 & 0x0010) ? 16 : 8;
        }
        o->has_imm = 1;
        o->mem_add = 1;
        set(o, load ? A_LOAD : A_STORE, load ? "ldrex" : "strex");
        return;
    }

    /* Load/store DUAL. Bit 6 separates these from the multiple forms below, and
     * the earlier mask excluded them by keeping that bit — so every LDRD/STRD
     * fell through to unknown. They are common: compilers use them to move
     * 64-bit values and adjacent struct fields in one instruction. */
    if ((h1 & 0xFE40) == 0xE840 && (h1 & 0x0120) != 0x0000) {
        int load = (h1 & 0x0010) != 0;
        o->op  = load ? OP_LDRD : OP_STRD;
        o->rn  = h1 & 0xF;
        o->rt  = (h2 >> 12) & 0xF;
        o->rt2 = (h2 >> 8) & 0xF;
        o->imm = (uint32_t)(h2 & 0xFF) * 4u;   /* scaled by 4 */
        o->has_imm = 1;
        o->mem_add   = (h1 & 0x0080) != 0;     /* U bit */
        o->writeback = (h1 & 0x0020) != 0;     /* W bit */
        o->mem_post  = (h1 & 0x0100) == 0;     /* P bit clear: post-indexed */
        set(o, load ? A_LOAD : A_STORE, load ? "ldrd" : "strd");
        return;
    }

    /* Load/store multiple. LDM with the PC in the list is a return. */
    if ((h1 & 0xFE40) == 0xE800) {
        int load = (h1 & 0x0010) != 0;
        o->op = load ? OP_POP : OP_PUSH;
        o->rn = h1 & 0xF;
        o->reglist = h2;
        o->writeback = (h1 & 0x0020) != 0;
        /* Bits 8:7 of the first halfword are the direction: 01 is
         * increment-after (LDMIA/STMIA, 0xE88x/0xE8Bx), 10 decrement-before
         * (LDMDB/STMDB, 0xE90x/0xE92x). Bit 7 alone tells them apart. */
        o->mem_add = (h1 & 0x0080) != 0;
        /* Only the SP-relative forms with the stack's own direction are
         * PUSH (STMDB sp!) and POP (LDMIA sp!); everything else is a general
         * LDM/STM against whatever base, which the emitter writes as the
         * loads and stores it is, in whichever direction. */
        if (o->rn != 13 || !o->writeback || (load && !o->mem_add) || (!load && o->mem_add))
            o->op = load ? OP_LDM : OP_STM;
        /* A pop that includes the PC is the return — AFTER the registers
         * before it are restored and the stack pointer moved. Classifying it
         * as a return without the list, as this once did, emitted a bare
         * `return` for every `ldmia.w sp!, {r4-r10, pc}`: callee-saved
         * registers came back holding the callee's locals, and the stack
         * pointer stayed where the callee left it. The 16-bit `pop {.., pc}`
         * never had the problem, which is why it took a function that saves
         * r8 and up to find. */
        if (load && (h2 & 0x8000)) { set(o, A_RETURN, "ldm {..,pc}"); return; }
        set(o, load ? A_LOADM : A_STOREM, "ldm/stm.w");
        return;
    }

    /* --- data processing -------------------------------------------------- */
    /*
     * The shifted-register (0xEA/0xEB) and modified-immediate (0xF0/0xF1)
     * groups share an operation field, an S bit, and the rn/rd placement, so
     * they are decoded together and differ only in how operand 2 is formed.
     *
     * Four operations change identity when a register field is r15, and the
     * architecture uses that rather than spending encoding space:
     *   rn == 15 turns ORR into MOV and ORN into MVN
     *   rd == 15 with S set turns AND/EOR/ADD/SUB into TST/TEQ/CMN/CMP
     * Missing either produces an instruction that reads a register which is not
     * an operand, and writes one that is not a destination.
     */
    if ((h1 & 0xFE00) == 0xEA00 || (h1 & 0xFA00) == 0xF000) {
        /* Which of the two groups matched — NOT a single bit. Both 0xEA/0xEB
         * and 0xF0/0xF1 have bit 15 set, so testing it classifies every
         * shifted-register instruction as an immediate one and then reads
         * operand 2 out of the wrong fields. */
        int is_imm = (h1 & 0xFA00) == 0xF000;
        if (is_imm && (h2 & 0x8000)) { set(o, A_UNKNOWN, "?"); return; }

        static const arm_op dp[16] = {
            OP_AND, OP_BIC, OP_ORR, OP_MVN, OP_EOR, OP_NONE, OP_NONE, OP_NONE,
            OP_ADD, OP_NONE, OP_ADC, OP_SBC, OP_NONE, OP_SUB, OP_RSB, OP_NONE
        };
        int opf = (h1 >> 5) & 0xF;
        o->op = dp[opf];
        if (o->op == OP_NONE) { set(o, A_ALU, "alu.w"); return; }

        o->rn = h1 & 0xF;
        o->rd = (h2 >> 8) & 0xF;
        o->sets_flags = (h1 & 0x0010) != 0;

        if (o->rn == 15) {
            if (opf == 0x2) o->op = OP_MOV;       /* orr rn=15 -> mov */
            if (opf == 0x3) o->op = OP_MVN;       /* orn rn=15 -> mvn */
            o->rn = ARM_NO_REG;
        }
        if (o->rd == 15) {
            /* rd == 15 is not a destination here. With S set it selects the
             * compare forms, where the result is discarded and only the flags
             * matter — but ONLY for these four encodings. Every other opf with
             * rd == 15 is UNPREDICTABLE in the architecture, and writing to
             * the PC without S is a branch this path does not model.
             *
             * Clearing rd for those too is how `? = r1 & ~0x18A0000` reached a
             * build: the op stayed BIC, the destination became "no register",
             * and the emitter printed the placeholder as though it were a
             * variable. Leaving op as OP_NONE traps instead, which is what an
             * encoding we cannot translate is supposed to do. */
            int compare = o->sets_flags
                       && (opf == 0x0 || opf == 0x4 || opf == 0x8 || opf == 0xD);
            if (compare) {
                if (opf == 0x0) o->op = OP_TST;
                if (opf == 0x4) o->op = OP_TST;   /* teq: same shape for us */
                if (opf == 0x8) o->op = OP_CMN;
                if (opf == 0xD) o->op = OP_CMP;
                o->rd = ARM_NO_REG;
            } else {
                o->op = OP_NONE;
                set(o, A_ALU, "alu.w rd=pc");
                return;
            }
        }

        if (is_imm) {
            uint32_t imm12 = (uint32_t)(((h1 >> 10) & 1) << 11)
                           | (uint32_t)(((h2 >> 12) & 7) << 8)
                           | (uint32_t)(h2 & 0xFF);
            o->imm = thumb_expand_imm(imm12);
            o->has_imm = 1;
        } else {
            o->rm = h2 & 0xF;
            o->shift_type = (h2 >> 4) & 3;
            o->shift_amt  = (uint8_t)((((h2 >> 12) & 7) << 2) | ((h2 >> 6) & 3));
        }
        set(o, A_ALU, "alu.w");
        return;
    }

    /* Plain binary immediate: MOVW/MOVT and the wide add/sub. These carry a
     * 16-bit literal rather than a modified immediate, and are how compilers
     * build addresses — so they are worth decoding even though they are only
     * two encodings. */
    /* The group is selected by bit 9, which is what separates plain binary
     * immediates from the modified immediates above. Masking any lower bit
     * excludes members of the group: an earlier mask of 0xFB40 kept bit 6,
     * which MOVW sets, so MOVW never matched at all. */
    if ((h1 & 0xFA00) == 0xF200 && !(h2 & 0x8000)) {
        uint32_t imm = (uint32_t)(((h1 >> 10) & 1) << 11)
                     | (uint32_t)(((h2 >> 12) & 7) << 8)
                     | (uint32_t)(h2 & 0xFF);
        o->rd = (h2 >> 8) & 0xF;
        o->has_imm = 1;

        uint16_t form = h1 & 0xFBF0;
        if (form == 0xF240) {                     /* movw: a 16-bit literal   */
            o->op = OP_MOV;
            o->imm = imm | ((uint32_t)(h1 & 0xF) << 12);
            set(o, A_ALU, "movw");
            return;
        }
        if (form == 0xF2C0) {                     /* movt                     */
            /* MOVT writes the top half and preserves the bottom, which is not
             * a move — but it IS trivially expressible, and it is load-bearing:
             * MOVW/MOVT is how ARM builds a 32-bit address, so leaving it
             * untranslated costs every constructed pointer its high 16 bits.
             * A trace of module_start showed the result — indirect branches to
             * 0x00000000 — which is what promoted this from "deliberately
             * trapped" to "the highest-value instruction remaining". */
            o->op = OP_MOVT;
            o->imm = imm | ((uint32_t)(h1 & 0xF) << 12);
            set(o, A_ALU, "movt");
            return;
        }
        if (form == 0xF200 || form == 0xF2A0) {   /* addw / subw              */
            o->op = (form == 0xF2A0) ? OP_SUB : OP_ADD;
            o->rn = h1 & 0xF;
            o->imm = imm;
            set(o, A_ALU, "addw/subw");
            return;
        }
        /* The rest of this group is bitfield and saturating arithmetic, split
         * by the same 5-bit op the forms above use. `imm` is not an immediate
         * here at all — the field holds a width or a bit position — which is
         * why these need their own operands rather than reusing `imm`. */
        uint32_t bop = (h1 >> 4) & 0x1F;
        uint32_t lsb = (uint32_t)(((h2 >> 12) & 7) << 2) | ((h2 >> 6) & 3);
        o->rn = h1 & 0xF;
        o->has_imm = 0;
        o->bf_lsb  = (uint8_t)lsb;

        if (bop == 0x14 || bop == 0x1C) {         /* sbfx / ubfx              */
            uint32_t width = (h2 & 0x1F) + 1;
            /* lsb + width past the end of the register is UNPREDICTABLE, and
             * a linear sweep over literal pools produces it. Refuse rather
             * than clamp: a trap says "this was not an instruction", a clamped
             * extract says nothing. */
            if (lsb + width <= 32) {
                o->op = (bop == 0x14) ? OP_SBFX : OP_UBFX;
                o->bf_width = (uint8_t)width;
            }
            set(o, A_ALU, (bop == 0x14) ? "sbfx" : "ubfx");
            return;
        }
        if (bop == 0x16) {                        /* bfi / bfc                */
            /* BFI names the field by its END bit, not its width — msb is an
             * absolute position, so the width has to be derived. BFC is the
             * same encoding with rn == 15, which here means "no source"
             * rather than "the PC". */
            uint32_t msb = h2 & 0x1F;
            int clear = (o->rn == 15);
            if (msb >= lsb) {
                o->op = clear ? OP_BFC : OP_BFI;
                o->bf_width = (uint8_t)(msb - lsb + 1);
            }
            if (clear) o->rn = ARM_NO_REG;
            set(o, A_ALU, clear ? "bfc" : "bfi");
            return;
        }
        /* SSAT/USAT: saturating arithmetic, not bitfield work. Named
         * separately so the trap report does not claim bitfield instructions
         * remain after these are the only thing left in the group. */
        o->rn = ARM_NO_REG;
        set(o, A_ALU, "sat");
        return;
    }

    /* Single load/store, immediate and register offset. A load into the PC is
     * a return. */
    if ((h1 & 0xFE00) == 0xF800) {
        int load = (h1 & 0x0010) != 0;
        int size = (h1 >> 5) & 3;                 /* 0 byte, 1 half, 2 word */

        if (load && ((h2 >> 12) & 0xF) == 0xF) {
            /* Rt = 15 in a WORD load is `ldr pc`: a return or a computed
             * jump. In a byte or halfword load it is not a load at all but
             * the cache hints PLD, PLDW and PLI, which share the encoding.
             * strlen opens with `pld [r0]`; reading it as `ldr pc` emitted a
             * bare return, and strlen returned its argument. */
            if (size == 2) { set(o, A_RETURN, "ldr pc"); return; }
            o->op = OP_NOP;
            set(o, A_NOP, "pld/pli");
            return;
        }
        int sign = (h1 & 0x0100) != 0;
        o->rt = (h2 >> 12) & 0xF;
        o->rn = h1 & 0xF;
        o->mem_add = 1;

        if (load) o->op = size == 0 ? (sign ? OP_LDRSB : OP_LDRB)
                        : size == 1 ? (sign ? OP_LDRSH : OP_LDRH) : OP_LDR;
        else      o->op = size == 0 ? OP_STRB : size == 1 ? OP_STRH : OP_STR;

        /* Rn = 15 is the literal form, 1111 1000 U S 0 1 1111 Rt imm12: the
         * offset is always twelve bits and bit 7 of the first halfword is
         * its sign, not the choice of the 12-bit form. Read through the
         * register-indexed branch, `ldr.w r2, [pc, #-32]` (a magic divide's
         * constant kept before the function) had an offset of zero, and the
         * word at PC is the next instruction: a vector's capacity came out
         * as 69 elements in a buffer of 30, and the heap went with it. */
        if (o->rn == 15 && load) {
            o->imm     = h2 & 0xFFF;
            o->has_imm = 1;
            o->mem_add = (uint8_t)((h1 & 0x0080) != 0);
            set(o, A_LOAD, "ldr literal.w");
            return;
        }

        if (h1 & 0x0080) {                        /* 12-bit unsigned offset */
            o->imm = h2 & 0xFFF;
            o->has_imm = 1;
        } else if (h2 & 0x0800) {
            /* The 8-bit form: 1 P U W imm8. P = 0 is post-indexed (the base is
             * used as it stands and the offset applied afterwards), W = 1
             * writes the base back, U is the sign. P = 1, W = 0 with U = 1 is
             * the unprivileged LDRT/STRT, which in user mode is the plain
             * positive-offset access. P = 0, W = 0 is not an encoding of
             * these and is left to fall through as unknown.
             *
             * Reading only the sign here, as this once did, turned
             * `strb.w r4, [r3], #1` into a store at r3 + 1 with no write-back:
             * a byte-fill loop that never advances. */
            uint32_t P = (h2 >> 10) & 1, U = (h2 >> 9) & 1, W = (h2 >> 8) & 1;
            if (!P && !W) { set(o, A_UNKNOWN, "?"); return; }
            o->imm       = h2 & 0xFF;
            o->has_imm   = 1;
            o->mem_add   = (uint8_t)U;
            o->writeback = (uint8_t)W;
            o->mem_post  = (uint8_t)!P;
        } else if ((h2 & 0x0FC0) == 0x0000) {     /* register offset */
            o->rm = h2 & 0xF;
            o->shift_amt = (uint8_t)((h2 >> 4) & 3);
        } else {
            set(o, A_UNKNOWN, "?");
            return;
        }
        set(o, load ? A_LOAD : A_STORE, "ldr/str.w");
        return;
    }

    /* The wide sign/zero extends, with their rotate, and the extend-and-add
     * forms that share the encoding (rn is 15 for the plain ones):
     * 1111 1010 0 op Rn | 1111 Rd 1 0 rotate Rm, op 000 SXTH, 001 UXTH, 010
     * SXTB16, 011 UXTB16, 100 SXTB, 101 UXTB. The op field is three bits. An
     * earlier revision took two of them and dropped the rotate, so every
     * `sxtb.w` was a halfword extend and every `uxtb.w r0, r1, ror #8` kept
     * sixteen bits of the wrong byte. No sample caught it: GCC reaches for
     * the 16-bit forms, and the rotated byte extracts are what SNC emits for
     * a hash loop. The retail title's resource names hashed wrong, and
     * nothing it asked for could be found. */
    if ((h1 & 0xFF80) == 0xFA00 && (h2 & 0xF0C0) == 0xF080) {
        static const arm_op   ex[6]  = { OP_SXTH, OP_UXTH, OP_SXTB16, OP_UXTB16, OP_SXTB, OP_UXTB };
        static const char    *exn[6] = { "sxth.w", "uxth.w", "sxtb16", "uxtb16", "sxtb.w", "uxtb.w" };
        uint32_t op = (h1 >> 4) & 7;
        if (op >= 6) { set(o, A_ALU, "alu.w"); return; }
        o->op = ex[op];
        o->rd = (h2 >> 8) & 0xF;
        o->rm = h2 & 0xF;
        o->rn = (h1 & 0xF) == 0xF ? ARM_NO_REG : (int8_t)(h1 & 0xF);
        o->shift_type = SH_ROR;
        o->shift_amt  = (uint8_t)(((h2 >> 4) & 3) * 8);
        set(o, A_ALU, exn[op]);
        return;
    }

    /* Table branch: a jump table in one instruction, indexing a byte or
     * halfword array to compute a forward branch. It is a computed transfer
     * that discovery cannot follow, and is named here rather than left unknown
     * so its cost is visible. */
    if ((h1 & 0xFFF0) == 0xE8D0 && (h2 & 0xFFE0) == 0xF000) {
        o->rn = h1 & 0xF;
        o->rm = h2 & 0xF;
        o->op = (h2 & 0x0010) ? OP_TBH : OP_TBB;
        set(o, A_INDIRECT, (h2 & 0x0010) ? "tbh" : "tbb");
        return;
    }

    /* Shift by register. */
    if ((h1 & 0xFF80) == 0xFA00 && (h2 & 0xF0F0) == 0xF000) {
        static const arm_op sh[4] = { OP_LSL, OP_LSR, OP_ASR, OP_ROR };
        o->op = sh[(h1 >> 5) & 3];
        o->rd = (h2 >> 8) & 0xF;
        o->rn = h1 & 0xF;
        o->rm = h2 & 0xF;
        o->sets_flags = (h1 & 0x0010) != 0;
        set(o, A_ALU, "shift reg.w");
        return;
    }

    /* Miscellaneous register operations: 1111 1010 1 op1 Rm | 1111 Rd 10 op2 Rm.
     * op1 = 01: REV (op2 00), REV16 (01), RBIT (10), REVSH (11);
     * op1 = 10, op2 = 00: SEL;   op1 = 11, op2 = 00: CLZ;
     * op1 = 00: the saturating QADD family, left classified. */
    if ((h1 & 0xFF80) == 0xFA80 && (h2 & 0xF0C0) == 0xF080) {
        uint32_t op1 = (h1 >> 4) & 3, op2 = (h2 >> 4) & 3;
        o->rd = (h2 >> 8) & 0xF;
        o->rn = h1 & 0xF;
        o->rm = h2 & 0xF;
        if (op1 == 1) {
            static const arm_op rv[4] = { OP_REV, OP_REV16, OP_RBIT, OP_REVSH };
            static const char *rvn[4] = { "rev.w", "rev16.w", "rbit", "revsh.w" };
            o->op = rv[op2];
            set(o, A_ALU, rvn[op2]);
            return;
        }
        if (op1 == 2 && op2 == 0) { o->op = OP_SEL; set(o, A_ALU, "sel"); return; }
        if (op1 == 3 && op2 == 0) { o->op = OP_CLZ; set(o, A_ALU, "clz"); return; }
        set(o, A_ALU, "sat");
        return;
    }

    /* Parallel addition and subtraction, unsigned: 1111 1010 1 op1 Rn | 1111 Rd 01 op2 Rm.
     * op2 = 00 is the modular form, which is what the C library's byte tricks
     * use: UADD8 (op1 000) and USUB8 (op1 100). The saturating and halving
     * forms (op2 01, 10) and the halfword and signed families stay classified
     * as "sat" until a module needs them. */
    if ((h1 & 0xFF80) == 0xFA80 && (h2 & 0xF0C0) == 0xF040) {
        uint32_t op1 = (h1 >> 4) & 7, op2 = (h2 >> 4) & 3;
        o->rd = (h2 >> 8) & 0xF;
        o->rn = h1 & 0xF;
        o->rm = h2 & 0xF;
        if (op2 == 0 && op1 == 0) { o->op = OP_UADD8; set(o, A_ALU, "uadd8"); return; }
        if (op2 == 0 && op1 == 4) { o->op = OP_USUB8; set(o, A_ALU, "usub8"); return; }
        set(o, A_ALU, "sat");
        return;
    }

    /* Multiply and multiply-accumulate: 1111 1011 0000 Rn | Ra Rd 000x Rm.
     * Ra == 15 is a plain MUL; x = 0 accumulates (MLA), x = 1 subtracts
     * from the accumulator (MLS). */
    if ((h1 & 0xFFF0) == 0xFB00 && (h2 & 0x00E0) == 0x0000) {
        uint32_t ra  = (h2 >> 12) & 0xF;
        uint32_t sub = h2 & 0x0010;
        o->rd = (h2 >> 8) & 0xF;
        o->rn = h1 & 0xF;
        o->rm = h2 & 0xF;
        if (ra == 0xF && !sub) {
            o->op = OP_MUL;
            set(o, A_ALU, "mul.w");
        } else {
            o->ra = (int8_t)ra;
            o->op = sub ? OP_MLS : OP_MLA;
            set(o, A_ALU, sub ? "mls" : "mla");
        }
        return;
    }

    /* Long multiply: 1111 1011 1xx0 Rn | RdLo RdHi 0000 Rm, where xx selects
     * SMULL (00), UMULL (01), SMLAL (10), UMLAL (11). The same first-halfword
     * space with other second-halfword op2 values holds the halfword and
     * dual forms (SMLALxy, SMLALD, UMAAL), which are not these and stay
     * classified, not translated. SDIV/UDIV (1011 1001 / 1011 1011, op2
     * 1111) are not Cortex-A9 instructions and never appear. */
    if ((h1 & 0xFF90) == 0xFB80 && (h2 & 0x00F0) == 0x0000) {
        static const arm_op lm[4] = { OP_SMULL, OP_UMULL, OP_SMLAL, OP_UMLAL };
        static const char *lmn[4] = { "smull", "umull", "smlal", "umlal" };
        uint32_t which = (h1 >> 5) & 3;
        o->op   = lm[which];
        o->rd   = (h2 >> 12) & 0xF;          /* RdLo */
        o->rdhi = (h2 >> 8) & 0xF;
        o->rn   = h1 & 0xF;
        o->rm   = h2 & 0xF;
        set(o, A_ALU, lmn[which]);
        return;
    }

    set(o, A_UNKNOWN, "?");
}

/* --- ARM -------------------------------------------------------------------- */

/* --- ARM ------------------------------------------------------------------------
 *
 * The other instruction set. Compilers for this console emit Thumb, and the
 * ARM a module holds is of two kinds: the import stubs, which are never
 * translated, and routines somebody wrote by hand, which are Advanced SIMD
 * from end to end with a few dozen integer instructions around it to fetch
 * pointers, count a loop and return. So the integer side here is what such
 * code uses and no more, decoded into the same fields the Thumb decoder
 * fills so that the emitter needs to know nothing new; what it does not
 * read it leaves as OP_NONE, which is a trap by name. The SIMD and VFP side
 * costs nothing new at all: an ARM encoding of either is the Thumb one with
 * its leading bits moved, and is handed to the decoders that already read
 * those.
 *
 * Every ARM instruction carries a condition, where Thumb needs an IT block
 * for one. `cond` is filled as an IT block's would be, and the emitter's
 * guard does the rest.
 */

static uint32_t ror32(uint32_t v, unsigned n) {
    n &= 31u;
    return n ? (v >> n) | (v << (32u - n)) : v;
}

/* An Advanced SIMD or VFP instruction, through its Thumb encoding:
 *   1111 001U ....  data processing   is  111U 1111 ....
 *   1111 0100 ....  load and store    is  1111 1001 ....
 *   cond 110. ....  and
 *   cond 1110 ....  the coprocessor   is  1110 110. .... and 1110 1110 ....
 * with the condition, where there is one, kept by the caller. */
static int a32_simd(uint32_t w, arm_insn *o) {
    uint16_t h1, h2 = (uint16_t)w;
    if ((w >> 28) == 0xF) {
        if ((w & 0xFE000000u) == 0xF2000000u)      h1 = (uint16_t)(0xEF00u | ((w >> 12) & 0x1000u) | ((w >> 16) & 0xFFu));
        else if ((w & 0xFF100000u) == 0xF4000000u) h1 = (uint16_t)(0xF900u | ((w >> 16) & 0xFFu));
        else return 0;
    } else if ((w & 0x0E000000u) == 0x0C000000u || (w & 0x0F000000u) == 0x0E000000u) {
        h1 = (uint16_t)(0xE000u | ((w >> 16) & 0x0FFFu));
    } else {
        return 0;
    }

    if (decode_neon(h1, h2, o)) {
        /* claimed */
    } else if (h1 == 0xEE1D && (h2 & 0x0FFF) == 0x0F70) {
        o->op = OP_MRC_TLS;
        o->rt = (h2 >> 12) & 0xF;
        set(o, A_SYS, "mrc tpidruro");
    } else if ((h1 & 0xEC00) == 0xEC00 && decode_vfp(h1, h2, o)) {
        /* claimed */
    } else {
        set(o, A_SIMD, "simd/vfp");
    }
    o->raw = w;
    return 1;
}

static void decode_a32(uint32_t w, arm_insn *o) {
    static const arm_op shifts[4] = { OP_LSL, OP_LSR, OP_ASR, OP_ROR };
    uint32_t cond = w >> 28;
    o->raw = w;

    /* The unconditional encoding space (cond == 1111) holds BLX immediate,
     * Advanced SIMD, and the memory hints. It is not "condition never":
     * reading it as a normal conditional instruction misdecodes all of it. */
    if (cond == 0xF) {
        if ((w & 0xFE000000) == 0xFA000000) {
            set(o, A_CALL, "blx imm");
            o->switches_mode = 1;
            o->has_target = 1;
            o->target = o->addr + 8 + sign_extend(w & 0x00FFFFFF, 24) * 4
                      + (((w >> 24) & 1) << 1);
            return;
        }
        /* PLD/PLDW/PLI (immediate and register): cache hints, nothing to
         * translate. 1111 010x x101 Rn | 1111 ... and 1111 011x x101 ... */
        if ((w & 0x0F70F000) == 0x0550F000 || (w & 0x0F70F000) == 0x0750F000 ||
            (w & 0x0F70F000) == 0x0450F000 || (w & 0x0F70F000) == 0x0650F000) {
            o->op = OP_NOP;
            set(o, A_NOP, "pld/pli");
            return;
        }
        if (a32_simd(w, o)) return;
        if ((w & 0x0F000000) == 0x0D000000 || (w & 0x0E000000) == 0x0C000000
            || (w & 0x0F000000) == 0x0E000000) {
            set(o, A_SIMD, "simd/vfp");
            return;
        }
        set(o, A_SYS, "sys");
        return;
    }

    o->cond = (uint8_t)cond;
    if (cond != ARM_COND_AL) o->conditional = 1;

    if (a32_simd(w, o)) return;

    /* BX / BLX register, which live inside the data-processing space and would
     * otherwise decode as a nonsensical MSR. */
    if ((w & 0x0FFFFFF0) == 0x012FFF10) {
        o->op = OP_BX;
        o->rm = w & 0xF;
        if (o->rm == 14) set(o, A_RETURN, "bx lr");
        else           { set(o, A_INDIRECT, "bx"); o->switches_mode = 1; }
        return;
    }
    if ((w & 0x0FFFFFF0) == 0x012FFF30) {
        o->op = OP_BLX;
        o->rm = w & 0xF;
        set(o, A_CALL, "blx reg");
        o->switches_mode = 1;
        return;
    }

    /* NOP: 0000 0011 0010 0000 1111 0000 0000 0000 */
    if ((w & 0x0FFFFFFF) == 0x0320F000) { o->op = OP_NOP; set(o, A_NOP, "nop"); return; }

    /* CLZ: 0001 0110 1111 Rd 1111 0001 Rm */
    if ((w & 0x0FFF0FF0) == 0x016F0F10) {
        o->op = OP_CLZ;
        o->rd = (w >> 12) & 0xF;
        o->rn = o->rm = w & 0xF;
        set(o, A_ALU, "clz");
        return;
    }

    /* MOVW and MOVT: 0011 0x00 imm4 Rd imm12 */
    if ((w & 0x0FB00000) == 0x03000000) {
        o->op = (w & 0x00400000) ? OP_MOVT : OP_MOV;
        o->rd = (w >> 12) & 0xF;
        o->imm = ((w >> 4) & 0xF000u) | (w & 0xFFFu);
        o->has_imm = 1;
        set(o, A_ALU, (w & 0x00400000) ? "movt" : "movw");
        return;
    }

    /* The multiplies: 0000 kkkS hhhh llll ssss 1001 mmmm. */
    if ((w & 0x0F0000F0) == 0x00000090) {
        static const arm_op   mul[8]  = { OP_MUL, OP_MLA, OP_NONE, OP_MLS, OP_UMULL, OP_UMLAL, OP_SMULL, OP_SMLAL };
        static const char    *muln[8] = { "mul", "mla", "umaal", "mls", "umull", "umlal", "smull", "smlal" };
        uint32_t kind = (w >> 21) & 7;
        int8_t high = (int8_t)((w >> 16) & 0xF), low = (int8_t)((w >> 12) & 0xF);
        o->op = mul[kind];
        o->rn = w & 0xF;
        o->rm = (w >> 8) & 0xF;
        o->sets_flags = (w >> 20) & 1;
        if (kind >= 4)      { o->rdhi = high; o->rd = low; }
        else                { o->rd = high; if (kind) o->ra = low; }
        if (o->sets_flags && kind != 0) o->op = OP_NONE;        /* flags from a wide or accumulated product */
        set(o, A_ALU, muln[kind]);
        return;
    }

    /* Halfwords, signed bytes and pairs: 000P UIWL Rn Rt immH 1SH1 immL (or Rm). */
    if ((w & 0x0E000090) == 0x00000090 && (w & 0x00000060)) {
        int P = (w >> 24) & 1, U = (w >> 23) & 1, I = (w >> 22) & 1, W = (w >> 21) & 1, L = (w >> 20) & 1;
        uint32_t sh = (w >> 5) & 3;
        int load;
        o->rn = (w >> 16) & 0xF;
        o->rt = (w >> 12) & 0xF;
        if (sh == 1)      { o->op = L ? OP_LDRH : OP_STRH;   load = L; }
        else if (sh == 2) { o->op = L ? OP_LDRSB : OP_LDRD;  load = 1; }
        else              { o->op = L ? OP_LDRSH : OP_STRD;  load = L; }
        if (o->op == OP_LDRD || o->op == OP_STRD) {
            o->rt2 = (int8_t)(o->rt + 1);
            if (o->rt & 1) o->op = OP_NONE;                     /* the pair starts on an even register */
        }
        o->mem_add = (uint8_t)U;
        o->mem_post = (uint8_t)!P;
        o->writeback = (uint8_t)(W || !P);
        if (I) {
            o->imm = ((w >> 4) & 0xF0u) | (w & 0xFu);
            o->has_imm = 1;
        } else {
            o->rm = w & 0xF;
            if (!U || o->writeback) o->op = OP_NONE;            /* an index subtracted, or kept: not read yet */
        }
        set(o, load ? A_LOAD : A_STORE, "ldr/str h");
        return;
    }

    /* Bit fields: 0111 1x1 width-1 Rd lsb 101 Rn extracts, 0111 110 msb Rd lsb 001 Rn inserts. */
    if ((w & 0x0FA00070) == 0x07A00050) {
        uint32_t width = ((w >> 16) & 0x1F) + 1, lsb = (w >> 7) & 0x1F;
        int is_unsigned = (w >> 22) & 1;
        o->rd = (w >> 12) & 0xF;
        o->rn = w & 0xF;
        o->bf_lsb = (uint8_t)lsb;
        if (lsb + width <= 32) {
            o->op = is_unsigned ? OP_UBFX : OP_SBFX;
            o->bf_width = (uint8_t)width;
        }
        set(o, A_ALU, is_unsigned ? "ubfx" : "sbfx");
        return;
    }
    if ((w & 0x0FE00070) == 0x07C00010) {
        uint32_t msb = (w >> 16) & 0x1F, lsb = (w >> 7) & 0x1F;
        int clear = (w & 0xF) == 15;
        o->rd = (w >> 12) & 0xF;
        o->rn = clear ? ARM_NO_REG : (int8_t)(w & 0xF);
        o->bf_lsb = (uint8_t)lsb;
        if (msb >= lsb) {
            o->op = clear ? OP_BFC : OP_BFI;
            o->bf_width = (uint8_t)(msb - lsb + 1);
        }
        set(o, A_ALU, clear ? "bfc" : "bfi");
        return;
    }

    switch ((w >> 25) & 7) {
        case 0: case 1: {                           /* data processing        */
            static const arm_op dp[16] = {
                OP_AND, OP_EOR, OP_SUB, OP_RSB, OP_ADD, OP_ADC, OP_SBC, OP_NONE,
                OP_TST, OP_NONE, OP_CMP, OP_CMN, OP_ORR, OP_MOV, OP_BIC, OP_MVN
            };
            int imm = (w >> 25) & 1;
            uint32_t opc = (w >> 21) & 0xF;
            int compare = opc >= 8 && opc <= 11;

            /* What is left of the 1001 space, and the compares without S,
             * which are the status-register moves. */
            if ((!imm && (w & 0x90) == 0x90) || (compare && !(w & 0x00100000))) {
                set(o, A_SYS, "sys");
                return;
            }
            o->op = dp[opc];
            o->rn = (w >> 16) & 0xF;
            o->rd = (w >> 12) & 0xF;
            o->sets_flags = (w >> 20) & 1;
            if (compare) o->rd = ARM_NO_REG;
            if (opc == 13 || opc == 15) o->rn = ARM_NO_REG;

            if (imm) {
                o->imm = ror32(w & 0xFF, 2 * ((w >> 8) & 0xF));
                o->has_imm = 1;
            } else {
                uint32_t type = (w >> 5) & 3;
                o->rm = w & 0xF;
                if (w & 0x10) {
                    /* shifted by a register: only as a shift in its own right, `mov rd, rm, lsl rs` */
                    if (opc == 13) { o->op = shifts[type]; o->rn = o->rm; o->rm = (w >> 8) & 0xF; }
                    else           o->op = OP_NONE;
                } else {
                    uint32_t amount = (w >> 7) & 0x1F;
                    if (!amount && type != SH_LSL) o->op = OP_NONE;         /* a shift of 32, or through carry */
                    o->shift_type = (uint8_t)type;
                    o->shift_amt = (uint8_t)amount;
                }
            }
            /* MOV pc, lr is a return written as an ALU operation. */
            if (o->rd == 15 && opc == 13 && !imm && o->rm == 14 && !(w & 0xFF0)) {
                o->op = OP_BX;
                o->rd = ARM_NO_REG;
                set(o, A_RETURN, "mov pc,lr");
                return;
            }
            set(o, A_ALU, "alu");
            return;
        }

        case 2: case 3: {                           /* load / store           */
            int R = (w >> 25) & 1, P = (w >> 24) & 1, U = (w >> 23) & 1, B = (w >> 22) & 1, W = (w >> 21) & 1, L = (w >> 20) & 1;
            if (R && (w & 0x00000010)) { set(o, A_UNDEF, "undef"); return; }
            o->rn = (w >> 16) & 0xF;
            o->rt = (w >> 12) & 0xF;
            o->op = L ? (B ? OP_LDRB : OP_LDR) : (B ? OP_STRB : OP_STR);
            o->mem_add = (uint8_t)U;
            o->mem_post = (uint8_t)!P;
            o->writeback = (uint8_t)(W || !P);
            if (!R) {
                o->imm = w & 0xFFF;
                o->has_imm = 1;
            } else {
                o->rm = w & 0xF;
                o->shift_amt = (uint8_t)((w >> 7) & 0x1F);
                /* the index scaled otherwise than by a left shift, subtracted, or kept in the base: not read yet */
                if (((w >> 5) & 3) != SH_LSL || !U || o->writeback) o->op = OP_NONE;
            }
            if (L && o->rt == 15) {
                /* `ldr pc, [sp], #4` is `pop {pc}`; any other load of the PC is a computed jump */
                if (!B && o->rn == 13 && !P && U && !R && o->imm == 4) {
                    o->op = OP_POP;
                    o->reglist = 0x8000;
                    o->rt = ARM_NO_REG;
                    o->has_imm = 0;
                    o->imm = 0;
                    o->mem_post = 0;
                    set(o, A_RETURN, "ldr pc");
                } else {
                    o->op = OP_NONE;
                    set(o, A_RETURN, "ldr pc");
                }
                return;
            }
            set(o, L ? A_LOAD : A_STORE, "ldr/str");
            return;
        }

        case 4: {                                   /* ldm / stm              */
            int P = (w >> 24) & 1, U = (w >> 23) & 1, S = (w >> 22) & 1, W = (w >> 21) & 1, L = (w >> 20) & 1;
            o->rn = (w >> 16) & 0xF;
            o->reglist = (uint16_t)w;
            o->writeback = (uint8_t)W;
            o->mem_add = (uint8_t)U;
            o->op = L ? OP_POP : OP_PUSH;
            if (o->rn != 13 || !W || (L && !U) || (!L && U)) o->op = L ? OP_LDM : OP_STM;
            /* The emitter writes increment-after and decrement-before, the two
             * Thumb has. The other two, and the form that reaches the user
             * mode's registers, wait for something that uses them. */
            if (S || P == U) o->op = OP_NONE;
            if (L && (w & 0x00008000)) { set(o, A_RETURN, "ldm {..,pc}"); return; }
            set(o, L ? A_LOADM : A_STOREM, "ldm/stm");
            return;
        }

        case 5:                                     /* b / bl                 */
            o->has_target = 1;
            o->target = o->addr + 8 + sign_extend(w & 0x00FFFFFF, 24) * 4;
            if (w & 0x01000000) {
                set(o, A_CALL, "bl");
            } else {
                o->op = OP_B;
                set(o, A_BRANCH, "b");
            }
            return;

        default:                                    /* the coprocessor forms went above; this is SVC */
            set(o, A_SYS, "svc");
            return;
    }
}

/* --- entry point ------------------------------------------------------------ */

/* --- jump tables -------------------------------------------------------------- */
/*
 * `tbb [pc, rm]` and `tbh [pc, rm, lsl #1]` are how a compiled switch
 * branches: a table of byte or halfword offsets follows the instruction at
 * PC, and the target is PC plus twice the entry. The instruction says nothing
 * about the table's length; the compiler always guards it with a compare of
 * the index against the last case, `cmp rm, #n; bhi default`, and that
 * compare is where the count comes from. Without it the table is left as the
 * computed transfer it looks like, and traps.
 */

static int bound_arriving(uint32_t cond, uint32_t imm, uint32_t *max) {
    switch (cond) {
    case ARM_COND_LS: case ARM_COND_LE: *max = imm; return 1;
    case ARM_COND_CC: case ARM_COND_LT: if (!imm) return 0; *max = imm - 1; return 1;
    default: return 0;
    }
}

/* The register an instruction writes, or ARM_NO_REG: the destination of
 * the data-processing forms (the compares have none), the target of a load. */
static int8_t writes_reg(const arm_insn *p, int which) {
    if (which == 0) {
        if (p->op == OP_CMP || p->op == OP_CMN || p->op == OP_TST) return ARM_NO_REG;
        if (p->cls == A_STORE || p->cls == A_STOREM || p->cls == A_BRANCH ||
            p->cls == A_CALL || p->cls == A_RETURN) return ARM_NO_REG;
        return p->rd;
    }
    if (p->cls != A_LOAD) return ARM_NO_REG;
    return which == 1 ? p->rt : p->rt2;
}

/* The immediate a compare tests against: its own, or the MOVW constant in
 * the register it compares with, when that register was last written by the
 * MOVW within the window. */
static int cmp_imm_of(const arm_insn *prev, uint32_t i, uint32_t *imm) {
    const arm_insn *c = &prev[i];
    if (c->has_imm) { *imm = c->imm; return 1; }
    if (c->rm == ARM_NO_REG || c->shift_amt) return 0;
    for (uint32_t k = i; k-- > 0;) {
        const arm_insn *p = &prev[k];
        if (writes_reg(p, 0) == c->rm || writes_reg(p, 1) == c->rm || writes_reg(p, 2) == c->rm) {
            if (p->op == OP_MOV && p->has_imm && p->rd == c->rm) { *imm = p->imm; return 1; }
            return 0;
        }
        if (p->cls == A_CALL && c->rm <= 3) return 0;
    }
    return 0;
}

/* Whether the compare at i bounds its register from above, from the branch
 * that reads its flags. A branch that leaves on "greater" lets the flow
 * continue toward the table, bounded above; the nearest compare of a dense
 * range `cmp r0, #58; bgt out; cmp r0, #39; ble out` is the lower bound,
 * and the one before it the upper. A compare with no branch in view, the
 * seeded guard included, is an upper bound; one whose flags are overwritten
 * before any branch is nothing this walk can read. */
static int cmp_upper(const arm_insn *prev, uint32_t nprev, uint32_t i, uint32_t imm, uint32_t *max) {
    for (uint32_t j = i + 1; j < nprev; j++) {
        const arm_insn *q = &prev[j];
        if (q->cls == A_BRANCH && q->conditional) {
            switch (q->cond) {
            case ARM_COND_HI: case ARM_COND_GT: *max = imm; return 1;
            case ARM_COND_CS: case ARM_COND_GE: if (!imm) return 0; *max = imm - 1; return 1;
            default: return 0;
            }
        }
        if (q->sets_flags) return 0;
    }
    *max = imm;
    return 1;
}

/* The compare a walk starts with when it begins at a guard's target: made
 * by arm_table_seed, not decoded, and so of no width. What it says holds
 * however far on the table is and whatever has set the flags since. */
static int is_seed(const arm_insn *p) { return p->op == OP_CMP && p->width == 0; }

/* The index is some register plus an offset, and the compiler moves it
 * between registers on the way: `adds r0, r7, #1; tbh [pc, r0]` is guarded
 * by a compare of r7, `subs r0, #40` by one of r0's earlier value, and
 * `adds r5, r1, #0` lets r1 be compared after the move. The walk back keeps
 * the pairs (register, offset) the index equals, and on learning a new one
 * looks forward again for a compare of it. */
typedef struct { int8_t reg; int32_t off; } tb_pair;

static int pair_find(const tb_pair *al, uint32_t nal, int8_t reg, uint32_t *at) {
    for (uint32_t i = 0; i < nal; i++) if (al[i].reg == reg) { *at = i; return 1; }
    return 0;
}

/* A compare at i of a register the index equals: the count, or 0. */
static uint32_t count_from(const arm_insn *prev, uint32_t nprev, uint32_t i, int32_t off) {
    uint32_t imm, max;
    if (!cmp_imm_of(prev, i, &imm) || !cmp_upper(prev, nprev, i, imm, &max)) return 0;
    int64_t n = (int64_t)max + off + 1;
    return n > 0 && n <= 0x10000 ? (uint32_t)n : 0;
}

uint32_t arm_table_count(const arm_insn *prev, uint32_t nprev, const arm_insn *tb) {
    if (tb->rn != 15 || tb->rm == ARM_NO_REG) return 0;
    tb_pair al[4];
    uint32_t nal = 1;
    al[0].reg = tb->rm; al[0].off = 0;
    for (uint32_t i = nprev; i-- > 0;) {
        const arm_insn *p = &prev[i];
        uint32_t at;
        if (p->op == OP_CMP) {
            int8_t creg = p->rn != ARM_NO_REG ? p->rn : p->rd;
            if (pair_find(al, nal, creg, &at)) {
                uint32_t n = count_from(prev, nprev, i, al[at].off);
                if (!n && is_seed(p)) {
                    int64_t m = (int64_t)p->imm + al[at].off + 1;
                    n = m > 0 && m <= 0x10000 ? (uint32_t)m : 0;
                }
                if (n) return n;
            }
            continue;
        }
        /* A call hands the argument registers back holding something else. */
        if (p->cls == A_CALL) {
            for (uint32_t k = 0; k < nal;) {
                if (al[k].reg <= 3) al[k] = al[--nal]; else k++;
            }
            if (!nal) return 0;
            continue;
        }
        for (int which = 0; which < 3; which++) {
            int8_t w = writes_reg(p, which);
            if (w == ARM_NO_REG || !pair_find(al, nal, w, &at)) continue;
            int8_t  src = ARM_NO_REG;
            int32_t off = al[at].off;
            if ((p->op == OP_ADD || p->op == OP_SUB) && p->has_imm && which == 0) {
                src = p->rn != ARM_NO_REG ? p->rn : p->rd;
                off += p->op == OP_ADD ? (int32_t)p->imm : -(int32_t)p->imm;
            } else if (p->op == OP_MOV && !p->has_imm && p->rm != ARM_NO_REG && !p->shift_amt && which == 0) {
                src = p->rm;
            }
            if (src == ARM_NO_REG) {
                al[at] = al[--nal];
                if (!nal) return 0;
                continue;
            }
            al[at].reg = src; al[at].off = off;
            if (src == w) continue;
            /* The index came from another register here: a compare of that
             * register between this instruction and the table, with no
             * write to it in between, is the guard. */
            for (uint32_t j = i + 1; j < nprev; j++) {
                const arm_insn *q = &prev[j];
                if (q->op == OP_CMP && (q->rn == src || q->rd == src)) {
                    uint32_t n = count_from(prev, nprev, j, off);
                    if (n) return n;
                    continue;
                }
                if (writes_reg(q, 0) == src || writes_reg(q, 1) == src || writes_reg(q, 2) == src) break;
                if (q->cls == A_CALL && src <= 3) break;
            }
        }
    }
    return 0;
}

/* What bounds `reg` from above at prev[i], from before it: an earlier
 * compare whose branch left when the register was greater, so that the flow
 * which went on has it no greater; or the guard this walk itself began
 * past. The register must not have been written since. */
static int bound_before(const arm_insn *prev, uint32_t i, int8_t reg, uint32_t *max) {
    for (uint32_t k = i; k-- > 0;) {
        const arm_insn *p = &prev[k];
        if (p->op == OP_CMP) {
            if ((p->rn != ARM_NO_REG ? p->rn : p->rd) != reg) continue;
            if (is_seed(p)) { *max = p->imm; return 1; }
            if (!p->has_imm) continue;
            for (uint32_t j = k + 1; j < i; j++) {
                const arm_insn *q = &prev[j];
                if (q->cls == A_BRANCH && q->conditional) {
                    if (q->cond == ARM_COND_HI || q->cond == ARM_COND_GT) { *max = p->imm; return 1; }
                    if ((q->cond == ARM_COND_CS || q->cond == ARM_COND_GE) && p->imm) { *max = p->imm - 1; return 1; }
                    break;
                }
                if (q->sets_flags) break;
            }
            continue;
        }
        if (writes_reg(p, 0) == reg || writes_reg(p, 1) == reg || writes_reg(p, 2) == reg) return 0;
        if (p->cls == A_CALL && reg <= 3) return 0;
        if (p->cls == A_BRANCH && !p->conditional) return 0;
    }
    return 0;
}

int arm_table_guard(const arm_insn *prev, uint32_t nprev, const arm_insn *br,
                    int8_t *reg, uint32_t *max) {
    if (br->cls != A_BRANCH || !br->conditional || !br->has_target) return 0;
    uint32_t seen = 0;
    for (uint32_t i = nprev; i-- > 0 && seen < 6; seen++) {
        const arm_insn *p = &prev[i];
        if (p->op == OP_CMP && p->has_imm && p->rn != ARM_NO_REG) {
            /* Nothing between the compare and the branch may have moved
             * the register the target will index with. */
            for (uint32_t j = i + 1; j < nprev; j++)
                if (prev[j].rd == p->rn || (prev[j].cls == A_LOAD && prev[j].rt == p->rn)) return 0;
            *reg = p->rn;
            if (bound_arriving(br->cond, p->imm, max)) return 1;
            /* A branch taken when the register is greater says nothing of
             * how great. In a tree of compares over a dense range the top
             * was settled earlier: `cmp r0, #103; bgt elsewhere; cmp r0,
             * #62; bgt here` arrives here with r0 no more than 103. */
            if (br->cond == ARM_COND_GT || br->cond == ARM_COND_HI || br->cond == ARM_COND_GE || br->cond == ARM_COND_CS)
                return bound_before(prev, i, p->rn, max);
            return 0;
        }
        if (p->sets_flags || p->cls == A_BRANCH || p->cls == A_CALL) return 0;
    }
    return 0;
}

int arm_table_guard_jump(const arm_insn *prev, uint32_t nprev, const arm_insn *br,
                         int8_t *reg, uint32_t *max) {
    if (br->cls != A_BRANCH || br->conditional || !br->has_target) return 0;
    uint32_t seen = 0;
    for (uint32_t i = nprev; i-- > 0 && seen < 8; seen++) {
        const arm_insn *p = &prev[i];
        if (p->op == OP_CMP) {
            /* the register last compared before the branch: the one a state
             * machine is about to index with, if it is going to a table */
            int8_t r = p->rn != ARM_NO_REG ? p->rn : p->rd;
            if (r == ARM_NO_REG) return 0;
            for (uint32_t j = i + 1; j < nprev; j++)
                if (writes_reg(&prev[j], 0) == r || writes_reg(&prev[j], 1) == r || writes_reg(&prev[j], 2) == r) return 0;
            *reg = r;
            return bound_before(prev, nprev, r, max);
        }
        if (p->cls == A_CALL) return 0;
    }
    return 0;
}

void arm_table_seed(arm_insn *out, int8_t reg, uint32_t max) {
    memset(out, 0, sizeof(*out));
    out->op = OP_CMP;
    out->cls = A_ALU;
    out->mnemonic = "cmp (guard)";
    out->rd = out->rn = reg;
    out->rm = out->rt = out->rt2 = ARM_NO_REG;
    out->vd = out->vn = out->vm = ARM_NO_REG;
    out->cond = ARM_COND_AL;
    out->has_imm = 1;
    out->imm = max;
    out->sets_flags = 1;
}

uint32_t arm_table_targets(const uint8_t *code, uint32_t code_addr, uint32_t code_len,
                           const arm_insn *tb, uint32_t count, uint32_t *out, uint32_t max) {
    if (tb->rn != 15 || tb->addr + 4 < code_addr) return 0;
    uint32_t pc = tb->addr + 4;
    uint32_t n = 0;
    int half = tb->op == OP_TBH;
    for (uint32_t i = 0; i < count && n < max; i++) {
        uint32_t off = pc - code_addr + (half ? i * 2 : i);
        if (off + (half ? 2u : 1u) > code_len) break;
        uint32_t e = half ? rd16le(code + off) : code[off];
        out[n++] = pc + e * 2;
    }
    return n;
}

int arm_decode(const uint8_t *code, uint32_t code_addr, uint32_t code_len,
               uint32_t addr, arm_mode mode, arm_insn *out) {
    memset(out, 0, sizeof(*out));
    out->addr = addr;
    out->mode = mode;
    out->mnemonic = "?";
    /* Registers default to "absent" rather than r0, so an emitter that reads a
     * field the decoder never filled produces an obvious error instead of a
     * silent reference to the wrong register. */
    out->rd = out->rn = out->rm = out->rt = out->rt2 = ARM_NO_REG;
    out->vd = out->vn = out->vm = ARM_NO_REG;
    out->cond = ARM_COND_AL;

    if (addr < code_addr) return 0;
    uint32_t off = addr - code_addr;

    if (mode == ARM_A32) {
        if (off + 4 > code_len) return 0;
        out->width = 4;
        decode_a32(rd32le(code + off), out);
        return 4;
    }

    if (off + 2 > code_len) return 0;
    uint16_t h1 = rd16le(code + off);

    /* Thumb width, from the rule rather than a table: the first halfword
     * introduces a 32-bit instruction when its top five bits are 11101, 11110
     * or 11111. Everything else is a complete 16-bit instruction. */
    int is32 = (h1 & 0xF800) == 0xE800
            || (h1 & 0xF800) == 0xF000
            || (h1 & 0xF800) == 0xF800;

    if (!is32) {
        out->width = 2;
        decode_t16(h1, out);

        /* `bx pc` that begins the linker's PIC long-branch veneer:
         *
         *     bx pc
         *     nop
         *     ldr ip, [pc]            (ARM)  ip = the word below
         *     add pc, ip, pc          (ARM)  pc = ip + (this + 8)
         *     .word offset
         *
         * vita-elf-create links with position-independent veneers, so every
         * Thumb tail call to an ARM symbol — which is every call from newlib
         * into a firmware stub — goes through one. Resolving the destination
         * here makes the veneer a plain branch to the stub; walking its ARM
         * body instead would translate twelve bytes of loader plumbing and
         * fall off the end into the next veneer. */
        if (out->op == OP_BX && out->rm == 15 && (addr & 3) == 0 && off + 16 <= code_len) {
            uint32_t w1 = rd32le(code + off + 4), w2 = rd32le(code + off + 8);
            if (w1 == 0xE59FC000u && w2 == 0xE08CF00Fu)
                out->target = addr + 16 + rd32le(code + off + 12);
        }
        return 2;
    }

    if (off + 4 > code_len) return 0;
    out->width = 4;
    decode_t32(h1, rd16le(code + off + 2), out);
    return 4;
}
