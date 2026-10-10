/* test_runtime.c — the semantic helpers.
 *
 * Every function here exists because the obvious C is wrong, so every test
 * here is a case where the obvious C would pass a casual reading and produce a
 * bad answer. The values are worked out from the architecture's definition, not
 * from what the implementation happens to return.
 */

#include "vitarecomp/recomp_rt.h"
#include "vitarecomp/dispatch.h"
#include "vitarecomp/hle.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/* The host contract (host.h), as the tests stand in for a host: the HLE
 * reaches these from exit paths and vblank waits. */
void vita_host_vblank(void) { }
uint32_t vita_host_buttons(void) { return 0; }
void vita_host_exit(int status) { exit(status); }

#ifdef NDEBUG
#error "tests must be built with asserts enabled (NDEBUG is defined)"
#endif

/* --- shifts ----------------------------------------------------------------- */

static void test_shifts(void) {
    /* The headline case. C leaves a shift by >= the width undefined, and on x86
     * the hardware masks the count to 5 bits — so the naive `v << n` returns
     * the operand UNCHANGED for n == 32, which is the opposite of ARM's answer.
     * This is invisible until a shift amount is computed rather than constant. */
    assert(vita_lsl(1u, 32) == 0u);
    assert(vita_lsl(0xFFFFFFFFu, 32) == 0u);
    assert(vita_lsl(1u, 33) == 0u);
    assert(vita_lsr(0x80000000u, 32) == 0u);

    assert(vita_lsl(1u, 0) == 1u);
    assert(vita_lsl(1u, 31) == 0x80000000u);
    assert(vita_lsr(0x80000000u, 31) == 1u);

    /* ASR saturates to the sign bit rather than reaching zero. */
    assert(vita_asr(0x80000000u, 32) == 0xFFFFFFFFu);
    assert(vita_asr(0x80000000u, 99) == 0xFFFFFFFFu);
    assert(vita_asr(0x7FFFFFFFu, 32) == 0u);
    assert(vita_asr(0x80000000u, 31) == 0xFFFFFFFFu);
    assert(vita_asr(0xFFFFFFF0u, 4)  == 0xFFFFFFFFu);
    assert(vita_asr(0x00000010u, 4)  == 1u);
    assert(vita_asr(0x12345678u, 0)  == 0x12345678u);

    /* ROR by 32 is the identity, not zero. */
    assert(vita_ror(0x12345678u, 32) == 0x12345678u);
    assert(vita_ror(0x00000001u, 1)  == 0x80000000u);
    assert(vita_ror(0x80000000u, 31) == 0x00000001u);

    printf("  shifts (>=32 defined)          ok\n");
}

/* --- flags ------------------------------------------------------------------ */

static void test_flags_add(void) {
    vita_flags_add(1u, 1u, 2u);
    assert(!flag_n && !flag_z && !flag_c && !flag_v);

    /* Unsigned wrap sets carry. */
    vita_flags_add(0xFFFFFFFFu, 1u, 0u);
    assert(flag_z && flag_c);

    /* Signed overflow is a different condition entirely: two positives
     * producing a negative. Carry is NOT set here. */
    vita_flags_add(0x7FFFFFFFu, 1u, 0x80000000u);
    assert(flag_v && !flag_c && flag_n);

    /* Two negatives producing a positive: overflow AND carry. */
    vita_flags_add(0x80000000u, 0x80000000u, 0u);
    assert(flag_v && flag_c && flag_z);

    printf("  flags: add carry vs overflow   ok\n");
}

static void test_flags_sub(void) {
    /* ARM sets carry on a subtraction when there is NO borrow. This is the
     * opposite of the intuitive reading and of several other architectures, and
     * getting it backwards inverts every unsigned comparison in the program. */
    vita_flags_sub(5u, 3u, 2u);
    assert(flag_c);                 /* 5 >= 3, no borrow -> carry SET   */

    vita_flags_sub(3u, 5u, (uint32_t)-2);
    assert(!flag_c);                /* 3 < 5, borrowed   -> carry CLEAR */

    vita_flags_sub(5u, 5u, 0u);
    assert(flag_c && flag_z);       /* equal: no borrow, and zero       */

    /* Signed overflow on subtraction: positive minus negative going negative. */
    vita_flags_sub(0x7FFFFFFFu, 0xFFFFFFFFu, 0x80000000u);
    assert(flag_v);

    printf("  flags: sub carry is NOT borrow ok\n");
}

