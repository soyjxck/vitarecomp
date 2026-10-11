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
#include "vitarecomp/host.h"
#include "../src/hle_format.h"
#include "../src/sched.h"
#include "../src/clock.h"
#include <string.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/* The host contract (host.h), as the tests stand in for a host: the HLE
 * reaches these from exit paths and vblank waits. The finger is the touch
 * test's to put down and lift. */
static int g_finger, g_finger_x, g_finger_y;
void vita_host_vblank(void) { }
uint32_t vita_host_buttons(void) { return 0; }
int vita_host_touch(int *x, int *y) {
    if (!g_finger) return 0;
    *x = g_finger_x; *y = g_finger_y;
    return 1;
}
void vita_host_exit(int status) { exit(status); }

/* The generated code's variable-import table, with one entry the test binds:
 * the C library's lowercase table, read through a word at 0x81001000. */
static uint32_t g_var_tolotab;
static const vita_var_site g_var_words[] = { { 0x81001000u, 0u }, { 0x81001004u, 8u } };
const vita_var_import vita_var_imports[] = {
    { 0xBE43BB07u, 0xD662E07Cu, "SceLibc::_Tolotab", &g_var_tolotab, g_var_words, 2u },
};
const uint32_t vita_var_import_count = 1u;

static void test_scanf(void) {
    assert(vita_mem_init(0x81000000u, 64 * 1024) == 0);
    const char *text = "947.2009911,-100 947.1035061,-99.2 TAP,TAP,1,0,1,,0.1 0x1F end";
    const char *fmt  = "%f,%f %f,%f %[^,],%[^,],%d,%d,%d,,%f %i %s";
    for (uint32_t i = 0; text[i]; i++) vita_write8(0x81001000u + i, (uint8_t)text[i]);
    vita_write8(0x81001000u + (uint32_t)strlen(text), 0);
    for (uint32_t i = 0; fmt[i]; i++) vita_write8(0x81001200u + i, (uint8_t)fmt[i]);
    vita_write8(0x81001200u + (uint32_t)strlen(fmt), 0);
    /* the va_list: twelve pointers in guest memory */
    uint32_t args = 0x81001400u, out = 0x81001500u;
    for (uint32_t i = 0; i < 12; i++) vita_write32(args + 4 * i, out + 0x20 * i);
    vita_vargs v = { 0, 0, args };
    int n = vita_guest_vscanf(0x81001000u, 0x81001200u, &v);
    assert(n == 12);
    union { float f; uint32_t u; } f0, f1, f2, f3, f9;
    f0.u = vita_read32(out); f1.u = vita_read32(out + 0x20); f2.u = vita_read32(out + 0x40); f3.u = vita_read32(out + 0x60);
    assert(f0.f > 947.2f && f0.f < 947.21f && f1.f == -100.0f && f2.f > 947.1f && f2.f < 947.11f && f3.f == -99.2f);
    assert(vita_read8(out + 0x80) == 'T' && vita_read8(out + 0x83) == 0);
    assert(vita_read8(out + 0xA0) == 'T');
    assert(vita_read32(out + 0xC0) == 1 && vita_read32(out + 0xE0) == 0 && vita_read32(out + 0x100) == 1);
    f9.u = vita_read32(out + 0x120);
    assert(f9.f > 0.09f && f9.f < 0.11f);
    assert(vita_read32(out + 0x140) == 0x1F);
    assert(vita_read8(out + 0x160) == 'e' && vita_read8(out + 0x163) == 0);
    /* input exhausted before the first conversion: EOF */
    vita_write8(0x81001000u, 0);
    v.mem = args;
    assert(vita_guest_vscanf(0x81001000u, 0x81001200u, &v) == -1);
    printf("  scanf                          ok\n");
}

