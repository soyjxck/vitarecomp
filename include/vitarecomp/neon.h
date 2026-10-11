/* neon.h — Advanced SIMD on 64-bit D registers, lane by lane.
 *
 * The D registers are the VFP file seen as 32 doublewords (vfp.h). Every
 * helper here takes and returns a D register's 64 bits and a lane width, and
 * does to each lane what the instruction does to each lane. A Q-register
 * instruction is two of these, one per half; the emitter writes it that way.
 *
 * Only the forms a running program has asked for are here. The first was the
 * C library: GCC vectorises newlib's own code, and memchr tests eight bytes
 * at a time with VCEQ and VPADD. The second was a retail title, whose trap
 * census was led by VDUP-by-lane, the f32 arithmetic and the two reciprocal
 * estimates: a vector maths library, in other words.
 *
 * Two things the hardware does that these helpers do not: NEON always runs
 * flush-to-zero with default NaNs, and VRECPE/VRSQRTE return estimates good
 * to about eight bits. The helpers compute exactly and let denormals through.
 * Code that follows the estimate with the Newton-Raphson step (VRECPS and
 * VRSQRTS, which compilers always emit) converges to the same answer; code
 * that depends on the exact estimate bits would differ in the last places.
 */

#ifndef VITARECOMP_NEON_H
#define VITARECOMP_NEON_H

#include "vfp.h"
#include "mem.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

/* --- D registers as 64-bit values ----------------------------------------------- */

static inline uint64_t vita_d_get(int n) {
    uint32_t lo = vfp_s[(n * 2) & 63].u, hi = vfp_s[(n * 2 + 1) & 63].u;
    return ((uint64_t)hi << 32) | lo;
}

static inline void vita_d_set(int n, uint64_t v) {
    vfp_s[(n * 2) & 63].u     = (uint32_t)v;
    vfp_s[(n * 2 + 1) & 63].u = (uint32_t)(v >> 32);
}

/* --- lanes --------------------------------------------------------------------- */

static inline uint64_t vita_lane_mask(int size) {
    return size >= 64 ? 0xFFFFFFFFFFFFFFFFull : ((1ull << size) - 1ull);
}

/* Lane-wise modular add and subtract. */
static inline uint64_t vita_neon_add(uint64_t a, uint64_t b, int size) {
    uint64_t r = 0, m = vita_lane_mask(size);
    for (int s = 0; s < 64; s += size)
        r |= (((a >> s) + (b >> s)) & m) << s;
    return r;
}

static inline uint64_t vita_neon_sub(uint64_t a, uint64_t b, int size) {
    uint64_t r = 0, m = vita_lane_mask(size);
    for (int s = 0; s < 64; s += size)
        r |= (((a >> s) - (b >> s)) & m) << s;
    return r;
}

/* Lane-wise equality: a lane of all ones where equal, zero where not. */
static inline uint64_t vita_neon_ceq(uint64_t a, uint64_t b, int size) {
    uint64_t r = 0, m = vita_lane_mask(size);
    for (int s = 0; s < 64; s += size)
        if (((a >> s) & m) == ((b >> s) & m)) r |= m << s;
    return r;
}

/* Lane-wise test: all ones where (a & b) is non-zero in the lane. */
static inline uint64_t vita_neon_tst(uint64_t a, uint64_t b, int size) {
    uint64_t r = 0, m = vita_lane_mask(size);
    for (int s = 0; s < 64; s += size)
        if (((a >> s) & (b >> s) & m) != 0) r |= m << s;
    return r;
}

/* Pairwise add: the result's low lanes are the sums of adjacent lanes of a,
 * its high lanes the sums of adjacent lanes of b. */
static inline uint64_t vita_neon_padd(uint64_t a, uint64_t b, int size) {
    uint64_t r = 0, m = vita_lane_mask(size);
    int lanes = 64 / size, out = 0;
    for (int i = 0; i < lanes; i += 2, out++)
        r |= (((a >> (i * size)) + (a >> ((i + 1) * size))) & m) << (out * size);
    for (int i = 0; i < lanes; i += 2, out++)
        r |= (((b >> (i * size)) + (b >> ((i + 1) * size))) & m) << (out * size);
    return r;
}

