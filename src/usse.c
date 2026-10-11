/* usse.c — the console's shader bytecode, decoded and run. See usse.h. */

#include "usse.h"
#include "usse_private.h"
#include "usse_fields.h"
#include "half_rows.h"
#include "vitarecomp/half.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define F(format, name) usse_field(word, U_##format##_##name)

/* --- operands -------------------------------------------------------------------- */

static const uint8_t DEST_BANK[2][4] = {
    { USSE_TEMP, USSE_OUT, USSE_PA, USSE_INDEXED1 },
    { USSE_SA, USSE_SPECIAL, USSE_INDEX, USSE_INDEXED2 },
};
static const uint8_t SRC0_BANK[2][2] = { { USSE_TEMP, USSE_PA }, { USSE_OUT, USSE_SA } };
static const uint8_t SRC12_BANK[2][4] = {
    { USSE_TEMP, USSE_OUT, USSE_PA, USSE_SA },
    { USSE_INDEXED1, USSE_SPECIAL, USSE_IMMEDIATE, USSE_INDEXED2 },
};

/* A register field becomes an operand. Fields of the vector formats count
 * 64-bit units and are doubled into words; the top of the temporaries'
 * range is the internal registers; a "special" register is a constant, or a
 * global when its bit 6 is set; and in a secondary program everything with
 * storage is a secondary attribute. */
static usse_operand operand(uint32_t n, uint8_t bank, int doubled, int reg_bits, int secondary, uint8_t role) {
    usse_operand op;
    memset(&op, 0, sizeof(op));
    op.bank = bank;
    op.type = USSE_F32;
    op.role = role;
    op.sw[0] = USSE_X; op.sw[1] = USSE_Y; op.sw[2] = USSE_Z; op.sw[3] = USSE_W;
    if (doubled && bank != USSE_SPECIAL && bank != USSE_IMMEDIATE) n = (n << 1) & 0xFF;
    uint32_t limit = (1u << reg_bits) - (doubled ? 8u : 4u);
    if (bank == USSE_TEMP && n >= limit) {
        n -= limit;
        if (doubled) n >>= 1;
        op.bank = USSE_INTERNAL;
    }
    if (op.bank == USSE_SPECIAL) {
        if (n & 0x40) { n &= ~0x40u; op.bank = USSE_GLOBAL; }
        else op.bank = USSE_CONST;
    }
    if (secondary && op.bank != USSE_INTERNAL && op.bank != USSE_CONST && op.bank != USSE_IMMEDIATE)
        op.bank = USSE_SA;
    op.num = (uint16_t)n;
    return op;
}

static usse_operand dest_operand(uint32_t n, uint32_t sel, uint32_t ext, int doubled, int bits, int secondary) {
    return operand(n, DEST_BANK[ext & 1][sel & 3], doubled, bits, secondary, 3);
}
static usse_operand src0_operand(uint32_t n, uint32_t sel, uint32_t ext, int doubled, int bits, int secondary) {
    return operand(n, SRC0_BANK[ext & 1][sel & 1], doubled, bits, secondary, 0);
}
static usse_operand src12_operand(uint32_t n, uint32_t sel, uint32_t ext, int doubled, int bits, int secondary, uint8_t role) {
    return operand(n, SRC12_BANK[ext & 1][sel & 3], doubled, bits, secondary, role);
}

/* One of the fixed slots an instruction has for an internal register. */
static usse_operand gpi_operand(uint32_t n, uint8_t role) {
    usse_operand op;
    memset(&op, 0, sizeof(op));
    op.bank = USSE_INTERNAL;
    op.type = USSE_F32;
    op.flags = USSE_GPI;
    op.role = role;
    op.num = (uint16_t)n;
    op.sw[0] = op.sw[1] = op.sw[2] = op.sw[3] = USSE_UNDEFINED;
    return op;
}

static void modifier(usse_operand *op, uint32_t mod) {
    if (mod & 1) op->flags |= USSE_NEG;
    if (mod & 2) op->flags |= USSE_ABS;
}

/* The two multiply-add formats that write 64 bits at most outside the
 * internal registers: two floats, or four halves chosen a pair at a time. */
static uint8_t wide_mask(uint8_t bank, uint32_t m, int f16) {
    int unified = bank == USSE_TEMP || bank == USSE_PA || bank == USSE_SA || bank == USSE_OUT;
    if (bank == USSE_INTERNAL || (f16 && !unified)) return (uint8_t)m;
    if (f16) return (uint8_t)((m & 1 ? 3 : 0) | (m & 4 ? 12 : 0));
    return (uint8_t)(m & 3);
}

/* --- swizzles -------------------------------------------------------------------- */

static void swizzle(usse_operand *op, const char *text) {
    for (int i = 0; i < 4; i++) {
        char c = text[i];
        if (!c) { for (; i < 4; i++) op->sw[i] = USSE_X; break; }       /* three channels widen with x */
        op->sw[i] = c == 'x' ? USSE_X : c == 'y' ? USSE_Y : c == 'z' ? USSE_Z : c == 'w' ? USSE_W
                  : c == '0' ? USSE_ZERO : c == '1' ? USSE_ONE : c == '2' ? USSE_TWO : c == 'H' ? USSE_HALF
                  : USSE_UNDEFINED;
    }
}

static const char *const SW4_STD[16] = { "xxxx", "yyyy", "zzzz", "wwww", "xyzw", "yzww", "xyzz", "xxyz",
                                         "xyxy", "xywz", "zxyw", "zwzw", "yzxz", "xxyy", "xzww", "xyz1" };
static const char *const SW4_EXT[16] = { "yzxw", "zwxy", "xzwy", "yyww", "wyzw", "wzwz", "xyzx", "zzww",
                                         "xwzx", "yyyx", "yyyz", "xzyw", "xxxy", "zyxw", "yyzz", "zzzy" };
static const char *const SW3_STD[16] = { "xxx", "yyy", "zzz", "www", "xyz", "yzw", "xxy", "xyx",
                                         "yyx", "yyz", "zxy", "xzy", "yzx", "zyx", "zzy", "xy1" };
static const char *const SW3_EXT[16] = { "xyy", "yxy", "xxz", "yxx", "xy0", "x10", "000", "111",
                                         "HHH", "222", "x00", "???", "???", "???", "???", "???" };
static const char *const SWS_STD[16] = { "xxxx", "yyyy", "zzzz", "wwww", "0000", "1111", "2222", "HHHH",
                                         "????", "????", "????", "????", "????", "????", "????", "????" };

/* kind: 0 scalar, 1 three components, 2 four */
static void swizzle34(usse_operand *op, uint32_t code, uint32_t ext, int kind) {
    if (kind == 0)      swizzle(op, ext ? "????" : SWS_STD[code & 15]);
    else if (kind == 1) swizzle(op, (ext ? SW3_EXT : SW3_STD)[code & 15]);
    else                swizzle(op, (ext ? SW4_EXT : SW4_STD)[code & 15]);
}

/* --- decoding -------------------------------------------------------------------- */

static const uint8_t EXT_VEC_PRED[8] = { USSE_PRED_NONE, USSE_PRED_P0, USSE_PRED_P1, USSE_PRED_P2,
                                         USSE_PRED_NOT_P0, USSE_PRED_NOT_P1, USSE_PRED_NONE, USSE_PRED_NONE };
static const uint8_t SHORT_PRED[4] = { USSE_PRED_NONE, USSE_PRED_P0, USSE_PRED_P1, USSE_PRED_NOT_P0 };
static const uint8_t MOVE_TYPES[10] = { USSE_S8, USSE_S16, USSE_S32, USSE_C10, USSE_F16, USSE_F32,
                                        USSE_U8, USSE_U16, USSE_U32, USSE_O8 };
static const uint8_t PACK_TYPES[8] = { USSE_U8, USSE_S8, USSE_O8, USSE_U16, USSE_S16, USSE_F16, USSE_F32, USSE_C10 };

static int is_float_type(uint8_t t) { return t == USSE_F32 || t == USSE_F16 || t == USSE_C10; }

static void set_mult(usse_insn *in, int s0, int s1, int s2, int d) {
    in->mult[0] = (uint8_t)s0; in->mult[1] = (uint8_t)s1; in->mult[2] = (uint8_t)s2; in->mult[3] = (uint8_t)d;
}

static void set_type(usse_insn *in, uint8_t type) {
    in->dest.type = in->src0.type = in->src1.type = in->src2.type = type;
}

static int decode_vmad2(uint64_t word, int sec, usse_insn *in) {
    static const char *const S0[8] = { "xxxx", "yyyy", "zzzz", "wwww", "xyzw", "yzxw", "xyww", "zwxy" };
    static const char *const S1[8] = { "xxxx", "yyyy", "zzzz", "wwww", "xyzw", "xyyz", "yyww", "wyzw" };
    static const char *const S2[8] = { "xxxx", "yyyy", "zzzz", "wwww", "xyzw", "xzww", "xxyz", "xyzz" };
    int f16 = (int)F(VMAD2, DAT_FMT);
    in->op = USSE_OP_MAD;
    in->pred = (uint8_t)F(VMAD2, PRED);                 /* two bits: none, p0, p1, p2 */
    in->dest = dest_operand(F(VMAD2, DEST_N), F(VMAD2, DEST_BANK), 0, 1, 7, sec);
    in->src0 = src0_operand(F(VMAD2, SRC0_N), F(VMAD2, SRC0_BANK), 0, 1, 7, sec);
    in->src1 = src12_operand(F(VMAD2, SRC1_N), F(VMAD2, SRC1_BANK), F(VMAD2, SRC1_BANK_EXT), 1, 7, sec, 1);
    in->src2 = src12_operand(F(VMAD2, SRC2_N), F(VMAD2, SRC2_BANK), F(VMAD2, SRC2_BANK_EXT), 1, 7, sec, 2);
    in->mask = in->src_mask = wide_mask(in->dest.bank, F(VMAD2, DEST_MASK), f16);
    set_type(in, f16 ? USSE_F16 : USSE_F32);
    swizzle(&in->src0, S0[F(VMAD2, SRC0_SWIZ_BITS01) | F(VMAD2, SRC0_SWIZ_BITS2) << 2]);
    swizzle(&in->src1, S1[F(VMAD2, SRC1_SWIZ_BITS01) | F(VMAD2, SRC1_SWIZ_BIT2) << 2]);
    swizzle(&in->src2, S2[F(VMAD2, SRC2_SWIZ)]);
    if (F(VMAD2, SRC0_ABS)) in->src0.flags |= USSE_ABS;
    modifier(&in->src1, F(VMAD2, SRC1_MOD));
    modifier(&in->src2, F(VMAD2, SRC2_MOD));
    in->a = (uint8_t)f16;
    in->b = 2;                                          /* printed with its "2" */
    return 1;
}

/* The two-operand vector group: the layout is the same for 32-bit and
 * 16-bit floats, so the V32NMAD field table serves both. */
static int decode_nmad(uint64_t word, int sec, usse_insn *in, int f16) {
    static const uint8_t OPS[8] = { USSE_OP_MUL, USSE_OP_ADD, USSE_OP_FRC, USSE_OP_DSX, USSE_OP_DSY,
                                    USSE_OP_MIN, USSE_OP_MAX, USSE_OP_DOT };
    in->op = OPS[F(V32NMAD, OP2)];
    in->pred = EXT_VEC_PRED[F(V32NMAD, PRED)];
    in->dest = dest_operand(F(V32NMAD, DEST_N), F(V32NMAD, DEST_BANK_SEL), F(V32NMAD, DEST_BANK_EXT), 1, 7, sec);
    in->src1 = src12_operand(F(V32NMAD, SRC1_N), F(V32NMAD, SRC1_BANK_SEL), F(V32NMAD, SRC1_BANK_EXT), 1, 7, sec, 1);
    in->src2 = src12_operand(F(V32NMAD, SRC2_N), F(V32NMAD, SRC2_BANK_SEL), F(V32NMAD, SRC2_BANK_EXT), 1, 7, sec, 2);
    modifier(&in->src1, F(V32NMAD, SRC1_MOD));
    if (F(V32NMAD, SRC2_MOD) == 1) in->src2.flags |= USSE_ABS;
    uint32_t enc = F(V32NMAD, SRC1_SWIZ_0_6) | F(V32NMAD, SRC1_SWIZ_7_8) << 7 | F(V32NMAD, SRC1_SWIZ_9) << 9
                 | F(V32NMAD, SRC1_SWIZ_10_11) << 10;
    for (int i = 0; i < 4; i++) in->src1.sw[i] = (uint8_t)((enc >> (3 * i)) & 7);
    swizzle34(&in->src2, F(V32NMAD, SRC2_SWIZ), 0, 2);
    in->mask = (uint8_t)F(V32NMAD, DEST_MASK);
    in->src_mask = in->op == USSE_OP_DOT ? 15 : in->mask;
    set_type(in, f16 ? USSE_F16 : USSE_F32);
    in->a = (uint8_t)f16;
    return 1;
}

static int decode_vmad(uint64_t word, int sec, usse_insn *in) {
    int kind = F(VMAD, OPCODE2) ? 2 : 1;
    in->op = USSE_OP_MAD;
    in->pred = EXT_VEC_PRED[F(VMAD, PRED)];
    in->src1 = src12_operand(F(VMAD, SRC1_N), F(VMAD, SRC1_BANK), F(VMAD, SRC1_BANK_EXT), 1, 7, sec, 1);
    in->dest = dest_operand(F(VMAD, DEST_N), F(VMAD, DEST_BANK), F(VMAD, DEST_USE_BANK_EXT), 1, 7, sec);
    in->src0 = gpi_operand(F(VMAD, GPI0_N), 0);
    in->src2 = gpi_operand(F(VMAD, GPI1_N), 2);
    if (kind == 1) in->dest.sw[3] = USSE_X;
    swizzle34(&in->src1, F(VMAD, SRC1_SWIZ), F(VMAD, SRC1_SWIZ_EXT), kind);
    swizzle34(&in->src0, F(VMAD, GPI0_SWIZ), F(VMAD, GPI0_SWIZ_EXT), kind);
    swizzle34(&in->src2, F(VMAD, GPI1_SWIZ), F(VMAD, GPI1_SWIZ_EXT), kind);
    if (F(VMAD, SRC1_ABS)) in->src1.flags |= USSE_ABS;
    if (F(VMAD, SRC1_NEG)) in->src1.flags |= USSE_NEG;
    if (F(VMAD, GPI0_ABS)) in->src0.flags |= USSE_ABS;
    if (F(VMAD, GPI0_NEG)) in->src0.flags |= USSE_NEG;
    if (F(VMAD, GPI1_ABS)) in->src2.flags |= USSE_ABS;
    if (F(VMAD, GPI1_NEG)) in->src2.flags |= USSE_NEG;
    in->mask = in->src_mask = (uint8_t)F(VMAD, WRITE_MASK);
    in->repeat = (uint8_t)F(VMAD, REPEAT_COUNT);
    in->repeat_mode = (uint8_t)F(VMAD, REPEAT_MODE);
    set_mult(in, 2, 2, 2, 4);
    return 1;
}

static int decode_vdp(uint64_t word, int sec, usse_insn *in) {
    int kind = F(VDP, OPCODE2) ? 2 : 1;
    in->op = USSE_OP_DOT_GPI;
    in->pred = EXT_VEC_PRED[F(VDP, PRED)];
    in->src1 = src12_operand(F(VDP, SRC1_N), F(VDP, SRC1_BANK), F(VDP, SRC1_BANK_EXT), 1, 7, sec, 1);
    in->dest = dest_operand(F(VDP, DEST_N), F(VDP, DEST_BANK), F(VDP, DEST_USE_BANK_EXT), 1, 7, sec);
    in->src2 = gpi_operand(F(VDP, GPI0_N), 2);
    swizzle34(&in->src2, F(VDP, GPI0_SWIZ), 0, kind);
    in->src1.sw[0] = (uint8_t)F(VDP, SRC1_SWIZ_X);
    in->src1.sw[1] = (uint8_t)F(VDP, SRC1_SWIZ_Y);
    in->src1.sw[2] = (uint8_t)F(VDP, SRC1_SWIZ_Z);
    in->src1.sw[3] = (uint8_t)F(VDP, SRC1_SWIZ_W);
    if (F(VDP, SRC1_NEG)) in->src1.flags |= USSE_NEG;
    if (F(VDP, SRC1_ABS)) in->src1.flags |= USSE_ABS;
    if (F(VDP, GPI0_ABS)) in->src2.flags |= USSE_ABS;
    in->mask = (uint8_t)F(VDP, WRITE_MASK);
    in->src_mask = kind == 2 ? 15 : 7;
    in->repeat = (uint8_t)F(VDP, REPEAT_COUNT);
    in->repeat_mode = (uint8_t)F(VDP, REPEAT_MODE);
    return 1;
}