static void test_var_imports(void) {
    assert(vita_mem_init(0x81000000u, 64 * 1024) == 0);
    vita_memblock_init(0x81008000u, 0x8000u);
    vita_write32(0x81001000u, 0x12345678u);           /* the placeholder */
    vita_vars_bind();
    assert(g_var_tolotab != 0);
    assert(vita_read32(0x81001000u) == g_var_tolotab);
    assert(vita_read32(0x81001004u) == g_var_tolotab + 8);   /* a vtable pointer's addend */
    uint32_t tab = vita_read32(g_var_tolotab);         /* the short * the variable holds */
    assert((int16_t)vita_read16(tab + 2u * 'A') == 'a');
    assert((int16_t)vita_read16(tab + 2u * 'z') == 'z');
    assert((int16_t)vita_read16(tab + 2u * '/') == '/');
    assert((int16_t)vita_read16(tab - 2u) == -1);       /* EOF, one before the table */
    printf("  variable imports                ok\n");
}

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

    /* The function an address falls in: the entry at or below it. */
    assert(vita_dispatch_floor(0x81000100u) == 0x81000100u);
    assert(vita_dispatch_floor(0x810001FEu) == 0x81000100u);
    assert(vita_dispatch_floor(0x81000201u) == 0x81000200u);
    assert(vita_dispatch_floor(0x8FFFFFFFu) == 0x81000300u);
    assert(vita_dispatch_floor(0x810000FEu) == 0);

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

    assert(vita_sxtb16(0x00FF0080u) == 0xFFFFFF80u);
    assert(vita_sxtb16(0x007F0001u) == 0x007F0001u);
    assert(vita_uxtb16(0x12345678u) == 0x00340078u);
    assert(vita_add16(0xFFFF0001u, 0x00010001u) == 0x00000002u);

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
    /* VTRN: two registers as the rows of a matrix, transposed. In 32-bit
     * lanes d = {a0, a1}, m = {b0, b1} become {a0, b0} and {a1, b1}. */
    vita_d_set(4, 0xA1A1A1A1A0A0A0A0ull); vita_d_set(5, 0xB1B1B1B1B0B0B0B0ull);
    vita_neon_trn(4, 5, 32);
    assert(vita_d_get(4) == 0xB0B0B0B0A0A0A0A0ull && vita_d_get(5) == 0xB1B1B1B1A1A1A1A1ull);
    vita_d_set(4, 0x0706050403020100ull); vita_d_set(5, 0x1716151413121110ull);
    vita_neon_trn(4, 5, 8);
    assert(vita_d_get(4) == 0x1606140412021000ull && vita_d_get(5) == 0x1707150513031101ull);

    /* VTBL: bytes looked up in a table of registers; beyond it, zero, or
     * with VTBX the byte that was there. */
    vita_d_set(8, 0x0706050403020100ull); vita_d_set(9, 0x0F0E0D0C0B0A0908ull);
    assert(vita_neon_tbl(0x5555555555555555ull, 8, 2, 0xFF100F0800010207ull, 0) == 0x00000F0800010207ull);
    assert(vita_neon_tbl(0x5555555555555555ull, 8, 2, 0xFF100F0800010207ull, 1) == 0x55550F0800010207ull);
    assert(vita_neon_tbl(0, 8, 1, 0x0807060504030201ull, 0) == 0x0007060504030201ull);

    /* VPADDL: neighbours added into a lane twice as wide, so nothing is lost. */
    assert(vita_neon_paddl(0xFFFF000102030405ull, 8, 1) == 0x01FE000100050009ull);
    assert(vita_neon_paddl(0xFFFF000102030405ull, 8, 0) == 0xFFFE000100050009ull);
    assert(vita_neon_paddl(0xFFFFFFFF00000002ull, 32, 1) == 0x0000000100000001ull);
    assert(vita_neon_paddl(0xFFFFFFFF00000002ull, 32, 0) == 0x0000000000000001ull);

    /* VCVT, four singles to four halves and back: 1, -2, 0.5 and 65536, which
     * no half holds and which becomes infinity. */
    assert(vita_neon_halve(0xC00000003F800000ull, 0x478000003F000000ull) == 0x7C003800C0003C00ull);
    vita_neon_unhalve(10, 0x7C003800C0003C00ull);
    assert(vita_d_get(10) == 0xC00000003F800000ull && vita_d_get(11) == 0x7F8000003F000000ull);

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

/* --- the audio synthesiser --------------------------------------------------------
 *
 * SceNgs with no sound: what a sound engine reads back has to be what it
 * wrote, and a voice has to last as long as its sample does. The first
 * version returned a new record from every lock and every voice-handle
 * query, and a retail engine's start-up filled the arena with them.
 */
void vita_hle_sceNgsSystemGetRequiredMemorySize(void);
void vita_hle_sceNgsSystemInit(void);
void vita_hle_sceNgsSystemUpdate(void);
void vita_hle_sceNgsRackGetRequiredMemorySize(void);
void vita_hle_sceNgsRackInit(void);
void vita_hle_sceNgsRackGetVoiceHandle(void);
void vita_hle_sceNgsVoiceDefGetScreamVoice(void);
void vita_hle_sceNgsVoiceDefGetScreamAtrac9Voice(void);
void vita_hle_sceNgsVoiceLockParams(void);
void vita_hle_sceNgsVoiceUnlockParams(void);
void vita_hle_sceNgsVoiceSetFinishedCallback(void);
void vita_hle_sceNgsVoicePlay(void);
void vita_hle_sceNgsVoiceKeyOff(void);
void vita_hle_sceNgsVoiceGetInfo(void);
void vita_hle_sceNgsVoiceGetStateData(void);
void vita_hle_sceNgsPatchCreateRouting(void);
void vita_hle_sceNgsPatchGetInfo(void);
void vita_hle_sceNgsPatchRemoveRouting(void);
void vita_hle_sceNgsAT9GetSectionDetails(void);