/* Every lane set to the low `size` bits of v. */
static inline uint64_t vita_neon_dup(uint32_t v, int size) {
    uint64_t r = 0, m = vita_lane_mask(size);
    for (int s = 0; s < 64; s += size) r |= ((uint64_t)v & m) << s;
    return r;
}

/* VBSL: bits of d choose n where set, m where clear. */
static inline uint64_t vita_neon_bsl(uint64_t d, uint64_t n, uint64_t m) {
    return (d & n) | (~d & m);
}

/* Insert a 32-bit lane. */
/* One lane of any width replaced. */
static inline uint64_t vita_neon_set_lane(uint64_t d, int lane, uint64_t v, int size) {
    uint64_t m = vita_lane_mask(size) << (lane * size);
    return (d & ~m) | ((v << (lane * size)) & m);
}

/* VLDn / VSTn, multiple structures: memory holds `groups` runs of
 * (64 / size) structures of n elements each, interleaved; register
 * d0 + g*n*spacing + k*spacing takes element k of every structure in run g.
 * VLD2.8 {d0, d1} is n 2, one group: d0 gets the even bytes, d1 the odd. */
static inline void vita_neon_ldn(int n, int d0, int spacing, int groups, uint32_t addr, int size) {
    int lanes = 64 / size, bytes = size / 8;
    for (int g = 0; g < groups; g++) {
        for (int k = 0; k < n; k++) {
            uint64_t v = 0;
            for (int l = 0; l < lanes; l++) {
                uint32_t a = addr + (uint32_t)(((g * lanes + l) * n + k) * bytes);
                uint64_t e = size == 8 ? vita_read8(a) : size == 16 ? vita_read16(a) : vita_read32(a);
                v |= e << (l * size);
            }
            vita_d_set(d0 + g * n * spacing + k * spacing, v);
        }
    }
}
static inline void vita_neon_stn(int n, int d0, int spacing, int groups, uint32_t addr, int size) {
    int lanes = 64 / size, bytes = size / 8;
    for (int g = 0; g < groups; g++) {
        for (int k = 0; k < n; k++) {
            uint64_t v = vita_d_get(d0 + g * n * spacing + k * spacing);
            for (int l = 0; l < lanes; l++) {
                uint32_t a = addr + (uint32_t)(((g * lanes + l) * n + k) * bytes);
                uint64_t e = (v >> (l * size)) & vita_lane_mask(size);
                if (size == 8)       vita_write8(a, (uint8_t)e);
                else if (size == 16) vita_write16(a, (uint16_t)e);
                else                 vita_write32(a, (uint32_t)e);
            }
        }
    }
}

static inline uint64_t vita_neon_set32(uint64_t d, int lane, uint32_t v) {
    if (lane) return (d & 0x00000000FFFFFFFFull) | ((uint64_t)v << 32);
    return (d & 0xFFFFFFFF00000000ull) | v;
}

static inline uint32_t vita_neon_get32(uint64_t d, int lane) {
    return lane ? (uint32_t)(d >> 32) : (uint32_t)d;
}

/* One lane, as the unsigned value and as the sign-extended one. */
static inline uint64_t vita_neon_lane(uint64_t d, int lane, int size) {
    return (d >> (lane * size)) & vita_lane_mask(size);
}

static inline int64_t vita_neon_slane(uint64_t d, int lane, int size) {
    uint64_t v = vita_neon_lane(d, lane, size);
    if (size < 64 && (v >> (size - 1)) & 1) v |= ~vita_lane_mask(size);
    return (int64_t)v;
}

/* --- integer lanes ------------------------------------------------------------- */

static inline uint64_t vita_neon_mul(uint64_t a, uint64_t b, int size) {
    uint64_t r = 0, m = vita_lane_mask(size);
    for (int s = 0; s < 64; s += size)
        r |= (((a >> s) & m) * ((b >> s) & m) & m) << s;
    return r;
}

static inline uint64_t vita_neon_mla(uint64_t d, uint64_t a, uint64_t b, int size) {
    return vita_neon_add(d, vita_neon_mul(a, b, size), size);
}

static inline uint64_t vita_neon_mls(uint64_t d, uint64_t a, uint64_t b, int size) {
    return vita_neon_sub(d, vita_neon_mul(a, b, size), size);
}

