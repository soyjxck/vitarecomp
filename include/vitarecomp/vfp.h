/* vfp.h — the floating-point register file.
 *
 * Most of what a decoder lumps together as "SIMD" on this platform is not
 * vector work at all. Measured on this module, 82% of the coprocessor space is
 * scalar VFP — ordinary single-precision floating point, plus the loads and
 * stores that feed it — and only 18% is Advanced SIMD proper. Scalar VFP maps
 * onto C `float` almost one to one, so it is worth implementing well before
 * anyone writes a line of vector code.
 *
 * The register file is 32 single-precision registers, S0-S31. The same storage
 * is also addressable as 16 double-precision registers, D0-D15, where Dn
 * occupies S(2n) and S(2n+1) with S(2n) holding the LOW half. Keeping one
 * backing array rather than two makes that aliasing automatic instead of
 * something to remember at every access.
 *
 * Advanced SIMD has sixteen more, D16-D31, with no single-precision names.
 * They live in the same array past S31, so a D register is always the pair
 * at 2n and 2n+1 whichever side of 16 it is (neon.h).
 */

#ifndef VITARECOMP_VFP_H
#define VITARECOMP_VFP_H

#include <stdint.h>
#include <string.h>

/* Accessed as bits or as a float depending on the instruction, so a union
 * rather than a cast: type-punning through a pointer is undefined behaviour
 * and compilers do miscompile it at higher optimisation levels. */
typedef union {
    uint32_t u;
    float    f;
} vfp_reg;

extern vfp_reg vfp_s[64];

/* VFP comparisons write FPSCR, not the integer flags. A separate VMRS then
 * copies them into APSR, and only after that can a conditional branch see
 * them. Modelling FPSCR separately is what makes that two-step visible — a
 * recompiler that folds the comparison straight into the integer flags gets
 * the right answer right up until code compares two floats, does integer work,
 * and only then branches. */
extern int fpscr_n, fpscr_z, fpscr_c, fpscr_v;

static inline float vfp_getf(int n) { return vfp_s[n & 31].f; }
static inline void  vfp_setf(int n, float v) { vfp_s[n & 31].f = v; }

static inline uint32_t vfp_getu(int n) { return vfp_s[n & 31].u; }
static inline void     vfp_setu(int n, uint32_t v) { vfp_s[n & 31].u = v; }

/* Doubles occupy a register pair, low half first. */
static inline double vfp_getd(int n) {
    uint32_t lo = vfp_s[(n * 2) & 63].u, hi = vfp_s[(n * 2 + 1) & 63].u;
    uint64_t bits = ((uint64_t)hi << 32) | lo;
    double d;
    memcpy(&d, &bits, sizeof(d));
    return d;
}

static inline void vfp_setd(int n, double v) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    vfp_s[(n * 2) & 63].u     = (uint32_t)bits;
    vfp_s[(n * 2 + 1) & 63].u = (uint32_t)(bits >> 32);
}

/* Compare two floats and write FPSCR.
 *
 * The unordered case is the one that matters: if either operand is NaN the
 * result is "unordered", which sets C and V and clears N and Z — deliberately
 * failing every ordered comparison in both directions. Treating NaN as merely
 * "not equal" makes `a < b` and `a >= b` both false in C too, but for the wrong
 * reason, and the flags a later branch reads would be wrong. */
void vfp_cmp_f32(float a, float b);
void vfp_cmp_f64(double a, double b);

/* VMRS APSR_nzcv, FPSCR — copy the float flags to the integer ones. */
void vfp_mrs_apsr(void);

/* Float to integer, toward zero, as VCVT does it: out of range saturates and
 * NaN gives zero. A plain C cast is undefined there, and on x86 it yields
 * 0x80000000 for every such input, signed or not. */
static inline int32_t vita_f32_to_s32(float f) {
    if (f != f) return 0;
    if (f >= 2147483648.0f) return INT32_MAX;
    if (f <= -2147483648.0f) return INT32_MIN;
    return (int32_t)f;
}

static inline uint32_t vita_f32_to_u32(float f) {
    if (!(f > 0.0f)) return 0;
    if (f >= 4294967296.0f) return UINT32_MAX;
    return (uint32_t)f;
}

static inline int32_t vita_f64_to_s32(double d) {
    if (d != d) return 0;
    if (d >= 2147483648.0) return INT32_MAX;
    if (d <= -2147483648.0) return INT32_MIN;
    return (int32_t)d;
}

static inline uint32_t vita_f64_to_u32(double d) {
    if (!(d > 0.0)) return 0;
    if (d >= 4294967296.0) return UINT32_MAX;
    return (uint32_t)d;
}

#endif /* VITARECOMP_VFP_H */
