/* usse_fields.h — where every field of a USSE instruction lies.
 *
 * A USSE instruction is one 64-bit word. Its top bits name a format, and a
 * format is a fixed partition of the word into fields. This file is that
 * partition and nothing else: for each format the bits that must match, and
 * each field as its lowest bit and its width. usse.c reads instructions
 * through it, so no shift or mask is written anywhere by hand.
 *
 * The formats are tried in the order they are listed and the first whose
 * fixed bits match wins: later formats are deliberately looser than the
 * ones before them. The layouts are the hardware's, as documented by the
 * Vita3K project's decoder; the field names follow its documentation.
 */

#ifndef VITARECOMP_USSE_FIELDS_H
#define VITARECOMP_USSE_FIELDS_H

#include <stdint.h>

/* A field: its lowest bit in the low byte, its width above. */
#define USSE_FIELD(low, width) (((width) << 8) | (low))

static inline uint32_t usse_field(uint64_t insn, int field) {
    return (uint32_t)(insn >> (field & 0xFF)) & (uint32_t)((1ull << (field >> 8)) - 1);
}

/* The formats, in matching order. */
enum {
    USSE_F_VMAD2,    /* multiply-add of three 64-bit operands */
    USSE_F_V32NMAD,    /* two-operand vector arithmetic, 32-bit floats */
    USSE_F_V16NMAD,    /* two-operand vector arithmetic, 16-bit floats */
    USSE_F_VMAD,    /* multiply-add with two internal registers */
    USSE_F_VDP,    /* dot product with an internal register */
    USSE_F_VDUAL,    /* two operations issued together */
    USSE_F_VCOMP,    /* reciprocal, reciprocal square root, logarithm, exponential */
    USSE_F_VMOV,    /* move, plain and conditional */
    USSE_F_VPCK,    /* pack and unpack between number formats */
    USSE_F_VTST,    /* test: compute, compare with zero, set a predicate */
    USSE_F_VTSTMSK,    /* test, the mask form */
    USSE_F_VBW,    /* bitwise and shifts */
    USSE_F_SOP2,    /* sum of products, two sources */
    USSE_F_SOP2M,    /* sum of products with a mask */
    USSE_F_SOP3,    /* sum of products, three sources */
    USSE_F_I8MAD,    /* 8-bit integer multiply-add */
    USSE_F_I16MAD,    /* 16-bit integer multiply-add */
    USSE_F_I32MAD,    /* 32-bit integer multiply-add */
    USSE_F_ILLEGAL22,
    USSE_F_ILLEGAL23,
    USSE_F_ILLEGAL24,
    USSE_F_I8MAD2,    /* 8-bit integer multiply-add, second form */
    USSE_F_I32MAD2,    /* 32-bit integer multiply-add, second form */
    USSE_F_ILLEGAL27,
    USSE_F_SMP,    /* texture sample */
    USSE_F_PHAS,    /* phase change */
    USSE_F_NOP,    /* no operation */
    USSE_F_BR,    /* branch */
    USSE_F_SMLSI,    /* set the per-operand repeat increments */
    USSE_F_SMBO,    /* set the per-operand base offsets */
    USSE_F_KILL,    /* discard the fragment */
    USSE_F_LIMM,    /* load a 32-bit immediate */
    USSE_F_DEPTHF,    /* depth replacement */
    USSE_F_SPEC,    /* special */
    USSE_F_VLDST,    /* load from and store to memory */
    USSE_F_COUNT
};

/* The bits each format fixes, and the values they must have. */
static const struct { uint64_t mask, value; } USSE_FORMATS[USSE_F_COUNT] = {
    { 0xF800000000000000ull, 0x0000000000000000ull },    /* vmad2 */
    { 0xF800000000000000ull, 0x0800000000000000ull },    /* v32nmad */
    { 0xF800000000000000ull, 0x1000000000000000ull },    /* v16nmad */
    { 0xF820000000000000ull, 0x1820000000000000ull },    /* vmad */
    { 0xF820000000000000ull, 0x1800000000000000ull },    /* vdp */
    { 0xF000000000000000ull, 0x2000000000000000ull },    /* vdual */
    { 0xF800000000000000ull, 0x3000000000000000ull },    /* vcomp */
    { 0xF800000000000000ull, 0x3800000000000000ull },    /* vmov */
    { 0xF800000000000000ull, 0x4000000000000000ull },    /* vpck */
    { 0xF800000000000000ull, 0x4800000000000000ull },    /* vtst */
    { 0xF800000000000000ull, 0x7800000000000000ull },    /* vtstmsk */
    { 0xC000000000000000ull, 0x4000000000000000ull },    /* vbw */
    { 0xF800000000000000ull, 0x8000000000000000ull },    /* sop2 */
    { 0xF800000000000000ull, 0x9000000000000000ull },    /* sop2m */
    { 0xF800000000000000ull, 0x8800000000000000ull },    /* sop3 */
    { 0xF800000000000000ull, 0x9800000000000000ull },    /* i8mad */
    { 0xF800000000000000ull, 0xA000000000000000ull },    /* i16mad */
    { 0xF800833800000000ull, 0xA800000000000000ull },    /* i32mad */
    { 0xF800000000000000ull, 0xB000000000000000ull },    /* illegal22 */
    { 0xF800000000000000ull, 0xB800000000000000ull },    /* illegal23 */
    { 0xF800000000000000ull, 0xC000000000000000ull },    /* illegal24 */
    { 0xF800000000000000ull, 0xC800000000000000ull },    /* i8mad2 */
    { 0xF8000C7800000000ull, 0xD000000000000000ull },    /* i32mad2 */
    { 0xF800000000000000ull, 0xD800000000000000ull },    /* illegal27 */
    { 0xF800000000000000ull, 0xE000000000000000ull },    /* smp */
    { 0xFF70000000000000ull, 0xFA40000000000000ull },    /* phas */
    { 0xF87001C000000000ull, 0xF800014000000000ull },    /* nop */
    { 0xF870018000000000ull, 0xF800000000000000ull },    /* br */
    { 0xFF30000000000000ull, 0xFA10000000000000ull },    /* smlsi */
    { 0xFF30000000000000ull, 0xFB10000000000000ull },    /* smbo */
    { 0xFF3FF9FFF0000000ull, 0xF9300006F0000000ull },    /* kill */
    { 0xFF30000000000000ull, 0xFC20000000000000ull },    /* limm */
    { 0xFF30000000000000ull, 0xFB30000000000000ull },    /* depthf */
    { 0xF800000000000000ull, 0xF800000000000000ull },    /* spec */
    { 0xE000000000000000ull, 0xE000000000000000ull },    /* vldst */
};

