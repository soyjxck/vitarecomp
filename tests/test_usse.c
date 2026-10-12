/* test_usse.c — the shader bytecode interpreter, on instructions built here.
 *
 * Every word is assembled field by field from the layout table, printed and
 * compared with the text it should read as, then run against registers
 * filled by hand. Where a word is given as a number instead, it is one a
 * title's programs have, and what it should do there is worked out by hand
 * the same way.
 */

#include "../src/usse.h"
#include "../src/usse_fields.h"
#include "vitarecomp/half.h"

#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "the tests check with assert(): build them without NDEBUG"
#endif

/* Bank selectors, as the instruction encodes them: a two-bit choice and an
 * extension bit. */
enum { D_TEMP = 0, D_OUT = 1, D_PA = 2 };               /* destination, extension 0 */
enum { D_INDEX = 2 };                                   /* destination, extension 1 */
enum { S_TEMP = 0, S_OUT = 1, S_PA = 2, S_SA = 3 };     /* source 1 and 2, extension 0 */
enum { S_INDEXED1 = 0, S_SPECIAL = 1, S_IMMEDIATE = 2 };/* source 1 and 2, extension 1 */
enum { S0_TEMP = 0, S0_PA = 1 };                        /* source 0, extension 0 */
enum { S0_SA = 1 };                                     /* source 0, extension 1 */

enum { XYZW = 4 };                                      /* the swizzle tables' identity */

/* A word of the given format with fields set: pairs of field and value,
 * ended by -1. */
static uint64_t word(int format, ...) {
    uint64_t w = USSE_FORMATS[format].value;
    va_list ap;
    va_start(ap, format);
    for (;;) {
        int field = va_arg(ap, int);
        if (field < 0) break;
        uint64_t v = (uint64_t)va_arg(ap, int) & ((1ull << (field >> 8)) - 1);
        w |= v << (field & 0xFF);
    }
    va_end(ap);
    return w;
}

static usse_state st;
static uint32_t sa[USSE_WORDS];

static void fresh(void) {
    memset(&st, 0, sizeof(st));
    memset(sa, 0, sizeof(sa));
    st.sa = sa;
    usse_reset(&st);
}

static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t halves(float lo, float hi) { return (uint32_t)vita_f32_to_f16(lo) | ((uint32_t)vita_f32_to_f16(hi) << 16); }
static float half_lo(uint32_t w) { return vita_f16_to_f32((uint16_t)w); }
static float half_hi(uint32_t w) { return vita_f16_to_f32((uint16_t)(w >> 16)); }
static int near(float a, float b) { return fabsf(a - b) < 1e-5f; }

/* Decode a word, check how it prints, and return it ready to run. The text
 * is compared without the hexadecimal word each line begins with. */
static usse_insn insn(uint64_t w, int secondary, const char *text) {
    usse_insn in;
    usse_state scratch;
    char line[1024], bare[1024];
    size_t n = 0;
    assert(usse_decode(w, secondary, &in));
    assert(usse_can_run(&in));
    memset(&scratch, 0, sizeof(scratch));
    usse_reset(&scratch);
    usse_disassemble(&in, &scratch, line, sizeof(line));
    for (const char *p = line; *p; ) {
        const char *end = strchr(p, '\n');
        assert(end && end - p >= 18 && p[16] == ':');
        memcpy(bare + n, p + 18, (size_t)(end - p) - 18 + 1);
        n += (size_t)(end - p) - 18 + 1;
        p = end + 1;
    }
    bare[n] = '\0';
    if (strcmp(bare, text)) {
        fprintf(stderr, "prints as:\n%swanted:\n%s", bare, text);
        assert(0);
    }
    return in;
}

static void run1(const usse_insn *in) { assert(usse_run(in, 1, &st) == 0); }

/* An instruction decoded, with no opinion on how it prints. */
static usse_insn decoded(uint64_t w) {
    usse_insn in;
    assert(usse_decode(w, 0, &in));
    assert(usse_can_run(&in));
    return in;
}

/* --- moves ------------------------------------------------------------------------------ */

static void test_moves(void) {
    /* two floats at a time, twice: one 64-bit register and then the next */
    usse_insn mov = insn(word(USSE_F_VMOV, U_VMOV_MOVE_DATA_TYPE, 5, U_VMOV_REPEAT_COUNT, 1, U_VMOV_SRC0_SWIZ, XYZW,
                              U_VMOV_DEST_BANK_SEL, D_OUT, U_VMOV_SRC1_BANK_SEL, S_PA, U_VMOV_DEST_MASK, 3, -1),
                         0, "VMOV.f32 o0.xy pa0.xy \nVMOV.f32 o2.xy pa2.xy \n");
    fresh();
    for (int i = 0; i < 4; i++) st.pa[i] = fbits(1.0f + (float)i);
    run1(&mov);
    for (int i = 0; i < 4; i++) assert(st.out[i] == fbits(1.0f + (float)i));

    /* in a secondary program every register is a secondary attribute */
    usse_insn sec = insn(word(USSE_F_VMOV, U_VMOV_MOVE_DATA_TYPE, 5, U_VMOV_SRC0_SWIZ, XYZW, U_VMOV_DEST_BANK_SEL, D_PA,
                              U_VMOV_SRC1_BANK_SEL, S_PA, U_VMOV_SRC1_N, 1, U_VMOV_DEST_MASK, 3, -1),
                         1, "VMOV.f32 sa0.xy sa2.xy \n");
    fresh();
    sa[2] = 7; sa[3] = 9;
    run1(&sec);
    assert(sa[0] == 7 && sa[1] == 9);

    /* constants: c1 is (0, 1) as floats, c32 is (0, 0, 0, 1) as halves */
    usse_insn c1 = insn(word(USSE_F_VMOV, U_VMOV_MOVE_DATA_TYPE, 5, U_VMOV_SRC0_SWIZ, XYZW, U_VMOV_DEST_BANK_SEL, D_OUT,
                             U_VMOV_SRC1_BANK_SEL, S_SPECIAL, U_VMOV_SRC1_BANK_EXT, 1, U_VMOV_SRC1_N, 1,
                             U_VMOV_DEST_MASK, 3, -1),
                        0, "VMOV.f32 o0.xy c1.xy \n");
    usse_insn c32 = insn(word(USSE_F_VMOV, U_VMOV_MOVE_DATA_TYPE, 4, U_VMOV_SRC0_SWIZ, XYZW, U_VMOV_DEST_BANK_SEL, D_TEMP,
                              U_VMOV_DEST_N, 60, U_VMOV_SRC1_BANK_SEL, S_SPECIAL, U_VMOV_SRC1_BANK_EXT, 1,
                              U_VMOV_SRC1_N, 32, U_VMOV_DEST_MASK, 15, -1),
                         0, "VMOV.f16 i0.xyzw c32.xyzw \n");
    fresh();
    run1(&c1);
    run1(&c32);
    assert(st.out[0] == fbits(0.0f) && st.out[1] == fbits(1.0f));
    assert(st.internal[0] == fbits(0.0f) && st.internal[2] == fbits(0.0f) && st.internal[3] == fbits(1.0f));

    /* a choice, component by component: where the tested value is negative
     * the first source, elsewhere the second. Bit 50 is not a bank bit. */
    uint64_t w = word(USSE_F_VMOV, U_VMOV_MOVE_TYPE, 1, U_VMOV_MOVE_DATA_TYPE, 4, U_VMOV_TEST_BIT_2, 1,
                      U_VMOV_DEST_BANK_SEL, D_PA, U_VMOV_DEST_N, 1, U_VMOV_DEST_MASK, 1,
                      U_VMOV_SRC0_BANK_SEL, S0_PA, U_VMOV_SRC0_N, 2,
                      U_VMOV_SRC1_BANK_SEL, S_SPECIAL, U_VMOV_SRC1_BANK_EXT, 1, U_VMOV_SRC0_SWIZ, 0,
                      U_VMOV_SRC2_BANK_SEL, S_SA, U_VMOV_SRC2_N, 3, -1);
    for (int end = 0; end < 2; end++) {
        usse_insn movc = insn(w | ((uint64_t)end << 50), 0, "VMOVC.f16 pa2.xy (pa4.xy < vec(0)) ? c0.xx : sa6.xx\n");
        fresh();
        st.pa[4] = halves(-1.0f, 2.0f);
        sa[6] = halves(5.0f, 0.0f);
        st.pa[2] = 0xFFFFFFFFu;
        run1(&movc);
        assert(half_lo(st.pa[2]) == 0.0f && half_hi(st.pa[2]) == 5.0f);
    }
    printf("  moves and choices              ok\n");
}

