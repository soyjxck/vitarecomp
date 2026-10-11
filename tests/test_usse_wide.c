/* test_usse_wide.c — the batched executor against the one-at-a-time interpreter.
 *
 * The interpreter that runs a program for one vertex or pixel is the
 * definition; the one that runs it for a row of them has to leave every lane
 * exactly as the first would. So: instruction words made at random, format
 * by format, decoded; registers filled at random, the same in both; one run
 * each; every register of every lane compared bit for bit. And the same for
 * short programs of such instructions, where predicates set by one are used
 * by the next.
 */

#include "../src/usse.h"
#include "../src/usse_fields.h"
#include "../src/half_rows.h"
#include "vitarecomp/half.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#error "the tests check with assert(): build them without NDEBUG"
#endif

static uint64_t g_seed = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) {
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17;
    return g_seed;
}

/* A register word that is two finite halves, and so also a finite float:
 * no input is a NaN, whichever way it is read. */
static uint32_t word(void) {
    uint32_t lo = (uint32_t)rnd() & 0xFFFF, hi = (uint32_t)rnd() & 0xFFFF;
    if ((lo & 0x7C00) == 0x7C00) lo ^= 0x4000;
    if ((hi & 0x7C00) == 0x7C00) hi ^= 0x4000;
    /* keep most of them of moderate size, so that sums and products stay finite too */
    if (rnd() & 3) { lo = (lo & 0x83FF) | 0x3800 | ((uint32_t)(rnd() & 3) << 10); hi = (hi & 0x83FF) | 0x3C00; }
    return lo | (hi << 16);
}

/* What a texture gives: something made of the coordinate, so that both
 * interpreters asking the same question get the same answer. */
static int sample(void *user, uint32_t reg, const float *coord, int dimensions, int lod_mode, float lod, float rgba[4]) {
    (void)user; (void)dimensions;
    float base = (float)(reg & 7) * 0.125f + (lod_mode ? lod * 0.03125f : 0.0f);
    rgba[0] = coord[0] * 0.5f + base;
    rgba[1] = coord[1] * 0.25f - base;
    rgba[2] = coord[0] - coord[1];
    rgba[3] = 0.75f;
    return 4;
}
static void gather(void *user, uint32_t reg, const float *coord, float texels[4], float uv[2]) {
    (void)user;
    for (int i = 0; i < 4; i++) texels[i] = coord[0] * (float)(i + 1) + (float)(reg & 3);
    uv[0] = 0.25f; uv[1] = 0.625f;
}
static const usse_env ENV = { NULL, NULL, sample, gather };

static usse_state scalar[USSE_LANES];
static usse_wide wide;
static uint32_t sa[USSE_WORDS];

/* The same random contents in both. */
static void fill(unsigned lanes) {
    for (int i = 0; i < USSE_WORDS; i++) sa[i] = word();
    memset(&wide, 0, sizeof(wide));
    wide.sa = sa;
    wide.env = &ENV;
    wide.lanes = lanes;
    for (unsigned l = 0; l < lanes; l++) {
        usse_state *s = &scalar[l];
        memset(s, 0, sizeof(*s));
        s->sa = sa;
        s->env = &ENV;
        /* the registers a seven-bit operand can reach, and a little beyond */
        for (int i = 0; i < USSE_WORDS; i++) {
            s->temp[i] = wide.temp[i][l] = word();
            s->pa[i] = wide.pa[i][l] = word();
            s->out[i] = wide.out[i][l] = word();
        }
        for (int i = 0; i < 16; i++) s->internal[i] = wide.internal[i][l] = word();
        for (int i = 0; i < 4; i++) s->pred[i] = wide.pred[i][l] = (uint8_t)(rnd() & 1);
    }
}

/* Whether lane l came out the same; says where not. */
static int same(unsigned l, const char *what) {
    const usse_state *s = &scalar[l];
    for (int i = 0; i < USSE_WORDS; i++) {
        if (s->temp[i] != wide.temp[i][l]) { fprintf(stderr, "%s: lane %u r%d is %08X, one at a time %08X\n", what, l, i, wide.temp[i][l], s->temp[i]); return 0; }
        if (s->pa[i] != wide.pa[i][l])     { fprintf(stderr, "%s: lane %u pa%d is %08X, one at a time %08X\n", what, l, i, wide.pa[i][l], s->pa[i]); return 0; }
        if (s->out[i] != wide.out[i][l])   { fprintf(stderr, "%s: lane %u o%d is %08X, one at a time %08X\n", what, l, i, wide.out[i][l], s->out[i]); return 0; }
    }
    for (int i = 0; i < 16; i++)
        if (s->internal[i] != wide.internal[i][l]) { fprintf(stderr, "%s: lane %u i%d.%d is %08X, one at a time %08X\n", what, l, i / 4, i % 4, wide.internal[i][l], s->internal[i]); return 0; }
    for (int i = 0; i < 4; i++)
        if (s->pred[i] != wide.pred[i][l]) { fprintf(stderr, "%s: lane %u p%d is %u, one at a time %u\n", what, l, i, wide.pred[i][l], s->pred[i]); return 0; }
    return 1;
}

/* A random word of one format: its fixed bits as they must be, the rest by chance. */
static uint64_t random_word(int format) { return (rnd() & ~USSE_FORMATS[format].mask) | USSE_FORMATS[format].value; }

static const int FORMATS[] = { USSE_F_VMAD2, USSE_F_V32NMAD, USSE_F_V16NMAD, USSE_F_VMAD, USSE_F_VDP, USSE_F_VCOMP, USSE_F_VMOV,
                               USSE_F_VPCK, USSE_F_VTST, USSE_F_VTSTMSK, USSE_F_VBW, USSE_F_SMP, USSE_F_KILL, USSE_F_SMLSI };