/* Lane-wise maximum and minimum, signed or unsigned. */
static inline uint64_t vita_neon_max(uint64_t a, uint64_t b, int size, int is_unsigned) {
    uint64_t r = 0;
    for (int i = 0; i * size < 64; i++) {
        int pick_a = is_unsigned ? vita_neon_lane(a, i, size) >= vita_neon_lane(b, i, size)
                                 : vita_neon_slane(a, i, size) >= vita_neon_slane(b, i, size);
        r |= vita_neon_lane(pick_a ? a : b, i, size) << (i * size);
    }
    return r;
}

static inline uint64_t vita_neon_min(uint64_t a, uint64_t b, int size, int is_unsigned) {
    uint64_t r = 0;
    for (int i = 0; i * size < 64; i++) {
        int pick_a = is_unsigned ? vita_neon_lane(a, i, size) <= vita_neon_lane(b, i, size)
                                 : vita_neon_slane(a, i, size) <= vita_neon_slane(b, i, size);
        r |= vita_neon_lane(pick_a ? a : b, i, size) << (i * size);
    }
    return r;
}

/* Lane-wise a > b and a >= b: all ones where true. */
static inline uint64_t vita_neon_cgt(uint64_t a, uint64_t b, int size, int is_unsigned) {
    uint64_t r = 0, m = vita_lane_mask(size);
    for (int i = 0; i * size < 64; i++) {
        int t = is_unsigned ? vita_neon_lane(a, i, size) > vita_neon_lane(b, i, size)
                            : vita_neon_slane(a, i, size) > vita_neon_slane(b, i, size);
        if (t) r |= m << (i * size);
    }
    return r;
}

static inline uint64_t vita_neon_cge(uint64_t a, uint64_t b, int size, int is_unsigned) {
    uint64_t r = 0, m = vita_lane_mask(size);
    for (int i = 0; i * size < 64; i++) {
        int t = is_unsigned ? vita_neon_lane(a, i, size) >= vita_neon_lane(b, i, size)
                            : vita_neon_slane(a, i, size) >= vita_neon_slane(b, i, size);
        if (t) r |= m << (i * size);
    }
    return r;
}

/* Lane compared with zero, signed: rel 0 is >, 1 >=, 2 ==, 3 <=, 4 <. */
static inline uint64_t vita_neon_cmp0(uint64_t a, int size, int rel) {
    uint64_t r = 0, m = vita_lane_mask(size);
    for (int i = 0; i * size < 64; i++) {
        int64_t v = vita_neon_slane(a, i, size);
        int t = rel == 0 ? v > 0 : rel == 1 ? v >= 0 : rel == 2 ? v == 0 : rel == 3 ? v <= 0 : v < 0;
        if (t) r |= m << (i * size);
    }
    return r;
}

/* Absolute value, modular like the negate: the most negative lane stays. */
static inline uint64_t vita_neon_abs(uint64_t a, int size) {
    uint64_t r = 0, m = vita_lane_mask(size);
    for (int i = 0; i * size < 64; i++) {
        int64_t v = vita_neon_slane(a, i, size);
        r |= ((uint64_t)(v < 0 ? -v : v) & m) << (i * size);
    }
    return r;
}

static inline uint64_t vita_neon_clz(uint64_t a, int size) {
    uint64_t r = 0;
    for (int i = 0; i * size < 64; i++) {
        uint64_t v = vita_neon_lane(a, i, size);
        int n = 0;
        for (int bit = size - 1; bit >= 0 && !((v >> bit) & 1); bit--) n++;
        r |= (uint64_t)n << (i * size);
    }
    return r;
}

/* VCNT: the set bits of each byte. */
static inline uint64_t vita_neon_cnt(uint64_t a) {
    uint64_t r = 0;
    for (int s = 0; s < 64; s += 8) {
        uint64_t v = (a >> s) & 0xFF;
        int n = 0;
        for (; v; v >>= 1) n += (int)(v & 1);
        r |= (uint64_t)n << s;
    }
    return r;
}

/* VREV: reverse the order of the `size`-bit lanes within each `group` bits. */
static inline uint64_t vita_neon_rev(uint64_t a, int size, int group) {
    uint64_t r = 0, m = vita_lane_mask(size);
    int per = group / size;
    for (int g = 0; g < 64; g += group)
        for (int i = 0; i < per; i++)
            r |= ((a >> (g + i * size)) & m) << (g + (per - 1 - i) * size);
    return r;
}