/* --- arithmetic ------------------------------------------------------------------------- */

static void test_arithmetic(void) {
    /* the two-operand group: the first source's swizzle is three bits a
     * component, spread over four fields */
    uint32_t swz = 0u | 1u << 3 | 2u << 6 | 3u << 9;
    #define NMAD(op, dest_sel, dest_n, mask)                                                                    \
        word(USSE_F_V32NMAD, U_V32NMAD_OP2, (op), U_V32NMAD_DEST_BANK_SEL, (dest_sel), U_V32NMAD_DEST_N, (dest_n), \
             U_V32NMAD_DEST_MASK, (mask), U_V32NMAD_SRC1_BANK_SEL, S_PA, U_V32NMAD_SRC2_BANK_SEL, S_SA,          \
             U_V32NMAD_SRC2_SWIZ, XYZW, U_V32NMAD_SRC1_SWIZ_0_6, (int)(swz & 0x7F),                             \
             U_V32NMAD_SRC1_SWIZ_7_8, (int)((swz >> 7) & 3), U_V32NMAD_SRC1_SWIZ_9, (int)((swz >> 9) & 1),      \
             U_V32NMAD_SRC1_SWIZ_10_11, (int)((swz >> 10) & 3), -1)
    usse_insn add = insn(NMAD(1, D_OUT, 0, 15), 0, "VADD o0.xyzw pa0.xyzw sa0.xyzw\n");
    usse_insn mul = insn(NMAD(0, D_OUT, 2, 3), 0, "VMUL o4.xy pa0.xy sa0.xy\n");
    usse_insn dot = insn(NMAD(7, D_TEMP, 61, 2), 0, "VDP i1.-y pa0.xyzw sa0.xyzw\n");
    usse_insn mx  = insn(NMAD(6, D_TEMP, 0, 1), 0, "VMAX r0.x pa0.x sa0.x\n");
    #undef NMAD
    fresh();
    for (int i = 0; i < 4; i++) { st.pa[i] = fbits(1.0f + (float)i); sa[i] = fbits(10.0f * (float)(i + 1)); }
    run1(&add); run1(&mul); run1(&dot); run1(&mx);
    assert(bitsf(st.out[0]) == 11.0f && bitsf(st.out[3]) == 44.0f);
    assert(bitsf(st.out[4]) == 10.0f && bitsf(st.out[5]) == 40.0f);
    assert(bitsf(st.internal[4 + 1]) == 10.0f + 40.0f + 90.0f + 160.0f);
    assert(bitsf(st.temp[0]) == 10.0f);

    /* multiply-add through two internal registers: i0 * pa0 + i1 */
    usse_insn mad = insn(word(USSE_F_VMAD, U_VMAD_OPCODE2, 1, U_VMAD_WRITE_MASK, 15, U_VMAD_DEST_BANK, D_OUT,
                              U_VMAD_SRC1_BANK, S_PA, U_VMAD_SRC1_SWIZ, XYZW, U_VMAD_GPI0_N, 0, U_VMAD_GPI0_SWIZ, XYZW,
                              U_VMAD_GPI1_N, 1, U_VMAD_GPI1_SWIZ, XYZW, -1),
                         0, "VMAD o0.xyzw i0.xyzw pa0.xyzw i1.xyzw\n");
    fresh();
    for (int i = 0; i < 4; i++) {
        st.pa[i] = fbits(2.0f);
        st.internal[i] = fbits((float)i);
        st.internal[4 + i] = fbits(0.5f);
    }
    run1(&mad);
    for (int i = 0; i < 4; i++) assert(bitsf(st.out[i]) == 2.0f * (float)i + 0.5f);

    /* a matrix times a vector: the dot product with an internal register,
     * four times, a row and an output component further each time */
    usse_insn rows = insn(word(USSE_F_VDP, U_VDP_OPCODE2, 1, U_VDP_WRITE_MASK, 1, U_VDP_REPEAT_COUNT, 3, U_VDP_DEST_BANK, D_OUT,
                               U_VDP_SRC1_BANK, S_SA, U_VDP_SRC1_SWIZ_X, 0, U_VDP_SRC1_SWIZ_Y, 1, U_VDP_SRC1_SWIZ_Z, 2,
                               U_VDP_SRC1_SWIZ_W, 3, U_VDP_GPI0_N, 0, U_VDP_GPI0_SWIZ, XYZW, -1),
                          0, "VDP o0.x sa0.xyzw i0.xyzw\nVDP o0.-y sa4.xyzw i0.xyzw\n"
                             "VDP o0.--z sa8.xyzw i0.xyzw\nVDP o0.---w sa12.xyzw i0.xyzw\n");
    fresh();
    static const float M[16] = { 2, 0, 0, 5,   0, 3, 0, 6,   0, 0, 4, 7,   0, 0, 0, 1 };
    for (int i = 0; i < 16; i++) sa[i] = fbits(M[i]);
    st.internal[0] = fbits(1.0f); st.internal[1] = fbits(1.0f); st.internal[2] = fbits(1.0f); st.internal[3] = fbits(1.0f);
    run1(&rows);
    assert(bitsf(st.out[0]) == 7.0f && bitsf(st.out[1]) == 9.0f && bitsf(st.out[2]) == 11.0f && bitsf(st.out[3]) == 1.0f);

    /* a reciprocal of one component, written to another */
    usse_insn rcp = insn(word(USSE_F_VCOMP, U_VCOMP_OP2, 0, U_VCOMP_DEST_BANK, D_TEMP, U_VCOMP_DEST_N, 124,
                              U_VCOMP_SRC1_BANK, S_PA, U_VCOMP_SRC_COMP, 1, U_VCOMP_WRITE_MASK, 4, -1),
                         0, "VRCP i0.--z pa0.-y\n");
    fresh();
    st.pa[1] = fbits(4.0f);
    run1(&rcp);
    assert(bitsf(st.internal[2]) == 0.25f);
    printf("  arithmetic                     ok\n");
}