static int decode_vcomp(uint64_t word, int sec, usse_insn *in) {
    static const uint8_t OPS[4] = { USSE_OP_RCP, USSE_OP_RSQ, USSE_OP_LOG, USSE_OP_EXP };
    static const uint8_t TYPES[4] = { USSE_F32, USSE_F16, USSE_C10, USSE_O8 };
    in->op = OPS[F(VCOMP, OP2)];
    in->pred = (uint8_t)F(VCOMP, PRED);
    in->dest = dest_operand(F(VCOMP, DEST_N), F(VCOMP, DEST_BANK), F(VCOMP, DEST_BANK_EXT), 1, 8, sec);
    in->src1 = src12_operand(F(VCOMP, SRC1_N), F(VCOMP, SRC1_BANK), F(VCOMP, SRC1_BANK_EXT), 1, 8, sec, 1);
    modifier(&in->src1, F(VCOMP, SRC1_MOD));
    in->dest.type = TYPES[F(VCOMP, DEST_TYPE)];
    in->src1.type = TYPES[F(VCOMP, SRC_TYPE)];
    in->mask = (uint8_t)F(VCOMP, WRITE_MASK);
    in->src_mask = (uint8_t)(1u << F(VCOMP, SRC_COMP));
    in->repeat = (uint8_t)F(VCOMP, REPEAT_COUNT);
    in->repeat_mode = 3;
    return 1;
}

static int decode_vmov(uint64_t word, int sec, usse_insn *in) {
    uint32_t kind = F(VMOV, MOVE_TYPE), dt = F(VMOV, MOVE_DATA_TYPE);
    if (kind > 2 || dt > 9) return 0;
    uint8_t type = MOVE_TYPES[dt];
    int doubled = is_float_type(type), bits = doubled ? 7 : 6;
    in->op = kind == 0 ? USSE_OP_MOV : kind == 1 ? USSE_OP_MOVC : USSE_OP_MOVCU8;
    in->pred = (uint8_t)F(VMOV, PRED);
    in->dest = dest_operand(F(VMOV, DEST_N), F(VMOV, DEST_BANK_SEL), F(VMOV, DEST_BANK_EXT), doubled, bits, sec);
    in->src1 = src12_operand(F(VMOV, SRC1_N), F(VMOV, SRC1_BANK_SEL), F(VMOV, SRC1_BANK_EXT), doubled, bits, sec, 1);
    uint32_t mask = F(VMOV, DEST_MASK);
    if (type == USSE_F16 || type == USSE_F32) mask = wide_mask(in->dest.bank, mask, type == USSE_F16);
    else if (!doubled) mask = 1;
    in->mask = in->src_mask = (uint8_t)mask;
    if (doubled) swizzle34(&in->src1, F(VMOV, SRC0_SWIZ), 0, 2);
    if (kind != 0) {
        /* bit 50 is the end-of-phase flag here as everywhere else, not a bank bit:
         * the tested operand is a temporary or a primary attribute */
        in->src0 = src0_operand(F(VMOV, SRC0_N), F(VMOV, SRC0_BANK_SEL), 0, doubled, bits, sec);
        in->src2 = src12_operand(F(VMOV, SRC2_N), F(VMOV, SRC2_BANK_SEL), F(VMOV, SRC2_BANK_EXT), doubled, bits, sec, 2);
        if (F(VMOV, SRC0_COMP_SEL)) memcpy(in->src0.sw, in->src1.sw, 4);
        memcpy(in->src2.sw, in->src1.sw, 4);
        in->a = (uint8_t)(F(VMOV, TEST_BIT_2) << 1 | F(VMOV, TEST_BIT_1));      /* ==, !=, <, <= against zero */
    }
    set_type(in, type);
    if (kind == 2) in->src0.type = USSE_U8;
    in->repeat = (uint8_t)F(VMOV, REPEAT_COUNT);
    in->repeat_mode = 3;
    if (type == USSE_F16 || type == USSE_F32) set_mult(in, 2, 2, 2, 2); else set_mult(in, 1, 1, 1, 1);
    return 1;
}

static int decode_vpck(uint64_t word, int sec, usse_insn *in) {
    uint8_t dty = PACK_TYPES[F(VPCK, DEST_FMT)], sty = PACK_TYPES[F(VPCK, SRC_FMT)];
    in->op = USSE_OP_PCK;
    in->pred = (uint8_t)F(VPCK, PRED);
    in->dest = dest_operand(F(VPCK, DEST_N), F(VPCK, DEST_BANK_SEL), F(VPCK, DEST_BANK_EXT), 0, 7, sec);
    /* A float source is a 64-bit register like any other; an integer one is
     * addressed to the word, its low bit borrowed from a component field. */
    if (is_float_type(sty))
        in->src1 = src12_operand(F(VPCK, SRC1_N), F(VPCK, SRC1_BANK_SEL), F(VPCK, SRC1_BANK_EXT), 1, 7, sec, 1);
    else
        in->src1 = src12_operand(F(VPCK, COMP0_SEL_BIT1) | F(VPCK, SRC1_N) << 1, F(VPCK, SRC1_BANK_SEL),
                                 F(VPCK, SRC1_BANK_EXT), 0, 7, sec, 1);
    in->src2 = src12_operand(F(VPCK, SRC2_N), F(VPCK, SRC2_BANK_SEL), F(VPCK, SRC2_BANK_EXT), 1, 7, sec, 2);
    uint32_t c0 = F(VPCK, COMP_SEL_0_BIT0);
    if (sty == USSE_F32) c0 |= (F(VPCK, COMP0_SEL_BIT1) & 1) << 1;
    else                 c0 |= (F(VPCK, SRC2_N) & 1) << 1;
    in->src1.sw[0] = (uint8_t)c0;
    in->src1.sw[1] = (uint8_t)F(VPCK, COMP_SEL_1);
    in->src1.sw[2] = (uint8_t)F(VPCK, COMP_SEL_2);
    in->src1.sw[3] = (uint8_t)F(VPCK, COMP_SEL_3);
    in->mask = in->src_mask = (uint8_t)F(VPCK, DEST_MASK);
    in->a = (uint8_t)F(VPCK, SCALE);
    in->dest.type = dty;
    in->src1.type = in->src2.type = sty;

    /* An F32 source of more than two components comes from two registers,
     * x and y from one and z and w from the other, unless the plain form
     * says the same thing. */
    in->b = 0;
    if (sty == USSE_F32 && in->src2.bank != USSE_IMMEDIATE) {
        int same = in->src1.num == in->src2.num && in->src1.bank == in->src2.bank;
        int contiguous = in->mask == 1 || in->mask == 3 || in->mask == 7 || in->mask == 15;
        int n = 0, identity = 1;
        for (int i = 0; i < 4; i++) n += (in->mask >> i) & 1;
        for (int i = 0; i < n; i++) if (in->src1.sw[i] != i) identity = 0;
        if (!identity || !same || !contiguous) in->b = 1;
    }
    /* Between an integer and a float format the component selectors are
     * read in the order of the components written, not by position. */
    int both_float = is_float_type(sty) && is_float_type(dty);
    int byte_scaled = in->a && (sty == USSE_U8 || dty == USSE_U8);
    if (!both_float && !byte_scaled) {
        uint8_t tmp[4];
        memcpy(tmp, in->src1.sw, 4);
        int two = sty == USSE_U8 || dty == USSE_U8, taken = 0;
        for (int i = 0; i < 4; i++) {
            if (!((in->mask >> i) & 1)) continue;
            if (two && in->mask == 15) in->src1.sw[i] = tmp[i % 2];
            else in->src1.sw[i] = tmp[taken++];
        }
    }
    in->repeat = (uint8_t)F(VPCK, REPEAT_COUNT);
    in->repeat_mode = 3;
    if (is_float_type(sty)) set_mult(in, 1, 2, 2, 1); else set_mult(in, 1, 1, 1, 1);
    return 1;
}

static int decode_vbw(uint64_t word, int sec, usse_insn *in) {
    uint32_t op1 = F(VBW, OP1), op2 = F(VBW, OP2);
    in->op = op1 == 2 ? (op2 ? USSE_OP_OR : USSE_OP_AND) : op1 == 3 ? USSE_OP_XOR
           : op1 == 4 ? (op2 ? USSE_OP_ROL : USSE_OP_SHL) : op1 == 5 ? (op2 ? USSE_OP_ASR : USSE_OP_SHR) : USSE_OP_INVALID;
    if (!in->op) return 0;
    in->pred = (uint8_t)F(VBW, PRED);
    in->src1 = src12_operand(F(VBW, SRC1_N), F(VBW, SRC1_BANK), F(VBW, SRC1_EXT), 0, 7, sec, 1);
    in->src2 = src12_operand(F(VBW, SRC2_N), F(VBW, SRC2_BANK), F(VBW, SRC2_EXT), 0, 7, sec, 2);
    in->dest = dest_operand(F(VBW, DEST_N), F(VBW, DEST_BANK), F(VBW, DEST_EXT), 0, 7, sec);
    in->a = (uint8_t)F(VBW, BITWISE_PARTIAL);          /* the 16-bit variant */
    in->b = 0;
    if (F(VBW, SRC2_EXT) && in->src2.bank == USSE_IMMEDIATE) {
        /* a 16-bit immediate, rotated left, then perhaps complemented */
        uint32_t v = F(VBW, SRC2_N) | F(VBW, SRC2_SEL) << 7 | F(VBW, SRC2_EXTH) << 14, rot = F(VBW, SRC2_ROT);
        if (rot) {
            if (in->a) { rot &= 15; v &= 0xFFFF; if (rot) v = ((v << rot) | (v >> (16 - rot))) & 0xFFFF; }
            else v = (v << rot) | (v >> (32 - rot));
        }
        if (F(VBW, SRC2_INVERT)) v = ~v;
        if (in->a) v &= 0xFFFF;
        in->imm = v;
        in->b = 1;
    } else {
        if (F(VBW, SRC2_INVERT)) in->b = 2;             /* a register operand, complemented */
        in->c = (uint8_t)F(VBW, SRC2_ROT);
    }
    set_type(in, in->a ? USSE_U16 : USSE_U32);
    in->mask = in->src_mask = 1;
    in->repeat = (uint8_t)F(VBW, REPEAT_COUNT);
    in->repeat_mode = 3;
    set_mult(in, 1, 1, 1, 1);
    return 1;
}

/* The operations a test can compute before comparing, by selector and
 * code, are named in usse_private.h. */

static const uint8_t TEST_FLOAT_OPS[16] = { 0, 0, T_ADD, T_FRC, T_RCP, T_RSQ, T_LOG, T_EXP, T_DOT, T_MIN, T_MAX,
                                        T_DSX, T_DSY, T_MUL, T_SUB, 0 };
static const uint8_t TEST_I16_OPS[16] = { 0, 0, 0, 0, 0, 0, T_ADD, T_SUB, T_MUL, T_ADD, T_SUB, T_MUL, T_ADD, T_ADD, T_SUB, T_SUB };
static const uint8_t TEST_I16_TYPES[16] = { 0, 0, 0, 0, 0, 0, USSE_S16, USSE_S16, USSE_S16, USSE_U16, USSE_U16, USSE_U16,
                                        USSE_S32, USSE_U32, USSE_S32, USSE_U32 };