/* vmad2: multiply-add of three 64-bit operands */
#define U_VMAD2_DAT_FMT            USSE_FIELD(58,  1)
#define U_VMAD2_PRED               USSE_FIELD(56,  2)
#define U_VMAD2_SKIPINV            USSE_FIELD(55,  1)
#define U_VMAD2_SRC0_SWIZ_BITS2    USSE_FIELD(53,  1)
#define U_VMAD2_SYNCSTART          USSE_FIELD(52,  1)
#define U_VMAD2_SRC0_ABS           USSE_FIELD(50,  1)
#define U_VMAD2_SRC1_BANK_EXT      USSE_FIELD(49,  1)
#define U_VMAD2_SRC2_BANK_EXT      USSE_FIELD(48,  1)
#define U_VMAD2_SRC2_SWIZ          USSE_FIELD(45,  3)
#define U_VMAD2_SRC1_SWIZ_BIT2     USSE_FIELD(44,  1)
#define U_VMAD2_NOSCHED            USSE_FIELD(43,  1)
#define U_VMAD2_DEST_MASK          USSE_FIELD(39,  4)
#define U_VMAD2_SRC1_MOD           USSE_FIELD(37,  2)
#define U_VMAD2_SRC2_MOD           USSE_FIELD(35,  2)
#define U_VMAD2_SRC0_BANK          USSE_FIELD(34,  1)
#define U_VMAD2_DEST_BANK          USSE_FIELD(32,  2)
#define U_VMAD2_SRC1_BANK          USSE_FIELD(30,  2)
#define U_VMAD2_SRC2_BANK          USSE_FIELD(28,  2)
#define U_VMAD2_DEST_N             USSE_FIELD(22,  6)
#define U_VMAD2_SRC1_SWIZ_BITS01   USSE_FIELD(20,  2)
#define U_VMAD2_SRC0_SWIZ_BITS01   USSE_FIELD(18,  2)
#define U_VMAD2_SRC0_N             USSE_FIELD(12,  6)
#define U_VMAD2_SRC1_N             USSE_FIELD( 6,  6)
#define U_VMAD2_SRC2_N             USSE_FIELD( 0,  6)

/* v32nmad: two-operand vector arithmetic, 32-bit floats */
#define U_V32NMAD_PRED              USSE_FIELD(56,  3)
#define U_V32NMAD_SKIPINV           USSE_FIELD(55,  1)
#define U_V32NMAD_SRC1_SWIZ_10_11   USSE_FIELD(53,  2)
#define U_V32NMAD_SYNCSTART         USSE_FIELD(52,  1)
#define U_V32NMAD_DEST_BANK_EXT     USSE_FIELD(51,  1)
#define U_V32NMAD_SRC1_SWIZ_9       USSE_FIELD(50,  1)
#define U_V32NMAD_SRC1_BANK_EXT     USSE_FIELD(49,  1)
#define U_V32NMAD_SRC2_BANK_EXT     USSE_FIELD(48,  1)
#define U_V32NMAD_SRC2_SWIZ         USSE_FIELD(44,  4)
#define U_V32NMAD_NOSCHED           USSE_FIELD(43,  1)
#define U_V32NMAD_DEST_MASK         USSE_FIELD(39,  4)
#define U_V32NMAD_SRC1_MOD          USSE_FIELD(37,  2)
#define U_V32NMAD_SRC2_MOD          USSE_FIELD(36,  1)
#define U_V32NMAD_SRC1_SWIZ_7_8     USSE_FIELD(34,  2)
#define U_V32NMAD_DEST_BANK_SEL     USSE_FIELD(32,  2)
#define U_V32NMAD_SRC1_BANK_SEL     USSE_FIELD(30,  2)
#define U_V32NMAD_SRC2_BANK_SEL     USSE_FIELD(28,  2)
#define U_V32NMAD_DEST_N            USSE_FIELD(22,  6)
#define U_V32NMAD_SRC1_SWIZ_0_6     USSE_FIELD(15,  7)
#define U_V32NMAD_OP2               USSE_FIELD(12,  3)
#define U_V32NMAD_SRC1_N            USSE_FIELD( 6,  6)
#define U_V32NMAD_SRC2_N            USSE_FIELD( 0,  6)

/* v16nmad: two-operand vector arithmetic, 16-bit floats */
#define U_V16NMAD_PRED              USSE_FIELD(56,  3)
#define U_V16NMAD_SKIPINV           USSE_FIELD(55,  1)
#define U_V16NMAD_SRC1_SWIZ_10_11   USSE_FIELD(53,  2)
#define U_V16NMAD_SYNCSTART         USSE_FIELD(52,  1)
#define U_V16NMAD_DEST_BANK_EXT     USSE_FIELD(51,  1)
#define U_V16NMAD_SRC1_SWIZ_9       USSE_FIELD(50,  1)
#define U_V16NMAD_SRC1_BANK_EXT     USSE_FIELD(49,  1)
#define U_V16NMAD_SRC2_BANK_EXT     USSE_FIELD(48,  1)
#define U_V16NMAD_SRC2_SWIZ         USSE_FIELD(44,  4)
#define U_V16NMAD_NOSCHED           USSE_FIELD(43,  1)
#define U_V16NMAD_DEST_MASK         USSE_FIELD(39,  4)
#define U_V16NMAD_SRC1_MOD          USSE_FIELD(37,  2)
#define U_V16NMAD_SRC2_MOD          USSE_FIELD(36,  1)
#define U_V16NMAD_SRC1_SWIZ_7_8     USSE_FIELD(34,  2)
#define U_V16NMAD_DEST_BANK_SEL     USSE_FIELD(32,  2)
#define U_V16NMAD_SRC1_BANK_SEL     USSE_FIELD(30,  2)
#define U_V16NMAD_SRC2_BANK_SEL     USSE_FIELD(28,  2)
#define U_V16NMAD_DEST_N            USSE_FIELD(22,  6)
#define U_V16NMAD_SRC1_SWIZ_0_6     USSE_FIELD(15,  7)
#define U_V16NMAD_OP2               USSE_FIELD(12,  3)
#define U_V16NMAD_SRC1_N            USSE_FIELD( 6,  6)
#define U_V16NMAD_SRC2_N            USSE_FIELD( 0,  6)