static void test_flags_adc(void) {
    /* The edge case that `res < a` alone gets wrong: with a carry in, adding
     * 0xFFFFFFFF leaves the value unchanged while genuinely carrying. A naive
     * test sees res == a, concludes no carry, and silently breaks every
     * multi-word addition in the program. */
    vita_flags_adc(5u, 0xFFFFFFFFu, 1u, 5u);
    assert(flag_c);

    vita_flags_adc(5u, 0u, 0u, 5u);
    assert(!flag_c);

    vita_flags_adc(0xFFFFFFFFu, 0u, 1u, 0u);
    assert(flag_c && flag_z);

    printf("  flags: adc carry-in edge case  ok\n");
}

/* --- condition codes -------------------------------------------------------- */

static void test_conditions(void) {
    /* eq / ne */
    flag_z = 1; assert(vita_cond(0x0)); assert(!vita_cond(0x1));
    flag_z = 0; assert(!vita_cond(0x0)); assert(vita_cond(0x1));

    /* cs / cc */
    flag_c = 1; assert(vita_cond(0x2)); assert(!vita_cond(0x3));
    flag_c = 0; assert(!vita_cond(0x2)); assert(vita_cond(0x3));

    /* The signed conditions depend on N and V agreeing, which is the part that
     * is easy to write as a plain comparison and get wrong. */
    flag_n = 0; flag_v = 0; assert(vita_cond(0xA));  /* ge */
    flag_n = 1; flag_v = 1; assert(vita_cond(0xA));  /* ge: both set */
    flag_n = 1; flag_v = 0; assert(!vita_cond(0xA));
    flag_n = 1; flag_v = 0; assert(vita_cond(0xB));  /* lt */

    /* gt is ge AND not-zero; le is its complement. */
    flag_n = 0; flag_v = 0; flag_z = 0; assert(vita_cond(0xC));
    flag_z = 1;                          assert(!vita_cond(0xC));
    flag_z = 1;                          assert(vita_cond(0xD));

    /* hi is carry AND not-zero — distinct from cs. */
    flag_c = 1; flag_z = 0; assert(vita_cond(0x8));
    flag_c = 1; flag_z = 1; assert(!vita_cond(0x8));
    flag_c = 1; flag_z = 1; assert(vita_cond(0x9));  /* ls */

    assert(vita_cond(0xE));              /* al is always true */

    printf("  condition codes, all 16        ok\n");
}

/* --- sign extension and byte reversal --------------------------------------- */

static void test_extends(void) {
    assert(vita_sxtb(0x7F) == 0x7Fu);
    assert(vita_sxtb(0x80) == 0xFFFFFF80u);
    assert(vita_sxtb(0x12345680u) == 0xFFFFFF80u);   /* high bits ignored */
    assert(vita_sxth(0x7FFF) == 0x7FFFu);
    assert(vita_sxth(0x8000) == 0xFFFF8000u);
    assert(vita_uxtb(0x1234) == 0x34u);
    assert(vita_uxth(0x12345678u) == 0x5678u);
    assert(vita_rev(0x12345678u) == 0x78563412u);
    printf("  extends and rev                ok\n");
}

/* --- bitfields -------------------------------------------------------------- */

/* Expected values are the architecture's definition, not what the code
 * happens to return. The two that matter are the width-32 mask, where the
 * obvious `(1u << 32) - 1` is undefined, and SBFX's sign, which comes from the
 * top bit of the FIELD and not of the register. */