static const uint8_t TEST_I8_OPS[16] = { T_ADD, T_SUB, T_ADD, T_SUB, T_MUL, T_MUL, T_MUL, T_ADD, T_SUB, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t TEST_BIT_OPS[16] = { T_AND, T_OR, T_XOR, T_SHL, T_SHR, T_ROL, 0, T_ASR, 0, 0, 0, 0, 0, 0, 0, 0 };

/* What a test computes and on what, from its selector and code. */
static uint8_t test_operation(uint32_t sel, uint32_t code, uint32_t prec, uint8_t *type) {
    if (sel == 0) { *type = prec ? USSE_F32 : USSE_F16; return TEST_FLOAT_OPS[code]; }
    if (sel == 1) { *type = TEST_I16_TYPES[code]; return TEST_I16_OPS[code]; }
    if (sel == 2) { *type = USSE_U8; return TEST_I8_OPS[code]; }
    *type = USSE_U32;
    return TEST_BIT_OPS[code];
}

static int decode_vtst(uint64_t word, int sec, usse_insn *in) {
    uint32_t sel = F(VTST, ALU_SEL), code = F(VTST, ALU_OP), chan = F(VTST, CHAN_CC);
    uint8_t type, top = test_operation(sel, code, F(VTST, PREC), &type);
    if (!top || chan >= 4) return 0;
    int doubled = sel == 0, bits = doubled ? 8 : 7;
    in->op = USSE_OP_TEST;
    in->pred = (uint8_t)F(VTST, PRED);
    in->src1 = src12_operand(F(VTST, SRC1_N), F(VTST, SRC1_BANK), F(VTST, SRC1_EXT), doubled, bits, sec, 1);
    in->src2 = src12_operand(F(VTST, SRC2_N), F(VTST, SRC2_BANK), F(VTST, SRC2_EXT), doubled, bits, sec, 2);
    if (F(VTST, SRC2_VSCOMP) && sel == 0) swizzle(&in->src2, "xxxx");
    if (F(VTST, SRC1_NEG)) in->src1.flags |= USSE_NEG;
    memset(&in->dest, 0, sizeof(in->dest));
    in->dest.bank = USSE_PREDICATE;
    in->dest.num = (uint16_t)F(VTST, PDST_N);
    in->dest.role = 3;
    in->src1.type = in->src2.type = type;
    in->mask = in->src_mask = (uint8_t)(1u << chan);
    in->a = top;
    in->b = (uint8_t)F(VTST, SIGN_TEST);
    in->c = (uint8_t)F(VTST, ZERO_TEST);
    in->d = (uint8_t)sel;
    return 1;
}

/* The test that writes its answers as numbers: one, or one per component. */
static int decode_vtstmsk(uint64_t word, int sec, usse_insn *in) {
    uint32_t sel = F(VTSTMSK, ALU_SEL), code = F(VTSTMSK, ALU_OP), kind = F(VTSTMSK, TST_MASK_TYPE);
    uint8_t type, top = test_operation(sel, code, F(VTSTMSK, PREC), &type);
    if (!top) return 0;
    int doubled = sel == 0, bits = doubled ? 8 : 7;
    in->op = USSE_OP_TESTMASK;
    in->pred = (uint8_t)F(VTSTMSK, PRED);
    in->dest = dest_operand(F(VTSTMSK, DEST_N), F(VTSTMSK, DEST_BANK), F(VTSTMSK, DEST_EXT), doubled, bits, sec);
    in->src1 = src12_operand(F(VTSTMSK, SRC1_N), F(VTSTMSK, SRC1_BANK), F(VTSTMSK, SRC1_EXT), doubled, bits, sec, 1);
    in->src2 = src12_operand(F(VTSTMSK, SRC2_N), F(VTSTMSK, SRC2_BANK), F(VTSTMSK, SRC2_EXT), doubled, bits, sec, 2);
    if (F(VTSTMSK, SRC2_VSCOMP) && sel == 0) swizzle(&in->src2, "xxxx");
    if (F(VTSTMSK, TEST_FLAG_2)) in->src1.flags |= USSE_NEG;
    in->dest.type = in->src1.type = in->src2.type = type;
    in->mask = in->src_mask = ((sel == 0 && kind == 2) || top == T_DOT) ? 15 : 1;
    in->a = top;
    in->b = (uint8_t)F(VTSTMSK, SIGN_TEST);
    in->c = (uint8_t)F(VTSTMSK, ZERO_TEST);
    in->d = (uint8_t)sel;
    in->imm = F(VTSTMSK, TEST_WBEN) && kind != 3;       /* otherwise it writes nothing */
    return 1;
}

static int decode_vldst(uint64_t word, int sec, usse_insn *in) {
    uint32_t op1 = F(VLDST, OP1), dt = F(VLDST, DATA_TYPE);
    if (op1 != 1 && op1 != 2) return 0;
    uint32_t size = dt == 1 ? 2 : dt == 2 ? 1 : 4;
    in->op = op1 == 2 ? USSE_OP_STR : USSE_OP_LDR;
    in->pred = (uint8_t)F(VLDST, PRED);
    in->src0 = src0_operand(F(VLDST, SRC0_N), F(VLDST, SRC0_BANK), F(VLDST, SRC0_BANK_EXT), 0, 7, sec);
    in->src1 = src12_operand(F(VLDST, SRC1_N), F(VLDST, SRC1_BANK), F(VLDST, SRC1_BANK_EXT), 0, 7, sec, 1);
    in->src2 = src12_operand(F(VLDST, SRC2_N), F(VLDST, SRC2_BANK), F(VLDST, SRC2_BANK_EXT), 0, 7, sec, 2);
    memset(&in->dest, 0, sizeof(in->dest));
    in->dest.role = 3;
    in->dest.type = USSE_U32;
    in->dest.bank = sec ? USSE_SA : F(VLDST, DEST_BANK_PRIMATTR) ? USSE_PA : USSE_TEMP;
    in->dest.num = (uint16_t)F(VLDST, DEST_N);
    in->dest.sw[0] = in->dest.sw[1] = in->dest.sw[2] = in->dest.sw[3] = USSE_UNDEFINED;
    if (in->src1.bank == USSE_IMMEDIATE) in->src1.num = (uint16_t)(in->src1.num * size);    /* in bytes */
    in->src0.type = in->src1.type = in->src2.type = USSE_U32;
    in->a = (uint8_t)size;
    in->b = (uint8_t)(F(VLDST, MASK_COUNT) + 1);       /* elements */
    in->c = (uint8_t)F(VLDST, MOE_EXPAND);
    in->mask = in->src_mask = 1;
    in->repeat = in->op == USSE_OP_STR ? (uint8_t)F(VLDST, MASK_COUNT) : 0;
    in->repeat_mode = 3;
    set_mult(in, 1, 1, 1, 1);
    return 1;
}

static int decode_smp(uint64_t word, int sec, usse_insn *in) {
    static const uint8_t SRC_TYPES[4] = { USSE_F32, USSE_F16, USSE_C10, USSE_C10 };
    uint32_t fconv = F(SMP, FCONV_TYPE);
    in->op = USSE_OP_SMP;
    in->pred = (uint8_t)F(SMP, PRED);
    in->src0 = src0_operand(F(SMP, SRC0_N), F(SMP, SRC0_BANK), F(SMP, SRC0_EXT), 1, 8, sec);
    in->src1 = src12_operand(F(SMP, SRC1_N), F(SMP, SRC1_BANK), F(SMP, SRC1_EXT), 1, 8, sec, 1);
    in->src2 = src12_operand(F(SMP, SRC2_N), F(SMP, SRC2_BANK), F(SMP, SRC2_EXT), 1, 8, sec, 2);
    memset(&in->dest, 0, sizeof(in->dest));
    in->dest.role = 3;
    in->dest.bank = F(SMP, DEST_USE_PA) ? (sec ? USSE_SA : USSE_PA) : USSE_TEMP;
    in->dest.num = (uint16_t)F(SMP, DEST_N);
    in->dest.sw[0] = USSE_X; in->dest.sw[1] = USSE_Y; in->dest.sw[2] = USSE_Z; in->dest.sw[3] = USSE_W;
    in->dest.type = fconv == 2 ? USSE_F16 : fconv == 3 ? USSE_F32 : USSE_U8;
    in->src0.type = SRC_TYPES[F(SMP, SRC0_TYPE)];
    in->a = (uint8_t)(F(SMP, DIM) + 1);
    in->b = (uint8_t)F(SMP, LOD_MODE);
    in->c = (uint8_t)F(SMP, SB_MODE);
    in->d = (uint8_t)fconv;
    in->src_mask = in->a == 3 ? 7 : in->a == 1 ? 1 : 3;
    in->mask = 15;
    return 1;
}

static int decode_i16mad(uint64_t word, int sec, usse_insn *in) {
    in->op = USSE_OP_IMAD16;
    in->pred = SHORT_PRED[F(I16MAD, PRED)];
    in->dest = dest_operand(F(I16MAD, DEST_N), F(I16MAD, DEST_BANK), F(I16MAD, DEST_BANK_EXT), 0, 7, sec);
    in->src0 = src0_operand(F(I16MAD, SRC0_N), F(I16MAD, SRC0_BANK), 0, 0, 7, sec);
    in->src1 = src12_operand(F(I16MAD, SRC1_N), F(I16MAD, SRC1_BANK), F(I16MAD, SRC1_BANK_EXT), 0, 7, sec, 1);
    in->src2 = src12_operand(F(I16MAD, SRC2_N), F(I16MAD, SRC2_BANK), F(I16MAD, SRC2_BANK_EXT), 0, 7, sec, 2);
    in->a = (uint8_t)F(I16MAD, SRC1_FORMAT);
    in->b = (uint8_t)F(I16MAD, SRC2_FORMAT);
    in->c = (uint8_t)((F(I16MAD, SEL1H_UPPER8) ? 1 : 0) | (F(I16MAD, SEL2H_UPPER8) ? 2 : 0));
    in->d = (uint8_t)(F(I16MAD, MODE) | F(I16MAD, ABS) << 2 | F(I16MAD, SRC2_NEG) << 3 | F(I16MAD, OR_SHIFT) << 4);
    in->mask = in->src_mask = 1;
    in->repeat = (uint8_t)F(I16MAD, REPEAT_COUNT);
    in->repeat_mode = 3;
    set_type(in, USSE_S16);
    return 1;
}

/* Two sixteen-bit halves multiplied and a third operand added, in thirty-two
 * bits: how a vertex program turns a bone's number into the address of its
 * matrix. Each of the three may be the upper half of its register; the
 * third may be a whole word. */
static int decode_i32mad(uint64_t word, int sec, usse_insn *in) {
    in->op = USSE_OP_IMAD32;
    in->pred = SHORT_PRED[F(I32MAD, PRED)];
    in->dest = dest_operand(F(I32MAD, DEST_N), F(I32MAD, DEST_BANK), F(I32MAD, DEST_BANK_EXT), 0, 7, sec);
    in->src0 = src0_operand(F(I32MAD, SRC0_N), F(I32MAD, SRC0_BANK), 0, 0, 7, sec);
    in->src1 = src12_operand(F(I32MAD, SRC1_N), F(I32MAD, SRC1_BANK), F(I32MAD, SRC1_BANK_EXT), 0, 7, sec, 1);
    in->src2 = src12_operand(F(I32MAD, SRC2_N), F(I32MAD, SRC2_BANK), F(I32MAD, SRC2_BANK_EXT), 0, 7, sec, 2);
    in->a = (uint8_t)(F(I32MAD, SRC0_HIGH) | F(I32MAD, SRC1_HIGH) << 1 | F(I32MAD, SRC2_HIGH) << 2);
    in->b = (uint8_t)F(I32MAD, SRC2_TYPE);
    in->d = (uint8_t)(F(I32MAD, IS_SIGNED) | F(I32MAD, IS_SAT) << 1);
    in->mask = in->src_mask = 1;
    in->repeat = (uint8_t)F(I32MAD, REPEAT_COUNT);
    in->repeat_mode = 3;
    set_mult(in, 1, 1, 1, 1);
    set_type(in, F(I32MAD, IS_SIGNED) ? USSE_S32 : USSE_U32);
    return 1;
}

int usse_decode(uint64_t word, int secondary, usse_insn *in) {
    memset(in, 0, sizeof(*in));
    in->raw = word;
    in->format = USSE_F_COUNT;
    set_mult(in, 2, 2, 2, 2);
    in->dest.bank = in->src0.bank = in->src1.bank = in->src2.bank = USSE_INVALID;
    for (int f = 0; f < USSE_F_COUNT; f++)
        if ((word & USSE_FORMATS[f].mask) == USSE_FORMATS[f].value) { in->format = (uint8_t)f; break; }

    switch (in->format) {
    case USSE_F_VMAD2:   return decode_vmad2(word, secondary, in);
    case USSE_F_V32NMAD: return decode_nmad(word, secondary, in, 0);
    case USSE_F_V16NMAD: return decode_nmad(word, secondary, in, 1);
    case USSE_F_VMAD:    return decode_vmad(word, secondary, in);
    case USSE_F_VDP:     return decode_vdp(word, secondary, in);
    case USSE_F_VCOMP:   return decode_vcomp(word, secondary, in);
    case USSE_F_VMOV:    return decode_vmov(word, secondary, in);
    case USSE_F_VPCK:    return decode_vpck(word, secondary, in);
    case USSE_F_VBW:     return decode_vbw(word, secondary, in);
    case USSE_F_VTST:    return decode_vtst(word, secondary, in);
    case USSE_F_VTSTMSK: return decode_vtstmsk(word, secondary, in);
    case USSE_F_VLDST:   return decode_vldst(word, secondary, in);
    case USSE_F_SMP:     return decode_smp(word, secondary, in);
    case USSE_F_I16MAD:  return decode_i16mad(word, secondary, in);
    case USSE_F_I32MAD:  return decode_i32mad(word, secondary, in);
    case USSE_F_PHAS:    in->op = USSE_OP_PHAS; return 1;
    case USSE_F_NOP:     in->op = USSE_OP_NOP; return 1;
    case USSE_F_SPEC:
        in->op = USSE_OP_SPEC;
        in->a = (uint8_t)F(SPEC, CATEGORY);
        in->b = (uint8_t)F(SPEC, SPECIAL);
        return 1;
    case USSE_F_KILL:
        in->op = USSE_OP_KILL;
        in->a = (uint8_t)F(KILL, PRED);                 /* which predicate spares the fragment */
        return 1;
    case USSE_F_BR: {
        uint32_t off = F(BR, BR_OFF);
        in->op = USSE_OP_BR;
        in->pred = (uint8_t)F(BR, PRED);
        if (F(BR, BR_TYPE) && (off & 0x80000u)) off |= 0xFFF00000u;      /* the relative form is signed */
        in->imm = off;
        in->a = (uint8_t)F(BR, SAVE_LINK);
        return 1;
    }
    case USSE_F_SMLSI:
        in->op = USSE_OP_SMLSI;
        in->a = (uint8_t)(F(SMLSI, SRC0_INC_MODE) | F(SMLSI, SRC1_INC_MODE) << 1 | F(SMLSI, SRC2_INC_MODE) << 2
                        | F(SMLSI, DEST_INC_MODE) << 3);
        in->imm = F(SMLSI, SRC0_INC) | F(SMLSI, SRC1_INC) << 8 | F(SMLSI, SRC2_INC) << 16 | F(SMLSI, DEST_INC) << 24;
        return 1;
    case USSE_F_LIMM:
        in->op = USSE_OP_LIMM;
        in->pred = (uint8_t)F(LIMM, PRED);
        in->dest = dest_operand(F(LIMM, DEST_NUM), F(LIMM, DEST_BANK), F(LIMM, DEST_BANK_EXT), 0, 7, secondary);
        in->dest.type = USSE_U32;
        in->imm = F(LIMM, IMM_VALUE_FIRST_21BITS) | F(LIMM, IMM_VALUE_BITS21TO25) << 21 | F(LIMM, IMM_VALUE_BITS26TO31) << 26;
        in->mask = 1;
        return 1;
    default:
        return 0;
    }
}

/* --- repeats --------------------------------------------------------------------- */

void usse_reset(usse_state *s) {
    for (int role = 0; role < 4; role++)
        for (int j = 0; j < 17; j++) s->inc[role][j] = j;
}

/* How far an operand has moved on the r-th execution of its instruction:
 * in words for the banks of words, in registers for the internal ones. */
static int32_t repeat_offset(const usse_state *s, const usse_insn *in, const usse_operand *op, int r) {
    int mode = in->repeat_mode;
    if ((mode == 1 || mode == 2) && op->bank == USSE_INTERNAL) return r;
    if (op->flags & USSE_GPI) return 0;
    if (mode == 2) return 4 * r;
    if (mode == 0 && op->bank != USSE_INTERNAL) return 4 * r;
    if (mode == 3) {
        int32_t step = s->inc[op->role][r];
        if (op->bank == USSE_TEMP || op->bank == USSE_PA || op->bank == USSE_OUT || op->bank == USSE_SA ||
            op->bank == USSE_PREDICATE || op->bank == USSE_INDEXED1 || op->bank == USSE_INDEXED2)
            step *= in->mult[op->role];
        return step;
    }
    return 0;
}

static void apply_smlsi(usse_state *s, const usse_insn *in) {
    static const int ROLE[4] = { 0, 1, 2, 3 };
    for (int k = 0; k < 4; k++) {
        uint32_t value = (in->imm >> (8 * k)) & 0xFF;
        if ((in->a >> k) & 1) {
            for (int j = 0; j < 4; j++) s->inc[ROLE[k]][j] = (int32_t)((value >> (2 * j)) & 3);
        } else {
            int32_t step = (int32_t)(int8_t)value;
            for (int j = 0; j < 17; j++) s->inc[ROLE[k]][j] = j * step;
        }
    }
}

/* The same for a translator of the bytecode (usse_private.h). */
int32_t usse_repeat_offset(const usse_state *s, const usse_insn *in, const usse_operand *op, int r) {
    return repeat_offset(s, in, op, r);
}

void usse_apply_smlsi(usse_state *s, const usse_insn *in) { apply_smlsi(s, in); }

/* --- text ------------------------------------------------------------------------- */

typedef struct { char *out; size_t cap, len; } text;

static void put(text *t, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (t->len < t->cap) {
        int n = vsnprintf(t->out + t->len, t->cap - t->len, fmt, ap);
        if (n > 0) t->len += (size_t)n < t->cap - t->len ? (size_t)n : t->cap - t->len - 1;
    }
    va_end(ap);
}

static void put_operand(text *t, const usse_operand *op, uint8_t mask, int32_t shift) {
    static const char CH[] = "xyzw012H?";
    char reg[48];
    uint32_t n = (uint32_t)((int32_t)op->num + shift);
    if (op->bank == USSE_INDEXED1 || op->bank == USSE_INDEXED2) {
        static const char *const TARGET[4] = { "r[", "o[", "pa[", "sa[" };
        snprintf(reg, sizeof(reg), "%sidx%d * 2 + %u]", TARGET[(n >> 5) & 3], op->bank - USSE_INDEXED1 + 1, n & 31);
    } else {
        const char *name = op->bank == USSE_PA ? "pa" : op->bank == USSE_SA ? "sa" : op->bank == USSE_TEMP ? "r"
                         : op->bank == USSE_OUT ? "o" : op->bank == USSE_INTERNAL ? "i" : op->bank == USSE_CONST ? "c"
                         : op->bank == USSE_INDEX ? "idx" : op->bank == USSE_IMMEDIATE ? "" : "INVALID";
        snprintf(reg, sizeof(reg), "%s%u", name, n);
    }
    if (op->flags & USSE_ABS) put(t, "abs(");
    if (op->flags & USSE_NEG) put(t, "-");
    put(t, "%s", reg);
    if (op->flags & USSE_ABS) put(t, ")");
    if (op->bank == USSE_IMMEDIATE || !mask) return;
    char sw[5];
    int last = -1;
    for (int i = 0; i < 4; i++) {
        sw[i] = (mask >> i) & 1 ? CH[op->sw[i] > 8 ? 8 : op->sw[i]] : '-';
        if ((mask >> i) & 1) last = i;
    }
    sw[last + 1] = '\0';
    put(t, ".%s", sw);
}

static const char *pred_text(uint8_t pred) {
    static const char *const P[8] = { "", "p0 ", "p1 ", "p2 ", "p3 ", "!p0 ", "!p1 ", "pN " };
    return P[pred & 7];
}

static const char *type_text(uint8_t type) {
    static const char *const T[10] = { "i8", "i16", "i32", "c10", "f16", "f32", "u8", "u16", "u32", "invalid" };
    return T[type > 9 ? 9 : type];
}

static const char *type_name(uint8_t type) {
    static const char *const T[10] = { "S8", "S16", "S32", "C10", "F16", "F32", "U8", "U16", "U32", "O8" };
    return T[type > 9 ? 9 : type];
}

size_t usse_disassemble(const usse_insn *in, usse_state *s, char *out, size_t cap) {
    text t = { out, cap, 0 };
    if (cap) out[0] = '\0';
    #define HEAD() put(&t, "%016llx: ", (unsigned long long)in->raw)
    #define OP(o, m, r) put_operand(&t, &in->o, (m), repeat_offset(s, in, &in->o, (r)))

    switch (in->op) {
    case USSE_OP_MAD:
        if (in->format == USSE_F_VMAD2) {
            HEAD(); put(&t, "%s%s2 ", pred_text(in->pred), in->a ? "VF16MAD" : "VMAD");
            put_operand(&t, &in->dest, in->mask, 0); put(&t, " ");
            put_operand(&t, &in->src0, in->mask, 0); put(&t, " ");
            put_operand(&t, &in->src1, in->mask, 0); put(&t, " ");
            put_operand(&t, &in->src2, in->mask, 0); put(&t, "\n");
            break;
        }
        for (int r = 0; r <= in->repeat; r++) {
            HEAD(); put(&t, "%sVMAD ", pred_text(in->pred));
            OP(dest, in->mask, r); put(&t, " "); OP(src0, in->mask, r); put(&t, " ");
            OP(src1, in->mask, r); put(&t, " "); OP(src2, in->mask, r); put(&t, "\n");
        }
        break;
    case USSE_OP_MUL: case USSE_OP_ADD: case USSE_OP_FRC: case USSE_OP_DSX: case USSE_OP_DSY:
    case USSE_OP_MIN: case USSE_OP_MAX: case USSE_OP_DOT: {
        static const char *const NAMES[8] = { "MUL", "ADD", "FRC", "DSX", "DSY", "MIN", "MAX", "DP" };
        HEAD(); put(&t, "%s%s%s ", pred_text(in->pred), in->a ? "VF16" : "V", NAMES[in->op - USSE_OP_MUL]);
        put_operand(&t, &in->dest, in->mask, 0); put(&t, " ");
        put_operand(&t, &in->src1, in->src_mask, 0); put(&t, " ");
        put_operand(&t, &in->src2, in->src_mask, 0); put(&t, "\n");
        break;
    }
    case USSE_OP_DOT_GPI: {
        uint8_t mask = in->mask;
        for (int r = 0; r <= in->repeat; r++) {
            HEAD(); put(&t, "%sVDP ", pred_text(in->pred));
            put_operand(&t, &in->dest, mask, 0); put(&t, " ");
            OP(src1, in->src_mask, r); put(&t, " "); OP(src2, in->src_mask, r); put(&t, "\n");
            mask = (uint8_t)((mask << 1) | (mask >> 3));
        }
        break;
    }
    case USSE_OP_RCP: case USSE_OP_RSQ: case USSE_OP_LOG: case USSE_OP_EXP: {
        static const char *const NAMES[4] = { "VRCP", "VRSQ", "VLOG", "VEXP" };
        for (int r = 0; r <= in->repeat; r++) {
            HEAD(); put(&t, "%s%s ", pred_text(in->pred), NAMES[in->op - USSE_OP_RCP]);
            OP(dest, in->mask, r); put(&t, " "); OP(src1, in->src_mask, r); put(&t, "\n");
        }
        break;
    }
    case USSE_OP_MOV: case USSE_OP_MOVC: case USSE_OP_MOVCU8: {
        static const char *const TESTS[4] = { "==", "!=", "<", "<=" };
        const char *name = in->op == USSE_OP_MOV ? "VMOV" : in->op == USSE_OP_MOVC ? "VMOVC" : "VMOVCU8";
        for (int r = 0; r <= in->repeat; r++) {
            HEAD(); put(&t, "%s%s.%s ", pred_text(in->pred), name, type_text(in->dest.type));
            OP(dest, in->mask, r);
            if (in->op != USSE_OP_MOV) {
                put(&t, " (");
                put_operand(&t, &in->src0, in->mask, 0);
                put(&t, " %s vec(0)) ?", TESTS[in->a & 3]);
            }
            put(&t, " "); OP(src1, in->mask, r); put(&t, " ");
            if (in->op != USSE_OP_MOV) { put(&t, ": "); OP(src2, in->mask, r); }
            put(&t, "\n");
        }
        break;
    }
    case USSE_OP_PCK:
        for (int r = 0; r <= in->repeat; r++) {
            HEAD(); put(&t, "%sVPCK%s%s ", pred_text(in->pred), type_name(in->dest.type), type_name(in->src1.type));
            OP(dest, in->mask, r); put(&t, " ");
            if (in->b) { put(&t, "("); OP(src1, in->mask, r); put(&t, " "); OP(src2, 15, r); put(&t, ")"); }
            else OP(src1, in->mask, r);
            put(&t, " [%s]\n", in->a ? "scale" : "noscale");
        }
        break;
    case USSE_OP_AND: case USSE_OP_OR: case USSE_OP_XOR: case USSE_OP_SHL: case USSE_OP_SHR: case USSE_OP_ASR: {
        static const char *const NAMES[6] = { "AND", "OR", "XOR", "SHL", "SHR", "ASR" };
        for (int r = 0; r <= in->repeat; r++) {
            HEAD(); put(&t, "%s%s%s ", pred_text(in->pred), NAMES[in->op - USSE_OP_AND], in->a ? "16" : "");
            OP(dest, 1, r); put(&t, " "); OP(src1, 1, r); put(&t, " ");
            if (in->b == 1) put(&t, "0x%X", in->imm); else OP(src2, 1, r);
            put(&t, "\n");
        }
        break;
    }
    case USSE_OP_ROL:
        break;                                          /* the reference prints nothing for it */
    case USSE_OP_TEST: case USSE_OP_TESTMASK: {
        static const char *const FLOAT32[21] = { "", "VADD", "VFRC", "VRCP", "VRSQ", "VLOG", "VEXP", "VDP", "VMIN", "VMAX",
                                                 "VDSX", "VDSY", "VMUL", "VSUB", "AND", "OR", "XOR", "SHL", "SHR", "ROL", "ASR" };
        static const char *const FLOAT16[14] = { "", "VF16ADD", "VF16FRC", "VRCP", "VRSQ", "VLOG", "VEXP", "VF16DP", "VF16MIN",
                                                 "VF16MAX", "VF16DSX", "VF16DSY", "VF16MUL", "VF16SUB" };
        static const char *const CMP[2][4] = { { "ne", "lt", "gt", "inv" }, { "equal", "le", "ge", "inv" } };
        const char *cmp = CMP[in->c == 1][in->b & 3];
        char name[24], to[24];
        uint8_t type = in->src1.type;
        if (in->d == 0) snprintf(name, sizeof(name), "%s", type == USSE_F16 ? FLOAT16[in->a] : FLOAT32[in->a]);
        else if (in->d == 3) snprintf(name, sizeof(name), "%s", FLOAT32[in->a]);
        else {
            uint32_t code = usse_field(in->raw, U_VTST_ALU_OP);
            static const char *const I16[16] = { "", "", "", "", "", "", "IADD16", "ISUB16", "IMUL16", "IADDU16", "ISUBU16",
                                                 "IMULU16", "IADD32", "IADDU32", "ISUB32", "ISUBU32" };
            static const char *const I8[16] = { "IADD8", "ISUB8", "IADDU8", "ISUBU8", "IMUL8", "FPMUL8", "IMULU8", "FPADD8",
                                                "FPSUB8", "", "", "", "", "", "", "" };
            snprintf(name, sizeof(name), "%s", in->d == 1 ? I16[code] : I8[code]);
        }
        /* the plain test names a predicate; the mask form a register, and its type twice */
        if (in->op == USSE_OP_TEST) snprintf(to, sizeof(to), "%s p%u", type_text(type), in->dest.num);
        else snprintf(to, sizeof(to), "%s.%s", type_text(type), type_text(type));
        HEAD();
        if (in->a == T_SUB) put(&t, "%sCMP%s.%s.%s ", pred_text(in->pred), in->op == USSE_OP_TEST ? "" : "MSK", cmp, to);
        else put(&t, "%s%s.%szero.%s ", pred_text(in->pred), name, cmp, to);
        if (in->op == USSE_OP_TESTMASK) { put_operand(&t, &in->dest, in->mask, 0); put(&t, " "); }
        put_operand(&t, &in->src1, in->mask, 0); put(&t, " ");
        put_operand(&t, &in->src2, in->mask, 0); put(&t, "\n");
        break;
    }
    case USSE_OP_LDR:
        HEAD(); put(&t, "%sLDR ", pred_text(in->pred));
        put_operand(&t, &in->dest, 1, 0); put(&t, " (");
        put_operand(&t, &in->src0, 1, 0); put(&t, " + ");
        put_operand(&t, &in->src1, 1, 0); put(&t, " + ");
        put_operand(&t, &in->src2, 1, 0); put(&t, ") [%u bytes]\n", (unsigned)in->a * in->b);
        break;
    case USSE_OP_STR:
        for (int r = 0; r <= in->repeat; r++) {
            usse_operand d = in->dest;
            HEAD(); put(&t, "%sSTR ", pred_text(in->pred));
            OP(src2, 1, r); put(&t, " ("); OP(src0, 1, r); put(&t, " + ");
            put_operand(&t, &in->src1, 1, repeat_offset(s, in, &d, r)); put(&t, " + 0) [4 bytes]\n");
        }
        break;
    case USSE_OP_SMP: {
        static const char *const MODES[4] = { "", ".gather4", ".info", ".gather4.uv" };
        usse_operand none;
        memset(&none, 0, sizeof(none));
        none.bank = USSE_INVALID;
        none.num = 0xFFFF;
        none.sw[0] = none.sw[1] = none.sw[2] = none.sw[3] = USSE_UNDEFINED;
        HEAD(); put(&t, "%sSMP%ud.%s.%s%s ", pred_text(in->pred), in->a, type_text(in->dest.type),
                    type_text(in->src0.type), MODES[in->c & 3]);
        put_operand(&t, &in->dest, 1, 0); put(&t, " ");
        put_operand(&t, &in->src0, in->src_mask, 0); put(&t, " ");
        put_operand(&t, &in->src1, 0, 0); put(&t, " ");
        if (in->b != 0) put_operand(&t, &none, 1, 0);
        put(&t, "\n");
        break;
    }
    case USSE_OP_IMAD16:
        for (int r = 0; r <= in->repeat; r++) {
            HEAD(); put(&t, "%sIMAD16 ", pred_text(in->pred));
            put_operand(&t, &in->dest, 1, 0); put(&t, " ");
            put_operand(&t, &in->src0, 1, 0); put(&t, " ");
            put_operand(&t, &in->src1, (in->a && (in->c & 1)) ? 2 : 1, 0); put(&t, "%s ", in->a ? "-8bits" : "");
            put_operand(&t, &in->src2, (in->b && (in->c & 2)) ? 2 : 1, 0); put(&t, "%s\n", in->b ? "-8bits" : "");
        }
        break;
    case USSE_OP_IMAD32:
        for (int r = 0; r <= in->repeat; r++) {
            HEAD(); put(&t, "%sIMAD2 ", pred_text(in->pred));
            put_operand(&t, &in->dest, 1, 0); put(&t, " ");
            put_operand(&t, &in->src0, 1, 0); put(&t, " ");
            put_operand(&t, &in->src1, 1, 0); put(&t, " ");
            put_operand(&t, &in->src2, 1, 0); put(&t, "\n");
        }
        break;
    case USSE_OP_SMLSI: {
        static const int ORDER[4] = { 3, 0, 1, 2 };     /* printed destination first */
        HEAD(); put(&t, "SMLSI ");
        for (int k = 0; k < 4; k++) {
            uint32_t value = (in->imm >> (8 * ORDER[k])) & 0xFF;
            if ((in->a >> ORDER[k]) & 1) {
                put(&t, "swizz.(");
                for (int j = 0; j < 4; j++) put(&t, "%u", (value >> (2 * j)) & 3);
                put(&t, ") ");
            } else {
                put(&t, " inc.%d ", (int)(int8_t)value);
            }
        }
        put(&t, "\n");
        apply_smlsi(s, in);
        break;
    }
    case USSE_OP_LIMM:
        HEAD(); put(&t, "%sMOV ", pred_text(in->pred));
        put_operand(&t, &in->dest, 1, 0); put(&t, " #0x%X\n", in->imm);
        break;
    case USSE_OP_PHAS: HEAD(); put(&t, "PHAS\n"); break;
    case USSE_OP_NOP:  HEAD(); put(&t, "NOP\n"); break;
    case USSE_OP_SPEC: HEAD(); put(&t, "SPEC category: %u, special: %s\n", in->a, in->b ? "true" : "false"); break;
    case USSE_OP_KILL: HEAD(); put(&t, "KILL %s\n", pred_text(SHORT_PRED[in->a & 3])); break;
    case USSE_OP_BR:   HEAD(); put(&t, "%sBR %+d\n", pred_text(in->pred), (int)(int32_t)in->imm); break;
    default:
        HEAD(); put(&t, "<format %u>\n", in->format);
        break;
    }
    #undef HEAD
    #undef OP
    return t.len;
}

/* --- the constants ------------------------------------------------------------------
 * Sixty-four registers of 64 bits each, read as two floats or as four
 * halves: zero and one in every arrangement, the powers of two up and down,
 * e, pi and their relatives, and what the series for sine need. */

static const uint32_t CONSTANTS[64][2] = {
    { 0x00000000, 0x00000000 }, { 0x00000000, 0x3F800000 }, { 0x3F800000, 0x00000000 }, { 0x3F800000, 0x3F800000 },
    { 0x40000000, 0x40800000 }, { 0x41000000, 0x41800000 }, { 0x42000000, 0x42800000 }, { 0x43000000, 0x43800000 },
    { 0x44000000, 0x44800000 }, { 0x45000000, 0x45800000 }, { 0x46000000, 0x46800000 }, { 0x47000000, 0x47800000 },
    { 0x3F000000, 0x3E800000 }, { 0x3E000000, 0x3D800000 }, { 0x3D000000, 0x3C800000 }, { 0x3C000000, 0x3B800000 },
    { 0x3B000000, 0x3A800000 }, { 0x3A000000, 0x39800000 }, { 0x39000000, 0x38800000 }, { 0x38000000, 0x37800000 },
    { 0x402DF854, 0x3EBC5AB2 }, { 0x3FB504F3, 0x3F3504F3 }, { 0x40490FDB, 0x3FC90FDB }, { 0x3F490FDB, 0x3EC90FDB },
    { 0x40C90FDB, 0x41490FDB }, { 0x41C90FDB, 0x00000000 }, { 0x37800000, 0x38000000 }, { 0x37800080, 0x38000100 },
    { 0x35D00D01, 0x37B60B61 }, { 0x39888889, 0x3B2AAAAB }, { 0x3CAAAAAB, 0x3E000000 }, { 0x3F000000, 0x3F800000 },
    { 0x00000000, 0x3C000000 }, { 0x00000000, 0x00000000 }, { 0x3C003C00, 0x3C003C00 }, { 0x44004000, 0x4C004800 },
    { 0x54005000, 0x5C005800 }, { 0x64006000, 0x6C006800 }, { 0x74007000, 0x00007800 }, { 0x34003800, 0x2C003000 },
    { 0x24002800, 0x1C002000 }, { 0x14001800, 0x0C001000 }, { 0x04000800, 0x00000000 }, { 0x35E2416F, 0x00000000 },
    { 0x39A83DA8, 0x00000000 }, { 0x3E484248, 0x36483A48 }, { 0x4A484648, 0x00004E48 }, { 0x00000000, 0x00000000 },
    { 0x00000000, 0x19550C44 }, { 0x30002555, 0x3C003800 }, { 0x00000000, 0x00000000 }, { 0x00000000, 0x00000000 },
    { 0x00000000, 0x00000000 }, { 0x00000000, 0x00000000 }, { 0x00000000, 0x00000000 }, { 0x00000000, 0x00000000 },
    { 0xFFFFFFFF, 0x00000000 }, { 0xFFFFFFFF, 0x00000000 }, { 0xFFFFFFFF, 0x00000000 }, { 0xFFFFFFFF, 0x00000000 },
    { 0x7FFF7FFF, 0x00000000 }, { 0x7FFF7FFF, 0x00000000 }, { 0x7FFF7FFF, 0x00000000 }, { 0x7FFF7FFF, 0x00000000 },
};

const uint32_t *usse_constant_words(unsigned n) { return CONSTANTS[n & 63]; }

/* --- values ------------------------------------------------------------------------
 * What a load gives and a store takes: up to four components, all floats,
 * all signed integers or all unsigned ones. */

enum { V_FLOAT = 0, V_SINT, V_UINT };

typedef struct {
    uint8_t n, kind;
    union { float f[4]; int32_t i[4]; uint32_t u[4]; } v;
} value;

static float bits_to_float(uint32_t u) { float f; memcpy(&f, &u, sizeof(f)); return f; }
static uint32_t float_to_bits(float f) { uint32_t u; memcpy(&u, &f, sizeof(u)); return u; }

static const float CHANNEL_CONSTANT[4] = { 0.0f, 1.0f, 2.0f, 0.5f };

static uint8_t kind_of(uint8_t type) {
    switch (type) {
    case USSE_S8: case USSE_S16: case USSE_S32: return V_SINT;
    case USSE_U8: case USSE_U16: case USSE_U32: return V_UINT;
    default: return V_FLOAT;
    }
}

static uint32_t *bank_words(usse_state *s, uint8_t bank) {
    switch (bank) {
    case USSE_TEMP: return s->temp;
    case USSE_PA:   return s->pa;
    case USSE_OUT:  return s->out;
    case USSE_SA:   return s->sa;
    default:        return NULL;
    }
}

/* Where an operand's words are: the bank, and the first word. An indexed
 * operand names a bank and an offset and adds an index register, which
 * counts 64-bit units. Returns NULL when the run of words would leave the
 * bank. */
static uint32_t *resolve(usse_state *s, const usse_operand *op, int32_t shift, uint8_t *type, uint32_t words) {
    uint32_t *w;
    int64_t base;
    if (op->bank == USSE_INTERNAL) {
        base = 4 * ((int64_t)op->num + shift);
        if (base < 0 || base + 4 > 16) return NULL;
        /* internal registers hold 32-bit lanes whatever the operand says */
        if (*type == USSE_F16) *type = USSE_F32;
        else if (*type == USSE_S16 || *type == USSE_S8) *type = USSE_S32;
        else if (*type == USSE_U16 || *type == USSE_U8) *type = USSE_U32;
        return s->internal + base;
    }
    if (op->bank == USSE_INDEXED1 || op->bank == USSE_INDEXED2) {
        static const uint8_t TARGET[4] = { USSE_TEMP, USSE_OUT, USSE_PA, USSE_SA };
        uint32_t n = (uint32_t)((int32_t)op->num + shift);
        w = bank_words(s, TARGET[(n >> 5) & 3]);
        base = (int64_t)s->index[op->bank - USSE_INDEXED1] * 2 + (n & 31);
    } else {
        w = bank_words(s, op->bank);
        base = (int64_t)op->num + shift;
    }
    if (!w || base < 0 || base + words > USSE_WORDS) return NULL;
    return w + base;
}

static value load_constant(const usse_operand *op, uint8_t mask) {
    value out;
    memset(&out, 0, sizeof(out));
    out.kind = kind_of(op->type);
    const uint32_t *c = CONSTANTS[op->num & 63];
    for (int i = 0; i < 4; i++) {
        if (!((mask >> i) & 1)) continue;
        uint8_t ch = op->sw[i];
        if (ch >= USSE_ZERO && ch <= USSE_HALF) {
            float k = CHANNEL_CONSTANT[ch - USSE_ZERO];
            if (out.kind == V_FLOAT) out.v.f[out.n] = k;
            else if (out.kind == V_SINT) out.v.i[out.n] = (int32_t)k;
            else out.v.u[out.n] = (uint32_t)k;
        } else if (ch > USSE_W) {
            out.v.u[out.n] = 0;
        } else if (op->type == USSE_F16 || op->type == USSE_U16 || op->type == USSE_S16) {
            /* four halves; two of them the reference's tables correct by hand */
            uint16_t h = (uint16_t)(c[ch >> 1] >> (16 * (ch & 1)));
            if (ch == USSE_X && (op->num & 63) == 12) h = 0x3800;
            if (ch == USSE_X && (op->num & 63) == 16) h = 0x3C00;
            if (op->type == USSE_F16) out.v.f[out.n] = vita_f16_to_f32(h);
            else if (op->type == USSE_U16) out.v.u[out.n] = h;
            else out.v.i[out.n] = (int32_t)vita_f16_to_f32(h);
        } else {
            uint32_t w = ch == USSE_Y ? c[1] : c[0];
            if (out.kind == V_FLOAT) out.v.f[out.n] = bits_to_float(w);
            else out.v.u[out.n] = w;
        }
        out.n++;
    }
    return out;
}

static void modifiers(value *v, uint8_t flags) {
    if (flags & USSE_ABS) {
        if (v->kind == V_UINT) return;
        for (int i = 0; i < v->n; i++) {
            if (v->kind == V_FLOAT) v->v.f[i] = fabsf(v->v.f[i]);
            else if (v->v.i[i] < 0) v->v.i[i] = (int32_t)(0u - (uint32_t)v->v.i[i]);
        }
    }
    if (flags & USSE_NEG) {
        for (int i = 0; i < v->n; i++) {
            if (v->kind == V_FLOAT) v->v.f[i] = 0.0f - v->v.f[i];
            else v->v.u[i] = 0u - v->v.u[i];
        }
    }
}

/* An operand that is a number in the instruction, or a register this
 * interpreter has nothing behind: the same for every component. */
static value load_immediate(const usse_operand *op, uint8_t mask) {
    value out;
    memset(&out, 0, sizeof(out));
    out.kind = kind_of(op->type);
    uint32_t imm = op->bank == USSE_IMMEDIATE ? op->num : 0;
    for (int i = 0; i < 4; i++) {
        if (!((mask >> i) & 1)) continue;
        if (out.kind == V_FLOAT) out.v.f[out.n] = (float)imm; else out.v.u[out.n] = imm;
        out.n++;
    }
    return out;
}

/* The components of an operand whose words are at `w` (NULL: nowhere), read
 * as `type`, before its modifiers. */
static value load_words(const usse_operand *op, uint8_t type, const uint32_t *w, uint8_t mask) {
    value out;
    memset(&out, 0, sizeof(out));
    out.kind = kind_of(type);
    for (int i = 0; i < 4; i++) {
        if (!((mask >> i) & 1)) continue;
        uint8_t ch = op->sw[i];
        if (ch >= USSE_ZERO && ch <= USSE_HALF) {
            /* a float, whatever the type: integer operands do not use these */
            if (out.kind == V_FLOAT) out.v.f[out.n] = CHANNEL_CONSTANT[ch - USSE_ZERO];
            else out.v.u[out.n] = (uint32_t)CHANNEL_CONSTANT[ch - USSE_ZERO];
            out.n++;
            continue;
        }
        if (!w || ch > USSE_W) { out.v.u[out.n++] = 0; continue; }
        switch (type) {
        case USSE_F32: out.v.f[out.n] = bits_to_float(w[ch]); break;
        case USSE_U32: out.v.u[out.n] = w[ch]; break;
        case USSE_S32: out.v.i[out.n] = (int32_t)w[ch]; break;
        case USSE_F16: out.v.f[out.n] = vita_f16_to_f32((uint16_t)(w[ch >> 1] >> (16 * (ch & 1)))); break;
        case USSE_U16: out.v.u[out.n] = (uint16_t)(w[ch >> 1] >> (16 * (ch & 1))); break;
        case USSE_S16: out.v.i[out.n] = (int16_t)(w[ch >> 1] >> (16 * (ch & 1))); break;
        case USSE_U8:  out.v.u[out.n] = (uint8_t)(w[0] >> (8 * ch)); break;
        case USSE_S8:  out.v.i[out.n] = (int8_t)(w[0] >> (8 * ch)); break;
        case USSE_C10: {
            int32_t f = (int32_t)((w[ch / 3] >> (10 * (ch % 3))) & 0x3FF);
            if (f & 0x200) f -= 0x400;
            float v = (float)f / 255.0f;
            out.v.f[out.n] = v < -2.0f ? -2.0f : v;
            break;
        }
        default: out.v.u[out.n] = 0; break;
        }
        out.n++;
    }
    return out;
}

/* Read an operand: for each component the mask asks for, the source channel
 * its swizzle names, or a constant. The result is closed up over the mask. */
static value load(usse_state *s, const usse_operand *op, uint8_t mask, int32_t shift) {
    value out;
    memset(&out, 0, sizeof(out));
    if (op->bank == USSE_CONST) { out = load_constant(op, mask); modifiers(&out, op->flags); return out; }
    if (op->bank == USSE_PREDICATE) { out.kind = V_UINT; out.n = 1; out.v.u[0] = s->pred[op->num & 3]; return out; }
    if (op->bank == USSE_INDEX) {
        int32_t v = s->index[(op->num - 1) & 1];
        if (op->type == USSE_U8 || op->type == USSE_S8) v &= 0xFF;
        if (op->type == USSE_U16 || op->type == USSE_S16) v &= 0xFFFF;
        out.kind = V_SINT; out.n = 1; out.v.i[0] = v;
        return out;
    }
    if (op->bank == USSE_IMMEDIATE || op->bank == USSE_GLOBAL || op->bank == USSE_SPECIAL || op->bank == USSE_INVALID) {
        out = load_immediate(op, mask);
        modifiers(&out, op->flags);
        return out;
    }
    uint8_t type = op->type;
    uint32_t *w = resolve(s, op, shift, &type, 4);
    out = load_words(op, type, w, mask);
    modifiers(&out, op->flags);
    return out;
}

/* Write an operand: the k-th component the mask selects takes the k-th
 * component of the value. A destination's swizzle means nothing. */
static void store(usse_state *s, const usse_operand *op, const value *val, uint8_t mask, int32_t shift) {
    if (op->bank == USSE_PREDICATE) { s->pred[op->num & 3] = val->v.u[0] != 0; return; }
    if (op->bank == USSE_INDEX) {
        int32_t v = val->v.i[0];
        if (op->type == USSE_U8 || op->type == USSE_S8) v &= 0xFF;
        if (op->type == USSE_U16 || op->type == USSE_S16) v &= 0xFFFF;
        s->index[(op->num - 1) & 1] = v;
        return;
    }
    if (op->bank != USSE_TEMP && op->bank != USSE_PA && op->bank != USSE_OUT && op->bank != USSE_SA &&
        op->bank != USSE_INTERNAL && op->bank != USSE_INDEXED1 && op->bank != USSE_INDEXED2) return;
    uint8_t type = op->type;
    uint32_t *w = resolve(s, op, shift, &type, 4);
    if (!w || !val->n) return;
    int k = 0;
    if (op->bank == USSE_INTERNAL || type == USSE_F32 || type == USSE_U32 || type == USSE_S32) {
        /* whole words: floats as floats, integers as their bits */
        for (int p = 0; p < 4; p++) {
            if (!((mask >> p) & 1) || k >= val->n) continue;
            w[p] = val->kind == V_FLOAT ? float_to_bits(val->v.f[k]) : val->v.u[k];
            k++;
        }
        return;
    }
    for (int p = 0; p < 4; p++) {
        if (!((mask >> p) & 1)) continue;
        int j = k < val->n ? k : val->n - 1;            /* one value serves every component */
        k++;
        switch (type) {
        case USSE_F16: {
            uint32_t h = vita_f32_to_f16(val->kind == V_FLOAT ? val->v.f[j] : (float)val->v.i[j]);
            int at = 16 * (p & 1);
            w[p >> 1] = (w[p >> 1] & ~(0xFFFFu << at)) | (h << at);
            break;
        }
        case USSE_U16: case USSE_S16: {
            int at = 16 * (p & 1);
            w[p >> 1] = (w[p >> 1] & ~(0xFFFFu << at)) | ((val->v.u[j] & 0xFFFFu) << at);
            break;
        }
        case USSE_U8: case USSE_S8:
            w[0] = (w[0] & ~(0xFFu << (8 * p))) | ((val->v.u[j] & 0xFFu) << (8 * p));
            break;
        case USSE_C10: {
            float f = val->v.f[j];
            f = f < -2.0f ? -2.0f : f > 2.0f ? 2.0f : f;
            uint32_t field = (uint32_t)(int32_t)lroundf(f * 255.0f) & 0x3FFu;
            int at = 10 * (p % 3);
            w[p / 3] = (w[p / 3] & ~(0x3FFu << at)) | (field << at);
            break;
        }
        default: break;
        }
    }
}

/* --- running --------------------------------------------------------------------------- */

static int predicate(const usse_state *s, uint8_t pred) {
    switch (pred) {
    case USSE_PRED_P0: case USSE_PRED_P1: case USSE_PRED_P2: case USSE_PRED_P3: return s->pred[pred - USSE_PRED_P0];
    case USSE_PRED_NOT_P0: return !s->pred[0];
    case USSE_PRED_NOT_P1: return !s->pred[1];
    default: return 1;
    }
}

static value floats(int n) {
    value v;
    memset(&v, 0, sizeof(v));
    v.kind = V_FLOAT;
    v.n = (uint8_t)n;
    return v;
}

static value one_integer(uint32_t bits, uint8_t kind) {
    value v;
    memset(&v, 0, sizeof(v));
    v.kind = kind;
    v.n = 1;
    v.v.u[0] = bits;
    return v;
}

static int bits_set(uint8_t mask) {
    int n = 0;
    for (int i = 0; i < 4; i++) n += (mask >> i) & 1;
    return n;
}

static void spread(value *v, float f, uint8_t mask) {
    *v = floats(bits_set(mask));
    for (int i = 0; i < v->n; i++) v->v.f[i] = f;
}

/* One 32-bit register of an operand as its bits, `index` registers on. */
static uint32_t read_word(usse_state *s, const usse_operand *op, int32_t shift, int index) {
    uint8_t type = USSE_U32;
    if (op->bank == USSE_IMMEDIATE) return op->num;
    if (op->bank == USSE_CONST) return CONSTANTS[op->num & 63][index & 1];
    if (op->bank == USSE_INDEX) return (uint32_t)s->index[(op->num - 1) & 1];
    uint32_t *w = resolve(s, op, shift, &type, (uint32_t)index + 1);
    return w ? w[index] : 0;
}

static void write_word(usse_state *s, const usse_operand *op, int32_t shift, int index, uint32_t bits) {
    uint8_t type = USSE_U32;
    uint32_t *w = resolve(s, op, shift, &type, (uint32_t)index + 1);
    if (w) w[index] = bits;
}

/* One integer element of an operand read as some other type than its own. */
static int32_t integer_element(usse_state *s, const usse_operand *op, uint8_t type, int element, int32_t shift) {
    usse_operand o = *op;
    if (o.bank == USSE_IMMEDIATE) return (int32_t)o.num;
    o.type = type;
    o.flags = 0;
    o.sw[0] = (uint8_t)element;
    value v = load(s, &o, 1, shift);
    return v.v.i[0];
}

/* The condition of a conditional move, on one component: against zero. */
static int zero_test(uint8_t kind, uint32_t bits, uint8_t test) {
    if (kind == V_FLOAT) {
        float x = bits_to_float(bits);
        return test == 0 ? x == 0.0f : test == 1 ? (x < 0.0f || x > 0.0f) : test == 2 ? x < 0.0f : x <= 0.0f;
    }
    if (kind == V_SINT) {
        int32_t x = (int32_t)bits;
        return test == 0 ? x == 0 : test == 1 ? x != 0 : test == 2 ? x < 0 : x <= 0;
    }
    return test == 1 ? bits != 0 : test == 2 ? 0 : bits == 0;
}

static int against_zero(const value *t, int i, uint8_t test) { return zero_test(t->kind, t->v.u[i], test); }

/* --- conversions between the formats a pack instruction knows --- */

static float type_max(uint8_t type) {
    return type == USSE_U8 ? 255.0f : type == USSE_S8 ? 127.0f : type == USSE_U16 ? 65535.0f : type == USSE_S16 ? 32767.0f : 1.0f;
}

static int32_t to_integer(float f) {
    if (f != f) return 0;
    if (f >= 2147483520.0f) return 2147483520;
    if (f <= -2147483648.0f) return INT32_MIN;
    return (int32_t)f;
}

static float integer_to_float(uint8_t kind, uint32_t bits, uint8_t source, int scale) {
    float f = kind == V_SINT ? (float)(int32_t)bits : (float)bits;
    if (scale) {
        f /= type_max(source);
        if (kind == V_SINT && f < -1.0f) f = -1.0f;
    }
    return f;
}

static int32_t float_to_integer(float f, uint8_t kind, uint8_t dest, int scale) {
    if (scale) {
        float low = kind == V_SINT ? -1.0f : 0.0f;
        f = f != f ? 0.0f : f < low ? low : f > 1.0f ? 1.0f : f;
        f = roundf(f * type_max(dest));
    }
    return to_integer(f);
}

/* Between floats nothing happens but the re-encoding the store does, and
 * between integers nothing at all. An integer becomes a float as its value,
 * or scaled into 0..1 or -1..1; a float becomes an integer by truncation, or
 * scaled the other way and rounded. */
static value convert(const value *v, uint8_t source, uint8_t dest, int scale) {
    value out = *v;
    int from_float = v->kind == V_FLOAT, to_float = is_float_type(dest);
    if (from_float == to_float) {
        if (!to_float) out.kind = kind_of(dest);
        return out;
    }
    if (to_float) {
        out.kind = V_FLOAT;
        for (int i = 0; i < v->n; i++) out.v.f[i] = integer_to_float(v->kind, v->v.u[i], source, scale);
        return out;
    }
    out.kind = kind_of(dest);
    for (int i = 0; i < v->n; i++) out.v.i[i] = float_to_integer(v->v.f[i], out.kind, dest, scale);
    return out;
}

/* --- tests --- */

/* An integer brought back into the width of its type. */
static int64_t wrap(int64_t v, uint8_t type) {
    switch (type) {
    case USSE_S8:  return (int8_t)v;
    case USSE_U8:  return (uint8_t)v;
    case USSE_S16: return (int16_t)v;
    case USSE_U16: return (uint16_t)v;
    case USSE_S32: return (int32_t)v;
    default:       return (uint32_t)v;
    }
}

/* A test on component i of two loaded values: the operation, then the
 * comparison of its result with zero; or, for a subtraction, the comparison
 * of the two operands themselves. Returns 0 for an operation not computed
 * here. */
static int test_component(const usse_insn *in, const value *a, const value *b, int i, int *result) {
    int exact = in->c == 1, sign = in->b & 3, less, more, equal;
    uint8_t type = in->src1.type;
    if (a->kind == V_FLOAT) {
        float x = a->v.f[i], y = b->v.f[i], v, zero = 0.0f;
        switch (in->a) {
        case T_SUB: v = x; zero = y; break;
        case T_ADD: v = x + y; break;
        case T_MUL: v = x * y; break;
        case T_MIN: v = y < x ? y : x; break;
        case T_MAX: v = x < y ? y : x; break;
        case T_FRC: v = x - floorf(y); break;
        default: return 0;
        }
        less = v < zero; more = v > zero; equal = v == zero;
    } else {
        int64_t x = a->kind == V_SINT ? (int64_t)a->v.i[i] : (int64_t)a->v.u[i];
        int64_t y = b->kind == V_SINT ? (int64_t)b->v.i[i] : (int64_t)b->v.u[i], v, zero = 0;
        int width = (type == USSE_S8 || type == USSE_U8) ? 8 : (type == USSE_S16 || type == USSE_U16) ? 16 : 32;
        switch (in->a) {
        case T_SUB: v = x; zero = y; break;
        case T_ADD: v = wrap(x + y, type); break;
        case T_MUL: v = wrap(x * y, type); break;
        case T_AND: v = x & y; break;
        case T_OR:  v = x | y; break;
        case T_XOR: v = x ^ y; break;
        case T_SHL: v = y >= width ? 0 : wrap((int64_t)((uint64_t)x << y), type); break;
        case T_SHR: v = y >= width ? 0 : (int64_t)(((uint64_t)x & ((1ull << width) - 1)) >> y); break;
        case T_ASR: v = wrap(x, type == USSE_U8 ? USSE_S8 : type == USSE_U16 ? USSE_S16 : USSE_S32) >> (y >= width ? width - 1 : y); break;
        default: return 0;
        }
        less = v < zero; more = v > zero; equal = v == zero;
    }
    switch (sign) {
    case 0:  *result = exact ? equal : (less || more); break;
    case 1:  *result = exact ? (less || equal) : less; break;
    case 2:  *result = exact ? (more || equal) : more; break;
    default: *result = 0; break;
    }
    return 1;
}

/* --- the instructions --- */

static float binary(uint8_t op, float x, float y) {
    return op == USSE_OP_MUL ? x * y : op == USSE_OP_ADD ? x + y : op == USSE_OP_FRC ? x - floorf(y)
         : op == USSE_OP_MIN ? (y < x ? y : x) : (x < y ? y : x);
}

static float unary(uint8_t op, float x) {
    if (op == USSE_OP_RCP) return 1.0f / x;
    if (op == USSE_OP_RSQ) return 1.0f / sqrtf(x);
    if (op == USSE_OP_LOG) return logf(x);
    return x != x ? 1.0f : expf(x);
}

/* The second operand of a bitwise instruction when it is a register: turned,
 * then perhaps complemented, within the instruction's width. */
static uint32_t second_operand(const usse_insn *in, uint32_t bits) {
    uint32_t width = in->a ? 16 : 32, low = in->a ? 0xFFFFu : 0xFFFFFFFFu, turn = in->c % width, b = bits & low;
    if (turn) b = (b << turn) | (b >> (width - turn));
    if (in->b == 2) b = ~b;
    return b & low;
}

static uint32_t bitwise(const usse_insn *in, uint32_t a, uint32_t b) {
    uint32_t width = in->a ? 16 : 32;
    switch (in->op) {
    case USSE_OP_AND: return a & b;
    case USSE_OP_OR:  return a | b;
    case USSE_OP_XOR: return a ^ b;
    case USSE_OP_SHL: return b >= width ? 0 : a << b;
    case USSE_OP_SHR: return b >= width ? 0 : a >> b;
    case USSE_OP_ROL: return b % width ? (a << (b % width)) | (a >> (width - b % width)) : a;
    default: {
        int32_t wide = in->a ? (int16_t)a : (int32_t)a;
        return (uint32_t)(wide >> (b >= width ? width - 1 : b));
    }
    }
}

/* The instructions this file does not execute end the run and say which
 * they were, so a program that needs one is named instead of producing a
 * wrong picture. */
static int execute(usse_state *s, const usse_insn *in) {
    switch (in->op) {
    case USSE_OP_MAD:
        for (int r = 0; r <= in->repeat; r++) {
            value a = load(s, &in->src0, in->mask, repeat_offset(s, in, &in->src0, r));
            value b = load(s, &in->src1, in->mask, repeat_offset(s, in, &in->src1, r));
            value c = load(s, &in->src2, in->mask, repeat_offset(s, in, &in->src2, r));
            value out = floats(a.n);
            for (int i = 0; i < a.n; i++) out.v.f[i] = a.v.f[i] * b.v.f[i] + c.v.f[i];
            store(s, &in->dest, &out, in->mask, repeat_offset(s, in, &in->dest, r));
        }
        return 1;
    case USSE_OP_MUL: case USSE_OP_ADD: case USSE_OP_FRC: case USSE_OP_MIN: case USSE_OP_MAX: {
        value a = load(s, &in->src1, in->src_mask, 0), b = load(s, &in->src2, in->src_mask, 0);
        value out = floats(a.n);
        for (int i = 0; i < a.n; i++) out.v.f[i] = binary(in->op, a.v.f[i], b.v.f[i]);
        store(s, &in->dest, &out, in->mask, 0);
        return 1;
    }
    case USSE_OP_DOT: {
        value a = load(s, &in->src1, 15, 0), b = load(s, &in->src2, 15, 0), out;
        float d = a.v.f[0] * b.v.f[0] + a.v.f[1] * b.v.f[1] + a.v.f[2] * b.v.f[2] + a.v.f[3] * b.v.f[3];
        spread(&out, d, in->mask);
        store(s, &in->dest, &out, in->mask, 0);
        return 1;
    }
    case USSE_OP_DOT_GPI: {
        uint8_t mask = in->mask;
        for (int r = 0; r <= in->repeat; r++) {
            value a = load(s, &in->src1, in->src_mask, repeat_offset(s, in, &in->src1, r));
            value b = load(s, &in->src2, in->src_mask, repeat_offset(s, in, &in->src2, r)), out;
            float d = 0.0f;
            for (int i = 0; i < a.n; i++) d += a.v.f[i] * b.v.f[i];
            spread(&out, d, mask);
            store(s, &in->dest, &out, mask, 0);
            mask = (uint8_t)(((mask << 1) | (mask >> 3)) & 15);
        }
        return 1;
    }
    case USSE_OP_RCP: case USSE_OP_RSQ: case USSE_OP_LOG: case USSE_OP_EXP:
        for (int r = 0; r <= in->repeat; r++) {
            value a = load(s, &in->src1, in->src_mask, repeat_offset(s, in, &in->src1, r)), out;
            spread(&out, unary(in->op, a.v.f[0]), in->mask);
            store(s, &in->dest, &out, in->mask, repeat_offset(s, in, &in->dest, r));
        }
        return 1;

    case USSE_OP_MOV: case USSE_OP_MOVC: case USSE_OP_MOVCU8:
        /* a move, or a choice between two values by a third's relation to zero */
        for (int r = 0; r <= in->repeat; r++) {
            value a = load(s, &in->src1, in->mask, repeat_offset(s, in, &in->src1, r));
            if (in->op != USSE_OP_MOV) {
                value t = load(s, &in->src0, in->mask, repeat_offset(s, in, &in->src0, r));
                value b = load(s, &in->src2, in->mask, repeat_offset(s, in, &in->src2, r));
                for (int i = 0; i < a.n; i++)
                    if (!against_zero(&t, i, in->a)) a.v.u[i] = b.v.u[i];
            }
            store(s, &in->dest, &a, in->mask, repeat_offset(s, in, &in->dest, r));
        }
        return 1;

    case USSE_OP_PCK:
        if (in->src1.type == USSE_O8 || in->dest.type == USSE_O8) return 0;
        for (int r = 0; r <= in->repeat; r++) {
            int32_t o1 = repeat_offset(s, in, &in->src1, r), o2 = repeat_offset(s, in, &in->src2, r);
            value v;
            if (in->b) {
                /* four floats from two registers: x and y of one, then of the other */
                v = floats(0);
                for (int i = 0; i < 4; i++) {
                    if (!((in->mask >> i) & 1)) continue;
                    int c = in->src1.sw[i] & 3;
                    v.v.u[v.n++] = c < 2 ? read_word(s, &in->src1, o1, c) : read_word(s, &in->src2, o2, c - 2);
                }
            } else {
                v = load(s, &in->src1, in->mask, o1);
            }
            value out = convert(&v, in->src1.type, in->dest.type, in->a);
            store(s, &in->dest, &out, in->mask, repeat_offset(s, in, &in->dest, r));
        }
        return 1;

    case USSE_OP_AND: case USSE_OP_OR: case USSE_OP_XOR: case USSE_OP_SHL: case USSE_OP_SHR:
    case USSE_OP_ASR: case USSE_OP_ROL: {
        uint32_t low = in->a ? 0xFFFFu : 0xFFFFFFFFu;
        for (int r = 0; r <= in->repeat; r++) {
            value x = load(s, &in->src1, 1, repeat_offset(s, in, &in->src1, r));
            uint32_t a = x.v.u[0] & low, b = in->imm, v;
            if (in->b != 1) {
                value y = load(s, &in->src2, 1, repeat_offset(s, in, &in->src2, r));
                b = second_operand(in, y.v.u[0]);
            }
            v = bitwise(in, a, b);
            value out = one_integer(v & low, V_UINT);
            store(s, &in->dest, &out, 1, repeat_offset(s, in, &in->dest, r));
        }
        return 1;
    }

    case USSE_OP_TEST: {
        value a = load(s, &in->src1, in->mask, 0), b = load(s, &in->src2, in->mask, 0);
        int result;
        if (!test_component(in, &a, &b, 0, &result)) return 0;
        s->pred[in->dest.num & 3] = (uint8_t)result;
        return 1;
    }
    case USSE_OP_TESTMASK: {
        if (!in->imm) return 1;
        value a = load(s, &in->src1, in->mask, 0), b = load(s, &in->src2, in->mask, 0), out = a;
        uint8_t type = in->dest.type;
        for (int i = 0; i < a.n; i++) {
            int result;
            if (!test_component(in, &a, &b, i, &result)) return 0;
            if (a.kind == V_FLOAT) out.v.f[i] = result ? 1.0f : 0.0f;
            else if (!result)      out.v.u[i] = 0;
            else out.v.u[i] = type == USSE_U8 ? 0xFFu : type == USSE_U16 ? 0xFFFFu : type == USSE_U32 ? 0xFFFFFFFFu
                            : type == USSE_S8 ? 0x7Fu : type == USSE_S16 ? 0x7FFFu : 0x7FFFFFFFu;
        }
        store(s, &in->dest, &out, in->mask, 0);
        return 1;
    }

    case USSE_OP_IMAD16: {
        /* src0 times src1, plus src2, in sixteen bits */
        int is_signed = (in->d & 2) != 0;
        uint8_t base = is_signed ? USSE_S16 : USSE_U16, narrow = is_signed ? USSE_S8 : USSE_U8;
        for (int r = 0; r <= in->repeat; r++) {
            int32_t a = integer_element(s, &in->src0, base, 0, repeat_offset(s, in, &in->src0, r));
            int32_t b = integer_element(s, &in->src1, in->a ? narrow : base, (in->a && (in->c & 1)) ? 1 : 0,
                                        repeat_offset(s, in, &in->src1, r));
            int32_t c = integer_element(s, &in->src2, in->b ? narrow : base, (in->b && (in->c & 2)) ? 1 : 0,
                                        repeat_offset(s, in, &in->src2, r));
            usse_operand dest = in->dest;
            value out = one_integer(((uint32_t)a * (uint32_t)b + (uint32_t)c) & 0xFFFFu, V_SINT);
            dest.type = base;
            store(s, &dest, &out, 1, repeat_offset(s, in, &in->dest, r));
        }
        return 1;
    }

    case USSE_OP_IMAD32: {
        /* src0 times src1, sixteen bits each, plus src2, in thirty-two */
        int is_signed = in->d & 1;
        uint8_t half = is_signed ? USSE_S16 : USSE_U16, whole = is_signed ? USSE_S32 : USSE_U32;
        for (int r = 0; r <= in->repeat; r++) {
            int32_t a = integer_element(s, &in->src0, half, in->a & 1, repeat_offset(s, in, &in->src0, r));
            int32_t b = integer_element(s, &in->src1, half, (in->a >> 1) & 1, repeat_offset(s, in, &in->src1, r));
            int32_t c = in->b == 2 ? integer_element(s, &in->src2, whole, 0, repeat_offset(s, in, &in->src2, r))
                                   : integer_element(s, &in->src2, half, (in->a >> 2) & 1, repeat_offset(s, in, &in->src2, r));
            usse_operand dest = in->dest;
            value out = one_integer((uint32_t)a * (uint32_t)b + (uint32_t)c, is_signed ? V_SINT : V_UINT);
            dest.type = whole;
            store(s, &dest, &out, 1, repeat_offset(s, in, &in->dest, r));
        }
        return 1;
    }

    case USSE_OP_LIMM: {
        value out = one_integer(in->imm, V_UINT);
        store(s, &in->dest, &out, 1, 0);
        return 1;
    }

    case USSE_OP_LDR: {
        /* Words of memory into registers. The base register holds an
         * address less four, which the instruction puts back unless told
         * the address is already whole. */
        if (!s->env || !s->env->read32) return 0;
        uint32_t address = read_word(s, &in->src0, 0, 0);
        address += in->src1.bank == USSE_IMMEDIATE ? in->src1.num : read_word(s, &in->src1, 0, 0) - 0x10000u;
        if (!in->c) address += 4;
        address += read_word(s, &in->src2, 0, 0);
        uint32_t bytes = (uint32_t)in->a * in->b;
        for (uint32_t j = 0; 4 * j < bytes; j++) {
            uint32_t word = s->env->read32(s->env->user, address + 4 * j), left = bytes - 4 * j;
            if (left < 4) {
                uint32_t keep = 0xFFFFFFFFu << (8 * left);
                word = (word & ~keep) | (read_word(s, &in->dest, 0, (int)j) & keep);
            }
            write_word(s, &in->dest, 0, (int)j, word);
        }
        return 1;
    }

    case USSE_OP_SMP: {
        if (!s->env || !s->env->sample) return 0;
        value c = load(s, &in->src0, in->src_mask, 0);
        float coord[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, lod = 0.0f;
        for (int i = 0; i < c.n; i++) coord[i] = c.v.f[i];
        if (in->b == 3) return 0;                       /* explicit gradients */
        if (in->b) {
            usse_operand bias = in->src2;
            bias.type = in->src0.type;
            value l = load(s, &bias, 1, 0);
            lod = l.v.f[0];
        }
        if (in->c == 0) {
            float rgba[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            int n = s->env->sample(s->env->user, in->src1.num, coord, in->a, in->b, lod, rgba);
            if (n < 1 || n > 4) n = 4;
            value out = floats(n);
            for (int i = 0; i < n; i++) out.v.f[i] = rgba[i];
            if (!is_float_type(in->dest.type)) out = convert(&out, USSE_F32, in->dest.type, 1);
            store(s, &in->dest, &out, (uint8_t)((1u << n) - 1), 0);
            return 1;
        }
        if ((in->c == 1 || in->c == 3) && s->env->gather && is_float_type(in->dest.type)) {
            /* the four texels of the footprint, and after them the weight
             * a bilinear filter would give each, as halves */
            float texels[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, uv[2] = { 0.0f, 0.0f };
            s->env->gather(s->env->user, in->src1.num, coord, texels, uv);
            value out = floats(4);
            for (int i = 0; i < 4; i++) out.v.f[i] = texels[i];
            store(s, &in->dest, &out, 15, 0);
            if (in->c == 3) {
                usse_operand weights = in->dest;
                weights.type = USSE_F16;
                weights.num = (uint16_t)(weights.num + (in->dest.type == USSE_F32 ? 4 : 2));
                out.v.f[0] = (1.0f - uv[0]) * (1.0f - uv[1]);
                out.v.f[1] = uv[0] * (1.0f - uv[1]);
                out.v.f[2] = uv[0] * uv[1];
                out.v.f[3] = (1.0f - uv[0]) * uv[1];
                store(s, &weights, &out, 15, 0);
            }
            return 1;
        }
        return 0;
    }

    case USSE_OP_SMLSI:
        apply_smlsi(s, in);
        return 1;
    case USSE_OP_KILL: {
        /* the predicate it names is the one that spares the fragment */
        int spared = in->a == 1 ? s->pred[0] : in->a == 2 ? s->pred[1] : in->a == 3 ? !s->pred[0] : 0;
        if (!spared) s->killed = 1;
        return 1;
    }
    case USSE_OP_PHAS: case USSE_OP_NOP: case USSE_OP_SPEC:
        return 1;
    default:
        return 0;
    }
}

int usse_can_run(const usse_insn *in) {
    switch (in->op) {
    case USSE_OP_INVALID: case USSE_OP_DSX: case USSE_OP_DSY: case USSE_OP_STR: case USSE_OP_SMBO: case USSE_OP_DEPTHF:
        return 0;
    case USSE_OP_PCK:
        return in->src1.type != USSE_O8 && in->dest.type != USSE_O8;
    case USSE_OP_TEST: case USSE_OP_TESTMASK:
        if (is_float_type(in->src1.type))
            return in->a == T_SUB || in->a == T_ADD || in->a == T_MUL || in->a == T_MIN || in->a == T_MAX || in->a == T_FRC;
        return in->a == T_SUB || in->a == T_ADD || in->a == T_MUL || in->a == T_AND || in->a == T_OR || in->a == T_XOR ||
               in->a == T_SHL || in->a == T_SHR || in->a == T_ASR;
    case USSE_OP_SMP:
        return in->b != 3 && in->c != 2;
    default:
        return 1;
    }
}

uint32_t usse_run(const usse_insn *code, uint32_t count, usse_state *s) {
    uint32_t pc = 0;
    /* a branch backwards that never stops being taken is cut off here */
    for (uint64_t budget = 64ull * count + 64; pc < count && budget; budget--) {
        const usse_insn *in = &code[pc];
        if (in->op == USSE_OP_BR) {
            int64_t target = (int64_t)pc + (int32_t)in->imm;
            if (!predicate(s, in->pred)) { pc++; continue; }
            if (target < 0 || target > (int64_t)count) return pc + 1;
            pc = (uint32_t)target;
            continue;
        }
        if (predicate(s, in->pred)) {
            if (!execute(s, in)) return pc + 1;
            if (s->killed) return 0;
        }
        pc++;
    }
    return pc < count ? pc + 1 : 0;
}

/* --- many at once -----------------------------------------------------------------------
 * The same instructions for a row of lanes. Everything below follows the
 * one-at-a-time interpreter above rule for rule: an operand that is the same
 * for every lane (a secondary attribute, a constant, an immediate) is read
 * by the very functions above and copied along the row; one that differs is
 * read from the rows of its register with the same arithmetic in a loop.
 * A few instructions have no such loop and are run for each lane of the
 * row by the interpreter above itself (wide_by_lane). What is handled
 * neither way is refused by usse_wide_can_run, and a program with any of it
 * runs a lane at a time from end to end. */

#define LANES USSE_LANES

typedef struct {
    uint8_t n, kind;
    union { float f[4][LANES]; int32_t i[4][LANES]; uint32_t u[4][LANES]; } v;
} wvalue;

static void fill_row(uint32_t *row, uint32_t bits, unsigned n) {
    for (unsigned l = 0; l < n; l++) row[l] = bits;
}

/* Copy a row to another, in every lane or only where `exec` says. */
static void copy_row(uint32_t *to, const uint32_t *from, unsigned n, const uint8_t *exec) {
    if (!exec) { memcpy(to, from, (size_t)n * 4); return; }
    for (unsigned l = 0; l < n; l++) if (exec[l]) to[l] = from[l];
}

static int in_rows(uint8_t bank) { return bank == USSE_TEMP || bank == USSE_PA || bank == USSE_OUT || bank == USSE_INTERNAL; }

/* The first row of an operand's register, and the type it is kept as; NULL
 * when it would leave its bank. Row k of the register is LANES words on. */
static uint32_t *rows_of(usse_wide *w, const usse_operand *op, int32_t shift, uint8_t *type) {
    int64_t base;
    if (op->bank == USSE_INTERNAL) {
        base = 4 * ((int64_t)op->num + shift);
        if (base < 0 || base + 4 > 16) return NULL;
        if (*type == USSE_F16) *type = USSE_F32;
        else if (*type == USSE_S16 || *type == USSE_S8) *type = USSE_S32;
        else if (*type == USSE_U16 || *type == USSE_U8) *type = USSE_U32;
        return w->internal[base];
    }
    uint32_t (*bank)[LANES] = op->bank == USSE_TEMP ? w->temp : op->bank == USSE_PA ? w->pa : op->bank == USSE_OUT ? w->out : NULL;
    base = (int64_t)op->num + shift;
    if (!bank || base < 0 || base + 4 > USSE_WORDS) return NULL;
    return bank[base];
}

static void wide_modifiers(wvalue *v, uint8_t flags, unsigned n) {
    if (flags & USSE_ABS) {
        if (v->kind == V_UINT) return;
        for (int k = 0; k < v->n; k++) {
            if (v->kind == V_FLOAT) for (unsigned l = 0; l < n; l++) v->v.f[k][l] = fabsf(v->v.f[k][l]);
            else for (unsigned l = 0; l < n; l++) if (v->v.i[k][l] < 0) v->v.i[k][l] = (int32_t)(0u - (uint32_t)v->v.i[k][l]);
        }
    }
    if (flags & USSE_NEG) {
        for (int k = 0; k < v->n; k++) {
            if (v->kind == V_FLOAT) for (unsigned l = 0; l < n; l++) v->v.f[k][l] = 0.0f - v->v.f[k][l];
            else for (unsigned l = 0; l < n; l++) v->v.u[k][l] = 0u - v->v.u[k][l];
        }
    }
}

void usse_extents(const usse_insn *code, uint32_t count, uint16_t words[3]) {
    words[0] = words[1] = words[2] = 0;
    for (uint32_t i = 0; i < count; i++) {
        const usse_insn *in = &code[i];
        const usse_operand *ops[4] = { &in->dest, &in->src0, &in->src1, &in->src2 };
        for (int k = 0; k < 4; k++) {
            int bank = ops[k]->bank == USSE_TEMP ? 0 : ops[k]->bank == USSE_PA ? 1 : ops[k]->bank == USSE_OUT ? 2 : -1;
            if (bank < 0) continue;
            /* a vector of four words, twice over for a pair packed from two registers,
             * and as far again for each time the instruction repeats */
            uint32_t reach = (uint32_t)ops[k]->num + 8u + 16u * in->repeat;
            if (reach > USSE_WORDS) reach = USSE_WORDS;
            if (reach > words[bank]) words[bank] = (uint16_t)reach;
        }
    }
}

/* --- reads of what nothing wrote (usse.h) ------------------------------------------------ */

int usse_wide_watch;
static _Thread_local usse_unwritten t_unwritten;
static _Thread_local uint32_t t_wide_pc;

const usse_unwritten *usse_wide_unwritten(void) { return &t_unwritten; }

static void note_unwritten(const usse_operand *op, uint8_t type, uint8_t channel, int64_t num) {
    if (!t_unwritten.hits++) {
        t_unwritten.instruction = t_wide_pc;
        t_unwritten.bank = op->bank;
        t_unwritten.channel = channel;
        t_unwritten.type = type;
        t_unwritten.num = (uint16_t)num;
    }
}

/* Whether every lane of the field a load is about to read still holds the
 * mark. A row of one lane is not asked: one pixel can hold any value. */
static void watch_load(const usse_operand *op, uint8_t type, uint8_t ch, const uint32_t *rows, unsigned n, int64_t num) {
    const uint32_t *row;
    uint32_t mark = USSE_UNWRITTEN, field = 0xFFFFFFFFu;
    unsigned at = 0;
    if (n < 2) return;
    switch (type) {
    case USSE_F32: case USSE_U32: case USSE_S32: row = rows + (size_t)ch * LANES; break;
    case USSE_F16: case USSE_U16: case USSE_S16: row = rows + (size_t)(ch >> 1) * LANES; at = 16u * (ch & 1u); field = 0xFFFFu; break;
    case USSE_U8: case USSE_S8: row = rows; at = 8u * ch; field = 0xFFu; break;
    default: return;
    }
    for (unsigned l = 0; l < n; l++)
        if (((row[l] >> at) & field) != ((mark >> at) & field)) return;
    note_unwritten(op, type, ch, num);
}

static void wide_load(usse_wide *w, const usse_operand *op, uint8_t mask, int32_t shift, wvalue *out) {
    unsigned n = w->lanes;
    out->n = 0;
    if (!in_rows(op->bank)) {
        value v;
        if (op->bank == USSE_SA) {
            int64_t base = (int64_t)op->num + shift;
            v = load_words(op, op->type, (w->sa && base >= 0 && base + 4 <= USSE_WORDS) ? w->sa + base : NULL, mask);
        } else if (op->bank == USSE_CONST) {
            v = load_constant(op, mask);
        } else {
            v = load_immediate(op, mask);
        }
        modifiers(&v, op->flags);
        out->n = v.n;
        out->kind = v.kind;
        for (int k = 0; k < v.n; k++) fill_row(out->v.u[k], v.v.u[k], n);
        return;
    }
    uint8_t type = op->type;
    const uint32_t *rows = rows_of(w, op, shift, &type);
    out->kind = kind_of(type);
    for (int i = 0; i < 4; i++) {
        if (!((mask >> i) & 1)) continue;
        uint8_t ch = op->sw[i];
        uint32_t *u = out->v.u[out->n];
        if (ch >= USSE_ZERO && ch <= USSE_HALF) {
            float c = CHANNEL_CONSTANT[ch - USSE_ZERO];
            fill_row(u, out->kind == V_FLOAT ? float_to_bits(c) : (uint32_t)c, n);
        } else if (!rows || ch > USSE_W) {
            fill_row(u, 0, n);
        } else {
            const uint32_t *half = rows + (size_t)(ch >> 1) * LANES;
            unsigned at = 16u * (ch & 1u), byte = 8u * ch;
            if (usse_wide_watch) watch_load(op, type, ch, rows, n, (int64_t)op->num + shift);
            switch (type) {
            case USSE_F32: case USSE_U32: case USSE_S32: memcpy(u, rows + (size_t)ch * LANES, (size_t)n * 4); break;
            case USSE_F16: vita_halves_to_floats(half, ch & 1, out->v.f[out->n], n); break;
            case USSE_U16: for (unsigned l = 0; l < n; l++) u[l] = (uint16_t)(half[l] >> at); break;
            case USSE_S16: for (unsigned l = 0; l < n; l++) u[l] = (uint32_t)(int32_t)(int16_t)(half[l] >> at); break;
            case USSE_U8:  for (unsigned l = 0; l < n; l++) u[l] = (uint8_t)(rows[l] >> byte); break;
            case USSE_S8:  for (unsigned l = 0; l < n; l++) u[l] = (uint32_t)(int32_t)(int8_t)(rows[l] >> byte); break;
            default:       fill_row(u, 0, n); break;
            }
        }
        out->n++;
    }
    wide_modifiers(out, op->flags, n);
}

static void wide_store(usse_wide *w, const usse_operand *op, const wvalue *val, uint8_t mask, int32_t shift, const uint8_t *exec) {
    unsigned n = w->lanes;
    if (op->bank == USSE_PREDICATE) {
        uint8_t *p = w->pred[op->num & 3];
        for (unsigned l = 0; l < n; l++) if (!exec || exec[l]) p[l] = val->v.u[0][l] != 0;
        return;
    }
    if (!in_rows(op->bank)) return;
    uint8_t type = op->type;
    uint32_t *rows = rows_of(w, op, shift, &type);
    if (!rows || !val->n) return;
    int k = 0;
    if (op->bank == USSE_INTERNAL || type == USSE_F32 || type == USSE_U32 || type == USSE_S32) {
        for (int p = 0; p < 4; p++) {
            if (!((mask >> p) & 1) || k >= val->n) continue;
            copy_row(rows + (size_t)p * LANES, val->v.u[k], n, exec);
            k++;
        }
        return;
    }
    for (int p = 0; p < 4; p++) {
        if (!((mask >> p) & 1)) continue;
        int j = k < val->n ? k : val->n - 1;
        k++;
        switch (type) {
        case USSE_F16: {
            uint32_t *half = rows + (size_t)(p >> 1) * LANES;
            float converted[LANES];
            const float *from = val->v.f[j];
            if (val->kind != V_FLOAT) {
                for (unsigned l = 0; l < n; l++) converted[l] = (float)val->v.i[j][l];
                from = converted;
            }
            if (!exec) {
                vita_floats_to_halves(from, half, p & 1, n);
            } else {
                unsigned at = 16u * ((unsigned)p & 1u);
                for (unsigned l = 0; l < n; l++)
                    if (exec[l]) half[l] = (half[l] & ~(0xFFFFu << at)) | ((uint32_t)vita_f32_to_f16(from[l]) << at);
            }
            break;
        }
        case USSE_U16: case USSE_S16: {
            uint32_t *half = rows + (size_t)(p >> 1) * LANES;
            unsigned at = 16u * ((unsigned)p & 1u);
            for (unsigned l = 0; l < n; l++)
                if (!exec || exec[l]) half[l] = (half[l] & ~(0xFFFFu << at)) | ((val->v.u[j][l] & 0xFFFFu) << at);
            break;
        }
        case USSE_U8: case USSE_S8: {
            unsigned at = 8u * (unsigned)p;
            for (unsigned l = 0; l < n; l++)
                if (!exec || exec[l]) rows[l] = (rows[l] & ~(0xFFu << at)) | ((val->v.u[j][l] & 0xFFu) << at);
            break;
        }
        default: break;
        }
    }
}

/* One register of an operand as its bits, `index` registers on, for every lane. */
static void wide_word(usse_wide *w, const usse_operand *op, int32_t shift, int index, uint32_t *row) {
    unsigned n = w->lanes;
    int64_t base = (int64_t)op->num + shift;
    const uint32_t *from = NULL;
    uint32_t bits = 0;
    if (op->bank == USSE_INTERNAL) {
        if (base >= 0 && 4 * base + 4 <= 16) from = w->internal[4 * base + index];
    } else if (op->bank == USSE_TEMP || op->bank == USSE_PA || op->bank == USSE_OUT) {
        uint32_t (*bank)[LANES] = op->bank == USSE_TEMP ? w->temp : op->bank == USSE_PA ? w->pa : w->out;
        if (base >= 0 && base + index + 1 <= USSE_WORDS) from = bank[base + index];
    } else if (op->bank == USSE_IMMEDIATE) {
        bits = op->num;
    } else if (op->bank == USSE_CONST) {
        bits = CONSTANTS[op->num & 63][index & 1];
    } else if (op->bank == USSE_SA && w->sa && base >= 0 && base + index + 1 <= USSE_WORDS) {
        bits = w->sa[base + index];
    }
    if (from) memcpy(row, from, (size_t)n * 4); else fill_row(row, bits, n);
}

/* What usse_reset and repeat_offset need of a state: the increments. */
static int32_t wide_offset(const usse_wide *w, const usse_insn *in, const usse_operand *op, int r) {
    usse_state s;                               /* only its increments are read */
    memcpy(s.inc, w->inc, sizeof(s.inc));
    return repeat_offset(&s, in, op, r);
}

static void wide_floats(wvalue *v, int n) { v->kind = V_FLOAT; v->n = (uint8_t)n; }

/* An operand the batched executor reads or writes as it stands. */
static int wide_operand_ok(const usse_operand *op, int dest) {
    if (in_rows(op->bank)) return op->type != USSE_C10 && op->type != USSE_O8;
    if (dest) return 0;
    return op->bank == USSE_SA || op->bank == USSE_CONST || op->bank == USSE_IMMEDIATE || op->bank == USSE_GLOBAL ||
           op->bank == USSE_SPECIAL || op->bank == USSE_INVALID;
}

/* An operand of an instruction that is run a lane at a time inside a row:
 * anything but the index registers, which are a lane's own and which a row
 * does not keep. */
static int lane_operand_ok(const usse_operand *op) {
    return op->bank != USSE_INDEX && op->bank != USSE_INDEXED1 && op->bank != USSE_INDEXED2 && op->bank != USSE_PREDICATE;
}

static int wide_can(const usse_insn *in) {
    if (!usse_can_run(in)) return 0;
    switch (in->op) {
    case USSE_OP_MAD:
        return wide_operand_ok(&in->src0, 0) && wide_operand_ok(&in->src1, 0) && wide_operand_ok(&in->src2, 0) && wide_operand_ok(&in->dest, 1);
    case USSE_OP_MUL: case USSE_OP_ADD: case USSE_OP_FRC: case USSE_OP_MIN: case USSE_OP_MAX: case USSE_OP_DOT:
    case USSE_OP_DOT_GPI:
        return wide_operand_ok(&in->src1, 0) && wide_operand_ok(&in->src2, 0) && wide_operand_ok(&in->dest, 1);
    case USSE_OP_RCP: case USSE_OP_RSQ: case USSE_OP_LOG: case USSE_OP_EXP:
        return wide_operand_ok(&in->src1, 0) && wide_operand_ok(&in->dest, 1);
    case USSE_OP_MOV:
        return wide_operand_ok(&in->src1, 0) && wide_operand_ok(&in->dest, 1);
    case USSE_OP_MOVC: case USSE_OP_MOVCU8:
        return wide_operand_ok(&in->src0, 0) && wide_operand_ok(&in->src1, 0) && wide_operand_ok(&in->src2, 0) && wide_operand_ok(&in->dest, 1);
    case USSE_OP_PCK:
        return wide_operand_ok(&in->src1, 0) && (!in->b || wide_operand_ok(&in->src2, 0)) && wide_operand_ok(&in->dest, 1) &&
               in->src1.type != USSE_C10;
    case USSE_OP_AND: case USSE_OP_OR: case USSE_OP_XOR: case USSE_OP_SHL: case USSE_OP_SHR: case USSE_OP_ASR: case USSE_OP_ROL:
        return wide_operand_ok(&in->src1, 0) && (in->b == 1 || wide_operand_ok(&in->src2, 0)) && wide_operand_ok(&in->dest, 1);
    case USSE_OP_TEST:
        return wide_operand_ok(&in->src1, 0) && wide_operand_ok(&in->src2, 0);
    case USSE_OP_TESTMASK:
        return wide_operand_ok(&in->src1, 0) && wide_operand_ok(&in->src2, 0) && wide_operand_ok(&in->dest, 1);
    case USSE_OP_SMP:
        /* the level operand is read in the coordinate's type */
        return wide_operand_ok(&in->src0, 0) && in_rows(in->dest.bank) &&
               (!in->b || (wide_operand_ok(&in->src2, 0) && !(in_rows(in->src2.bank) && in->src0.type == USSE_C10))) &&
               (in->c == 0 || ((in->c == 1 || in->c == 3) && is_float_type(in->dest.type)));
    case USSE_OP_KILL: case USSE_OP_PHAS: case USSE_OP_NOP: case USSE_OP_SPEC:
        return 1;
    case USSE_OP_SMLSI:
        return in->pred == USSE_PRED_NONE;     /* the increments are the row's, not a lane's */
    case USSE_OP_IMAD16: case USSE_OP_IMAD32: case USSE_OP_LDR:
        /* taken a lane at a time inside the row (wide_by_lane) */
        return lane_operand_ok(&in->src0) && lane_operand_ok(&in->src1) && lane_operand_ok(&in->src2) && in_rows(in->dest.bank);
    default:
        return 0;                               /* branches, indices, stores to memory: a lane at a time */
    }
}

int usse_wide_can_run(const usse_insn *code, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) if (!wide_can(&code[i])) return 0;
    return 1;
}

/* An instruction with no form for a row, run for each lane by the
 * one-at-a-time interpreter itself: the registers it can reach are taken
 * out of the rows into a state of that interpreter's, it is executed there,
 * and what it wrote is put back. A program that is rows of arithmetic with
 * a few of these in it, a vertex program that works out where four
 * matrices are and loads them, then goes a row at a time everywhere else. */
static void wide_by_lane(usse_wide *w, const usse_insn *in, const uint8_t *exec) {
    static _Thread_local usse_state s;
    const usse_operand *ops[4] = { &in->dest, &in->src0, &in->src1, &in->src2 };
    struct { uint32_t (*rows)[LANES]; uint32_t *words; unsigned lo, hi; } part[4];
    unsigned parts = 0, n = w->lanes;
    int writes_rows = 0;
    s.sa = (uint32_t *)w->sa;                   /* read, never written, by what is run this way */
    s.env = w->env;
    memcpy(s.inc, w->inc, sizeof(s.inc));
    s.index[0] = s.index[1] = 0;
    for (int k = 0; k < 4; k++) {
        const usse_operand *op = ops[k];
        uint32_t (*rows)[LANES] = op->bank == USSE_TEMP ? w->temp : op->bank == USSE_PA ? w->pa : op->bank == USSE_OUT ? w->out : NULL;
        if (!rows) continue;
        /* From the register it names, at each repeat as far on as the
         * increments in force put it: a vector and a pair's second
         * register from there, and for a load every word it loads. */
        int64_t reach = 8 + (k == 0 && in->op == USSE_OP_LDR ? ((int64_t)in->a * in->b + 3) / 4 : 0);
        int64_t lo = op->num, hi = lo + reach;          /* the register itself: a load does not move with the repeats */
        for (int r = 0; r <= in->repeat; r++) {
            int64_t at = (int64_t)op->num + repeat_offset(&s, in, op, r);
            if (at < lo) lo = at;
            if (at + reach > hi) hi = at + reach;
        }
        if (lo < 0) lo = 0;
        if (hi > USSE_WORDS) hi = USSE_WORDS;
        if (lo > hi) lo = hi;
        part[parts].rows = rows;
        part[parts].words = op->bank == USSE_TEMP ? s.temp : op->bank == USSE_PA ? s.pa : s.out;
        part[parts].lo = (unsigned)lo; part[parts].hi = (unsigned)hi;
        if (k == 0) writes_rows = 1;
        parts++;
    }
    for (unsigned l = 0; l < n; l++) {
        if (exec && !exec[l]) continue;
        for (unsigned p = 0; p < parts; p++)
            for (unsigned r = part[p].lo; r < part[p].hi; r++) part[p].words[r] = part[p].rows[r][l];
        for (int i = 0; i < 16; i++) s.internal[i] = w->internal[i][l];
        for (int i = 0; i < 4; i++) s.pred[i] = w->pred[i][l];
        s.killed = 0;
        execute(&s, in);
        if (writes_rows) for (unsigned r = part[0].lo; r < part[0].hi; r++) part[0].rows[r][l] = part[0].words[r];
        else for (int i = 0; i < 16; i++) w->internal[i][l] = s.internal[i];
    }
}

static void wide_execute(usse_wide *w, const usse_insn *in, const uint8_t *exec) {
    wvalue a, b, c, out;                            /* a kilobyte each */
    unsigned n = w->lanes;
    switch (in->op) {
    case USSE_OP_IMAD16: case USSE_OP_IMAD32: case USSE_OP_LDR:
        wide_by_lane(w, in, exec);
        return;
    case USSE_OP_MAD:
        for (int r = 0; r <= in->repeat; r++) {
            wide_load(w, &in->src0, in->mask, wide_offset(w, in, &in->src0, r), &a);
            wide_load(w, &in->src1, in->mask, wide_offset(w, in, &in->src1, r), &b);
            wide_load(w, &in->src2, in->mask, wide_offset(w, in, &in->src2, r), &c);
            wide_floats(&out, a.n);
            for (int k = 0; k < a.n; k++) {
                const float *x = a.v.f[k], *y = b.v.f[k], *z = c.v.f[k];
                float *o = out.v.f[k];
                for (unsigned l = 0; l < n; l++) o[l] = x[l] * y[l] + z[l];
            }
            wide_store(w, &in->dest, &out, in->mask, wide_offset(w, in, &in->dest, r), exec);
        }
        return;
    case USSE_OP_MUL: case USSE_OP_ADD: case USSE_OP_FRC: case USSE_OP_MIN: case USSE_OP_MAX:
        wide_load(w, &in->src1, in->src_mask, 0, &a);
        wide_load(w, &in->src2, in->src_mask, 0, &b);
        wide_floats(&out, a.n);
        for (int k = 0; k < a.n; k++) {
            const float *x = a.v.f[k], *y = b.v.f[k];
            float *o = out.v.f[k];
            switch (in->op) {
            case USSE_OP_MUL: for (unsigned l = 0; l < n; l++) o[l] = x[l] * y[l]; break;
            case USSE_OP_ADD: for (unsigned l = 0; l < n; l++) o[l] = x[l] + y[l]; break;
            case USSE_OP_MIN: for (unsigned l = 0; l < n; l++) o[l] = y[l] < x[l] ? y[l] : x[l]; break;
            case USSE_OP_MAX: for (unsigned l = 0; l < n; l++) o[l] = x[l] < y[l] ? y[l] : x[l]; break;
            default:          for (unsigned l = 0; l < n; l++) o[l] = x[l] - floorf(y[l]); break;
            }
        }
        wide_store(w, &in->dest, &out, in->mask, 0, exec);
        return;
    case USSE_OP_DOT:
        wide_load(w, &in->src1, 15, 0, &a);
        wide_load(w, &in->src2, 15, 0, &b);
        wide_floats(&out, bits_set(in->mask));
        for (unsigned l = 0; l < n; l++)
            out.v.f[0][l] = a.v.f[0][l] * b.v.f[0][l] + a.v.f[1][l] * b.v.f[1][l] + a.v.f[2][l] * b.v.f[2][l] + a.v.f[3][l] * b.v.f[3][l];
        for (int k = 1; k < out.n; k++) memcpy(out.v.f[k], out.v.f[0], (size_t)n * 4);
        wide_store(w, &in->dest, &out, in->mask, 0, exec);
        return;
    case USSE_OP_DOT_GPI: {
        uint8_t mask = in->mask;
        for (int r = 0; r <= in->repeat; r++) {
            wide_load(w, &in->src1, in->src_mask, wide_offset(w, in, &in->src1, r), &a);
            wide_load(w, &in->src2, in->src_mask, wide_offset(w, in, &in->src2, r), &b);
            wide_floats(&out, bits_set(mask));
            for (unsigned l = 0; l < n; l++) {
                float d = 0.0f;
                for (int k = 0; k < a.n; k++) d += a.v.f[k][l] * b.v.f[k][l];
                out.v.f[0][l] = d;
            }
            for (int k = 1; k < out.n; k++) memcpy(out.v.f[k], out.v.f[0], (size_t)n * 4);
            wide_store(w, &in->dest, &out, mask, 0, exec);
            mask = (uint8_t)(((mask << 1) | (mask >> 3)) & 15);
        }
        return;
    }
    case USSE_OP_RCP: case USSE_OP_RSQ: case USSE_OP_LOG: case USSE_OP_EXP:
        for (int r = 0; r <= in->repeat; r++) {
            wide_load(w, &in->src1, in->src_mask, wide_offset(w, in, &in->src1, r), &a);
            wide_floats(&out, bits_set(in->mask));
            if (in->op == USSE_OP_RCP)      for (unsigned l = 0; l < n; l++) out.v.f[0][l] = 1.0f / a.v.f[0][l];
            else if (in->op == USSE_OP_RSQ) for (unsigned l = 0; l < n; l++) out.v.f[0][l] = 1.0f / sqrtf(a.v.f[0][l]);
            else                            for (unsigned l = 0; l < n; l++) out.v.f[0][l] = unary(in->op, a.v.f[0][l]);
            for (int k = 1; k < out.n; k++) memcpy(out.v.f[k], out.v.f[0], (size_t)n * 4);
            wide_store(w, &in->dest, &out, in->mask, wide_offset(w, in, &in->dest, r), exec);
        }
        return;

    case USSE_OP_MOV: case USSE_OP_MOVC: case USSE_OP_MOVCU8:
        for (int r = 0; r <= in->repeat; r++) {
            wide_load(w, &in->src1, in->mask, wide_offset(w, in, &in->src1, r), &a);
            if (in->op != USSE_OP_MOV) {
                wide_load(w, &in->src0, in->mask, wide_offset(w, in, &in->src0, r), &c);
                wide_load(w, &in->src2, in->mask, wide_offset(w, in, &in->src2, r), &b);
                for (int k = 0; k < a.n; k++)
                    for (unsigned l = 0; l < n; l++)
                        if (!zero_test(c.kind, c.v.u[k][l], in->a)) a.v.u[k][l] = b.v.u[k][l];
            }
            wide_store(w, &in->dest, &a, in->mask, wide_offset(w, in, &in->dest, r), exec);
        }
        return;

    case USSE_OP_PCK:
        for (int r = 0; r <= in->repeat; r++) {
            int32_t o1 = wide_offset(w, in, &in->src1, r), o2 = wide_offset(w, in, &in->src2, r);
            if (in->b) {
                wide_floats(&a, 0);
                for (int i = 0; i < 4; i++) {
                    if (!((in->mask >> i) & 1)) continue;
                    int ch = in->src1.sw[i] & 3;
                    if (ch < 2) wide_word(w, &in->src1, o1, ch, a.v.u[a.n]); else wide_word(w, &in->src2, o2, ch - 2, a.v.u[a.n]);
                    a.n++;
                }
            } else {
                wide_load(w, &in->src1, in->mask, o1, &a);
            }
            /* between floats and between integers the values pass as they are */
            int from_float = a.kind == V_FLOAT, to_float = is_float_type(in->dest.type);
            if (from_float != to_float) {
                uint8_t kind = kind_of(in->dest.type);
                for (int k = 0; k < a.n; k++) {
                    if (to_float) for (unsigned l = 0; l < n; l++) a.v.f[k][l] = integer_to_float(a.kind, a.v.u[k][l], in->src1.type, in->a);
                    else          for (unsigned l = 0; l < n; l++) a.v.i[k][l] = float_to_integer(a.v.f[k][l], kind, in->dest.type, in->a);
                }
                a.kind = to_float ? V_FLOAT : kind;
            } else if (!to_float) {
                a.kind = kind_of(in->dest.type);
            }
            wide_store(w, &in->dest, &a, in->mask, wide_offset(w, in, &in->dest, r), exec);
        }
        return;

    case USSE_OP_AND: case USSE_OP_OR: case USSE_OP_XOR: case USSE_OP_SHL: case USSE_OP_SHR:
    case USSE_OP_ASR: case USSE_OP_ROL: {
        uint32_t low = in->a ? 0xFFFFu : 0xFFFFFFFFu;
        for (int r = 0; r <= in->repeat; r++) {
            wide_load(w, &in->src1, 1, wide_offset(w, in, &in->src1, r), &a);
            if (in->b != 1) wide_load(w, &in->src2, 1, wide_offset(w, in, &in->src2, r), &b);
            out.kind = V_UINT;
            out.n = 1;
            for (unsigned l = 0; l < n; l++) {
                uint32_t x = a.v.u[0][l] & low, y = in->b != 1 ? second_operand(in, b.v.u[0][l]) : in->imm;
                out.v.u[0][l] = bitwise(in, x, y) & low;
            }
            wide_store(w, &in->dest, &out, 1, wide_offset(w, in, &in->dest, r), exec);
        }
        return;
    }

    case USSE_OP_TEST: case USSE_OP_TESTMASK: {
        if (in->op == USSE_OP_TESTMASK && !in->imm) return;
        wide_load(w, &in->src1, in->mask, 0, &a);
        wide_load(w, &in->src2, in->mask, 0, &b);
        uint8_t type = in->dest.type;
        out.kind = in->op == USSE_OP_TEST ? V_UINT : a.kind;
        out.n = in->op == USSE_OP_TEST ? 1 : a.n;
        for (int k = 0; k < out.n; k++)
            for (unsigned l = 0; l < n; l++) {
                value x, y;
                int result = 0;
                x.kind = a.kind; x.n = 1; x.v.u[0] = a.v.u[k][l];
                y.kind = b.kind; y.n = 1; y.v.u[0] = b.v.u[k][l];
                test_component(in, &x, &y, 0, &result);
                if (in->op == USSE_OP_TEST)  out.v.u[k][l] = (uint32_t)result;
                else if (a.kind == V_FLOAT)  out.v.f[k][l] = result ? 1.0f : 0.0f;
                else if (!result)            out.v.u[k][l] = 0;
                else out.v.u[k][l] = type == USSE_U8 ? 0xFFu : type == USSE_U16 ? 0xFFFFu : type == USSE_U32 ? 0xFFFFFFFFu
                                   : type == USSE_S8 ? 0x7Fu : type == USSE_S16 ? 0x7FFFu : 0x7FFFFFFFu;
            }
        if (in->op == USSE_OP_TEST) {
            uint8_t *p = w->pred[in->dest.num & 3];
            for (unsigned l = 0; l < n; l++) if (!exec || exec[l]) p[l] = (uint8_t)out.v.u[0][l];
        } else {
            wide_store(w, &in->dest, &out, in->mask, 0, exec);
        }
        return;
    }

    case USSE_OP_SMP: {
        if (!w->env || !w->env->sample) return;
        wide_load(w, &in->src0, in->src_mask, 0, &a);
        if (in->b) {
            usse_operand bias = in->src2;
            bias.type = in->src0.type;
            wide_load(w, &bias, 1, 0, &b);
        }
        if (in->c == 0) {
            int comps = 4;
            for (unsigned l = 0; l < n; l++) {
                float coord[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, rgba[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                if (exec && !exec[l]) continue;
                for (int k = 0; k < a.n; k++) coord[k] = a.v.f[k][l];
                int m = w->env->sample(w->env->user, in->src1.num, coord, in->a, in->b, in->b ? b.v.f[0][l] : 0.0f, rgba);
                comps = (m < 1 || m > 4) ? 4 : m;
                for (int k = 0; k < 4; k++) out.v.f[k][l] = rgba[k];
            }
            wide_floats(&out, comps);
            if (!is_float_type(in->dest.type)) {
                uint8_t kind = kind_of(in->dest.type);
                for (int k = 0; k < comps; k++)
                    for (unsigned l = 0; l < n; l++) out.v.i[k][l] = float_to_integer(out.v.f[k][l], kind, in->dest.type, 1);
                out.kind = kind;
            }
            wide_store(w, &in->dest, &out, (uint8_t)((1u << comps) - 1), 0, exec);
        } else if (w->env->gather) {
            for (unsigned l = 0; l < n; l++) {
                float coord[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, texels[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, uv[2] = { 0.0f, 0.0f };
                if (exec && !exec[l]) continue;
                for (int k = 0; k < a.n; k++) coord[k] = a.v.f[k][l];
                w->env->gather(w->env->user, in->src1.num, coord, texels, uv);
                for (int k = 0; k < 4; k++) out.v.f[k][l] = texels[k];
                c.v.f[0][l] = (1.0f - uv[0]) * (1.0f - uv[1]);
                c.v.f[1][l] = uv[0] * (1.0f - uv[1]);
                c.v.f[2][l] = uv[0] * uv[1];
                c.v.f[3][l] = (1.0f - uv[0]) * uv[1];
            }
            wide_floats(&out, 4);
            wide_store(w, &in->dest, &out, 15, 0, exec);
            if (in->c == 3) {
                usse_operand weights = in->dest;
                weights.type = USSE_F16;
                weights.num = (uint16_t)(weights.num + (in->dest.type == USSE_F32 ? 4 : 2));
                wide_floats(&c, 4);
                wide_store(w, &weights, &c, 15, 0, exec);
            }
        }
        return;
    }

    case USSE_OP_SMLSI: {
        usse_state s;                           /* only its increments are written */
        memcpy(s.inc, w->inc, sizeof(s.inc));
        apply_smlsi(&s, in);
        memcpy(w->inc, s.inc, sizeof(s.inc));
        return;
    }
    case USSE_OP_KILL:
        for (unsigned l = 0; l < n; l++) {
            int spared = in->a == 1 ? w->pred[0][l] : in->a == 2 ? w->pred[1][l] : in->a == 3 ? !w->pred[0][l] : 0;
            if (!spared && (!exec || exec[l])) w->killed[l] = 1;
        }
        return;
    default:
        return;
    }
}

void usse_run_wide(const usse_insn *code, uint32_t count, usse_wide *w) {
    uint8_t mask[LANES];
    unsigned n = w->lanes;
    for (int role = 0; role < 4; role++)
        for (int j = 0; j < 17; j++) w->inc[role][j] = j;
    memset(w->killed, 0, n);
    if (usse_wide_watch) memset(&t_unwritten, 0, sizeof(t_unwritten));
    for (uint32_t pc = 0; pc < count; pc++) {
        const usse_insn *in = &code[pc];
        const uint8_t *exec = NULL;
        if (usse_wide_watch) t_wide_pc = pc;
        if (in->pred != USSE_PRED_NONE && in->pred != USSE_PRED_PN) {
            /* a lane whose predicate is false is left exactly as it was */
            const uint8_t *p = w->pred[in->pred == USSE_PRED_NOT_P0 ? 0 : in->pred == USSE_PRED_NOT_P1 ? 1 : in->pred - USSE_PRED_P0];
            int negate = in->pred == USSE_PRED_NOT_P0 || in->pred == USSE_PRED_NOT_P1, any = 0;
            if (usse_wide_watch && n > 1) {
                unsigned l = 0;
                usse_operand which = { USSE_PREDICATE, 0, 0, 0, (uint16_t)(p - w->pred[0]) / LANES, { 0, 0, 0, 0 } };
                while (l < n && p[l] == USSE_UNWRITTEN_PREDICATE) l++;
                if (l == n) note_unwritten(&which, 0, 0, which.num);
            }
            for (unsigned l = 0; l < n; l++) { mask[l] = (uint8_t)((p[l] != 0) != negate); any |= mask[l]; }
            if (!any) continue;
            exec = mask;
        }
        wide_execute(w, in, exec);
    }
}