/* vmad: multiply-add with two internal registers */
#define U_VMAD_PRED                USSE_FIELD(56,  3)
#define U_VMAD_SKIPINV             USSE_FIELD(55,  1)
#define U_VMAD_GPI1_SWIZ_EXT       USSE_FIELD(54,  1)
#define U_VMAD_OPCODE2             USSE_FIELD(52,  1)
#define U_VMAD_DEST_USE_BANK_EXT   USSE_FIELD(51,  1)
#define U_VMAD_END                 USSE_FIELD(50,  1)
#define U_VMAD_SRC1_BANK_EXT       USSE_FIELD(49,  1)
#define U_VMAD_REPEAT_MODE         USSE_FIELD(47,  2)
#define U_VMAD_GPI0_ABS            USSE_FIELD(46,  1)
#define U_VMAD_REPEAT_COUNT        USSE_FIELD(44,  2)
#define U_VMAD_NOSCHED             USSE_FIELD(43,  1)
#define U_VMAD_WRITE_MASK          USSE_FIELD(39,  4)
#define U_VMAD_SRC1_NEG            USSE_FIELD(38,  1)
#define U_VMAD_SRC1_ABS            USSE_FIELD(37,  1)
#define U_VMAD_GPI1_NEG            USSE_FIELD(36,  1)
#define U_VMAD_GPI1_ABS            USSE_FIELD(35,  1)
#define U_VMAD_GPI0_SWIZ_EXT       USSE_FIELD(34,  1)
#define U_VMAD_DEST_BANK           USSE_FIELD(32,  2)
#define U_VMAD_SRC1_BANK           USSE_FIELD(30,  2)
#define U_VMAD_GPI0_N              USSE_FIELD(28,  2)
#define U_VMAD_DEST_N              USSE_FIELD(22,  6)
#define U_VMAD_GPI0_SWIZ           USSE_FIELD(18,  4)
#define U_VMAD_GPI1_SWIZ           USSE_FIELD(14,  4)
#define U_VMAD_GPI1_N              USSE_FIELD(12,  2)
#define U_VMAD_GPI0_NEG            USSE_FIELD(11,  1)
#define U_VMAD_SRC1_SWIZ_EXT       USSE_FIELD(10,  1)
#define U_VMAD_SRC1_SWIZ           USSE_FIELD( 6,  4)
#define U_VMAD_SRC1_N              USSE_FIELD( 0,  6)

/* vdp: dot product with an internal register */
#define U_VDP_PRED                USSE_FIELD(56,  3)
#define U_VDP_SKIPINV             USSE_FIELD(55,  1)
#define U_VDP_CLIP_PLANE_ENABLE   USSE_FIELD(54,  1)
#define U_VDP_OPCODE2             USSE_FIELD(52,  1)
#define U_VDP_DEST_USE_BANK_EXT   USSE_FIELD(51,  1)
#define U_VDP_END                 USSE_FIELD(50,  1)
#define U_VDP_SRC1_BANK_EXT       USSE_FIELD(49,  1)
#define U_VDP_REPEAT_MODE         USSE_FIELD(47,  2)
#define U_VDP_GPI0_ABS            USSE_FIELD(46,  1)
#define U_VDP_REPEAT_COUNT        USSE_FIELD(44,  2)
#define U_VDP_NOSCHED             USSE_FIELD(43,  1)
#define U_VDP_WRITE_MASK          USSE_FIELD(39,  4)
#define U_VDP_SRC1_NEG            USSE_FIELD(38,  1)
#define U_VDP_SRC1_ABS            USSE_FIELD(37,  1)
#define U_VDP_CLIP_PLANE_N        USSE_FIELD(34,  3)
#define U_VDP_DEST_BANK           USSE_FIELD(32,  2)
#define U_VDP_SRC1_BANK           USSE_FIELD(30,  2)
#define U_VDP_GPI0_N              USSE_FIELD(28,  2)
#define U_VDP_DEST_N              USSE_FIELD(22,  6)
#define U_VDP_GPI0_SWIZ           USSE_FIELD(18,  4)
#define U_VDP_SRC1_SWIZ_W         USSE_FIELD(15,  3)
#define U_VDP_SRC1_SWIZ_Z         USSE_FIELD(12,  3)
#define U_VDP_SRC1_SWIZ_Y         USSE_FIELD( 9,  3)
#define U_VDP_SRC1_SWIZ_X         USSE_FIELD( 6,  3)
#define U_VDP_SRC1_N              USSE_FIELD( 0,  6)

/* vdual: two operations issued together */
#define U_VDUAL_COMP_COUNT_TYPE                            USSE_FIELD(59,  1)
#define U_VDUAL_GPI1_NEG                                   USSE_FIELD(58,  1)
#define U_VDUAL_SV_PRED                                    USSE_FIELD(56,  2)
#define U_VDUAL_SKIPINV                                    USSE_FIELD(55,  1)
#define U_VDUAL_DUAL_OP1_EXT_VEC3_OR_HAS_W_VEC4            USSE_FIELD(54,  1)
#define U_VDUAL_TYPE_F16                                   USSE_FIELD(53,  1)
#define U_VDUAL_GPI1_SWIZZ_EXT                             USSE_FIELD(52,  1)
#define U_VDUAL_UNIFIED_STORE_SWIZZ                        USSE_FIELD(48,  4)
#define U_VDUAL_UNIFIED_STORE_NEG                          USSE_FIELD(47,  1)
#define U_VDUAL_DUAL_OP1                                   USSE_FIELD(44,  3)
#define U_VDUAL_DUAL_OP2_EXT                               USSE_FIELD(43,  1)
#define U_VDUAL_PRIM_USTORE                                USSE_FIELD(42,  1)
#define U_VDUAL_GPI0_SWIZZ                                 USSE_FIELD(38,  4)
#define U_VDUAL_GPI1_SWIZZ                                 USSE_FIELD(34,  4)
#define U_VDUAL_PRIM_DEST_BANK                             USSE_FIELD(32,  2)
#define U_VDUAL_UNIFIED_STORE_SLOT_BANK                    USSE_FIELD(30,  2)
#define U_VDUAL_PRIM_DEST_NUM_GPI_CASE                     USSE_FIELD(28,  2)
#define U_VDUAL_PRIM_DEST_NUM                              USSE_FIELD(21,  7)
#define U_VDUAL_DUAL_OP2                                   USSE_FIELD(18,  3)
#define U_VDUAL_SRC_CONFIG                                 USSE_FIELD(16,  2)
#define U_VDUAL_GPI2_SLOT_NUM_BIT_1                        USSE_FIELD(15,  1)
#define U_VDUAL_GPI2_SLOT_NUM_BIT_0_OR_UNIFIED_STORE_ABS   USSE_FIELD(14,  1)
#define U_VDUAL_GPI1_SLOT_NUM                              USSE_FIELD(12,  2)
#define U_VDUAL_GPI0_SLOT_NUM                              USSE_FIELD(10,  2)
#define U_VDUAL_WRITE_MASK_NON_GPI                         USSE_FIELD( 7,  3)
#define U_VDUAL_UNIFIED_STORE_SLOT_NUM                     USSE_FIELD( 0,  7)

/* vcomp: reciprocal, reciprocal square root, logarithm, exponential */
#define U_VCOMP_PRED            USSE_FIELD(56,  3)
#define U_VCOMP_SKIPINV         USSE_FIELD(55,  1)
#define U_VCOMP_DEST_TYPE       USSE_FIELD(53,  2)
#define U_VCOMP_SYNCSTART       USSE_FIELD(52,  1)
#define U_VCOMP_DEST_BANK_EXT   USSE_FIELD(51,  1)
#define U_VCOMP_END             USSE_FIELD(50,  1)
#define U_VCOMP_SRC1_BANK_EXT   USSE_FIELD(49,  1)
#define U_VCOMP_REPEAT_COUNT    USSE_FIELD(44,  4)
#define U_VCOMP_NOSCHED         USSE_FIELD(43,  1)
#define U_VCOMP_OP2             USSE_FIELD(41,  2)
#define U_VCOMP_SRC_TYPE        USSE_FIELD(39,  2)
#define U_VCOMP_SRC1_MOD        USSE_FIELD(37,  2)
#define U_VCOMP_SRC_COMP        USSE_FIELD(35,  2)
#define U_VCOMP_DEST_BANK       USSE_FIELD(32,  2)
#define U_VCOMP_SRC1_BANK       USSE_FIELD(30,  2)
#define U_VCOMP_DEST_N          USSE_FIELD(21,  7)
#define U_VCOMP_SRC1_N          USSE_FIELD( 7,  7)
#define U_VCOMP_WRITE_MASK      USSE_FIELD( 0,  4)