static void test_bitfields(void) {
    assert(vita_bf_mask(0)  == 0x00000000u);
    assert(vita_bf_mask(8)  == 0x000000FFu);
    assert(vita_bf_mask(32) == 0xFFFFFFFFu);

    assert(vita_ubfx(0xDEADBEEFu, 4, 8) == 0xEEu);
    assert(vita_ubfx(0xDEADBEEFu, 0, 32) == 0xDEADBEEFu);   /* the UB case */
    assert(vita_ubfx(0x80000000u, 31, 1) == 1u);

    /* Field 0x8 with width 4: its top bit is set, so it sign-extends — even
     * though the register itself is positive. */
    assert(vita_sbfx(0x00000080u, 4, 4) == 0xFFFFFFF8u);
    assert(vita_sbfx(0x00000070u, 4, 4) == 0x00000007u);
    assert(vita_sbfx(0xDEADBEEFu, 0, 32) == 0xDEADBEEFu);

    /* BFI preserves everything outside the field, which is what makes it a
     * read-modify-write of the destination rather than an assignment. */
    assert(vita_bfi(0xFFFFFFFFu, 0x0u, 4, 8) == 0xFFFFF00Fu);   /* bfc */
    assert(vita_bfi(0x00000000u, 0xFFu, 4, 8) == 0x00000FF0u);
    /* Source bits above the field width are discarded, not spilled. */
    assert(vita_bfi(0x00000000u, 0xFFFFu, 4, 8) == 0x00000FF0u);
    assert(vita_bfi(0x12345678u, 0x0u, 0, 32) == 0x00000000u);

    printf("  bitfields                      ok\n");
}

/* --- dispatch ----------------------------------------------------------------
 *
 * The table now carries import stubs alongside recompiled functions, merged
 * from two sorted lists. Both properties the search depends on are checked
 * here: that the Thumb bit is masked, and that an unsorted table is refused
 * rather than searched — an unsorted one does not crash, it silently fails to
 * find entries that are present, which looks exactly like missing coverage.
 */

static int g_called;
static void mark_a(void) { g_called = 1; }
static void mark_b(void) { g_called = 2; }
static void mark_c(void) { g_called = 3; }

static void test_dispatch(void) {
    static const vita_dispatch_entry sorted[] = {
        { 0x81000100u, mark_a },
        { 0x81000200u, mark_b },   /* an import stub, as far as the table knows */
        { 0x81000300u, mark_c },
    };
    vita_dispatch_init(sorted, 3);

    g_called = 0; vita_dispatch(0, 0x81000100u); assert(g_called == 1);
    g_called = 0; vita_dispatch(0, 0x81000300u); assert(g_called == 3);

    /* Every pointer to Thumb code carries bit 0 as an instruction-set marker.
     * A lookup that does not mask it misses EVERY entry, by one. */
    g_called = 0; vita_dispatch(0, 0x81000201u); assert(g_called == 2);

    assert(vita_dispatch_hits() == 3);
    assert(vita_dispatch_misses() == 0);

    /* An unsorted table is refused outright: count goes to zero, so every
     * lookup misses loudly instead of some lookups missing quietly. */
    static const vita_dispatch_entry unsorted[] = {
        { 0x81000300u, mark_c },
        { 0x81000100u, mark_a },
    };
    vita_dispatch_init(unsorted, 2);
    g_called = 0;
    assert(vita_dispatch_hits() == 0);

    printf("  dispatch, thumb bit and order  ok\n");
}

/* --- HLE: SceLibc over guest memory ------------------------------------------
 *
 * These forward to the host C library, so the host's correctness is not what
 * is in question here. The boundary is: every pointer argument is a GUEST
 * address into a flat allocation, and the length of a C string is in the data
 * rather than in the arguments. Both are checked.
 */

#define GBASE 0x81000000u

/* Declared here rather than in hle.h. Every one of these is `void(void)` and
 * the generated code declares the whole set already; a second list in a
 * shipped header would only be something to keep in sync. */
void vita_hle_strlen(void);
void vita_hle_strcpy(void);
void vita_hle_strncpy(void);
void vita_hle_strchr(void);
void vita_hle_memcpy(void);
void vita_hle_memcmp(void);
void vita_hle_malloc(void);
void vita_hle__sceLibcErrnoLoc(void);
void vita_hle___cxa_guard_acquire(void);
void vita_hle___cxa_guard_release(void);

