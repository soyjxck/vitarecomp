/* recomp_rt.h — the instructions whose C translation is not obvious.
 *
 * Everything here exists because a direct C expression would be WRONG, not
 * merely verbose. Each one is a place where ARM's definition and C's differ,
 * and where the difference is invisible until it produces a bad result deep in
 * arithmetic-heavy code.
 */

#ifndef VITARECOMP_RECOMP_RT_H
#define VITARECOMP_RECOMP_RT_H

#include "cpu.h"
#include "mem.h"
#include "vfp.h"
#include "neon.h"

#include <math.h>

#include <stdint.h>

/* --- shifts ----------------------------------------------------------------
 *
 * C leaves a shift by >= the operand width UNDEFINED. ARM defines it: shifting
 * a 32-bit value left by 32 or more yields zero. On x86 the natural C
 * translation silently masks the count to 5 bits, so `x << 32` returns x
 * unchanged — the exact opposite of the right answer, and a bug that only
 * appears when a shift amount is computed rather than constant.
 */

static inline uint32_t vita_lsl(uint32_t v, uint32_t n) {
    return n >= 32 ? 0u : v << n;
}

static inline uint32_t vita_lsr(uint32_t v, uint32_t n) {
    return n >= 32 ? 0u : v >> n;
}

/* Arithmetic shift right saturates to the sign bit rather than reaching zero,
 * and C's >> on a signed value is only implementation-defined, so the sign
 * extension is done explicitly. */
static inline uint32_t vita_asr(uint32_t v, uint32_t n) {
    if (n >= 32) return (v & 0x80000000u) ? 0xFFFFFFFFu : 0u;
    if (n == 0)  return v;
    return (uint32_t)(((int32_t)v) >> n) |
           ((v & 0x80000000u) ? ~(0xFFFFFFFFu >> n) : 0u);
}

static inline uint32_t vita_ror(uint32_t v, uint32_t n) {
    n &= 31;
    return n ? ((v >> n) | (v << (32 - n))) : v;
}

/* --- flags -----------------------------------------------------------------
 *
 * N and Z are mechanical. C and V are not, and they are the reason this file
 * exists: carry on an addition is an unsigned overflow, carry on a SUBTRACTION
 * is NOT a borrow but its complement, and overflow is a signed condition that
 * shares no bits with either. Writing these inline at every call site is how a
 * recompiler ends up with subtly different flag behaviour in three places.
 */

static inline void vita_flags_nz(uint32_t res) {
    flag_n = (int)(res >> 31);
    flag_z = (res == 0);
}

static inline void vita_flags_add(uint32_t a, uint32_t b, uint32_t res) {
    vita_flags_nz(res);
    flag_c = (res < a);
    flag_v = (int)(((a ^ res) & (b ^ res)) >> 31);
}

static inline void vita_flags_adc(uint32_t a, uint32_t b, uint32_t carry,
                                  uint32_t res) {
    vita_flags_nz(res);
    /* With a carry in, `res < a` is not sufficient: a + b + 1 can equal a
     * exactly when b is 0xFFFFFFFF, which carries but does not decrease. */
    flag_c = carry ? (res <= a) : (res < a);
    flag_v = (int)(((a ^ res) & (b ^ res)) >> 31);
}

/* ARM's carry on subtraction is set when there is NO borrow — the opposite of
 * most people's intuition and of several other architectures. */
static inline void vita_flags_sub(uint32_t a, uint32_t b, uint32_t res) {
    vita_flags_nz(res);
    flag_c = (a >= b);
    flag_v = (int)(((a ^ b) & (a ^ res)) >> 31);
}

/* --- condition codes -------------------------------------------------------
 *
 * Used by conditional branches and by every instruction inside an IT block.
 */