/* vmov: move, plain and conditional */
#define U_VMOV_PRED                   USSE_FIELD(56,  3)
#define U_VMOV_SKIPINV                USSE_FIELD(55,  1)
#define U_VMOV_TEST_BIT_2             USSE_FIELD(54,  1)
#define U_VMOV_SRC0_COMP_SEL          USSE_FIELD(53,  1)
#define U_VMOV_SYNCSTART              USSE_FIELD(52,  1)
#define U_VMOV_DEST_BANK_EXT          USSE_FIELD(51,  1)
#define U_VMOV_END_OR_SRC0_BANK_EXT   USSE_FIELD(50,  1)
#define U_VMOV_SRC1_BANK_EXT          USSE_FIELD(49,  1)
#define U_VMOV_SRC2_BANK_EXT          USSE_FIELD(48,  1)
#define U_VMOV_MOVE_TYPE              USSE_FIELD(46,  2)
#define U_VMOV_REPEAT_COUNT           USSE_FIELD(44,  2)
#define U_VMOV_NOSCHED                USSE_FIELD(43,  1)
#define U_VMOV_MOVE_DATA_TYPE         USSE_FIELD(40,  3)
#define U_VMOV_TEST_BIT_1             USSE_FIELD(39,  1)
#define U_VMOV_SRC0_SWIZ              USSE_FIELD(35,  4)
#define U_VMOV_SRC0_BANK_SEL          USSE_FIELD(34,  1)
#define U_VMOV_DEST_BANK_SEL          USSE_FIELD(32,  2)
#define U_VMOV_SRC1_BANK_SEL          USSE_FIELD(30,  2)
#define U_VMOV_SRC2_BANK_SEL          USSE_FIELD(28,  2)
#define U_VMOV_DEST_MASK              USSE_FIELD(24,  4)
#define U_VMOV_DEST_N                 USSE_FIELD(18,  6)
#define U_VMOV_SRC0_N                 USSE_FIELD(12,  6)
#define U_VMOV_SRC1_N                 USSE_FIELD( 6,  6)
#define U_VMOV_SRC2_N                 USSE_FIELD( 0,  6)

/* vpck: pack and unpack between number formats */
#define U_VPCK_PRED              USSE_FIELD(56,  3)
#define U_VPCK_SKIPINV           USSE_FIELD(55,  1)
#define U_VPCK_NOSCHED           USSE_FIELD(54,  1)
#define U_VPCK_UNKNOWN           USSE_FIELD(53,  1)
#define U_VPCK_SYNCSTART         USSE_FIELD(52,  1)
#define U_VPCK_DEST_BANK_EXT     USSE_FIELD(51,  1)
#define U_VPCK_END               USSE_FIELD(50,  1)
#define U_VPCK_SRC1_BANK_EXT     USSE_FIELD(49,  1)
#define U_VPCK_SRC2_BANK_EXT     USSE_FIELD(48,  1)
#define U_VPCK_REPEAT_COUNT      USSE_FIELD(44,  4)
#define U_VPCK_SRC_FMT           USSE_FIELD(41,  3)
#define U_VPCK_DEST_FMT          USSE_FIELD(38,  3)
#define U_VPCK_DEST_MASK         USSE_FIELD(34,  4)
#define U_VPCK_DEST_BANK_SEL     USSE_FIELD(32,  2)
#define U_VPCK_SRC1_BANK_SEL     USSE_FIELD(30,  2)
#define U_VPCK_SRC2_BANK_SEL     USSE_FIELD(28,  2)
#define U_VPCK_DEST_N            USSE_FIELD(21,  7)
#define U_VPCK_COMP_SEL_3        USSE_FIELD(19,  2)
#define U_VPCK_SCALE             USSE_FIELD(18,  1)
#define U_VPCK_COMP_SEL_1        USSE_FIELD(16,  2)
#define U_VPCK_COMP_SEL_2        USSE_FIELD(14,  2)
#define U_VPCK_SRC1_N            USSE_FIELD( 8,  6)
#define U_VPCK_COMP0_SEL_BIT1    USSE_FIELD( 7,  1)
#define U_VPCK_SRC2_N            USSE_FIELD( 1,  6)
#define U_VPCK_COMP_SEL_0_BIT0   USSE_FIELD( 0,  1)

/* vtst: test: compute, compare with zero, set a predicate */
#define U_VTST_PRED              USSE_FIELD(56,  3)
#define U_VTST_SKIPINV           USSE_FIELD(55,  1)
#define U_VTST_ONCEONLY          USSE_FIELD(53,  1)
#define U_VTST_SYNCSTART         USSE_FIELD(52,  1)
#define U_VTST_DEST_EXT          USSE_FIELD(51,  1)
#define U_VTST_SRC1_NEG          USSE_FIELD(50,  1)
#define U_VTST_SRC1_EXT          USSE_FIELD(49,  1)
#define U_VTST_SRC2_EXT          USSE_FIELD(48,  1)
#define U_VTST_PREC              USSE_FIELD(47,  1)
#define U_VTST_SRC2_VSCOMP       USSE_FIELD(46,  1)
#define U_VTST_RPT_COUNT         USSE_FIELD(44,  2)
#define U_VTST_SIGN_TEST         USSE_FIELD(42,  2)
#define U_VTST_ZERO_TEST         USSE_FIELD(40,  2)
#define U_VTST_TEST_CRCOMB_AND   USSE_FIELD(39,  1)
#define U_VTST_CHAN_CC           USSE_FIELD(36,  3)
#define U_VTST_PDST_N            USSE_FIELD(34,  2)
#define U_VTST_DEST_BANK         USSE_FIELD(32,  2)
#define U_VTST_SRC1_BANK         USSE_FIELD(30,  2)
#define U_VTST_SRC2_BANK         USSE_FIELD(28,  2)
#define U_VTST_DEST_N            USSE_FIELD(21,  7)
#define U_VTST_TEST_WBEN         USSE_FIELD(20,  1)
#define U_VTST_ALU_SEL           USSE_FIELD(18,  2)
#define U_VTST_ALU_OP            USSE_FIELD(14,  4)
#define U_VTST_SRC1_N            USSE_FIELD( 7,  7)
#define U_VTST_SRC2_N            USSE_FIELD( 0,  7)