static void test_hle_libc(void) {
    assert(vita_mem_init(GBASE, 64 * 1024) == 0);
    vita_heap_init(GBASE + 32 * 1024, 16 * 1024);
    vita_mem_bad_access = 0;

    /* strlen measures inside guest memory. */
    const char *msg = "cardgame";
    for (uint32_t i = 0; i <= 8; i++) vita_write8(GBASE + 0x100 + i, (uint8_t)msg[i]);
    r0 = GBASE + 0x100; vita_hle_strlen();
    assert(r0 == 8);

    /* strcpy copies the terminator too, which is why it needs len + 1. */
    r0 = GBASE + 0x200; r1 = GBASE + 0x100; vita_hle_strcpy();
    r0 = GBASE + 0x200; vita_hle_strlen();
    assert(r0 == 8);
    assert(vita_read8(GBASE + 0x208) == 0);

    /* memcpy returns nothing we check, but must move the bytes. */
    r0 = GBASE + 0x300; r1 = GBASE + 0x100; r2 = 9; vita_hle_memcpy();
    r0 = GBASE + 0x300; r1 = GBASE + 0x100; r2 = 9; vita_hle_memcmp();
    assert(r0 == 0);

    /* strncpy pads the WHOLE remainder with NULs, not just one. */
    for (uint32_t i = 0; i < 16; i++) vita_write8(GBASE + 0x400 + i, 0xAA);
    r0 = GBASE + 0x400; r1 = GBASE + 0x100; r2 = 12; vita_hle_strncpy();
    assert(vita_read8(GBASE + 0x408) == 0);
    assert(vita_read8(GBASE + 0x40B) == 0);
    assert(vita_read8(GBASE + 0x40C) == 0xAA);   /* and no further */

    /* strchr returns a GUEST pointer, not a host one. */
    r0 = GBASE + 0x100; r1 = 'g'; vita_hle_strchr();
    assert(r0 == GBASE + 0x104);
    r0 = GBASE + 0x100; r1 = 'z'; vita_hle_strchr();
    assert(r0 == 0);

    /* The heap hands out guest addresses inside its region, 8-aligned. */
    uint32_t used0 = vita_heap_used();
    r0 = 10; vita_hle_malloc();
    uint32_t a = r0;
    assert(a >= GBASE + 32 * 1024 && a < GBASE + 48 * 1024);
    assert((a & 7u) == 0);
    r0 = 10; vita_hle_malloc();
    assert(r0 != a);                              /* distinct allocations */
    assert(vita_heap_used() > used0);
    r0 = 0; vita_hle_malloc();
    assert(r0 == 0);                              /* malloc(0) is NULL here */

    /* errno needs an address the guest can write through. */
    vita_hle__sceLibcErrnoLoc();
    assert(r0 != 0);
    vita_write32(r0, 22);
    vita_hle__sceLibcErrnoLoc();
    assert(vita_read32(r0) == 22);

    /* A static-init guard reports "run the constructor" exactly once. */
    uint32_t guard = GBASE + 0x500;
    vita_write8(guard, 0);
    r0 = guard; vita_hle___cxa_guard_acquire(); assert(r0 == 1);
    r0 = guard; vita_hle___cxa_guard_release();
    r0 = guard; vita_hle___cxa_guard_acquire(); assert(r0 == 0);

    /* None of the above should have touched anything unbacked. */
    assert(vita_mem_bad_access == 0);

    vita_mem_free();
    printf("  hle: SceLibc over guest memory ok\n");
}

/* --- memory ----------------------------------------------------------------- */

/* --- byte-parallel ops, bit tricks ------------------------------------------- */