/* --- shifts by an immediate ----------------------------------------------------
 *
 * ARM defines every one of these at every amount the encoding can express,
 * which includes a right shift by the whole lane; C does not, so the edge
 * is handled before the shift is written. */

static inline uint64_t vita_neon_shl(uint64_t a, int size, int n) {
    uint64_t r = 0, m = vita_lane_mask(size);
    if (n >= size) return 0;
    for (int s = 0; s < 64; s += size)
        r |= ((((a >> s) & m) << n) & m) << s;
    return r;
}

/* Arithmetic right shift of a two's complement value without leaning on
 * what C does to a negative operand of >>. */
static inline int64_t vita_neon_asr64(int64_t v, int n) {
    if (n >= 64) return v < 0 ? -1 : 0;
    return v < 0 ? (int64_t)~((uint64_t)~v >> n) : (int64_t)((uint64_t)v >> n);
}

/* VSHL by register: each lane of `a` shifted by the signed low byte of the
 * same lane of `by`. A positive count shifts left and a negative one right,
 * arithmetically for the signed form; a count as wide as the lane or wider
 * leaves nothing but, shifting a signed value right, its sign. */
static inline uint64_t vita_neon_shl_reg(uint64_t a, uint64_t by, int size, int is_unsigned) {
    uint64_t r = 0, m = vita_lane_mask(size);
    for (int i = 0; i * size < 64; i++) {
        int n = (int8_t)(uint8_t)(by >> (i * size));
        uint64_t v = vita_neon_lane(a, i, size), out;
        if (n >= 0)          out = n >= size ? 0 : (v << n) & m;
        else if (is_unsigned) out = -n >= size ? 0 : v >> -n;
        else                 out = (uint64_t)vita_neon_asr64(vita_neon_slane(a, i, size), -n) & m;
        r |= out << (i * size);
    }
    return r;
}

static inline uint64_t vita_neon_shr(uint64_t a, int size, int n, int is_unsigned) {
    uint64_t r = 0, m = vita_lane_mask(size);
    for (int i = 0; i * size < 64; i++) {
        uint64_t v;
        if (is_unsigned) v = n >= size ? 0 : vita_neon_lane(a, i, size) >> n;
        else             v = (uint64_t)vita_neon_asr64(vita_neon_slane(a, i, size), n);
        r |= (v & m) << (i * size);
    }
    return r;
}

/* Rounding right shift: (v + 2^(n-1)) >> n, which is (v >> n) plus the bit
 * shifted out last. Written that way so the add cannot overflow a lane. */
static inline uint64_t vita_neon_rshr(uint64_t a, int size, int n, int is_unsigned) {
    uint64_t r = 0, m = vita_lane_mask(size);
    for (int i = 0; i * size < 64; i++) {
        uint64_t v, half;
        if (is_unsigned) {
            uint64_t x = vita_neon_lane(a, i, size);
            v    = n >= size ? 0 : x >> n;
            half = (x >> (n - 1)) & 1;
        } else {
            int64_t x = vita_neon_slane(a, i, size);
            v    = (uint64_t)vita_neon_asr64(x, n);
            half = (uint64_t)vita_neon_asr64(x, n - 1) & 1;
        }
        r |= ((v + half) & m) << (i * size);
    }
    return r;
}

/* VSLI: each lane of m shifted left by n, inserted over d's lane with d's
 * low n bits kept. VSRI: shifted right, with d's top n bits kept. */
static inline uint64_t vita_neon_sli(uint64_t d, uint64_t m, int size, int n) {
    uint64_t r = 0, lm = vita_lane_mask(size);
    for (int i = 0; i * size < 64; i++) {
        uint64_t keep = n >= size ? lm : (lm >> (size - n));
        uint64_t v = (vita_neon_lane(d, i, size) & keep) | ((vita_neon_lane(m, i, size) << n) & lm);
        r |= (n >= size ? vita_neon_lane(d, i, size) : v) << (i * size);
    }
    return r;
}