/* vtstmsk: test, the mask form */
#define U_VTSTMSK_PRED              USSE_FIELD(56,  3)
#define U_VTSTMSK_SKIPINV           USSE_FIELD(55,  1)
#define U_VTSTMSK_ONCEONLY          USSE_FIELD(53,  1)
#define U_VTSTMSK_SYNCSTART         USSE_FIELD(52,  1)
#define U_VTSTMSK_DEST_EXT          USSE_FIELD(51,  1)
#define U_VTSTMSK_TEST_FLAG_2       USSE_FIELD(50,  1)
#define U_VTSTMSK_SRC1_EXT          USSE_FIELD(49,  1)
#define U_VTSTMSK_SRC2_EXT          USSE_FIELD(48,  1)
#define U_VTSTMSK_PREC              USSE_FIELD(47,  1)
#define U_VTSTMSK_SRC2_VSCOMP       USSE_FIELD(46,  1)
#define U_VTSTMSK_RPT_COUNT         USSE_FIELD(44,  2)
#define U_VTSTMSK_SIGN_TEST         USSE_FIELD(42,  2)
#define U_VTSTMSK_ZERO_TEST         USSE_FIELD(40,  2)
#define U_VTSTMSK_TEST_CRCOMB_AND   USSE_FIELD(39,  1)
#define U_VTSTMSK_TST_MASK_TYPE     USSE_FIELD(36,  2)
#define U_VTSTMSK_DEST_BANK         USSE_FIELD(32,  2)
#define U_VTSTMSK_SRC1_BANK         USSE_FIELD(30,  2)
#define U_VTSTMSK_SRC2_BANK         USSE_FIELD(28,  2)
#define U_VTSTMSK_DEST_N            USSE_FIELD(21,  7)
#define U_VTSTMSK_TEST_WBEN         USSE_FIELD(20,  1)
#define U_VTSTMSK_ALU_SEL           USSE_FIELD(18,  2)
#define U_VTSTMSK_ALU_OP            USSE_FIELD(14,  4)
#define U_VTSTMSK_SRC1_N            USSE_FIELD( 7,  7)
#define U_VTSTMSK_SRC2_N            USSE_FIELD( 0,  7)

/* vbw: bitwise and shifts */
#define U_VBW_OP1               USSE_FIELD(59,  3)
#define U_VBW_PRED              USSE_FIELD(56,  3)
#define U_VBW_SKIPINV           USSE_FIELD(55,  1)
#define U_VBW_NOSCHED           USSE_FIELD(54,  1)
#define U_VBW_REPEAT_SEL        USSE_FIELD(53,  1)
#define U_VBW_SYNC_START        USSE_FIELD(52,  1)
#define U_VBW_DEST_EXT          USSE_FIELD(51,  1)
#define U_VBW_END               USSE_FIELD(50,  1)
#define U_VBW_SRC1_EXT          USSE_FIELD(49,  1)
#define U_VBW_SRC2_EXT          USSE_FIELD(48,  1)
#define U_VBW_REPEAT_COUNT      USSE_FIELD(44,  4)
#define U_VBW_SRC2_INVERT       USSE_FIELD(43,  1)
#define U_VBW_SRC2_ROT          USSE_FIELD(38,  5)
#define U_VBW_SRC2_EXTH         USSE_FIELD(36,  2)
#define U_VBW_OP2               USSE_FIELD(35,  1)
#define U_VBW_BITWISE_PARTIAL   USSE_FIELD(34,  1)
#define U_VBW_DEST_BANK         USSE_FIELD(32,  2)
#define U_VBW_SRC1_BANK         USSE_FIELD(30,  2)
#define U_VBW_SRC2_BANK         USSE_FIELD(28,  2)
#define U_VBW_DEST_N            USSE_FIELD(21,  7)
#define U_VBW_SRC2_SEL          USSE_FIELD(14,  7)
#define U_VBW_SRC1_N            USSE_FIELD( 7,  7)
#define U_VBW_SRC2_N            USSE_FIELD( 0,  7)

/* sop2: sum of products, two sources */
#define U_SOP2_PRED            USSE_FIELD(57,  2)
#define U_SOP2_CMOD1           USSE_FIELD(56,  1)
#define U_SOP2_SKIPINV         USSE_FIELD(55,  1)
#define U_SOP2_NOSCHED         USSE_FIELD(54,  1)
#define U_SOP2_ASEL1           USSE_FIELD(52,  2)
#define U_SOP2_DEST_BANK_EXT   USSE_FIELD(51,  1)
#define U_SOP2_END             USSE_FIELD(50,  1)
#define U_SOP2_SRC1_BANK_EXT   USSE_FIELD(49,  1)
#define U_SOP2_SRC2_BANK_EXT   USSE_FIELD(48,  1)
#define U_SOP2_CMOD2           USSE_FIELD(47,  1)
#define U_SOP2_COUNT           USSE_FIELD(44,  3)
#define U_SOP2_AMOD1           USSE_FIELD(43,  1)
#define U_SOP2_ASEL2           USSE_FIELD(41,  2)
#define U_SOP2_CSEL1           USSE_FIELD(38,  3)
#define U_SOP2_CSEL2           USSE_FIELD(35,  3)
#define U_SOP2_AMOD2           USSE_FIELD(34,  1)
#define U_SOP2_DEST_BANK       USSE_FIELD(32,  2)
#define U_SOP2_SRC1_BANK       USSE_FIELD(30,  2)
#define U_SOP2_SRC2_BANK       USSE_FIELD(28,  2)
#define U_SOP2_DEST_N          USSE_FIELD(21,  7)
#define U_SOP2_SRC1_MOD        USSE_FIELD(20,  1)
#define U_SOP2_COP             USSE_FIELD(18,  2)
#define U_SOP2_AOP             USSE_FIELD(16,  2)
#define U_SOP2_ASRC1_MOD       USSE_FIELD(15,  1)
#define U_SOP2_DEST_MOD        USSE_FIELD(14,  1)
#define U_SOP2_SRC1_N          USSE_FIELD( 7,  7)
#define U_SOP2_SRC2_N          USSE_FIELD( 0,  7)

/* sop2m: sum of products with a mask */
#define U_SOP2M_PRED          USSE_FIELD(57,  2)
#define U_SOP2M_MOD1          USSE_FIELD(56,  1)
#define U_SOP2M_SKIPINV       USSE_FIELD(55,  1)
#define U_SOP2M_NOSCHED       USSE_FIELD(54,  1)
#define U_SOP2M_COP           USSE_FIELD(52,  2)
#define U_SOP2M_DESTBANKEXT   USSE_FIELD(51,  1)
#define U_SOP2M_END           USSE_FIELD(50,  1)
#define U_SOP2M_SRC1BANKEXT   USSE_FIELD(49,  1)
#define U_SOP2M_SRC2BANKEXT   USSE_FIELD(48,  1)
#define U_SOP2M_MOD2          USSE_FIELD(47,  1)
#define U_SOP2M_WMASK         USSE_FIELD(43,  4)
#define U_SOP2M_AOP           USSE_FIELD(41,  2)
#define U_SOP2M_SEL1          USSE_FIELD(38,  3)
#define U_SOP2M_SEL2          USSE_FIELD(35,  3)
#define U_SOP2M_DESTBANK      USSE_FIELD(32,  2)
#define U_SOP2M_SRC1BANK      USSE_FIELD(30,  2)
#define U_SOP2M_SRC2BANK      USSE_FIELD(28,  2)
#define U_SOP2M_DESTNUM       USSE_FIELD(21,  7)
#define U_SOP2M_SRC1NUM       USSE_FIELD( 7,  7)
#define U_SOP2M_SRC2NUM       USSE_FIELD( 0,  7)