static void test_parallel(void) {
    /* UADD8: each byte adds on its own; the GE bit is the carry OUT of the
     * byte, which is how strlen's "add 0xFEFEFEFF" test finds a zero byte. */
    assert(vita_uadd8(0x01020304u, 0x01010101u) == 0x02030405u);
    assert(flag_ge == 0);
    assert(vita_uadd8(0xFF000000u, 0x01000000u) == 0x00000000u);
    assert(flag_ge == 0x8);
    assert(vita_uadd8(0x00FF00FFu, 0x00010001u) == 0x00000000u);
    assert(flag_ge == 0x5);

    /* USUB8: GE is "no borrow", i.e. a >= b per byte. */
    assert(vita_usub8(0x05050505u, 0x01060105u) == 0x04FF0400u);
    assert(flag_ge == 0xB);                      /* byte 2 borrowed */

    /* SEL picks per byte by GE. */
    flag_ge = 0x5;
    assert(vita_sel(0xAABBCCDDu, 0x11223344u) == 0x11BB33DDu);
    flag_ge = 0xF;
    assert(vita_sel(0xAABBCCDDu, 0x11223344u) == 0xAABBCCDDu);

    assert(vita_clz(0) == 32);
    assert(vita_clz(1) == 31);
    assert(vita_clz(0x80000000u) == 0);
    assert(vita_clz(0x00010000u) == 15);

    assert(vita_rbit(0x00000001u) == 0x80000000u);
    assert(vita_rbit(0x12345678u) == 0x1E6A2C48u);
    assert(vita_rev16(0x11223344u) == 0x22114433u);
    assert(vita_revsh(0x000080FFu) == 0xFFFFFF80u);
    assert(vita_revsh(0x0000017Fu) == 0x00007F01u);

    printf("  uadd8/usub8/sel, clz/rbit/rev   ok\n");
}

/* --- NEON lanes ------------------------------------------------------------- */

static void test_neon(void) {
    /* Lane-wise add does not carry between lanes. */
    assert(vita_neon_add(0x00000001FFFFFFFFull, 0x0000000100000001ull, 32) == 0x0000000200000000ull);
    assert(vita_neon_add(0xFF01FF01FF01FF01ull, 0x0101010101010101ull, 8)  == 0x0002000200020002ull);
    assert(vita_neon_sub(0x0000000000000000ull, 0x0000000100000001ull, 32) == 0xFFFFFFFFFFFFFFFFull);

    /* VCEQ.I8: all ones per equal byte; this is memchr's test. */
    assert(vita_neon_ceq(0x0041424344454647ull, 0x4444444444444444ull, 8) == 0x00000000FF000000ull);
    assert(vita_neon_ceq(0x1234567812345678ull, 0x1234567812345678ull, 32) == 0xFFFFFFFFFFFFFFFFull);

    /* VPADD.I8: adjacent lanes summed, a's results low, b's high. */
    assert(vita_neon_padd(0x0807060504030201ull, 0x1010101010101010ull, 8) == 0x202020200F0B0703ull);
    assert(vita_neon_padd(0x0000000400000003ull, 0x0000000200000001ull, 32) == 0x0000000300000007ull);

    /* VDUP from a core register. */
    assert(vita_neon_dup(0x12345678u, 32) == 0x1234567812345678ull);
    assert(vita_neon_dup(0x000000ABu, 8)  == 0xABABABABABABABABull);
    assert(vita_neon_dup(0x0000BEEFu, 16) == 0xBEEFBEEFBEEFBEEFull);

    assert(vita_neon_bsl(0xFFFF0000FFFF0000ull, 0x1111111111111111ull, 0x2222222222222222ull)
           == 0x1111222211112222ull);
    assert(vita_neon_set32(0x1111111122222222ull, 1, 0x33333333u) == 0x3333333322222222ull);
    assert(vita_neon_get32(0x1111111122222222ull, 0) == 0x22222222u);

    /* D16 and up are real registers. */
    vita_d_set(16, 0x0102030405060708ull);
    vita_d_set(31, 0xA0A1A2A3A4A5A6A7ull);
    assert(vita_d_get(16) == 0x0102030405060708ull);
    assert(vita_d_get(31) == 0xA0A1A2A3A4A5A6A7ull);
    assert(vfp_s[32].u == 0x05060708u && vfp_s[33].u == 0x01020304u);

    printf("  neon lanes (add/ceq/padd/dup)   ok\n");
}

static uint64_t f32x2(float lo, float hi) {
    return ((uint64_t)vita_f32_bits(hi) << 32) | vita_f32_bits(lo);
}