static inline uint64_t vita_neon_sri(uint64_t d, uint64_t m, int size, int n) {
    uint64_t r = 0, lm = vita_lane_mask(size);
    for (int i = 0; i * size < 64; i++) {
        uint64_t keep = n >= size ? lm : (lm & ~(lm >> n));
        uint64_t v = n >= size ? vita_neon_lane(d, i, size)
                   : (vita_neon_lane(d, i, size) & keep) | (vita_neon_lane(m, i, size) >> n);
        r |= v << (i * size);
    }
    return r;
}

/* VMOVL: the lanes of one half of a D register, each widened to twice its
 * size with sign or zero extension. */
static inline uint64_t vita_neon_widen(uint32_t half, int size, int is_unsigned) {
    uint64_t r = 0, m = vita_lane_mask(size), om = vita_lane_mask(size * 2);
    for (int i = 0; i * size < 32; i++) {
        uint64_t v = ((uint64_t)half >> (i * size)) & m;
        if (!is_unsigned && ((v >> (size - 1)) & 1)) v |= ~m;
        r |= (v & om) << (i * size * 2);
    }
    return r;
}

/* VMOVN, VSHRN, VRSHRN: each `size`-bit lane of the Q register lo:hi shifted
 * right by n (rounded first when asked) and its low half kept. Truncation
 * makes the sign irrelevant, so the shift is logical and the rounding add
 * is modular. */
static inline uint64_t vita_neon_narrow(uint64_t lo, uint64_t hi, int size, int n, int round) {
    int out = size / 2, per = 64 / size;
    uint64_t r = 0, m = vita_lane_mask(size), om = vita_lane_mask(out);
    for (int i = 0; i < per * 2; i++) {
        uint64_t v = vita_neon_lane(i < per ? lo : hi, i % per, size);
        if (round && n > 0) v = (v + (1ull << (n - 1))) & m;
        v = n >= size ? 0 : v >> n;
        r |= (v & om) << (i * out);
    }
    return r;
}

/* VEXT: bytes pos.. of the 16-byte pair m:n, n being the low half. */
static inline uint64_t vita_neon_ext(uint64_t n, uint64_t m, int pos) {
    if (pos == 0) return n;
    return (n >> (8 * pos)) | (m << (64 - 8 * pos));
}

/* The Q form reads 32 bytes and writes 16; it goes through memory so the
 * destination may be either source. */
static inline void vita_neon_ext_q(int d, int n, int m, int pos) {
    uint64_t in[4] = { vita_d_get(n), vita_d_get(n + 1), vita_d_get(m), vita_d_get(m + 1) };
    uint8_t bytes[32], out[16];
    memcpy(bytes, in, sizeof(bytes));
    memcpy(out, bytes + pos, sizeof(out));
    uint64_t lo, hi;
    memcpy(&lo, out, 8);
    memcpy(&hi, out + 8, 8);
    vita_d_set(d, lo);
    vita_d_set(d + 1, hi);
}

/* VTRN: the two registers as the rows of a matrix two lanes wide, turned
 * over. The odd lanes of d and the even lanes of m change places. */
static inline void vita_neon_trn(int d, int m, int size) {
    uint64_t a = vita_d_get(d), b = vita_d_get(m), mask = vita_lane_mask(size), ra = a, rb = b;
    for (int s = 0; s < 64; s += 2 * size) {
        uint64_t odd_of_a = (a >> (s + size)) & mask, even_of_b = (b >> s) & mask;
        ra = (ra & ~(mask << (s + size))) | (even_of_b << (s + size));
        rb = (rb & ~(mask << s)) | (odd_of_a << s);
    }
    vita_d_set(d, ra);
    vita_d_set(m, rb);
}

/* VTBL and VTBX: each byte of m is a place in a table of `count` registers'
 * bytes, starting at register `first`. Beyond the table VTBL gives zero;
 * VTBX (`keep`) leaves the byte of d as it was. */
static inline uint64_t vita_neon_tbl(uint64_t d, int first, int count, uint64_t m, int keep) {
    uint8_t table[32];
    uint64_t r = 0;
    for (int i = 0; i < count && i < 4; i++) {
        uint64_t v = vita_d_get(first + i);
        for (int k = 0; k < 8; k++) table[8 * i + k] = (uint8_t)(v >> (8 * k));
    }
    for (int i = 0; i < 8; i++) {
        unsigned at = (unsigned)((m >> (8 * i)) & 0xFF);
        uint64_t byte = at < (unsigned)(8 * count) ? table[at] : keep ? (d >> (8 * i)) & 0xFF : 0;
        r |= byte << (8 * i);
    }
    return r;
}