static inline int vita_cond(int cc) {
    switch (cc & 0xF) {
        case 0x0: return  flag_z;                        /* eq */
        case 0x1: return !flag_z;                        /* ne */
        case 0x2: return  flag_c;                        /* cs/hs */
        case 0x3: return !flag_c;                        /* cc/lo */
        case 0x4: return  flag_n;                        /* mi */
        case 0x5: return !flag_n;                        /* pl */
        case 0x6: return  flag_v;                        /* vs */
        case 0x7: return !flag_v;                        /* vc */
        case 0x8: return  flag_c && !flag_z;             /* hi */
        case 0x9: return !flag_c ||  flag_z;             /* ls */
        case 0xA: return  flag_n == flag_v;              /* ge */
        case 0xB: return  flag_n != flag_v;              /* lt */
        case 0xC: return !flag_z && (flag_n == flag_v);  /* gt */
        case 0xD: return  flag_z || (flag_n != flag_v);  /* le */
        default:  return 1;                              /* al */
    }
}

/* --- sign extension --------------------------------------------------------- */

static inline uint32_t vita_sxtb(uint32_t v) { return (uint32_t)(int32_t)(int8_t)(v & 0xFF); }
static inline uint32_t vita_sxth(uint32_t v) { return (uint32_t)(int32_t)(int16_t)(v & 0xFFFF); }
static inline uint32_t vita_uxtb(uint32_t v) { return v & 0xFF; }
static inline uint32_t vita_uxth(uint32_t v) { return v & 0xFFFF; }

/* Both bytes of a halfword pair, each extended into its halfword. */
static inline uint32_t vita_sxtb16(uint32_t v) {
    return (uint32_t)(uint16_t)(int16_t)(int8_t)(v & 0xFF)
         | ((uint32_t)(uint16_t)(int16_t)(int8_t)((v >> 16) & 0xFF) << 16);
}
static inline uint32_t vita_uxtb16(uint32_t v) { return v & 0x00FF00FFu; }

/* Two halfword adds with no carry between them, for the extend-and-add pair forms. */
static inline uint32_t vita_add16(uint32_t a, uint32_t b) {
    return ((a + b) & 0x0000FFFFu) | (((a & 0xFFFF0000u) + (b & 0xFFFF0000u)) & 0xFFFF0000u);
}

/* A variable the module imports from a firmware library. The generated code
 * reads its guest address through `value`; the loader writes that address
 * into the image at every word that held a placeholder (`abs_sites`), and
 * binds the value to what the runtime provides under `name`. */
typedef struct { uint32_t addr, addend; } vita_var_site;
typedef struct {
    uint32_t             lib_nid, var_nid;
    const char          *name;        /* "SceLibc::_Tolotab"; NULL when the database had no name */
    uint32_t            *value;
    const vita_var_site *abs_sites;   /* each gets the address plus its addend */
    uint32_t             abs_count;
} vita_var_import;
extern const vita_var_import vita_var_imports[];
extern const uint32_t        vita_var_import_count;

static inline uint32_t vita_rev(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8)
         | ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
}

static inline uint32_t vita_rev16(uint32_t v) {
    return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
}

static inline uint32_t vita_revsh(uint32_t v) {
    return vita_sxth(((v & 0xFF) << 8) | ((v >> 8) & 0xFF));
}

/* --- bit counting and reversal ----------------------------------------------- */

static inline uint32_t vita_clz(uint32_t v) {
    if (v == 0) return 32;
    uint32_t n = 0;
    while (!(v & 0x80000000u)) { v <<= 1; n++; }
    return n;
}

static inline uint32_t vita_rbit(uint32_t v) {
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
    return vita_rev(v);
}

/* --- byte-parallel arithmetic ----------------------------------------------------
 *
 * UADD8 adds four bytes independently and records the carry out of each in
 * the GE bits; USUB8 records "no borrow"; SEL then picks each byte from one
 * operand or the other by those bits. The C library's strlen and memchr
 * use the three together to test four bytes per iteration, which is why they
 * are here before any other SIMD.
 */

static inline uint32_t vita_uadd8(uint32_t a, uint32_t b) {
    uint32_t r = 0;
    flag_ge = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t s = ((a >> (8 * i)) & 0xFF) + ((b >> (8 * i)) & 0xFF);
        if (s >= 0x100) flag_ge |= 1 << i;
        r |= (s & 0xFF) << (8 * i);
    }
    return r;
}