/* --- packing ---------------------------------------------------------------------------- */

static void test_packing(void) {
    /* four floats out of two 64-bit registers, into four halves */
    usse_insn down = insn(word(USSE_F_VPCK, U_VPCK_DEST_FMT, 5, U_VPCK_SRC_FMT, 6, U_VPCK_DEST_MASK, 15,
                               U_VPCK_DEST_BANK_SEL, D_PA, U_VPCK_SRC1_BANK_SEL, S_SA, U_VPCK_SRC1_N, 4,
                               U_VPCK_SRC2_BANK_SEL, S_SA, U_VPCK_SRC2_N, 5,
                               U_VPCK_COMP_SEL_1, 1, U_VPCK_COMP_SEL_2, 2, U_VPCK_COMP_SEL_3, 3, -1),
                          0, "VPCKF16F32 pa0.xyzw (sa8.xyzw sa10.xyzw) [noscale]\n");
    fresh();
    sa[8] = fbits(1.5f); sa[9] = fbits(-2.0f); sa[10] = fbits(0.25f); sa[11] = fbits(100.0f);
    run1(&down);
    assert(half_lo(st.pa[0]) == 1.5f && half_hi(st.pa[0]) == -2.0f && half_lo(st.pa[1]) == 0.25f && half_hi(st.pa[1]) == 100.0f);

    /* halves to bytes, scaled: 0..1 becomes 0..255 and what lies outside is clamped */
    usse_insn bytes = insn(word(USSE_F_VPCK, U_VPCK_DEST_FMT, 0, U_VPCK_SRC_FMT, 5, U_VPCK_DEST_MASK, 15, U_VPCK_SCALE, 1,
                                U_VPCK_DEST_BANK_SEL, D_OUT, U_VPCK_SRC1_BANK_SEL, S_PA,
                                U_VPCK_SRC2_BANK_SEL, S_IMMEDIATE, U_VPCK_SRC2_BANK_EXT, 1,
                                U_VPCK_COMP_SEL_1, 1, U_VPCK_COMP_SEL_2, 2, U_VPCK_COMP_SEL_3, 3, -1),
                           0, "VPCKU8F16 o0.xyzw pa0.xyzw [scale]\n");
    fresh();
    st.pa[0] = halves(0.0f, 0.5f);
    st.pa[1] = halves(1.0f, 2.0f);
    run1(&bytes);
    assert(st.out[0] == 0xFFFF8000u);

    /* bytes to floats, scaled, into an internal register */
    usse_insn up = insn(word(USSE_F_VPCK, U_VPCK_DEST_FMT, 6, U_VPCK_SRC_FMT, 0, U_VPCK_DEST_MASK, 15, U_VPCK_SCALE, 1,
                             U_VPCK_DEST_BANK_SEL, D_TEMP, U_VPCK_DEST_N, 124, U_VPCK_SRC1_BANK_SEL, S_PA, U_VPCK_SRC1_N, 1,
                             U_VPCK_SRC2_BANK_SEL, S_IMMEDIATE, U_VPCK_SRC2_BANK_EXT, 1,
                             U_VPCK_COMP_SEL_1, 1, U_VPCK_COMP_SEL_2, 2, U_VPCK_COMP_SEL_3, 3, -1),
                        0, "VPCKF32U8 i0.xyzw pa2.xyzw [scale]\n");
    fresh();
    st.pa[2] = 0xFF804000u;
    run1(&up);
    assert(bitsf(st.internal[0]) == 0.0f && near(bitsf(st.internal[1]), 64.0f / 255.0f) &&
           near(bitsf(st.internal[2]), 128.0f / 255.0f) && bitsf(st.internal[3]) == 1.0f);

    /* halves to signed bytes: the selectors are taken in order by the
     * components written, here z and w into the upper two bytes */
    usse_insn snorm = insn(word(USSE_F_VPCK, U_VPCK_DEST_FMT, 1, U_VPCK_SRC_FMT, 5, U_VPCK_DEST_MASK, 12, U_VPCK_SCALE, 1,
                                U_VPCK_DEST_BANK_SEL, D_PA, U_VPCK_DEST_N, 1, U_VPCK_SRC1_BANK_SEL, S_PA, U_VPCK_SRC1_N, 2,
                                U_VPCK_SRC2_BANK_SEL, S_IMMEDIATE, U_VPCK_SRC2_BANK_EXT, 1, U_VPCK_SRC2_N, 1,
                                U_VPCK_COMP_SEL_1, 3, -1),
                           0, "VPCKS8F16 pa1.--zw pa4.--zw [scale]\n");
    fresh();
    st.pa[1] = 0x12345678u;
    st.pa[5] = halves(0.5f, -1.0f);
    run1(&snorm);
    assert(st.pa[1] == 0x81405678u);

    /* a float to a 16-bit integer, unscaled: truncated, and its neighbour kept */
    usse_insn whole = insn(word(USSE_F_VPCK, U_VPCK_DEST_FMT, 4, U_VPCK_SRC_FMT, 6, U_VPCK_DEST_MASK, 1,
                                U_VPCK_DEST_BANK_SEL, D_PA, U_VPCK_SRC1_BANK_SEL, S_PA,
                                U_VPCK_SRC2_BANK_SEL, S_IMMEDIATE, U_VPCK_SRC2_BANK_EXT, 1, -1),
                           0, "VPCKS16F32 pa0.x pa0.x [noscale]\n");
    fresh();
    st.pa[0] = fbits(7.9f);
    run1(&whole);
    assert((st.pa[0] & 0xFFFF) == 7 && (st.pa[0] >> 16) == (fbits(7.9f) >> 16));
    printf("  packing and unpacking          ok\n");
}

/* --- bits and integers -------------------------------------------------------------------- */