#define NB 0x81000000u

static int      g_finished;
static uint32_t g_finished_voice, g_finished_user;
static void finished_entry(void) {
    g_finished++;
    g_finished_voice = vita_read32(r0);
    g_finished_user = vita_read32(r0 + 24);
}

static uint32_t ngs_voice_state(uint32_t voice) {
    r0 = voice; r1 = NB + 0x1500; vita_hle_sceNgsVoiceGetInfo();
    assert(r0 == 0);
    return vita_read32(NB + 0x1500);
}

static void ngs_update(uint32_t sys, int times) {
    for (int i = 0; i < times; i++) { r0 = sys; vita_hle_sceNgsSystemUpdate(); assert(r0 == 0); }
}

static void test_ngs(void) {
    assert(vita_mem_init(NB, 0x100000) == 0);
    vita_memblock_init(NB + 0x80000, 0x10000);
    vita_mem_bad_access = 0;
    static const vita_dispatch_entry table[] = { { NB + 0x500, finished_entry } };
    vita_dispatch_init(table, 1);
    sp = NB + 0x70000;

    /* A system of 512-sample updates at 48 kHz, in memory the title owns. */
    uint32_t params = NB + 0x1000;
    vita_write32(params, 8); vita_write32(params + 4, 64); vita_write32(params + 8, 512);
    vita_write32(params + 12, 48000); vita_write32(params + 16, 0);
    r0 = params; r1 = NB + 0x1100; vita_hle_sceNgsSystemGetRequiredMemorySize();
    assert(r0 == 0 && vita_read32(NB + 0x1100) != 0);
    r0 = NB + 0x10000; r1 = vita_read32(NB + 0x1100); r2 = params; r3 = NB + 0x1104;
    vita_hle_sceNgsSystemInit();
    assert(r0 == 0);
    uint32_t sys = vita_read32(NB + 0x1104);
    assert(sys == NB + 0x10000);

    /* The definitions are distinct, and the same each time they are asked for. */
    vita_hle_sceNgsVoiceDefGetScreamVoice();
    uint32_t def = r0;
    vita_hle_sceNgsVoiceDefGetScreamAtrac9Voice();
    assert(def && r0 && r0 != def);
    vita_hle_sceNgsVoiceDefGetScreamVoice();
    assert(r0 == def);

    /* A rack of four voices in a block of the size the runtime asked for. */
    uint32_t desc = NB + 0x1200;
    vita_write32(desc, def); vita_write32(desc + 4, 4); vita_write32(desc + 8, 1);
    vita_write32(desc + 12, 0); vita_write32(desc + 16, 2); vita_write32(desc + 20, 0x1234);
    r0 = sys; r1 = desc; r2 = NB + 0x1300; vita_hle_sceNgsRackGetRequiredMemorySize();
    assert(r0 == 0);
    uint32_t need = vita_read32(NB + 0x1300);
    assert(need > 4 * 84 && need < 0x10000);
    vita_write32(NB + 0x1310, NB + 0x20000); vita_write32(NB + 0x1314, need - 16);
    r0 = sys; r1 = NB + 0x1310; r2 = desc; r3 = NB + 0x1320; vita_hle_sceNgsRackInit();
    assert(r0 != 0);                                    /* a block too small is refused */
    vita_write32(NB + 0x1314, need);
    r0 = sys; r1 = NB + 0x1310; r2 = desc; r3 = NB + 0x1320; vita_hle_sceNgsRackInit();
    assert(r0 == 0);
    uint32_t rack = vita_read32(NB + 0x1320);
    assert(rack == NB + 0x20000);

    /* A voice has one handle. */
    r0 = rack; r1 = 1; r2 = NB + 0x1400; vita_hle_sceNgsRackGetVoiceHandle();
    assert(r0 == 0);
    uint32_t voice = vita_read32(NB + 0x1400);
    r0 = rack; r1 = 1; r2 = NB + 0x1404; vita_hle_sceNgsRackGetVoiceHandle();
    assert(r0 == 0 && vita_read32(NB + 0x1404) == voice);
    r0 = rack; r1 = 2; r2 = NB + 0x1404; vita_hle_sceNgsRackGetVoiceHandle();
    uint32_t other = vita_read32(NB + 0x1404);
    assert(other != voice && other >= rack && other < rack + need);
    r0 = rack; r1 = 4; r2 = NB + 0x1404; vita_hle_sceNgsRackGetVoiceHandle();
    assert(r0 != 0);

    /* The player's block: locked, written, and still there at the next lock. */
    r0 = voice; r1 = 0; r2 = 0x01015CE6u; r3 = NB + 0x1410; vita_hle_sceNgsVoiceLockParams();
    assert(r0 == 0);
    uint32_t block = vita_read32(NB + 0x1410), block_size = vita_read32(NB + 0x1414);
    assert(block >= rack && block + block_size <= rack + need && block_size >= 84);
    assert(vita_read32(block) == 0x01015CE6u);
    r0 = voice; r1 = 0; r2 = 0x01015CE6u; r3 = NB + 0x1418; vita_hle_sceNgsVoiceLockParams();
    assert(r0 != 0);                                    /* locked already */
    vita_write32(block + 4, 84);
    vita_write32(block + 8, NB + 0x30000);              /* buffer 0: 4096 bytes, played once */
    vita_write32(block + 12, 4096);
    vita_write16(block + 16, 0);
    vita_write16(block + 18, 0xFFFF);
    { float hz = 48000.0f, one = 1.0f; uint32_t u;
      memcpy(&u, &hz, 4); vita_write32(block + 56, u);
      memcpy(&u, &one, 4); vita_write32(block + 60, u); }
    vita_write8(block + 76, 1);                         /* mono, 16-bit PCM */
    r0 = voice; r1 = 0; vita_hle_sceNgsVoiceUnlockParams();
    assert(r0 == 0);
    r0 = voice; r1 = 0; r2 = 0x01015CE6u; r3 = NB + 0x1418; vita_hle_sceNgsVoiceLockParams();
    assert(r0 == 0 && vita_read32(NB + 0x1418) == block && vita_read32(block + 12) == 4096);
    r0 = voice; r1 = 0; vita_hle_sceNgsVoiceUnlockParams();
    r0 = other; r1 = 0; r2 = 0x01015CE6u; r3 = NB + 0x1418; vita_hle_sceNgsVoiceLockParams();
    assert(r0 == 0 && vita_read32(NB + 0x1418) != block);   /* each voice has its own */
    r0 = other; r1 = 0; vita_hle_sceNgsVoiceUnlockParams();

    /* 4096 bytes of mono PCM are 2048 samples: four updates of 512. */
    r0 = voice; r1 = NB + 0x501; r2 = 0xCAFE; vita_hle_sceNgsVoiceSetFinishedCallback();
    assert(r0 == 0 && ngs_voice_state(voice) == 0);
    r0 = voice; vita_hle_sceNgsVoicePlay();
    assert(r0 == 0 && ngs_voice_state(voice) == 1);
    ngs_update(sys, 3);
    assert(g_finished == 0 && ngs_voice_state(voice) == 1);
    r0 = voice; r1 = 0; r2 = NB + 0x1600; r3 = 24; vita_hle_sceNgsVoiceGetStateData();
    assert(r0 == 0 && vita_read32(NB + 0x1600) == 3072 && vita_read32(NB + 0x1608) == 1536);
    ngs_update(sys, 1);
    assert(g_finished == 1 && g_finished_voice == voice && g_finished_user == 0xCAFE);
    assert(ngs_voice_state(voice) == 0);
    ngs_update(sys, 2);
    assert(g_finished == 1);                            /* and it does not finish twice */

    /* Looped once, the same sample lasts eight updates. */
    vita_write16(block + 16, 1);
    r0 = voice; vita_hle_sceNgsVoicePlay();
    ngs_update(sys, 7);
    assert(g_finished == 1);
    ngs_update(sys, 1);
    assert(g_finished == 2);

    /* A key-off ends a voice at the next update, whatever is left of it. */
    r0 = voice; vita_hle_sceNgsVoicePlay();
    ngs_update(sys, 1);
    r0 = voice; vita_hle_sceNgsVoiceKeyOff();
    assert(r0 == 0 && ngs_voice_state(voice) == 0x41 && g_finished == 2);
    ngs_update(sys, 1);
    assert(g_finished == 3 && ngs_voice_state(voice) == 0);

    /* A patch from one voice's output to another's input. */
    uint32_t setup = NB + 0x1700;
    vita_write32(setup, voice); vita_write32(setup + 4, 1); vita_write32(setup + 8, 0xFFFFFFFFu);
    vita_write32(setup + 12, other); vita_write32(setup + 16, 0);
    r0 = setup; r1 = NB + 0x1720; vita_hle_sceNgsPatchCreateRouting();
    assert(r0 == 0);
    uint32_t patch = vita_read32(NB + 0x1720);
    assert(patch >= rack && patch < rack + need);
    r0 = patch; r1 = NB + 0x1740; r2 = NB + 0x1760; vita_hle_sceNgsPatchGetInfo();
    assert(r0 == 0 && vita_read32(NB + 0x1760) == voice && vita_read32(NB + 0x1764) == 1);
    assert(vita_read32(NB + 0x1768) == 0 && vita_read32(NB + 0x176C) == other);
    r0 = setup; r1 = NB + 0x1724; vita_hle_sceNgsPatchCreateRouting();
    assert(r0 == 0 && vita_read32(NB + 0x1724) != patch);   /* the second free slot */
    r0 = setup; r1 = NB + 0x1728; vita_hle_sceNgsPatchCreateRouting();
    assert(r0 != 0);                                    /* two patches an output, and both taken */
    r0 = patch; vita_hle_sceNgsPatchRemoveRouting();
    assert(r0 == 0);
    r0 = patch; r1 = NB + 0x1740; r2 = NB + 0x1760; vita_hle_sceNgsPatchGetInfo();
    assert(r0 != 0);

    /* ATRAC9: 96-byte frames of 128 samples. 1024 samples from the start are
     * nine frames, the first of them the decoder's delay; in super packets
     * of four frames they are three packets, with 128 to drop at the start
     * and 384 at the end. */
    uint32_t config = 0xFEu | (4u << 12) | (0x0Bu << 16) | (7u << 29);
    r0 = 0; r1 = 1024; r2 = config; r3 = NB + 0x1800; vita_hle_sceNgsAT9GetSectionDetails();
    assert(r0 == 0 && vita_read32(NB + 0x1800) == 0 && vita_read32(NB + 0x1804) == 9 * 96);
    assert(vita_read16(NB + 0x1808) == 128 && vita_read16(NB + 0x180A) == 0 && vita_read32(NB + 0x180C) == 0);
    r0 = 0; r1 = 1024; r2 = config | (1u << 27); r3 = NB + 0x1800; vita_hle_sceNgsAT9GetSectionDetails();
    assert(r0 == 0 && vita_read32(NB + 0x1800) == 0 && vita_read32(NB + 0x1804) == 3 * 384);
    assert(vita_read16(NB + 0x1808) == 128 && vita_read16(NB + 0x180A) == 384 && vita_read32(NB + 0x180C) == 1);
    r0 = 0; r1 = 1024; r2 = 0x12345678u; r3 = NB + 0x1800; vita_hle_sceNgsAT9GetSectionDetails();
    assert(r0 != 0);

    assert(vita_mem_bad_access == 0);
    vita_mem_free();
    printf("  audio synthesiser, silent      ok\n");
}