static inline uint32_t vita_usub8(uint32_t a, uint32_t b) {
    uint32_t r = 0;
    flag_ge = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t x = (a >> (8 * i)) & 0xFF, y = (b >> (8 * i)) & 0xFF;
        if (x >= y) flag_ge |= 1 << i;
        r |= ((x - y) & 0xFF) << (8 * i);
    }
    return r;
}

static inline uint32_t vita_sel(uint32_t a, uint32_t b) {
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t m = 0xFFu << (8 * i);
        r |= ((flag_ge >> i) & 1) ? (a & m) : (b & m);
    }
    return r;
}

/* --- bitfields ---------------------------------------------------------------
 *
 * The obvious C is wrong in two places. `(1u << width) - 1` is UNDEFINED at
 * width 32, which is a legal UBFX width — and the natural way to write SBFX,
 * a left shift followed by a signed right shift, leans on `>>` sign-extending,
 * which C only implementation-defines. Both produce correct answers on the
 * usual compilers right up until they do not.
 */

static inline uint32_t vita_bf_mask(uint32_t width) {
    return width >= 32 ? 0xFFFFFFFFu : (1u << width) - 1u;
}

static inline uint32_t vita_ubfx(uint32_t v, uint32_t lsb, uint32_t width) {
    return vita_lsr(v, lsb) & vita_bf_mask(width);
}

static inline uint32_t vita_sbfx(uint32_t v, uint32_t lsb, uint32_t width) {
    uint32_t f = vita_lsr(v, lsb) & vita_bf_mask(width);
    if (width == 0 || width >= 32) return f;
    /* Sign is the field's top bit, not the register's. */
    return (f & (1u << (width - 1))) ? (f | ~vita_bf_mask(width)) : f;
}

/* BFI, and BFC with src == 0. The bits outside the field are preserved, which
 * is the whole point — this is the one place a destination register is read
 * before it is written. */
static inline uint32_t vita_bfi(uint32_t dst, uint32_t src,
                                uint32_t lsb, uint32_t width) {
    uint32_t m = vita_lsl(vita_bf_mask(width), lsb);
    return (dst & ~m) | (vita_lsl(src, lsb) & m);
}

/* --- gaps that announce themselves ------------------------------------------
 *
 * Anything not translated calls one of these. A gap that stops the program and
 * names itself is worth far more than one that produces a program which runs
 * and is quietly wrong.
 */

void vita_trap_unimpl(uint32_t addr, uint32_t raw, const char *what);
void vita_trap_indirect(uint32_t addr, uint32_t target);
void vita_trap_import(uint32_t addr, uint32_t nid, const char *name);

/* Total traps hit. Only meaningful under VITARECOMP_TRACE=1, where execution
 * continues past them; otherwise the first one ends the program. */
uint32_t vita_trap_count(void);

/* Function-entry trace (host.h). The generated C calls this first thing in
 * every function; it does nothing unless the host turned it on. */
void vita_trace_fn(uint32_t addr);

/* --- the time slice -----------------------------------------------------------
 *
 * One guest thread runs at a time (sched.h), and on the console several do:
 * a thread that waits for another by spinning on a word of memory waits for
 * ever if the other never gets a turn. Every backward branch the generated
 * C takes counts the running thread's slice down, and when it runs out the
 * scheduler lets the other runnable threads go. A count rather than a
 * clock, so a run schedules the same way every time; at backward branches
 * because every loop has one, and an exclusive pair has none between its
 * load and its store.
 *
 * A loop that reads memory, compares, and branches back, with nothing in it
 * that could change what it reads, cannot end by anything its own thread
 * does. The emitter recognises that shape and calls vita_spin_wait in it,
 * which hands the baton over at once instead of burning the slice. */
#define VITA_SLICE 20000

extern int32_t vita_slice;
void vita_slice_end(void);
void vita_spin_wait(uint32_t addr);

static inline void vita_back_edge(void) {
    if (--vita_slice < 0) vita_slice_end();
}

/* TPIDRURO: the running thread's TLS block, as `mrc p15, 0, rt, c13, c0, 3`
 * reads it. Implemented by the thread manager (hle_threadmgr.c). */
uint32_t vita_tls_base(void);

#endif /* VITARECOMP_RECOMP_RT_H */

