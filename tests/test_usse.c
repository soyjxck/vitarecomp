/* test_usse.c — the shader bytecode interpreter, on instructions built here.
 *
 * Every word is assembled field by field from the layout table, printed and
 * compared with the text it should read as, then run against registers
 * filled by hand. Nothing here comes from a title's shaders.
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

int main(void) {
    printf("usse:\n");
    test_moves();
    test_arithmetic();
    test_packing();
    test_integers();
    test_flow();
    test_outside();
    test_repeats();
    printf("all usse tests passed\n");
    return 0;
}