/* --- the time slice -------------------------------------------------------------
 *
 * One guest thread runs at a time, and a thread that waits for another by
 * spinning has to give way or it waits for ever. Two ways it does: a loop
 * that only waits calls vita_spin_wait and the other thread goes at once,
 * and any loop counts its backward branches against a slice. The worker
 * here stands in for generated code: it is reached through the dispatch
 * table and makes the calls the emitter would have written into it.
 */
static volatile int g_gate;
static int g_worker_at;

static void worker_entry(void) {
    g_worker_at = 1;
    while (!g_gate) vita_spin_wait(0x81000400u);
    g_worker_at = 2;
    for (int i = 0; i < 3 * VITA_SLICE + 10; i++) vita_back_edge();
    g_worker_at = 3;
}

static void test_time_slice(void) {
    assert(vita_mem_init(0x81000000u, 64 * 1024) == 0);
    vita_memblock_init(0x81008000u, 0x8000u);
    static const vita_dispatch_entry table[] = { { 0x81000400u, worker_entry } };
    vita_dispatch_init(table, 1);

    /* A started thread runs first. This one spins on the gate, which only
     * the thread that started it can open: the spin hands the baton back,
     * or this line is never reached. */
    uint32_t uid = vita_thread_create("spinner", 0x81000401u, 0x10000100u, 0x1000);
    assert(uid != 0);
    assert(vita_thread_start(uid, 0, 0) == 0);
    assert(g_worker_at == 1);

    /* Now this thread computes without a kernel call. Each slice it uses up
     * gives the worker one of its own; the worker needs more than three. */
    g_gate = 1;
    int turns = 0;
    while (g_worker_at != 3) {
        for (int i = 0; i <= VITA_SLICE; i++) vita_back_edge();
        turns++;
        assert(turns < 100);
    }
    assert(turns >= 3);

    assert(vita_thread_wait_end(uid, 0, NULL) == 0);
    assert(vita_thread_delete(uid) == 0);
    vita_mem_free();
    printf("  time slice and spin wait       ok\n");
}