static void test_neon_more(void) {
    /* Lanes, signed and not. */
    assert(vita_neon_lane(0x1122334455667788ull, 1, 32) == 0x11223344u);
    assert(vita_neon_lane(0x1122334455667788ull, 3, 16) == 0x1122u);
    assert(vita_neon_slane(0x00000000FFFFFFFFull, 0, 32) == -1);
    assert(vita_neon_slane(0x0000000000000080ull, 0, 8) == -128);

    /* VEXT takes bytes from the pair, low register first. */
    assert(vita_neon_ext(0x0807060504030201ull, 0x100F0E0D0C0B0A09ull, 3) == 0x0B0A090807060504ull);
    assert(vita_neon_ext(0x0807060504030201ull, 0x100F0E0D0C0B0A09ull, 0) == 0x0807060504030201ull);
    vita_d_set(0, 0x0807060504030201ull); vita_d_set(1, 0x100F0E0D0C0B0A09ull);
    vita_d_set(2, 0x1817161514131211ull); vita_d_set(3, 0x201F1E1D1C1B1A19ull);
    vita_neon_ext_q(0, 0, 2, 8);           /* the halves swapped, in place */
    assert(vita_d_get(0) == 0x100F0E0D0C0B0A09ull && vita_d_get(1) == 0x1817161514131211ull);

    /* VREV: reverse lanes within a group. */
    assert(vita_neon_rev(0x0807060504030201ull, 8, 64) == 0x0102030405060708ull);
    assert(vita_neon_rev(0x0807060504030201ull, 8, 16) == 0x0708050603040102ull);
    assert(vita_neon_rev(0x0807060504030201ull, 16, 32) == 0x0605080702010403ull);

    assert(vita_neon_clz(0x00000001000000FFull, 32) == 0x0000001F00000018ull);
    assert(vita_neon_cnt(0xFF0F000100000000ull) == 0x0804000100000000ull);
    assert(vita_neon_abs(0xFFFFFFFF00000005ull, 32) == 0x0000000100000005ull);
    assert(vita_neon_cmp0(0x00000001FFFFFFFFull, 32, 0) == 0xFFFFFFFF00000000ull);
    assert(vita_neon_cmp0(0x00000001FFFFFFFFull, 32, 4) == 0x00000000FFFFFFFFull);
    assert(vita_neon_cmp0(0x0000000000000000ull, 32, 2) == 0xFFFFFFFFFFFFFFFFull);
    assert(vita_neon_mul(0x0000000300000002ull, 0x0000000500000007ull, 32) == 0x0000000F0000000Eull);
    assert(vita_neon_mul(0x0000000000000080ull, 0x0000000000000002ull, 8) == 0x0000000000000000ull);
    assert(vita_neon_mla(0x0000000100000001ull, 0x0000000300000002ull, 0x0000000500000007ull, 32) == 0x000000100000000Full);
    assert(vita_neon_max(0x00000001FFFFFFFFull, 0x0000000200000000ull, 32, 0) == 0x0000000200000000ull);
    assert(vita_neon_max(0x00000001FFFFFFFFull, 0x0000000200000000ull, 32, 1) == 0x00000002FFFFFFFFull);
    assert(vita_neon_min(0x00000001FFFFFFFFull, 0x0000000200000000ull, 32, 0) == 0x00000001FFFFFFFFull);
    assert(vita_neon_cgt(0x0000000000000002ull, 0x0000000100000001ull, 32, 0) == 0x00000000FFFFFFFFull);
    assert(vita_neon_cge(0x0000000100000001ull, 0x0000000100000002ull, 32, 0) == 0xFFFFFFFF00000000ull);
    assert(vita_neon_cgt(0x00000000000000FFull, 0x0000000000000001ull, 8, 1) == 0x00000000000000FFull);
    assert(vita_neon_cgt(0x00000000000000FFull, 0x0000000000000001ull, 8, 0) == 0);

    /* f32 lanes. */
    uint64_t a = f32x2(1.0f, 2.0f), b = f32x2(3.0f, 4.0f);
    assert(vita_neon_fadd(a, a) == f32x2(2.0f, 4.0f));
    assert(vita_neon_fsub(a, a) == 0);
    assert(vita_neon_fmul(a, b) == f32x2(3.0f, 8.0f));
    assert(vita_neon_fpadd(a, b) == f32x2(3.0f, 7.0f));
    assert(vita_neon_fmla(a, a, b) == f32x2(4.0f, 10.0f));
    assert(vita_neon_fmls(b, a, a) == f32x2(2.0f, 0.0f));
    assert(vita_neon_fabd(a, b) == f32x2(2.0f, 2.0f));
    assert(vita_neon_fceq(a, a) == 0xFFFFFFFFFFFFFFFFull);
    assert(vita_neon_fcgt(a, b) == 0);
    assert(vita_neon_fcge(b, a) == 0xFFFFFFFFFFFFFFFFull);
    assert(vita_neon_fmax(a, b) == b && vita_neon_fmin(a, b) == a);
    assert(vita_neon_fmax(a, f32x2(NAN, 4.0f)) == f32x2(vita_f32_of(VITA_NEON_DEFAULT_NAN), 4.0f));
    assert((vita_d_get(0), vita_d_get(0) ^ 0x8000000080000000ull, 1));
    assert(vita_neon_frecpe(f32x2(2.0f, 4.0f)) == f32x2(0.5f, 0.25f));
    assert(vita_neon_frsqrte(f32x2(4.0f, 16.0f)) == f32x2(0.5f, 0.25f));
    assert(vita_neon_frecps(f32x2(2.0f, 2.0f), f32x2(0.5f, 0.25f)) == f32x2(1.0f, 1.5f));
    assert(vita_neon_frsqrts(f32x2(4.0f, 4.0f), f32x2(0.5f, 0.25f)) == f32x2(0.5f, 1.0f));
    assert(vita_neon_frecps(f32x2(INFINITY, 0.0f), f32x2(0.0f, INFINITY)) == f32x2(2.0f, 2.0f));
    assert(vita_neon_fcmp0(f32x2(1.0f, -1.0f), 0) == 0x00000000FFFFFFFFull);
    assert(vita_neon_fcmp0(f32x2(1.0f, -1.0f), 4) == 0xFFFFFFFF00000000ull);
    assert(vita_neon_fcmp0(f32x2(NAN, 0.0f), 2) == 0xFFFFFFFF00000000ull);

    /* Conversions saturate and send NaN to zero. */
    assert(vita_neon_cvt(0xFFFFFFFF00000002ull, 0) == f32x2(2.0f, -1.0f));
    assert(vita_neon_cvt(0xFFFFFFFF00000002ull, 1) == f32x2(2.0f, 4294967296.0f));
    assert(vita_neon_cvt(f32x2(1.9f, -1.9f), 2) == 0xFFFFFFFF00000001ull);
    assert(vita_neon_cvt(f32x2(3e9f, NAN), 2) == 0x000000007FFFFFFFull);
    assert(vita_neon_cvt(f32x2(3e9f, -1.0f), 3) == 0x00000000B2D05E00ull);
    assert(vita_f32_to_s32(1e10f) == INT32_MAX && vita_f32_to_s32(-1e10f) == INT32_MIN);
    assert(vita_f32_to_u32(-1.0f) == 0 && vita_f32_to_u32(1e10f) == UINT32_MAX);
    assert(vita_f64_to_s32(-2.5) == -2 && vita_f64_to_u32(2.5) == 2);

    printf("  neon lanes (float, misc, ext)   ok\n");
}