/* VPADDL: neighbouring lanes added, each sum in a lane twice as wide. */
static inline uint64_t vita_neon_paddl(uint64_t a, int size, int is_unsigned) {
    uint64_t r = 0, wide = vita_lane_mask(2 * size);
    for (int lane = 0; lane * size < 64; lane += 2) {
        int64_t x = is_unsigned ? (int64_t)vita_neon_lane(a, lane, size) : vita_neon_slane(a, lane, size);
        int64_t y = is_unsigned ? (int64_t)vita_neon_lane(a, lane + 1, size) : vita_neon_slane(a, lane + 1, size);
        r |= ((uint64_t)(x + y) & wide) << (lane * size);
    }
    return r;
}

/* --- f32 lanes ----------------------------------------------------------------- */

static inline float vita_f32_of(uint32_t b) { float f; memcpy(&f, &b, sizeof(f)); return f; }
static inline uint32_t vita_f32_bits(float f) { uint32_t b; memcpy(&b, &f, sizeof(b)); return b; }

/* VCVT, four singles to four halves and back (half.h has one of each). */
static inline uint64_t vita_neon_halve(uint64_t lo, uint64_t hi) {
    return (uint64_t)vita_f32_to_f16(vita_f32_of((uint32_t)lo))
         | (uint64_t)vita_f32_to_f16(vita_f32_of((uint32_t)(lo >> 32))) << 16
         | (uint64_t)vita_f32_to_f16(vita_f32_of((uint32_t)hi)) << 32
         | (uint64_t)vita_f32_to_f16(vita_f32_of((uint32_t)(hi >> 32))) << 48;
}

static inline void vita_neon_unhalve(int d, uint64_t m) {
    uint64_t lo = (uint64_t)vita_f32_bits(vita_f16_to_f32((uint16_t)m))
                | (uint64_t)vita_f32_bits(vita_f16_to_f32((uint16_t)(m >> 16))) << 32;
    uint64_t hi = (uint64_t)vita_f32_bits(vita_f16_to_f32((uint16_t)(m >> 32)))
                | (uint64_t)vita_f32_bits(vita_f16_to_f32((uint16_t)(m >> 48))) << 32;
    vita_d_set(d, lo);
    vita_d_set(d + 1, hi);
}

#define VITA_NEON_DEFAULT_NAN 0x7FC00000u

/* Each of these applies one expression to both 32-bit lanes; x and y are the
 * lanes of a and b as floats. Spelling the loop out twelve times would hide
 * the one line per operation that matters. */
#define VITA_NEON_F32_BINARY(name, expr)                                       \
    static inline uint64_t name(uint64_t a, uint64_t b) {                      \
        uint32_t r[2];                                                         \
        for (int i = 0; i < 2; i++) {                                          \
            float x = vita_f32_of((uint32_t)(a >> (32 * i)));                  \
            float y = vita_f32_of((uint32_t)(b >> (32 * i)));                  \
            r[i] = (expr);                                                     \
        }                                                                      \
        return ((uint64_t)r[1] << 32) | r[0];                                  \
    }

VITA_NEON_F32_BINARY(vita_neon_fadd, vita_f32_bits(x + y))
VITA_NEON_F32_BINARY(vita_neon_fsub, vita_f32_bits(x - y))
VITA_NEON_F32_BINARY(vita_neon_fmul, vita_f32_bits(x * y))
VITA_NEON_F32_BINARY(vita_neon_fabd, vita_f32_bits(fabsf(x - y)))
VITA_NEON_F32_BINARY(vita_neon_fceq, (x == y) ? 0xFFFFFFFFu : 0u)
VITA_NEON_F32_BINARY(vita_neon_fcge, (x >= y) ? 0xFFFFFFFFu : 0u)
VITA_NEON_F32_BINARY(vita_neon_fcgt, (x >  y) ? 0xFFFFFFFFu : 0u)
/* VMAX/VMIN give a NaN whenever either input is one, unlike C's fmaxf. */
VITA_NEON_F32_BINARY(vita_neon_fmax, (x != x || y != y) ? VITA_NEON_DEFAULT_NAN : vita_f32_bits(x >= y ? x : y))
VITA_NEON_F32_BINARY(vita_neon_fmin, (x != x || y != y) ? VITA_NEON_DEFAULT_NAN : vita_f32_bits(x <= y ? x : y))
/* The Newton-Raphson steps: 2 - xy for a reciprocal, (3 - xy) / 2 for a
 * reciprocal square root. The architecture special-cases infinity times
 * zero to give the plain constant rather than NaN, so one bad estimate
 * cannot poison the refinement. */