static void test_integers(void) {
    /* OR with nothing: how the compiler copies a register whole */
    usse_insn copy = insn(word(USSE_F_VBW, U_VBW_OP1, 2, U_VBW_OP2, 1, U_VBW_DEST_BANK, D_PA, U_VBW_DEST_N, 2,
                               U_VBW_SRC1_BANK, S_SA, U_VBW_SRC1_N, 4, U_VBW_SRC2_BANK, S_IMMEDIATE, U_VBW_SRC2_EXT, 1, -1),
                          0, "OR pa2.x sa4.x 0x0\n");
    /* a shift of the low half only */
    usse_insn shl = insn(word(USSE_F_VBW, U_VBW_OP1, 4, U_VBW_BITWISE_PARTIAL, 1, U_VBW_DEST_BANK, D_PA,
                              U_VBW_SRC1_BANK, S_PA, U_VBW_SRC2_BANK, S_IMMEDIATE, U_VBW_SRC2_EXT, 1, U_VBW_SRC2_N, 3, -1),
                         0, "SHL16 pa0.x pa0.x 0x3\n");
    fresh();
    sa[4] = 0xDEADBEEFu;
    st.pa[0] = 0xABCD1234u;
    run1(&copy); run1(&shl);
    assert(st.pa[2] == 0xDEADBEEFu && st.pa[0] == 0xABCD91A0u);

    /* an index register from a multiply-add, then a register found through it */
    usse_insn mad = insn(word(USSE_F_I16MAD, U_I16MAD_MODE, 2, U_I16MAD_SRC1_FORMAT, 1, U_I16MAD_SRC2_FORMAT, 1,
                              U_I16MAD_DEST_BANK, D_INDEX, U_I16MAD_DEST_BANK_EXT, 1, U_I16MAD_DEST_N, 1,
                              U_I16MAD_SRC0_BANK, S0_PA, U_I16MAD_SRC1_BANK, S_IMMEDIATE, U_I16MAD_SRC1_BANK_EXT, 1,
                              U_I16MAD_SRC1_N, 1, U_I16MAD_SRC2_BANK, S_IMMEDIATE, U_I16MAD_SRC2_BANK_EXT, 1,
                              U_I16MAD_SRC2_N, 6, -1),
                         0, "IMAD16 idx1.x pa0.x 1-8bits 6-8bits\n");
    usse_insn via = insn(word(USSE_F_VMOV, U_VMOV_MOVE_DATA_TYPE, 5, U_VMOV_SRC0_SWIZ, XYZW, U_VMOV_DEST_BANK_SEL, D_TEMP,
                              U_VMOV_SRC1_BANK_SEL, S_INDEXED1, U_VMOV_SRC1_BANK_EXT, 1, U_VMOV_SRC1_N, 0x31,
                              U_VMOV_DEST_MASK, 3, -1),
                         0, "VMOV.f32 r0.xy sa[idx1 * 2 + 2].xy \n");
    fresh();
    st.pa[0] = 0xFFFF0005u;
    sa[24] = fbits(3.0f); sa[25] = fbits(4.0f);
    run1(&mad); run1(&via);
    assert(st.index[0] == 11);
    assert(bitsf(st.temp[0]) == 3.0f && bitsf(st.temp[1]) == 4.0f);

    /* a bone's number into the address of its matrix: pa7.x = pa4.x (as a
     * short) * 48 + sa37.x (a whole word), the instruction as a title's
     * skinning programs have it, and with the upper half of pa4 for the number */
    usse_insn bone = decoded(0xA8820886B0811825ull);
    assert(bone.op == USSE_OP_IMAD32 && bone.dest.bank == USSE_PA && bone.src0.bank == USSE_PA);
    fresh();
    st.pa[bone.src0.num] = 0x00070003u;
    {
        uint32_t before = 0x40000000u;
        int32_t times = bone.src1.bank == USSE_IMMEDIATE ? (int32_t)bone.src1.num : 0;
        if (bone.src2.bank == USSE_SA) sa[bone.src2.num] = before; else if (bone.src2.bank == USSE_PA) st.pa[bone.src2.num] = before;
        assert(times != 0);
        run1(&bone);
        assert(st.pa[bone.dest.num] == before + 3u * (uint32_t)times);
    }
    usse_insn upper = decoded(word(USSE_F_I32MAD, U_I32MAD_IS_SIGNED, 1, U_I32MAD_SRC0_HIGH, 1, U_I32MAD_SRC2_TYPE, 2,
                                U_I32MAD_DEST_BANK, D_PA, U_I32MAD_DEST_N, 9, U_I32MAD_SRC0_BANK, S0_PA, U_I32MAD_SRC0_N, 4,
                                U_I32MAD_SRC1_BANK, S_IMMEDIATE, U_I32MAD_SRC1_BANK_EXT, 1, U_I32MAD_SRC1_N, 48,
                                U_I32MAD_SRC2_BANK, S_PA, U_I32MAD_SRC2_N, 6, -1));
    fresh();
    st.pa[4] = 0xFFFE0003u;                     /* the upper half is -2 */
    st.pa[6] = 1000u;
    run1(&upper);
    assert(st.pa[9] == 1000u - 96u);

    /* a 32-bit literal */
    usse_insn imm = insn(word(USSE_F_LIMM, U_LIMM_DEST_BANK, D_PA, U_LIMM_DEST_NUM, 3, U_LIMM_IMM_VALUE_FIRST_21BITS, 0x12345,
                              U_LIMM_IMM_VALUE_BITS21TO25, 0x1F, U_LIMM_IMM_VALUE_BITS26TO31, 0x20, -1),
                         0, "MOV pa3.x #0x83E12345\n");
    run1(&imm);
    assert(st.pa[3] == 0x83E12345u);
    printf("  bits, integers and indices     ok\n");
}

/* --- tests, discards and branches --------------------------------------------------------- */

static void test_flow(void) {
    /* p1 = pa2.x >= sa6.x, in halves; then the fragment lives only if p1 */
    usse_insn cmp = insn(word(USSE_F_VTST, U_VTST_ALU_SEL, 0, U_VTST_ALU_OP, 14, U_VTST_SIGN_TEST, 2, U_VTST_ZERO_TEST, 1,
                              U_VTST_PDST_N, 1, U_VTST_SRC1_BANK, S_PA, U_VTST_SRC1_N, 1, U_VTST_SRC2_BANK, S_SA,
                              U_VTST_SRC2_N, 3, -1),
                         0, "CMP.ge.f16 p1 pa2.x sa6.x\n");
    usse_insn kill = insn(word(USSE_F_KILL, U_KILL_PRED, 2, -1), 0, "KILL p1 \n");
    usse_insn code[4];
    code[0] = cmp; code[1] = kill;
    for (int pass = 0; pass < 2; pass++) {
        fresh();
        st.pa[2] = halves(pass ? 0.75f : 0.25f, 0.0f);
        sa[6] = halves(0.5f, 0.0f);
        assert(usse_run(code, 2, &st) == 0);
        assert(st.pred[1] == pass && st.killed == !pass);
    }

    /* an addition compared with zero, the first operand negated, on the
     * third component: p0 = -pa4.z + pa2.x > 0 */
    usse_insn add = insn(word(USSE_F_VTST, U_VTST_ALU_SEL, 0, U_VTST_ALU_OP, 2, U_VTST_PREC, 1, U_VTST_SIGN_TEST, 2,
                              U_VTST_ZERO_TEST, 2, U_VTST_TEST_CRCOMB_AND, 1, U_VTST_CHAN_CC, 2, U_VTST_SRC1_NEG, 1,
                              U_VTST_SRC2_VSCOMP, 1, U_VTST_SRC1_BANK, S_PA, U_VTST_SRC1_N, 2, U_VTST_SRC2_BANK, S_PA,
                              U_VTST_SRC2_N, 1, -1),
                         0, "VADD.gtzero.f32 p0 -pa4.--z pa2.--x\n");
    fresh();
    st.pa[6] = fbits(1.0f); st.pa[2] = fbits(3.0f);
    run1(&add);
    assert(st.pred[0] == 1);
    st.pa[2] = fbits(1.0f);
    run1(&add);
    assert(st.pred[0] == 0);

    /* the mask form: one and zero per component instead of a predicate */
    usse_insn msk = insn(word(USSE_F_VTSTMSK, U_VTSTMSK_ALU_SEL, 0, U_VTSTMSK_ALU_OP, 14, U_VTSTMSK_PREC, 1,
                              U_VTSTMSK_TST_MASK_TYPE, 2, U_VTSTMSK_TEST_WBEN, 1, U_VTSTMSK_SIGN_TEST, 2,
                              U_VTSTMSK_ZERO_TEST, 2, U_VTSTMSK_TEST_CRCOMB_AND, 1, U_VTSTMSK_DEST_BANK, D_TEMP,
                              U_VTSTMSK_DEST_N, 124, U_VTSTMSK_SRC1_BANK, S_TEMP, U_VTSTMSK_SRC2_BANK, S_TEMP,
                              U_VTSTMSK_SRC2_N, 124, -1),
                         0, "CMPMSK.gt.f32.f32 i0.xyzw r0.xyzw i0.xyzw\n");
    fresh();
    for (int i = 0; i < 4; i++) { st.temp[i] = fbits(1.0f + (float)i); st.internal[i] = fbits(2.0f); }
    run1(&msk);
    assert(bitsf(st.internal[0]) == 0.0f && bitsf(st.internal[1]) == 0.0f &&
           bitsf(st.internal[2]) == 1.0f && bitsf(st.internal[3]) == 1.0f);

    /* a branch over one instruction when p0 is clear; taken, the literal is
     * never loaded, and not taken it is */
    usse_insn skip = insn(word(USSE_F_BR, U_BR_PRED, 5, U_BR_BR_TYPE, 1, U_BR_BR_OFF, 2, -1), 0, "!p0 BR +2\n");
    usse_insn one = insn(word(USSE_F_LIMM, U_LIMM_DEST_BANK, D_PA, U_LIMM_DEST_NUM, 9, U_LIMM_IMM_VALUE_FIRST_21BITS, 1, -1),
                         0, "MOV pa9.x #0x1\n");
    usse_insn two = insn(word(USSE_F_LIMM, U_LIMM_DEST_BANK, D_PA, U_LIMM_DEST_NUM, 10, U_LIMM_IMM_VALUE_FIRST_21BITS, 2, -1),
                         0, "MOV pa10.x #0x2\n");
    code[0] = skip; code[1] = one; code[2] = two;
    fresh();
    assert(usse_run(code, 3, &st) == 0);
    assert(st.pa[9] == 0 && st.pa[10] == 2);
    fresh();
    st.pred[0] = 1;
    assert(usse_run(code, 3, &st) == 0);
    assert(st.pa[9] == 1 && st.pa[10] == 2);

    /* a relative branch is signed; one that never stops being taken is cut off */
    usse_insn back = insn(word(USSE_F_BR, U_BR_BR_TYPE, 1, U_BR_BR_OFF, 0xFFFFF, -1), 0, "BR -1\n");
    code[0] = one; code[1] = back;
    fresh();
    assert(usse_run(code, 2, &st) != 0);
    printf("  tests, discards and branches   ok\n");
}