static void test_half_precision(void) {
    assert(vita_f32_to_f16(1.0f) == 0x3C00);
    assert(vita_f32_to_f16(-2.0f) == 0xC000);
    assert(vita_f32_to_f16(65504.0f) == 0x7BFF);
    assert(vita_f32_to_f16(65536.0f) == 0x7C00);            /* overflow to infinity */
    assert(vita_f32_to_f16(0.0f) == 0x0000);
    assert(vita_f32_to_f16(5.9604645e-8f) == 0x0001);        /* the smallest subnormal */
    assert(vita_f32_to_f16(0.333251953125f) == 0x3555);
    assert(vita_f16_to_f32(0x3C00) == 1.0f);
    assert(vita_f16_to_f32(0xC000) == -2.0f);
    assert(vita_f16_to_f32(0x7BFF) == 65504.0f);
    assert(vita_f16_to_f32(0x0001) == 5.9604645e-8f);
    assert(vita_f16_to_f32(0x3555) == 0.333251953125f);
    assert(vita_f16_to_f32(0x8000) == 0.0f);
    { float n = vita_f16_to_f32(0x7E00); assert(n != n); }   /* NaN stays NaN */
    printf("  half precision                 ok\n");
}

static void test_neon_structures(void) {
    assert(vita_mem_init(0x81000000u, 64 * 1024) == 0);
    assert(vita_neon_set_lane(0x1111111111111111ull, 2, 0xAB, 8) == 0x1111111111AB1111ull);
    assert(vita_neon_set_lane(0, 1, 0xDEADBEEF, 32) == 0xDEADBEEF00000000ull);
    /* vld2.8 {d0, d1}: even bytes to d0, odd to d1; vst2 puts them back */
    for (uint32_t i = 0; i < 16; i++) vita_write8(0x81002000u + i, (uint8_t)i);
    vita_neon_ldn(2, 0, 1, 1, 0x81002000u, 8);
    assert(vita_d_get(0) == 0x0E0C0A0806040200ull);
    assert(vita_d_get(1) == 0x0F0D0B0907050301ull);
    vita_neon_stn(2, 0, 1, 1, 0x81002100u, 8);
    for (uint32_t i = 0; i < 16; i++) assert(vita_read8(0x81002100u + i) == (uint8_t)i);
    /* vld4.32 {d0, d2, d4, d6}: words 0,4 to d0; 1,5 to d2; ... */
    for (uint32_t i = 0; i < 8; i++) vita_write32(0x81002200u + 4 * i, 0x100 + i);
    vita_neon_ldn(4, 0, 2, 1, 0x81002200u, 32);
    assert(vita_d_get(0) == 0x0000010400000100ull && vita_d_get(2) == 0x0000010500000101ull);
    assert(vita_d_get(4) == 0x0000010600000102ull && vita_d_get(6) == 0x0000010700000103ull);
    printf("  neon structure loads           ok\n");
}