#define FORMAT_COUNT (sizeof(FORMATS) / sizeof(FORMATS[0]))

static void show(const usse_insn *in) {
    char line[1024];
    usse_state s;
    memset(&s, 0, sizeof(s));
    usse_reset(&s);
    usse_disassemble(in, &s, line, sizeof(line));
    fputs(line, stderr);
}

static void test_single(void) {
    unsigned ran[USSE_F_COUNT];
    memset(ran, 0, sizeof(ran));
    for (int round = 0; round < 60000; round++) {
        int format = FORMATS[round % FORMAT_COUNT];
        usse_insn in;
        if (!usse_decode(random_word(format), 0, &in) || !usse_wide_can_run(&in, 1)) continue;
        unsigned lanes = round % 5 == 0 ? 1 + (unsigned)(rnd() % USSE_LANES) : USSE_LANES;
        fill(lanes);
        for (unsigned l = 0; l < lanes; l++) {
            usse_reset(&scalar[l]);
            assert(usse_run(&in, 1, &scalar[l]) == 0);
        }
        usse_run_wide(&in, 1, &wide);
        for (unsigned l = 0; l < lanes; l++) {
            assert(scalar[l].killed == wide.killed[l]);
            if (!same(l, "one instruction")) { show(&in); assert(0); }
        }
        ran[in.format]++;
    }
    /* every format was really exercised, not refused */
    for (size_t f = 0; f < FORMAT_COUNT; f++) {
        if (ran[FORMATS[f]] < 100) { fprintf(stderr, "format %d ran only %u times\n", FORMATS[f], ran[FORMATS[f]]); assert(0); }
    }
    printf("  single instructions            ok\n");
}

static void test_programs(void) {
    unsigned compared = 0, discarded = 0;
    for (int round = 0; round < 4000; round++) {
        usse_insn code[12];
        uint32_t count = 0;
        while (count < 12) {
            usse_insn in;
            int format = FORMATS[rnd() % FORMAT_COUNT];
            /* tests and predicated instructions more often than chance would have them */
            if (rnd() % 3 == 0) format = USSE_F_VTST;
            if (!usse_decode(random_word(format), 0, &in) || !usse_wide_can_run(&in, 1)) continue;
            code[count++] = in;
        }
        fill(USSE_LANES);
        for (unsigned l = 0; l < USSE_LANES; l++) {
            usse_reset(&scalar[l]);
            assert(usse_run(code, count, &scalar[l]) == 0);
        }
        usse_run_wide(code, count, &wide);
        for (unsigned l = 0; l < USSE_LANES; l++) {
            assert(scalar[l].killed == wide.killed[l]);
            /* a discarded lane stopped where it was discarded; what the batch did with it after is nobody's */
            if (scalar[l].killed) { discarded++; continue; }
            if (!same(l, "a program")) { for (uint32_t i = 0; i < count; i++) show(&code[i]); assert(0); }
            compared++;
        }
    }
    assert(compared > 20000 && discarded > 1000);
    printf("  programs of twelve             ok\n");
}

static void test_half_rows(void) {
    static uint32_t words[65536];
    static float out[65536], back[65536];
    for (int high = 0; high < 2; high++) {
        for (uint32_t h = 0; h < 65536; h++) words[h] = high ? (h << 16) | 0x1234u : h | 0xABCD0000u;
        vita_halves_to_floats(words, high, out, 65536);
        for (uint32_t h = 0; h < 65536; h++) {
            float want = vita_f16_to_f32((uint16_t)h);
            uint32_t a, b;
            memcpy(&a, &out[h], 4);
            memcpy(&b, &want, 4);
            assert(a == b);
        }
        /* and back: every half survives the round trip, the other half of the word untouched */
        for (uint32_t h = 0; h < 65536; h++) { back[h] = vita_f16_to_f32((uint16_t)h); words[h] = high ? 0x00005678u : 0x9ABC0000u; }
        vita_floats_to_halves(back, words, high, 65536);
        for (uint32_t h = 0; h < 65536; h++) {
            uint32_t got = high ? words[h] >> 16 : words[h] & 0xFFFF, kept = high ? words[h] & 0xFFFF : words[h] >> 16;
            uint32_t want = vita_f32_to_f16(back[h]);
            assert(got == want && kept == (high ? 0x5678u : 0x9ABCu));
        }
    }
    /* floats that are not halves: rounded the same way as one at a time */
    for (int round = 0; round < 200; round++) {
        for (int i = 0; i < 1000; i++) {
            uint32_t bits = (uint32_t)rnd();
            if ((bits & 0x7F800000u) == 0x7F800000u) bits &= ~0x00800000u;
            if (round & 1) bits = (bits & 0x807FFFFFu) | ((uint32_t)(0x70 + rnd() % 0x20) << 23);   /* near the range of a half */
            memcpy(&back[i], &bits, 4);
            words[i] = 0;
        }
        vita_floats_to_halves(back, words, 0, 1000);
        for (int i = 0; i < 1000; i++) assert(words[i] == vita_f32_to_f16(back[i]));
    }
    printf("  rows of halves (%s)       ok\n", vita_half_rows_fast() ? "eight at a time" : "one at a time  ");
}

int main(void) {
    printf("usse, many at once:\n");
    test_half_rows();
    test_single();
    test_programs();
    printf("all batched tests passed\n");
    return 0;
}