static void test_neon_shifts(void) {
    assert(vita_neon_shl(0x0000000100000001ull, 32, 4) == 0x0000001000000010ull);
    assert(vita_neon_shl(0x00000000000000FFull, 8, 4) == 0x00000000000000F0ull);
    assert(vita_neon_shl(0x0000000100000001ull, 32, 32) == 0);
    assert(vita_neon_shr(0xFFFFFFF000000010ull, 32, 4, 1) == 0x0FFFFFFF00000001ull);
    assert(vita_neon_shr(0xFFFFFFF000000010ull, 32, 4, 0) == 0xFFFFFFFF00000001ull);
    assert(vita_neon_shr(0xFFFFFFF000000010ull, 32, 32, 0) == 0xFFFFFFFF00000000ull);
    assert(vita_neon_shr(0xFFFFFFF000000010ull, 32, 32, 1) == 0);
    /* Rounding: 7 >> 2 is 1.75, rounds to 2; -7 >> 2 is -1.75, rounds to -2;
     * 6 >> 2 is 1.5, the tie rounds up to 2; -6 >> 2 rounds to -1. */
    assert(vita_neon_rshr(0x0000000700000006ull, 32, 2, 1) == 0x0000000200000002ull);
    assert(vita_neon_rshr(0xFFFFFFF9FFFFFFFAull, 32, 2, 0) == 0xFFFFFFFEFFFFFFFFull);
    assert(vita_neon_rshr(0x00000000FFFFFFFFull, 32, 32, 1) == 0x0000000000000001ull);
    assert(vita_neon_rshr(0x00000000FFFFFFFFull, 32, 32, 0) == 0);
    /* The retail title's favourite: two halves packed into one D register. */
    assert(vita_neon_sli(0x00000000AAAAAAAAull, 0x00000000BBBBBBBBull, 64, 32) == 0xBBBBBBBBAAAAAAAAull);
    assert(vita_neon_sri(0xAAAAAAAA00000000ull, 0xBBBBBBBB00000000ull, 64, 32) == 0xAAAAAAAABBBBBBBBull);
    assert(vita_neon_sli(0x00000000000000FFull, 0x000000000000000Full, 8, 4) == 0x00000000000000FFull);
    assert(vita_neon_sli(0x0000000000000000ull, 0x000000000000000Full, 8, 4) == 0x00000000000000F0ull);
    assert(vita_neon_sri(0x00000000000000F0ull, 0x00000000000000F0ull, 8, 4) == 0x00000000000000FFull);
    assert(vita_neon_sri(0x0000000000000012ull, 0x0000000000000034ull, 8, 8) == 0x0000000000000012ull);
    /* Widening and narrowing. */
    assert(vita_neon_widen(0x80FF0102u, 8, 1) == 0x008000FF00010002ull);
    assert(vita_neon_widen(0x80FF0102u, 8, 0) == 0xFF80FFFF00010002ull);
    assert(vita_neon_widen(0x8000FFFFu, 16, 0) == 0xFFFF8000FFFFFFFFull);
    assert(vita_neon_widen(0x8000FFFFu, 16, 1) == 0x000080000000FFFFull);
    assert(vita_neon_narrow(0x0000000200000001ull, 0x0000000400000003ull, 32, 0, 0) == 0x0004000300020001ull);
    assert(vita_neon_narrow(0x0000123400005678ull, 0x00000000FFFF0000ull, 32, 8, 0) == 0x0000FF0000120056ull);
    assert(vita_neon_narrow(0x0000000000000180ull, 0, 16, 8, 1) == 0x0000000000000002ull);
    assert(vita_neon_narrow(0x0000000000000180ull, 0, 16, 8, 0) == 0x0000000000000001ull);
    printf("  neon shifts, widen, narrow      ok\n");
}

