/* neon.h — Advanced SIMD on 64-bit D registers, lane by lane.
 *
 * The D registers are the VFP file seen as 32 doublewords (vfp.h). Every
 * helper here takes and returns a D register's 64 bits and a lane width, and
 * does to each lane what the instruction does to each lane. A Q-register
 * instruction is two of these, one per half; the emitter writes it that way.
 *
 * Only the forms a running program has asked for are here. The first was the
 * C library: GCC vectorises newlib's own code, and memchr tests eight bytes
 * at a time with VCEQ and VPADD. The rest of NEON is milestone B.
 */

#ifndef VITARECOMP_NEON_H
#define VITARECOMP_NEON_H

#include "vfp.h"

#include <stdint.h>

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
static inline uint64_t vita_neon_set32(uint64_t d, int lane, uint32_t v) {
    if (lane) return (d & 0x00000000FFFFFFFFull) | ((uint64_t)v << 32);
    return (d & 0xFFFFFFFF00000000ull) | v;
}

static inline uint32_t vita_neon_get32(uint64_t d, int lane) {
    return lane ? (uint32_t)(d >> 32) : (uint32_t)d;
}

#endif /* VITARECOMP_NEON_H */