/* sop3: sum of products, three sources */
#define U_SOP3_PRED       USSE_FIELD(57,  2)
#define U_SOP3_CMOD1      USSE_FIELD(56,  1)
#define U_SOP3_SKIPINV    USSE_FIELD(55,  1)
#define U_SOP3_NOSCHED    USSE_FIELD(54,  1)
#define U_SOP3_COP        USSE_FIELD(52,  2)
#define U_SOP3_DESTBEXT   USSE_FIELD(51,  1)
#define U_SOP3_END        USSE_FIELD(50,  1)
#define U_SOP3_SRC1BEXT   USSE_FIELD(49,  1)
#define U_SOP3_SRC2BEXT   USSE_FIELD(48,  1)
#define U_SOP3_CMOD2      USSE_FIELD(47,  1)
#define U_SOP3_AMOD1      USSE_FIELD(46,  1)
#define U_SOP3_ASEL1      USSE_FIELD(44,  2)
#define U_SOP3_DMOD       USSE_FIELD(43,  1)
#define U_SOP3_AOP        USSE_FIELD(41,  2)
#define U_SOP3_CSEL1      USSE_FIELD(38,  3)
#define U_SOP3_CSEL2      USSE_FIELD(35,  3)
#define U_SOP3_SRC0BANK   USSE_FIELD(34,  1)
#define U_SOP3_DESTBANK   USSE_FIELD(32,  2)
#define U_SOP3_SRC1BANK   USSE_FIELD(30,  2)
#define U_SOP3_SRC2BANK   USSE_FIELD(28,  2)
#define U_SOP3_DESTN      USSE_FIELD(21,  7)
#define U_SOP3_SRC0N      USSE_FIELD(14,  7)
#define U_SOP3_SRC1N      USSE_FIELD( 7,  7)
#define U_SOP3_SRC2N      USSE_FIELD( 0,  7)

/* i8mad: 8-bit integer multiply-add */
#define U_I8MAD_PRED            USSE_FIELD(57,  2)
#define U_I8MAD_CMOD1           USSE_FIELD(56,  1)
#define U_I8MAD_SKIPINV         USSE_FIELD(55,  1)
#define U_I8MAD_NOSCHED         USSE_FIELD(54,  1)
#define U_I8MAD_CSEL0           USSE_FIELD(52,  2)
#define U_I8MAD_DEST_BANK_EXT   USSE_FIELD(51,  1)
#define U_I8MAD_END             USSE_FIELD(50,  1)
#define U_I8MAD_SRC1_BANK_EXT   USSE_FIELD(49,  1)
#define U_I8MAD_SRC2_BANK_EXT   USSE_FIELD(48,  1)
#define U_I8MAD_CMOD2           USSE_FIELD(47,  1)
#define U_I8MAD_REPEAT_COUNT    USSE_FIELD(44,  3)
#define U_I8MAD_SATURATED       USSE_FIELD(43,  1)
#define U_I8MAD_CMOD0           USSE_FIELD(42,  1)
#define U_I8MAD_ASEL0           USSE_FIELD(41,  1)
#define U_I8MAD_AMOD2           USSE_FIELD(40,  1)
#define U_I8MAD_AMOD1           USSE_FIELD(39,  1)
#define U_I8MAD_AMOD0           USSE_FIELD(38,  1)
#define U_I8MAD_CSEL1           USSE_FIELD(37,  1)
#define U_I8MAD_CSEL2           USSE_FIELD(36,  1)
#define U_I8MAD_SRC0_NEG        USSE_FIELD(35,  1)
#define U_I8MAD_SRC0_BANK       USSE_FIELD(34,  1)
#define U_I8MAD_DEST_BANK       USSE_FIELD(32,  2)
#define U_I8MAD_SRC1_BANK       USSE_FIELD(30,  2)
#define U_I8MAD_SRC2_BANK       USSE_FIELD(28,  2)
#define U_I8MAD_DEST_NUM        USSE_FIELD(21,  7)
#define U_I8MAD_SRC0_NUM        USSE_FIELD(14,  7)
#define U_I8MAD_SRC1_NUM        USSE_FIELD( 7,  7)
#define U_I8MAD_SRC2_NUM        USSE_FIELD( 0,  7)

/* i16mad: 16-bit integer multiply-add */
#define U_I16MAD_PRED            USSE_FIELD(57,  2)
#define U_I16MAD_ABS             USSE_FIELD(56,  1)
#define U_I16MAD_SKIPINV         USSE_FIELD(55,  1)
#define U_I16MAD_NOSCHED         USSE_FIELD(54,  1)
#define U_I16MAD_SRC2_NEG        USSE_FIELD(53,  1)
#define U_I16MAD_SEL1H_UPPER8    USSE_FIELD(52,  1)
#define U_I16MAD_DEST_BANK_EXT   USSE_FIELD(51,  1)
#define U_I16MAD_END             USSE_FIELD(50,  1)
#define U_I16MAD_SRC1_BANK_EXT   USSE_FIELD(49,  1)
#define U_I16MAD_SRC2_BANK_EXT   USSE_FIELD(48,  1)
#define U_I16MAD_REPEAT_COUNT    USSE_FIELD(44,  3)
#define U_I16MAD_MODE            USSE_FIELD(42,  2)
#define U_I16MAD_SRC2_FORMAT     USSE_FIELD(40,  2)
#define U_I16MAD_SRC1_FORMAT     USSE_FIELD(38,  2)
#define U_I16MAD_SEL2H_UPPER8    USSE_FIELD(37,  1)
#define U_I16MAD_OR_SHIFT        USSE_FIELD(35,  2)
#define U_I16MAD_SRC0_BANK       USSE_FIELD(34,  1)
#define U_I16MAD_DEST_BANK       USSE_FIELD(32,  2)
#define U_I16MAD_SRC1_BANK       USSE_FIELD(30,  2)
#define U_I16MAD_SRC2_BANK       USSE_FIELD(28,  2)
#define U_I16MAD_DEST_N          USSE_FIELD(21,  7)
#define U_I16MAD_SRC0_N          USSE_FIELD(14,  7)
#define U_I16MAD_SRC1_N          USSE_FIELD( 7,  7)
#define U_I16MAD_SRC2_N          USSE_FIELD( 0,  7)