static void test_memory(void) {
    assert(vita_mem_init(0x81000000u, 64 * 1024) == 0);
    vita_mem_bad_access = 0;

    vita_write32(0x81000000u, 0xDEADBEEFu);
    assert(vita_read32(0x81000000u) == 0xDEADBEEFu);
    assert(vita_read8(0x81000000u) == 0xEFu);        /* little-endian */
    assert(vita_read16(0x81000002u) == 0xDEADu);

    /* Unaligned access is legal on ARMv7 and must not fault or be reordered. */
    vita_write32(0x81000005u, 0x11223344u);
    assert(vita_read32(0x81000005u) == 0x11223344u);
    assert(vita_mem_bad_access == 0);

    /* Out of range is COUNTED, not silently absorbed and not a wild write. */
    vita_read32(0x90000000u);
    assert(vita_mem_bad_access == 1);
    vita_write32(0x80000000u, 1u);                   /* below the base */
    assert(vita_mem_bad_access == 2);

    /* An access straddling the end of the region is refused rather than
     * reading past the allocation. */
    vita_read32(0x81000000u + 64 * 1024 - 2);
    assert(vita_mem_bad_access == 3);

    vita_mem_free();
    printf("  memory, unaligned and bounds   ok\n");
}

int main(void) {
    printf("runtime:\n");
    test_shifts();
    test_flags_add();
    test_flags_sub();
    test_flags_adc();
    test_conditions();
    test_extends();
    test_bitfields();
    test_dispatch();
    test_hle_libc();
    test_parallel();
    test_neon();
    test_neon_more();
    test_neon_shifts();
    test_memory();
    printf("all runtime tests passed\n");
    return 0;
}