/* --- memory and textures ------------------------------------------------------------------ */

static uint32_t memory_words[8] = { 0, 0, 0, 0, 11, 22, 33, 44 };
static uint32_t read_memory(void *user, uint32_t address) {
    (void)user;
    assert(address >= 0x1000 && address < 0x1020 && address % 4 == 0);
    return memory_words[(address - 0x1000) / 4];
}

static uint32_t sampled_register;
static float sampled_at[3];
static int sample_texture(void *user, uint32_t reg, const float *coord, int dimensions, int lod_mode, float lod, float rgba[4]) {
    (void)user; (void)lod;
    assert(dimensions == 2 && lod_mode == 0);
    sampled_register = reg;
    sampled_at[0] = coord[0]; sampled_at[1] = coord[1];
    rgba[0] = 0.25f; rgba[1] = 0.5f; rgba[2] = 0.75f; rgba[3] = 1.0f;
    return 4;
}
static void gather_texture(void *user, uint32_t reg, const float *coord, float texels[4], float uv[2]) {
    (void)user; (void)coord;
    sampled_register = reg;
    texels[0] = 0.1f; texels[1] = 0.2f; texels[2] = 0.3f; texels[3] = 0.4f;
    uv[0] = 0.25f; uv[1] = 0.5f;
}

static void test_outside(void) {
    static const usse_env env = { NULL, read_memory, sample_texture, gather_texture };

    /* three words from the buffer whose address, less four, is in sa42 */
    usse_insn ldr = insn(word(USSE_F_VLDST, U_VLDST_OP1, 1, U_VLDST_MASK_COUNT, 2, U_VLDST_DEST_BANK_PRIMATTR, 1,
                              U_VLDST_SRC0_BANK, S0_SA, U_VLDST_SRC0_BANK_EXT, 1, U_VLDST_SRC0_N, 42,
                              U_VLDST_SRC1_BANK, S_IMMEDIATE, U_VLDST_SRC1_BANK_EXT, 1, U_VLDST_SRC1_N, 4,
                              U_VLDST_SRC2_BANK, S_IMMEDIATE, U_VLDST_SRC2_BANK_EXT, 1, -1),
                         0, "LDR pa0.? (sa42.x + 16 + 0) [12 bytes]\n");
    fresh();
    st.env = &env;
    sa[42] = 0x1000 - 4;
    st.pa[3] = 99;
    run1(&ldr);
    assert(st.pa[0] == 11 && st.pa[1] == 22 && st.pa[2] == 33 && st.pa[3] == 99);

    /* a sample at a coordinate held as halves, the texel stored as halves */
    usse_insn smp = insn(word(USSE_F_SMP, U_SMP_FCONV_TYPE, 2, U_SMP_DIM, 1, U_SMP_DEST_N, 2, U_SMP_SRC0_TYPE, 1,
                              U_SMP_SRC0_BANK, S0_PA, U_SMP_SRC0_N, 1, U_SMP_SRC1_BANK, S_SA, U_SMP_SRC1_N, 40, -1),
                         0, "SMP2d.f16.f16 r2.x pa2.xy sa80 \n");
    fresh();
    st.env = &env;
    st.pa[2] = halves(0.125f, 0.875f);
    run1(&smp);
    assert(sampled_register == 80 && sampled_at[0] == 0.125f && sampled_at[1] == 0.875f);
    assert(half_lo(st.temp[2]) == 0.25f && half_hi(st.temp[2]) == 0.5f && half_lo(st.temp[3]) == 0.75f && half_hi(st.temp[3]) == 1.0f);

    /* the four texels of a bilinear footprint as floats, and after them the
     * weight each would get, as halves */
    usse_insn gat = insn(word(USSE_F_SMP, U_SMP_FCONV_TYPE, 3, U_SMP_DIM, 1, U_SMP_SB_MODE, 3, U_SMP_SRC0_BANK, S0_PA,
                              U_SMP_SRC0_N, 1, U_SMP_SRC1_BANK, S_SA, U_SMP_SRC1_N, 40, -1),
                         0, "SMP2d.f32.f32.gather4.uv r0.x pa2.xy sa80 \n");
    fresh();
    st.env = &env;
    run1(&gat);
    assert(near(bitsf(st.temp[0]), 0.1f) && near(bitsf(st.temp[3]), 0.4f));
    assert(half_lo(st.temp[4]) == 0.375f && half_hi(st.temp[4]) == 0.125f);
    assert(half_lo(st.temp[5]) == 0.125f && half_hi(st.temp[5]) == 0.375f);

    /* without the means, such an instruction stops the run and is named */
    fresh();
    assert(usse_run(&smp, 1, &st) == 1);
    printf("  memory and textures            ok\n");
}