/* i32mad: 32-bit integer multiply-add */
#define U_I32MAD_PRED            USSE_FIELD(57,  2)
#define U_I32MAD_SRC0_HIGH       USSE_FIELD(56,  1)
#define U_I32MAD_NOSCHED         USSE_FIELD(54,  1)
#define U_I32MAD_SRC1_HIGH       USSE_FIELD(53,  1)
#define U_I32MAD_SRC2_HIGH       USSE_FIELD(52,  1)
#define U_I32MAD_DEST_BANK_EXT   USSE_FIELD(51,  1)
#define U_I32MAD_END             USSE_FIELD(50,  1)
#define U_I32MAD_SRC1_BANK_EXT   USSE_FIELD(49,  1)
#define U_I32MAD_SRC2_BANK_EXT   USSE_FIELD(48,  1)
#define U_I32MAD_REPEAT_COUNT    USSE_FIELD(44,  3)
#define U_I32MAD_IS_SIGNED       USSE_FIELD(43,  1)
#define U_I32MAD_IS_SAT          USSE_FIELD(42,  1)
#define U_I32MAD_SRC2_TYPE       USSE_FIELD(38,  2)
#define U_I32MAD_SRC0_BANK       USSE_FIELD(34,  1)
#define U_I32MAD_DEST_BANK       USSE_FIELD(32,  2)
#define U_I32MAD_SRC1_BANK       USSE_FIELD(30,  2)
#define U_I32MAD_SRC2_BANK       USSE_FIELD(28,  2)
#define U_I32MAD_DEST_N          USSE_FIELD(21,  7)
#define U_I32MAD_SRC0_N          USSE_FIELD(14,  7)
#define U_I32MAD_SRC1_N          USSE_FIELD( 7,  7)
#define U_I32MAD_SRC2_N          USSE_FIELD( 0,  7)

/* i32mad2: 32-bit integer multiply-add, second form */
#define U_I32MAD2_PRED            USSE_FIELD(56,  3)
#define U_I32MAD2_NOSCHED         USSE_FIELD(54,  1)
#define U_I32MAD2_SN              USSE_FIELD(52,  2)
#define U_I32MAD2_DEST_BANK_EXT   USSE_FIELD(51,  1)
#define U_I32MAD2_END             USSE_FIELD(50,  1)
#define U_I32MAD2_SRC1_BANK_EXT   USSE_FIELD(49,  1)
#define U_I32MAD2_SRC2_BANK_EXT   USSE_FIELD(48,  1)
#define U_I32MAD2_SRC0_BANK_EXT   USSE_FIELD(47,  1)
#define U_I32MAD2_COUNT           USSE_FIELD(44,  3)
#define U_I32MAD2_IS_SIGNED       USSE_FIELD(41,  1)
#define U_I32MAD2_NEGATIVE_SRC1   USSE_FIELD(40,  1)
#define U_I32MAD2_NEGATIVE_SRC2   USSE_FIELD(39,  1)
#define U_I32MAD2_SRC0_BANK       USSE_FIELD(34,  1)
#define U_I32MAD2_DEST_BANK       USSE_FIELD(32,  2)
#define U_I32MAD2_SRC1_BANK       USSE_FIELD(30,  2)
#define U_I32MAD2_SRC2_BANK       USSE_FIELD(28,  2)
#define U_I32MAD2_DEST_N          USSE_FIELD(21,  7)
#define U_I32MAD2_SRC0_N          USSE_FIELD(14,  7)
#define U_I32MAD2_SRC1_N          USSE_FIELD( 7,  7)
#define U_I32MAD2_SRC2_N          USSE_FIELD( 0,  7)

/* smp: texture sample */
#define U_SMP_PRED          USSE_FIELD(56,  3)
#define U_SMP_SKIPINV       USSE_FIELD(55,  1)
#define U_SMP_NOSCHED       USSE_FIELD(54,  1)
#define U_SMP_SYNCSTART     USSE_FIELD(52,  1)
#define U_SMP_MINPACK       USSE_FIELD(51,  1)
#define U_SMP_SRC0_EXT      USSE_FIELD(50,  1)
#define U_SMP_SRC1_EXT      USSE_FIELD(49,  1)
#define U_SMP_SRC2_EXT      USSE_FIELD(48,  1)
#define U_SMP_FCONV_TYPE    USSE_FIELD(46,  2)
#define U_SMP_MASK_COUNT    USSE_FIELD(44,  2)
#define U_SMP_DIM           USSE_FIELD(42,  2)
#define U_SMP_LOD_MODE      USSE_FIELD(40,  2)
#define U_SMP_DEST_USE_PA   USSE_FIELD(39,  1)
#define U_SMP_SB_MODE       USSE_FIELD(37,  2)
#define U_SMP_SRC0_TYPE     USSE_FIELD(35,  2)
#define U_SMP_SRC0_BANK     USSE_FIELD(34,  1)
#define U_SMP_DRC_SEL       USSE_FIELD(32,  2)
#define U_SMP_SRC1_BANK     USSE_FIELD(30,  2)
#define U_SMP_SRC2_BANK     USSE_FIELD(28,  2)
#define U_SMP_DEST_N        USSE_FIELD(21,  7)
#define U_SMP_SRC0_N        USSE_FIELD(14,  7)
#define U_SMP_SRC1_N        USSE_FIELD( 7,  7)
#define U_SMP_SRC2_N        USSE_FIELD( 0,  7)

/* phas: phase change */
#define U_PHAS_SPRVV                    USSE_FIELD(55,  1)
#define U_PHAS_END                      USSE_FIELD(51,  1)
#define U_PHAS_IMM                      USSE_FIELD(50,  1)
#define U_PHAS_SRC1_BANK_EXT            USSE_FIELD(49,  1)
#define U_PHAS_SRC2_BANK_EXT            USSE_FIELD(48,  1)
#define U_PHAS_MODE                     USSE_FIELD(45,  1)
#define U_PHAS_RATE_HI                  USSE_FIELD(44,  1)
#define U_PHAS_RATE_LO_OR_NOSCHED       USSE_FIELD(43,  1)
#define U_PHAS_WAIT_COND                USSE_FIELD(40,  3)
#define U_PHAS_TEMP_COUNT               USSE_FIELD(32,  8)
#define U_PHAS_SRC1_BANK                USSE_FIELD(30,  2)
#define U_PHAS_SRC2_BANK                USSE_FIELD(28,  2)
#define U_PHAS_EXE_ADDR_HIGH            USSE_FIELD(14,  6)
#define U_PHAS_SRC1_N_OR_EXE_ADDR_MID   USSE_FIELD( 7,  7)
#define U_PHAS_SRC2_N_OR_EXE_ADDR_LOW   USSE_FIELD( 0,  7)

