/* half.h — sixteen-bit floats.
 *
 * IEEE binary16 to and from single precision: the subnormals, round to
 * nearest even on the way down, infinities kept, a NaN kept and made quiet. The CPU's VCVTB
 * and VCVTT use it, and so does everything on the GPU side, where half the
 * registers of a shader are pairs of these.
 */

#ifndef VITARECOMP_HALF_H
#define VITARECOMP_HALF_H

#include <stdint.h>

static inline float vita_f16_to_f32(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
    uint32_t bits;
    if (exp == 0) {
        if (man == 0) bits = sign;
        else {
            /* a subnormal: normalise it */
            int e = -1;
            do { e++; man <<= 1; } while (!(man & 0x400));
            man &= 0x3FF;
            bits = sign | ((uint32_t)(127 - 15 - e) << 23) | (man << 13);
        }
    } else if (exp == 31) {
        /* infinity, or a NaN, which comes out quiet as it does from the processor */
        bits = sign | 0x7F800000u | (man << 13) | (man ? 0x00400000u : 0);
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
    }
    union { uint32_t u; float f; } u; u.u = bits; return u.f;
}

static inline uint16_t vita_f32_to_f16(float f) {
    union { uint32_t u; float f; } u; u.f = f;
    uint32_t sign = (u.u >> 16) & 0x8000, exp = (u.u >> 23) & 0xFF, man = u.u & 0x7FFFFF;
    if (exp == 0xFF) return (uint16_t)(sign | 0x7C00 | (man ? 0x200 | (man >> 13) : 0));
    int e = (int)exp - 127 + 15;
    if (e >= 31) return (uint16_t)(sign | 0x7C00);               /* overflow: infinity */
    if (e <= 0) {
        if (e < -10) return (uint16_t)sign;                        /* underflow: zero */
        man |= 0x800000;
        uint32_t shift = (uint32_t)(14 - e);
        uint32_t half = man >> shift, rem = man & ((1u << shift) - 1), mid = 1u << (shift - 1);
        if (rem > mid || (rem == mid && (half & 1))) half++;
        return (uint16_t)(sign | half);
    }
    uint32_t half = ((uint32_t)e << 10) | (man >> 13), rem = man & 0x1FFF;
    if (rem > 0x1000 || (rem == 0x1000 && (half & 1))) half++;     /* may carry into the exponent: that is correct */
    return (uint16_t)(sign | half);
}

#endif /* VITARECOMP_HALF_H */