/* --- repeats ------------------------------------------------------------------------------ */

static void test_repeats(void) {
    /* the destination moves on by one register pair a time and the source back */
    usse_insn code[2];
    code[0] = insn(word(USSE_F_SMLSI, U_SMLSI_DEST_INC, 1, U_SMLSI_SRC1_INC, 0xFF, -1), 0,
                   "SMLSI  inc.1  inc.0  inc.-1  inc.0 \n");
    usse_decode(word(USSE_F_VMOV, U_VMOV_MOVE_DATA_TYPE, 5, U_VMOV_REPEAT_COUNT, 1, U_VMOV_SRC0_SWIZ, XYZW,
                     U_VMOV_DEST_BANK_SEL, D_OUT, U_VMOV_DEST_N, 1, U_VMOV_SRC1_BANK_SEL, S_PA, U_VMOV_SRC1_N, 2,
                     U_VMOV_DEST_MASK, 3, -1), 0, &code[1]);
    fresh();
    for (int i = 0; i < 8; i++) st.pa[i] = (uint32_t)(100 + i);
    assert(usse_run(code, 2, &st) == 0);
    assert(st.out[2] == 104 && st.out[3] == 105 && st.out[4] == 102 && st.out[5] == 103);

    /* and the listing follows the same increments */
    char text[512];
    usse_state scratch;
    memset(&scratch, 0, sizeof(scratch));
    usse_reset(&scratch);
    usse_disassemble(&code[0], &scratch, text, sizeof(text));
    usse_disassemble(&code[1], &scratch, text, sizeof(text));
    assert(strstr(text, "VMOV.f32 o2.xy pa4.xy") && strstr(text, "VMOV.f32 o4.xy pa2.xy"));

    /* each program starts from the default again */
    usse_reset(&st);
    memset(st.out, 0, sizeof(st.out));
    assert(usse_run(&code[1], 1, &st) == 0);
    assert(st.out[2] == 104 && st.out[4] == 106);
    printf("  repeats and their increments   ok\n");
}

/* --- two operations in one instruction ------------------------------------------------------ */

static void test_dual(void) {
    /* a reciprocal square root into one internal register while two floats
     * of another are moved out to a temporary */
    usse_insn rsq = insn(0x2000610000b408fdull, 0, "FRSQ i0.x i1.x + VMOV r10.xy i2.xy\n");
    fresh();
    st.internal[1] = fbits(77.0f);
    st.internal[4] = fbits(4.0f);
    st.internal[8] = fbits(3.0f); st.internal[9] = fbits(5.0f); st.internal[10] = fbits(6.0f);
    run1(&rsq);
    assert(bitsf(st.internal[0]) == 0.5f && bitsf(st.internal[1]) == 77.0f);
    assert(bitsf(st.temp[10]) == 3.0f && bitsf(st.temp[11]) == 5.0f && st.temp[12] == 0);

    /* Both read before either writes: the reciprocal is of what i0 held,
     * not of what the move beside it puts there. The move's destination
     * is written as a temporary past the last and is an internal
     * register, three components of it. */
    usse_insn rcp = insn(0x20047000df950088ull, 0, "FRCP i1.x i0.x + VMOV i0.xyz sa16.xyz\n");
    fresh();
    st.internal[0] = fbits(4.0f); st.internal[3] = fbits(1.5f);
    sa[16] = fbits(7.0f); sa[17] = fbits(8.0f); sa[18] = fbits(9.0f); sa[19] = fbits(10.0f);
    run1(&rcp);
    assert(bitsf(st.internal[4]) == 0.25f);
    assert(bitsf(st.internal[0]) == 7.0f && bitsf(st.internal[1]) == 8.0f && bitsf(st.internal[2]) == 9.0f && bitsf(st.internal[3]) == 1.5f);

    /* a difference of vectors, the operand of any bank negated, beside a
     * sum of squares that leaves one number in a primary attribute */
    usse_insn ssq = insn(0x2084c112e028079aull, 0, "VADD i2.xyz -sa52.xyz i0.xyz + VSSQ pa1.x i1.xyz\n");
    fresh();
    for (int i = 0; i < 3; i++) {
        sa[52 + i] = fbits(1.0f + (float)i);
        st.internal[i] = fbits(10.0f * (float)(i + 1));
    }
    st.internal[4] = fbits(1.0f); st.internal[5] = fbits(2.0f); st.internal[6] = fbits(2.0f); st.internal[7] = fbits(100.0f);
    run1(&ssq);
    assert(bitsf(st.internal[8]) == 9.0f && bitsf(st.internal[9]) == 18.0f && bitsf(st.internal[10]) == 27.0f && st.internal[11] == 0);
    assert(bitsf(st.pa[1]) == 9.0f && st.pa[0] == 0 && st.pa[2] == 0);

    /* the operand of any bank as halves: three of them, by one float spread over three */
    usse_insn mul = insn(0x20a43400efbc1897ull, 0, "VMUL i1.xyz sa46.xyz i1.xxx + FRCP i2.x i2.x\n");
    fresh();
    sa[46] = halves(2.0f, 3.0f); sa[47] = halves(4.0f, 99.0f);
    st.internal[4] = fbits(0.5f); st.internal[7] = fbits(-1.0f);
    st.internal[8] = fbits(8.0f);
    run1(&mul);
    assert(bitsf(st.internal[4]) == 1.0f && bitsf(st.internal[5]) == 1.5f && bitsf(st.internal[6]) == 2.0f && bitsf(st.internal[7]) == -1.0f);
    assert(bitsf(st.internal[8]) == 0.125f);

    /* three operands to the first: a row of a matrix times a component, added to what has gathered */
    usse_insn mad = insn(0x28c4000ae01c878cull, 0, "VMAD i2.xyzw sa24.xyzw i0.zzzz i2.xyzw + FRCP pa0.x i1.x\n");
    fresh();
    for (int i = 0; i < 4; i++) { sa[24 + i] = fbits(1.0f + (float)i); st.internal[8 + i] = fbits(100.0f); }
    st.internal[2] = fbits(3.0f);
    st.internal[4] = fbits(2.0f);
    run1(&mad);
    for (int i = 0; i < 4; i++) assert(bitsf(st.internal[8 + i]) == 3.0f * (1.0f + (float)i) + 100.0f);
    assert(bitsf(st.pa[0]) == 0.5f && st.pa[1] == 0);

    /* the same with the move going to an output: 64 bits of it, two floats */
    usse_insn out = insn(0x28c40109d0d44394ull, 0, "VMAD i1.xyzw sa40.xyzw i0.zzzz i1.xyzw + VMOV o12.xy i0.xy\n");
    fresh();
    for (int i = 0; i < 4; i++) { sa[40 + i] = fbits(2.0f); st.internal[i] = fbits(1.0f + (float)i); st.internal[4 + i] = fbits(0.5f); }
    run1(&out);
    for (int i = 0; i < 4; i++) assert(bitsf(st.internal[4 + i]) == 6.5f);
    assert(bitsf(st.out[12]) == 1.0f && bitsf(st.out[13]) == 2.0f && st.out[14] == 0);

    /* the first writing the register of any bank, and a dot product into the internal one */
    usse_insn dot = insn(0x2080751290042480ull, 0, "FRCP pa0.x pa0.x + VDP i1.x i1.xyz i2.xyz\n");
    fresh();
    st.pa[0] = fbits(8.0f);
    for (int i = 0; i < 4; i++) { st.internal[4 + i] = fbits(1.0f + (float)i); st.internal[8 + i] = fbits(2.0f); }
    run1(&dot);
    assert(bitsf(st.pa[0]) == 0.125f);
    assert(bitsf(st.internal[4]) == 12.0f && bitsf(st.internal[5]) == 2.0f);

    /* its predicate is two bits: none, p0, not p0. Nothing of it is printed. */
    usse_insn when = insn(word(USSE_F_VDUAL, U_VDUAL_SV_PRED, 2, U_VDUAL_DUAL_OP1, 5, U_VDUAL_DUAL_OP2, 7, U_VDUAL_PRIM_USTORE, 1,
                               U_VDUAL_PRIM_DEST_BANK, D_PA, U_VDUAL_PRIM_DEST_NUM, 1, U_VDUAL_PRIM_DEST_NUM_GPI_CASE, 1,
                               U_VDUAL_WRITE_MASK_NON_GPI, 1, U_VDUAL_UNIFIED_STORE_SLOT_BANK, S_SA,
                               U_VDUAL_UNIFIED_STORE_SLOT_NUM, 2, U_VDUAL_UNIFIED_STORE_SWIZZ, 4, -1),
                          0, "VMOV pa2.xy sa4.xy + FRCP i1.x i0.x\n");
    assert(when.pred == USSE_PRED_NOT_P0);
    for (int p0 = 0; p0 < 2; p0++) {
        fresh();
        st.pred[0] = (uint8_t)p0;
        sa[4] = fbits(6.0f); sa[5] = fbits(7.0f);
        st.internal[0] = fbits(2.0f);
        run1(&when);
        assert(bitsf(st.pa[2]) == (p0 ? 0.0f : 6.0f) && bitsf(st.pa[3]) == (p0 ? 0.0f : 7.0f));
        assert(bitsf(st.internal[4]) == (p0 ? 0.0f : 0.5f));
    }

    /* a second operation of no kind is not an instruction */
    usse_insn none;
    assert(!usse_decode(word(USSE_F_VDUAL, U_VDUAL_DUAL_OP1, 5, U_VDUAL_DUAL_OP2, 0, -1), 0, &none));
    printf("  two operations at once         ok\n");
}