/* nop: no operation */
/* br: branch */
#define U_BR_PRED         USSE_FIELD(56,  3)
#define U_BR_SYNCEND      USSE_FIELD(55,  1)
#define U_BR_EXCEPTION    USSE_FIELD(51,  1)
#define U_BR_PWAIT        USSE_FIELD(45,  1)
#define U_BR_SYNC_EXT     USSE_FIELD(44,  1)
#define U_BR_NOSCHED      USSE_FIELD(43,  1)
#define U_BR_BR_MONITOR   USSE_FIELD(42,  1)
#define U_BR_SAVE_LINK    USSE_FIELD(41,  1)
#define U_BR_BR_TYPE      USSE_FIELD(38,  1)
#define U_BR_ANY_INST     USSE_FIELD(21,  1)
#define U_BR_ALL_INST     USSE_FIELD(20,  1)
#define U_BR_BR_OFF       USSE_FIELD( 0, 20)

/* smlsi: set the per-operand repeat increments */
#define U_SMLSI_NOSCHED         USSE_FIELD(50,  1)
#define U_SMLSI_TEMP_LIMIT      USSE_FIELD(44,  4)
#define U_SMLSI_PA_LIMIT        USSE_FIELD(40,  4)
#define U_SMLSI_SA_LIMIT        USSE_FIELD(36,  4)
#define U_SMLSI_DEST_INC_MODE   USSE_FIELD(35,  1)
#define U_SMLSI_SRC0_INC_MODE   USSE_FIELD(34,  1)
#define U_SMLSI_SRC1_INC_MODE   USSE_FIELD(33,  1)
#define U_SMLSI_SRC2_INC_MODE   USSE_FIELD(32,  1)
#define U_SMLSI_DEST_INC        USSE_FIELD(24,  8)
#define U_SMLSI_SRC0_INC        USSE_FIELD(16,  8)
#define U_SMLSI_SRC1_INC        USSE_FIELD( 8,  8)
#define U_SMLSI_SRC2_INC        USSE_FIELD( 0,  8)

/* smbo: set the per-operand base offsets */
#define U_SMBO_NOSCHED       USSE_FIELD(50,  1)
#define U_SMBO_DEST_OFFSET   USSE_FIELD(36, 12)
#define U_SMBO_SRC0_OFFSET   USSE_FIELD(24, 12)
#define U_SMBO_SRC1_OFFSET   USSE_FIELD(12, 12)
#define U_SMBO_SRC2_OFFSET   USSE_FIELD( 0, 12)

/* kill: discard the fragment */
#define U_KILL_PRED   USSE_FIELD(41,  2)

/* limm: load a 32-bit immediate */
#define U_LIMM_SKIPINV                  USSE_FIELD(55,  1)
#define U_LIMM_NOSCHED                  USSE_FIELD(54,  1)
#define U_LIMM_DEST_BANK_EXT            USSE_FIELD(51,  1)
#define U_LIMM_END                      USSE_FIELD(50,  1)
#define U_LIMM_IMM_VALUE_BITS26TO31     USSE_FIELD(44,  6)
#define U_LIMM_PRED                     USSE_FIELD(41,  3)
#define U_LIMM_IMM_VALUE_BITS21TO25     USSE_FIELD(36,  5)
#define U_LIMM_DEST_BANK                USSE_FIELD(32,  2)
#define U_LIMM_DEST_NUM                 USSE_FIELD(21,  7)
#define U_LIMM_IMM_VALUE_FIRST_21BITS   USSE_FIELD( 0, 21)

/* depthf: depth replacement */
#define U_DEPTHF_SYNC            USSE_FIELD(55,  1)
#define U_DEPTHF_SRC0_BANK_EXT   USSE_FIELD(51,  1)
#define U_DEPTHF_END             USSE_FIELD(50,  1)
#define U_DEPTHF_SRC1_BANK_EXT   USSE_FIELD(49,  1)
#define U_DEPTHF_SRC2_BANK_EXT   USSE_FIELD(48,  1)
#define U_DEPTHF_NOSCHED         USSE_FIELD(43,  1)
#define U_DEPTHF_PRED            USSE_FIELD(41,  2)
#define U_DEPTHF_TWO_SIDED       USSE_FIELD(37,  1)
#define U_DEPTHF_FEEDBACK        USSE_FIELD(35,  2)
#define U_DEPTHF_SRC0_BANK       USSE_FIELD(34,  1)
#define U_DEPTHF_SRC1_BANK       USSE_FIELD(30,  2)
#define U_DEPTHF_SRC2_BANK       USSE_FIELD(28,  2)
#define U_DEPTHF_DEST_N          USSE_FIELD(21,  7)
#define U_DEPTHF_SRC0_N          USSE_FIELD(14,  7)
#define U_DEPTHF_SRC1_N          USSE_FIELD( 7,  7)
#define U_DEPTHF_SRC2_N          USSE_FIELD( 0,  7)

/* spec: special */
#define U_SPEC_SPECIAL    USSE_FIELD(54,  1)
#define U_SPEC_CATEGORY   USSE_FIELD(52,  2)

/* vldst: load from and store to memory */
#define U_VLDST_OP1                      USSE_FIELD(59,  2)
#define U_VLDST_PRED                     USSE_FIELD(56,  3)
#define U_VLDST_SKIPINV                  USSE_FIELD(55,  1)
#define U_VLDST_NOSCHED                  USSE_FIELD(54,  1)
#define U_VLDST_MOE_EXPAND               USSE_FIELD(53,  1)
#define U_VLDST_SYNC_START               USSE_FIELD(52,  1)
#define U_VLDST_CACHE_EXT                USSE_FIELD(51,  1)
#define U_VLDST_SRC0_BANK_EXT            USSE_FIELD(50,  1)
#define U_VLDST_SRC1_BANK_EXT            USSE_FIELD(49,  1)
#define U_VLDST_SRC2_BANK_EXT            USSE_FIELD(48,  1)
#define U_VLDST_MASK_COUNT               USSE_FIELD(44,  4)
#define U_VLDST_ADDR_MODE                USSE_FIELD(42,  2)
#define U_VLDST_MODE                     USSE_FIELD(40,  2)
#define U_VLDST_DEST_BANK_PRIMATTR       USSE_FIELD(39,  1)
#define U_VLDST_RANGE_ENABLE             USSE_FIELD(38,  1)
#define U_VLDST_DATA_TYPE                USSE_FIELD(36,  2)
#define U_VLDST_INCREMENT_OR_DECREMENT   USSE_FIELD(35,  1)
#define U_VLDST_SRC0_BANK                USSE_FIELD(34,  1)
#define U_VLDST_CACHE_BY_PASS12          USSE_FIELD(33,  1)
#define U_VLDST_DRC_SEL                  USSE_FIELD(32,  1)
#define U_VLDST_SRC1_BANK                USSE_FIELD(30,  2)
#define U_VLDST_SRC2_BANK                USSE_FIELD(28,  2)
#define U_VLDST_DEST_N                   USSE_FIELD(21,  7)
#define U_VLDST_SRC0_N                   USSE_FIELD(14,  7)
#define U_VLDST_SRC1_N                   USSE_FIELD( 7,  7)
#define U_VLDST_SRC2_N                   USSE_FIELD( 0,  7)

#endif /* VITARECOMP_USSE_FIELDS_H */