VITA_NEON_F32_BINARY(vita_neon_frecps,
    ((isinf(x) && y == 0.0f) || (x == 0.0f && isinf(y))) ? vita_f32_bits(2.0f) : vita_f32_bits(2.0f - x * y))
VITA_NEON_F32_BINARY(vita_neon_frsqrts,
    ((isinf(x) && y == 0.0f) || (x == 0.0f && isinf(y))) ? vita_f32_bits(1.5f) : vita_f32_bits((3.0f - x * y) / 2.0f))

#undef VITA_NEON_F32_BINARY

/* VPADD.F32: a's two lanes summed into the low result lane, b's into the high. */
static inline uint64_t vita_neon_fpadd(uint64_t a, uint64_t b) {
    float lo = vita_f32_of((uint32_t)a) + vita_f32_of((uint32_t)(a >> 32));
    float hi = vita_f32_of((uint32_t)b) + vita_f32_of((uint32_t)(b >> 32));
    return ((uint64_t)vita_f32_bits(hi) << 32) | vita_f32_bits(lo);
}

/* VMLA/VMLS.F32 round twice where the hardware may fuse; last-bit differences. */
static inline uint64_t vita_neon_fmla(uint64_t d, uint64_t a, uint64_t b) {
    return vita_neon_fadd(d, vita_neon_fmul(a, b));
}

static inline uint64_t vita_neon_fmls(uint64_t d, uint64_t a, uint64_t b) {
    return vita_neon_fsub(d, vita_neon_fmul(a, b));
}

static inline uint64_t vita_neon_frecpe(uint64_t a) {
    uint32_t lo = vita_f32_bits(1.0f / vita_f32_of((uint32_t)a));
    uint32_t hi = vita_f32_bits(1.0f / vita_f32_of((uint32_t)(a >> 32)));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t vita_neon_frsqrte(uint64_t a) {
    uint32_t lo = vita_f32_bits(1.0f / sqrtf(vita_f32_of((uint32_t)a)));
    uint32_t hi = vita_f32_bits(1.0f / sqrtf(vita_f32_of((uint32_t)(a >> 32))));
    return ((uint64_t)hi << 32) | lo;
}

/* Compared with zero, as the integer one: rel 0 is >, 1 >=, 2 ==, 3 <=, 4 <.
 * NaN is false in every direction. */
static inline uint64_t vita_neon_fcmp0(uint64_t a, int rel) {
    uint64_t r = 0;
    for (int i = 0; i < 2; i++) {
        float x = vita_f32_of((uint32_t)(a >> (32 * i)));
        int t = rel == 0 ? x > 0.0f : rel == 1 ? x >= 0.0f : rel == 2 ? x == 0.0f : rel == 3 ? x <= 0.0f : x < 0.0f;
        if (t) r |= 0xFFFFFFFFull << (32 * i);
    }
    return r;
}

/* VCVT between f32 and 32-bit integers, toward zero and saturating as the
 * scalar form does: kind 0 s32->f32, 1 u32->f32, 2 f32->s32, 3 f32->u32. */
static inline uint64_t vita_neon_cvt(uint64_t a, int kind) {
    uint64_t r = 0;
    for (int i = 0; i < 2; i++) {
        uint32_t v = (uint32_t)(a >> (32 * i)), out;
        switch (kind) {
        case 0:  out = vita_f32_bits((float)(int32_t)v); break;
        case 1:  out = vita_f32_bits((float)v); break;
        case 2:  out = (uint32_t)vita_f32_to_s32(vita_f32_of(v)); break;
        default: out = vita_f32_to_u32(vita_f32_of(v)); break;
        }
        r |= (uint64_t)out << (32 * i);
    }
    return r;
}

#endif /* VITARECOMP_NEON_H */