static void test_neon_shift_by_register(void) {
    /* 32-bit lanes: the low one left by 4, the high one right by 4. */
    uint64_t by = 0x000000FC00000004ull;
    assert(vita_neon_shl_reg(0x8000001000000001ull, by, 32, 0) == 0xF800000100000010ull);
    assert(vita_neon_shl_reg(0x8000001000000001ull, by, 32, 1) == 0x0800000100000010ull);
    /* Only the count's low byte is read, as a signed one. */
    assert(vita_neon_shl_reg(0x0000000000000003ull, 0x00000000FFFFFF01ull, 32, 1) == 0x0000000000000006ull);
    /* A count as wide as the lane: nothing left, or the sign. */
    assert(vita_neon_shl_reg(0x0000000100000001ull, 0x0000002000000020ull, 32, 0) == 0);
    assert(vita_neon_shl_reg(0x80000000FFFFFFFFull, 0x000000E0000000E0ull, 32, 1) == 0);
    assert(vita_neon_shl_reg(0x8000000070000000ull, 0x000000E0000000E0ull, 32, 0) == 0xFFFFFFFF00000000ull);
    /* Bytes and a whole doubleword. */
    assert(vita_neon_shl_reg(0x0000000000008001ull, 0x000000000000FF01ull, 8, 0) == 0x000000000000C002ull);
    assert(vita_neon_shl_reg(0x8000000000000000ull, 0x00000000000000C1ull, 64, 0) == 0xFFFFFFFFFFFFFFFFull);
    assert(vita_neon_shl_reg(0x8000000000000000ull, 0x00000000000000C1ull, 64, 1) == 0x0000000000000001ull);
    printf("  neon shift by register         ok\n");
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

void vita_hle_sceTouchPeek(void);
void vita_hle_sceTouchSetSamplingState(void);
void vita_hle_sceTouchGetPanelInfo(void);

/* The front panel reports what the host says is on it, once it has been
 * told to sample; a contact keeps its number while it lasts. */
static void test_touch(void) {
    assert(vita_mem_init(0x81000000u, 64 * 1024) == 0);
    vita_mem_bad_access = 0;
    uint32_t data = 0x81001000u;

    /* not sampling: no finger is reported, though there is one */
    g_finger = 1; g_finger_x = 961; g_finger_y = 681;
    r0 = 0; r1 = data; r2 = 1; vita_hle_sceTouchPeek();
    assert(r0 == 1 && vita_read32(data + 12) == 0);

    r0 = 0; r1 = 1; vita_hle_sceTouchSetSamplingState();
    assert(r0 == 0);
    r0 = 0; r1 = data; r2 = 2; vita_hle_sceTouchPeek();
    assert(r0 == 2);
    for (uint32_t rec = data; rec < data + 0x120; rec += 0x90) {
        assert(vita_read32(rec + 12) == 1);
        assert(vita_read16(rec + 18) == 961 && vita_read16(rec + 20) == 681);
        assert(vita_read8(rec + 17) != 0);
    }
    uint8_t first = vita_read8(data + 16);

    /* held and moved it is the same contact; lifted and put down, another */
    g_finger_x = 1000;
    r0 = 0; r1 = data; r2 = 1; vita_hle_sceTouchPeek();
    assert(vita_read32(data + 12) == 1 && vita_read8(data + 16) == first && vita_read16(data + 18) == 1000);
    g_finger = 0;
    r0 = 0; r1 = data; r2 = 1; vita_hle_sceTouchPeek();
    assert(r0 == 1 && vita_read32(data + 12) == 0);
    g_finger = 1;
    r0 = 0; r1 = data; r2 = 1; vita_hle_sceTouchPeek();
    assert(vita_read32(data + 12) == 1 && vita_read8(data + 16) != first);

    /* nothing touches the back, and there is no third panel */
    r0 = 1; r1 = 1; vita_hle_sceTouchSetSamplingState();
    assert(r0 == 0);
    r0 = 1; r1 = data; r2 = 1; vita_hle_sceTouchPeek();
    assert(r0 == 1 && vita_read32(data + 12) == 0);
    r0 = 2; r1 = data; r2 = 1; vita_hle_sceTouchPeek();
    assert((int32_t)r0 < 0);

    /* the panel is twice the display each way */
    r0 = 0; r1 = data; vita_hle_sceTouchGetPanelInfo();
    assert(r0 == 0 && vita_read16(data + 4) == 1919 && vita_read16(data + 6) == 1087);

    g_finger = 0;
    assert(vita_mem_bad_access == 0);
    vita_mem_free();
    printf("  touch panel                    ok\n");
}

void vita_hle_sceAppUtilSaveDataSlotCreate(void);
void vita_hle_sceAppUtilSaveDataSlotGetParam(void);
void vita_hle_sceSaveDataDialogInit(void);
void vita_hle_sceSaveDataDialogContinue(void);
void vita_hle_sceSaveDataDialogFinish(void);
void vita_hle_sceSaveDataDialogSubClose(void);
void vita_hle_sceSaveDataDialogGetStatus(void);
void vita_hle_sceSaveDataDialogGetSubStatus(void);
void vita_hle_sceSaveDataDialogGetResult(void);
void vita_hle_sceSaveDataDialogTerm(void);

/* The save data dialog, driven as a title drives it: a list to choose a
 * slot from, a "saving" screen the title closes itself, and the end. Nobody
 * answers the list; it is answered for them, with the slot it had focused,
 * once it has been up for half a second of the title's time. This test is
 * the last, because it makes time the program's own. */
static void test_save_dialog(void) {
    assert(vita_mem_init(0x81000000u, 64 * 1024) == 0);
    vita_mem_bad_access = 0;
    vita_clock_virtual();
    uint32_t param = 0x81001000u, list_param = 0x81001100u, list = 0x81001180u, sys_param = 0x81001200u;
    uint32_t result = 0x81001300u, info = 0x81001380u, finish = 0x81001400u, record = 0x81002000u, got = 0x81003000u;

    /* slot 2 exists, with a record that can be told from zeros; slot 3 does not */
    vita_write16(record + 4, 'A');
    r0 = 2; r1 = record; r2 = 0; vita_hle_sceAppUtilSaveDataSlotCreate();
    assert(r0 == 0);
    r0 = 2; r1 = record; r2 = 0; vita_hle_sceAppUtilSaveDataSlotCreate();
    assert((int32_t)r0 < 0);                                    /* it is there already */
    r0 = 3; r1 = got; r2 = 0; vita_hle_sceAppUtilSaveDataSlotGetParam();
    assert((int32_t)r0 < 0);
    r0 = 2; r1 = got; r2 = 0; vita_hle_sceAppUtilSaveDataSlotGetParam();
    assert(r0 == 0 && vita_read16(got + 4) == 'A' && vita_read16(got + 0x30C) >= 2025);    /* and dated */

    /* a list of slots 2 and 3 to save into, the first empty one focused */
    vita_write32(list, 2); vita_write32(list + 16, 3);
    vita_write32(list_param, list); vita_write32(list_param + 4, 2); vita_write32(list_param + 8, 2);
    vita_write32(param + 0x50, 2); vita_write32(param + 0x54, 1); vita_write32(param + 0x5C, list_param);
    vita_write32(param + 0x78, 0x1234u);
    vita_hle_sceSaveDataDialogGetStatus();
    assert(r0 == 0);                                            /* no dialog yet */
    r0 = param; vita_hle_sceSaveDataDialogInit();
    assert(r0 == 0);
    r0 = param; vita_hle_sceSaveDataDialogInit();
    assert((int32_t)r0 < 0);                                    /* one at a time */
    vita_hle_sceSaveDataDialogGetStatus();    assert(r0 == 1);
    vita_hle_sceSaveDataDialogGetSubStatus(); assert(r0 == 1);
    vita_write32(result + 16, info);
    r0 = result; vita_hle_sceSaveDataDialogGetResult();
    assert((int32_t)r0 < 0);                                    /* not answered yet */
    r0 = finish; vita_hle_sceSaveDataDialogFinish();
    assert((int32_t)r0 < 0);

    vita_clock_wait(600000);
    vita_hle_sceSaveDataDialogGetSubStatus(); assert(r0 == 2);
    vita_hle_sceSaveDataDialogGetStatus();    assert(r0 == 1);
    r0 = result; vita_hle_sceSaveDataDialogGetResult();
    assert(r0 == 0 && vita_read32(result) == 2 && vita_read32(result + 4) == 0);
    assert(vita_read32(result + 12) == 3 && vita_read32(result + 20) == 0x1234u);
    assert(vita_read32(result + 16) == info && vita_read32(info) == 0);     /* the slot chosen is empty */

    /* "saving", which has no buttons: up until the title takes it down */
    vita_write32(sys_param, 5); vita_write32(sys_param + 8, 3);
    vita_write32(param + 0x50, 4); vita_write32(param + 0x54, 0); vita_write32(param + 0x64, sys_param);
    r0 = param; vita_hle_sceSaveDataDialogContinue();
    assert(r0 == 0);
    vita_clock_wait(5000000);
    vita_hle_sceSaveDataDialogGetSubStatus(); assert(r0 == 1);
    vita_hle_sceSaveDataDialogSubClose();     assert(r0 == 0);
    vita_hle_sceSaveDataDialogGetSubStatus(); assert(r0 == 2);

    /* "saved", with an OK button: answered in its turn */
    vita_write32(sys_param, 6);
    r0 = param; vita_hle_sceSaveDataDialogContinue();
    assert(r0 == 0);
    vita_hle_sceSaveDataDialogGetSubStatus(); assert(r0 == 1);
    vita_clock_wait(600000);
    vita_hle_sceSaveDataDialogGetSubStatus(); assert(r0 == 2);
    r0 = result; vita_hle_sceSaveDataDialogGetResult();
    assert(r0 == 0 && vita_read32(result) == 4 && vita_read32(result + 8) == 1);

    vita_hle_sceSaveDataDialogTerm();
    assert((int32_t)r0 < 0);                                    /* not before it is finished */
    r0 = finish; vita_hle_sceSaveDataDialogFinish();
    assert(r0 == 0);
    vita_hle_sceSaveDataDialogGetStatus();    assert(r0 == 2);
    vita_hle_sceSaveDataDialogTerm();         assert(r0 == 0);
    vita_hle_sceSaveDataDialogGetStatus();    assert(r0 == 0);

    assert(vita_mem_bad_access == 0);
    vita_mem_free();
    printf("  save data slots and dialog     ok\n");
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
    test_neon_shift_by_register();
    test_neon_structures();
    test_half_precision();
    test_memory();
    test_var_imports();
    test_scanf();
    test_time_slice();
    test_ngs();
    test_touch();
    test_save_dialog();
    printf("all runtime tests passed\n");
    return 0;
}