/* --- colours of a byte a channel ------------------------------------------------------------ */

/* Four bytes as the word that holds them: x lowest. */
static uint32_t bytes4(unsigned x, unsigned y, unsigned z, unsigned w) { return x | y << 8 | z << 16 | w << 24; }

static void test_bytes(void) {
    usse_insn in;
    char text[256];

    /* the colour of one source and the alpha of the other: factors of one and of nothing */
    usse_insn join = insn(0x81800006b0800003ull, 0, "SOP2:FADD\nSOP2:FADD\n");
    fresh();
    st.pa[0] = 0x11223344u;
    sa[3] = 0xAABBCCDDu;
    run1(&join);
    assert(st.pa[4] == 0xAA223344u);

    /* a sum that stops at all the colour there is */
    usse_insn add = insn(0x81808805a0000200ull, 0, "SOP2:FADD\nSOP2:FADD\n");
    fresh();
    st.pa[4] = bytes4(0x01, 0x7F, 0xFF, 0x80);
    st.pa[0] = bytes4(0x02, 0x7F, 0x01, 0x90);
    run1(&add);
    assert(st.out[0] == bytes4(0x03, 0xFE, 0xFF, 0xFF));

    /* the colours added and the larger alpha kept */
    usse_insn larger = insn(0x8180880590030000ull, 0, "SOP2:FADD\nSOP2:FMAX\n");
    fresh();
    st.pa[0] = bytes4(0x30, 0x20, 0x10, 0x40);
    st.out[0] = bytes4(0x03, 0x02, 0x01, 0x80);
    run1(&larger);
    assert(st.out[0] == bytes4(0x33, 0x22, 0x11, 0x80));

    /* A blend by the first source's alpha: src1 * a + src2 * (1 - a), and
     * for the alpha a + src2's * (1 - a). 200 at 128/255 over 50 is
     * (128 * 200 + 127 * 50) / 255 = 125.3, and so on. */
    usse_insn blend = insn(word(USSE_F_SOP2, U_SOP2_CSEL1, 3, U_SOP2_CSEL2, 3, U_SOP2_CMOD2, 1, U_SOP2_AMOD1, 1,
                                U_SOP2_ASEL2, 1, U_SOP2_AMOD2, 1, U_SOP2_DEST_BANK, D_OUT, U_SOP2_SRC1_BANK, S_PA,
                                U_SOP2_SRC1_N, 1, U_SOP2_SRC2_BANK, S_OUT, -1),
                           0, "SOP2:FADD\nSOP2:FADD\n");
    fresh();
    st.pa[1] = bytes4(200, 100, 0, 128);
    st.out[0] = bytes4(50, 150, 255, 255);
    run1(&blend);
    assert(st.out[0] == bytes4(125, 125, 127, 255));

    /* a difference stops at nothing; the smaller alpha */
    usse_insn sub = insn(word(USSE_F_SOP2, U_SOP2_CMOD1, 1, U_SOP2_CMOD2, 1, U_SOP2_COP, 1, U_SOP2_AMOD1, 1, U_SOP2_AMOD2, 1,
                              U_SOP2_AOP, 2, U_SOP2_DEST_BANK, D_PA, U_SOP2_DEST_N, 3, U_SOP2_SRC1_BANK, S_PA, U_SOP2_SRC1_N, 1,
                              U_SOP2_SRC2_BANK, S_SA, U_SOP2_SRC2_N, 2, -1),
                         0, "SOP2:FSUB\nSOP2:FMIN\n");
    fresh();
    st.pa[1] = bytes4(0x10, 0x20, 0x30, 0x40);
    sa[2] = bytes4(0x30, 0x20, 0x10, 0x80);
    run1(&sub);
    assert(st.pa[3] == bytes4(0x00, 0x00, 0x20, 0x40));

    /* A form there is no account of, here a modifier on the destination,
     * is listed and not run; one the reference does not translate, here a
     * factor of the saturated alpha, is not listed either. */
    assert(usse_decode(word(USSE_F_SOP2, U_SOP2_CMOD1, 1, U_SOP2_DEST_MOD, 1, U_SOP2_DEST_BANK, D_PA, U_SOP2_SRC1_BANK, S_PA,
                            U_SOP2_SRC2_BANK, S_PA, -1), 0, &in));
    assert(!usse_can_run(&in) && usse_run(&in, 1, &st) == 1);
    usse_disassemble(&in, &st, text, sizeof(text));
    assert(strstr(text, "SOP2:FADD"));
    assert(usse_decode(word(USSE_F_SOP2, U_SOP2_CSEL1, 5, U_SOP2_DEST_BANK, D_PA, U_SOP2_SRC1_BANK, S_PA, U_SOP2_SRC2_BANK, S_PA, -1),
                       0, &in));
    assert(!usse_can_run(&in));
    usse_disassemble(&in, &st, text, sizeof(text));
    assert(text[0] == '\0');

    /* with a mask: two bytes of a secondary attribute into a temporary, the other two left */
    usse_insn two = insn(0x91013000e0000100ull, 0, "SOP2M:FADD\nSOP2M:FADD\n");
    fresh();
    sa[2] = 0x44332211u;
    st.temp[0] = 0xAABBCCDDu;
    run1(&two);
    assert(st.temp[0] == 0xAABB2211u);

    /* The mask's lowest bit is the alpha. An output onto itself, its first
     * channel and its alpha, changes nothing: each channel written is the
     * one worked out for it. */
    usse_insn same = insn(0x91801a0150000000ull, 0, "SOP2M:FADD\nSOP2M:FSUB\n");
    assert(same.mask == 9);
    fresh();
    st.out[0] = 0x12345678u;
    run1(&same);
    assert(st.out[0] == 0x12345678u);

    /* the alpha alone, a product of two: 128/255 squared is 64.25/255 */
    usse_insn alpha = insn(word(USSE_F_SOP2M, U_SOP2M_SEL1, 6, U_SOP2M_WMASK, 1, U_SOP2M_DESTBANK, D_PA, U_SOP2M_SRC1BANK, S_PA,
                                U_SOP2M_SRC1NUM, 1, U_SOP2M_SRC2BANK, S_PA, U_SOP2M_SRC2NUM, 2, -1),
                           0, "SOP2M:FADD\nSOP2M:FADD\n");
    fresh();
    st.pa[0] = 0x11223344u;
    st.pa[1] = bytes4(255, 255, 255, 128);
    st.pa[2] = bytes4(0, 0, 0, 128);
    run1(&alpha);
    assert(st.pa[0] == 0x40223344u);

    /* three sources: the first one's channels weigh the second against the
     * third, src1 * src0 + src2 * (1 - src0) */
    usse_insn weigh = insn(word(USSE_F_SOP3, U_SOP3_CSEL1, 4, U_SOP3_CSEL2, 4, U_SOP3_CMOD2, 1, U_SOP3_ASEL1, 1, U_SOP3_DESTBANK, D_OUT,
                                U_SOP3_SRC0BANK, S0_PA, U_SOP3_SRC1BANK, S_PA, U_SOP3_SRC1N, 1, U_SOP3_SRC2BANK, S_PA,
                                U_SOP3_SRC2N, 2, -1),
                           0, "SOP3:FADD\nSOP3:FADD\n");
    fresh();
    st.pa[0] = bytes4(255, 0, 128, 255);
    st.pa[1] = bytes4(10, 20, 30, 40);
    st.pa[2] = bytes4(50, 60, 70, 80);
    run1(&weigh);
    assert(st.out[0] == bytes4(10, 60, 50, 40));

    /* Bytes as whole numbers. Times one and added, held to a byte: how a
     * program adds colours that must not wrap; */
    usse_insn held = insn(0x98000800b0c18011ull, 0, "IMA8 r6.xyzw pa0.xyzw sa17.xyzw (r6.xyz, r6.---w)\n");
    fresh();
    st.pa[0] = bytes4(0x01, 0x80, 0xFF, 0x10);
    sa[17] = 0x01010101u;
    st.temp[6] = bytes4(0x02, 0x80, 0x01, 0xF0);
    run1(&held);
    assert(st.temp[6] == bytes4(0x03, 0xFF, 0xFF, 0xFF));

    /* the same not held, cut to a byte; */
    usse_insn cut = insn(0x98000006b0008011ull, 0, "IMA8 pa0.xyzw pa0.xyzw sa17.xyzw (pa2.xyz, pa2.---w)\n");
    fresh();
    st.pa[0] = bytes4(0x01, 0x80, 0xFF, 0x10);
    sa[17] = 0x01010101u;
    st.pa[2] = bytes4(0x02, 0x80, 0x01, 0xF0);
    run1(&cut);
    assert(st.pa[0] == bytes4(0x03, 0x00, 0x00, 0x00));

    /* and a product that is a product: 5 times 8 and 1 is 41, 64 times 8 is 512 and cut to nothing */
    usse_insn times = insn(0x98c0000030008185ull, 0, "IMA8 r0.xyzw r3.xyzw sa5.xyzw (r2.xyz, r2.---w)\n");
    fresh();
    st.temp[3] = bytes4(5, 4, 3, 64);
    sa[5] = 0x08080808u;
    st.temp[2] = 0x01010101u;
    run1(&times);
    assert(st.temp[0] == bytes4(41, 33, 25, 1));

    /* Subtracted and held at nothing; source 1's alpha in each of its
     * channels; the addend's colour from source 0's alpha and its alpha
     * from source 1's. */
    usse_insn pick = insn(word(USSE_F_I8MAD, U_I8MAD_SRC0_NEG, 1, U_I8MAD_SATURATED, 1, U_I8MAD_CSEL1, 1, U_I8MAD_CSEL0, 2,
                               U_I8MAD_ASEL0, 1, U_I8MAD_DEST_BANK, D_OUT, U_I8MAD_DEST_NUM, 1, U_I8MAD_SRC0_BANK, S0_TEMP,
                               U_I8MAD_SRC0_NUM, 2, U_I8MAD_SRC1_BANK, S_PA, U_I8MAD_SRC1_NUM, 1, U_I8MAD_SRC2_BANK, S_SA,
                               U_I8MAD_SRC2_NUM, 4, -1),
                          0, "IMA8 o1.xyzw pa1.wwww sa4.xyzw -(r2.www, pa1.---w)\n");
    fresh();
    st.pa[1] = bytes4(9, 9, 9, 3);
    sa[4] = bytes4(10, 20, 100, 2);
    st.temp[2] = bytes4(0, 0, 0, 50);
    run1(&pick);
    assert(st.out[1] == bytes4(0, 10, 250, 3));

    /* one of its six modifiers, which nothing describes: listed, not run */
    assert(usse_decode(word(USSE_F_I8MAD, U_I8MAD_CMOD1, 1, U_I8MAD_DEST_BANK, D_PA, U_I8MAD_SRC0_BANK, S0_PA, U_I8MAD_SRC1_BANK, S_PA,
                            U_I8MAD_SRC2_BANK, S_PA, -1), 0, &in));
    assert(!usse_can_run(&in) && usse_run(&in, 1, &st) == 1);
    usse_disassemble(&in, &st, text, sizeof(text));
    assert(strstr(text, "IMA8 pa0.xyzw pa0.xyzw pa0.xyzw (pa0.xyz, pa0.---w)"));
    printf("  colours of a byte a channel    ok\n");
}

/* --- a depth of the program's own ------------------------------------------------------------ */

static void test_depth(void) {
    usse_insn depth = insn(0xfb300000f0000000ull, 0, "DEPTHF r0.x sa0.x sa0.x\n");
    fresh();
    st.temp[0] = fbits(0.25f);
    assert(!st.depth_written);
    run1(&depth);
    assert(st.depth_written && st.depth == 0.25f);
    printf("  a depth from a register        ok\n");
}

int main(void) {
    printf("usse:\n");
    test_moves();
    test_arithmetic();
    test_packing();
    test_integers();
    test_flow();
    test_outside();
    test_repeats();
    test_dual();
    test_bytes();
    test_depth();
    printf("all usse tests passed\n");
    return 0;
}
