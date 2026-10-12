/* hle_libm.c — SceLibm, forwarded to the host's math library.
 *
 * The user-mode ABI on this console passes floating-point arguments in the
 * VFP registers and returns results there: a float in s0, a double in d0,
 * further ones in s1/d1 and s2/d2. Integer arguments and pointers take the
 * core registers as usual, counted separately: `ldexp(double x, int n)` is
 * d0 and r0. Vita3K's bridge reads these the same way, which is the
 * confirmation that it is the hard-float convention and not the soft one.
 *
 * The underscored names are the library's own internals, the Dinkumware
 * shapes that sin/cos and log/log10 are thin wrappers over: `_Sin(x, qoff)`
 * is sin(x) for qoff 0 and cos(x) for 1; `_Sinh(x, y)` is y * sinh(x), the
 * same for `_Cosh`; `_Log(x, baseflag)` is the natural log for 0, log10 for
 * 1 and log2 otherwise. A program that calls them directly is one the
 * compiler inlined the wrapper for.
 *
 * Calling convention: see hle.h, plus the VFP side above.
 */

#include "vitarecomp/recomp_rt.h"

#include <math.h>

#define F1(name, expr) void vita_hle_##name(void) { float x = vfp_getf(0); vfp_setf(0, (expr)); }
#define F2(name, expr) void vita_hle_##name(void) { float x = vfp_getf(0), y = vfp_getf(1); vfp_setf(0, (expr)); }
#define D1(name, expr) void vita_hle_##name(void) { double x = vfp_getd(0); vfp_setd(0, (expr)); }
#define D2(name, expr) void vita_hle_##name(void) { double x = vfp_getd(0), y = vfp_getd(1); vfp_setd(0, (expr)); }

F1(sinf,   sinf(x))
F1(cosf,   cosf(x))
F1(tanf,   tanf(x))
F1(asinf,  asinf(x))
F1(acosf,  acosf(x))
F1(atanf,  atanf(x))
F1(sinhf,  sinhf(x))
F1(coshf,  coshf(x))
F1(tanhf,  tanhf(x))
F1(expf,   expf(x))
F1(exp2f,  exp2f(x))
F1(logf,   logf(x))
F1(log10f, log10f(x))
F1(log2f,  log2f(x))
F1(sqrtf,  sqrtf(x))
F1(cbrtf,  cbrtf(x))
F1(fabsf,  fabsf(x))
F1(floorf, floorf(x))
F1(ceilf,  ceilf(x))
F1(truncf, truncf(x))
F1(roundf, roundf(x))
F2(atan2f, atan2f(x, y))
F2(powf,   powf(x, y))
F2(fmodf,  fmodf(x, y))
F2(hypotf, hypotf(x, y))
F2(fmaxf,  fmaxf(x, y))
F2(fminf,  fminf(x, y))

D1(sin,   sin(x))
D1(cos,   cos(x))
D1(tan,   tan(x))
D1(asin,  asin(x))
D1(acos,  acos(x))
D1(atan,  atan(x))
D1(sinh,  sinh(x))
D1(cosh,  cosh(x))
D1(tanh,  tanh(x))
D1(exp,   exp(x))
D1(exp2,  exp2(x))
D1(log,   log(x))
D1(log10, log10(x))
D1(log2,  log2(x))
D1(sqrt,  sqrt(x))
D1(cbrt,  cbrt(x))
D1(fabs,  fabs(x))
D1(floor, floor(x))
D1(ceil,  ceil(x))
D1(trunc, trunc(x))
D1(round, round(x))
D2(atan2, atan2(x, y))
D2(pow,   pow(x, y))
D2(fmod,  fmod(x, y))
D2(hypot, hypot(x, y))
D2(fmax,  fmax(x, y))
D2(fmin,  fmin(x, y))

/* float fmaf(float x, float y, float z): s0, s1, s2 */
void vita_hle_fmaf(void) { vfp_setf(0, fmaf(vfp_getf(0), vfp_getf(1), vfp_getf(2))); }
void vita_hle_fma(void)  { vfp_setd(0, fma(vfp_getd(0), vfp_getd(1), vfp_getd(2))); }

/* double ldexp(double x, int n): d0 and r0 */
void vita_hle_ldexp(void)  { vfp_setd(0, ldexp(vfp_getd(0), (int)r0)); }
void vita_hle_ldexpf(void) { vfp_setf(0, ldexpf(vfp_getf(0), (int)r0)); }

/* double frexp(double x, int *exp): the exponent to guest memory at r0 */
void vita_hle_frexp(void) {
    int e = 0;
    double m = frexp(vfp_getd(0), &e);
    if (vita_mem_ptr(r0, 4)) vita_write32(r0, (uint32_t)e);
    vfp_setd(0, m);
}
void vita_hle_frexpf(void) {
    int e = 0;
    float m = frexpf(vfp_getf(0), &e);
    if (vita_mem_ptr(r0, 4)) vita_write32(r0, (uint32_t)e);
    vfp_setf(0, m);
}

/* double modf(double x, double *iptr): the integral part to guest memory */
void vita_hle_modf(void) {
    double ip = 0;
    double f = modf(vfp_getd(0), &ip);
    if (vita_mem_ptr(r0, 8)) {
        uint64_t bits; memcpy(&bits, &ip, 8);
        vita_write32(r0, (uint32_t)bits); vita_write32(r0 + 4, (uint32_t)(bits >> 32));
    }
    vfp_setd(0, f);
}
void vita_hle_modff(void) {
    float ip = 0;
    float f = modff(vfp_getf(0), &ip);
    if (vita_mem_ptr(r0, 4)) { uint32_t bits; memcpy(&bits, &ip, 4); vita_write32(r0, bits); }
    vfp_setf(0, f);
}

/* --- the library's internals ---------------------------------------------------- */

/* float _FSin(float x, unsigned int qoff) / double _Sin(double x, unsigned int qoff):
 * sin for qoff 0, cos for qoff 1 (a quadrant offset). */
void vita_hle__FSin(void) { float x = vfp_getf(0); vfp_setf(0, (r0 & 1) ? cosf(x) : sinf(x)); }
void vita_hle__Sin(void)  { double x = vfp_getd(0); vfp_setd(0, (r0 & 1) ? cos(x) : sin(x)); }

/* float _FSinx(float x, unsigned int qoff, int quads) / double _Sinx: the
 * later shape of the same thing, the sine of x moved on by qoff + quads
 * quarter turns. sinf is (x, 0, 0) and cosf is (x, 1, 0). */
static double sin_quarters(double x, uint32_t quarters) {
    switch (quarters & 3) {
    case 0:  return sin(x);
    case 1:  return cos(x);
    case 2:  return -sin(x);
    default: return -cos(x);
    }
}
void vita_hle__FSinx(void) { vfp_setf(0, (float)sin_quarters((double)vfp_getf(0), r0 + r1)); }
void vita_hle__Sinx(void)  { vfp_setd(0, sin_quarters(vfp_getd(0), r0 + r1)); }

/* double _Sinh(double x, double y) = y * sinh(x); _Cosh likewise; the float forms too. */
void vita_hle__Sinh(void)  { vfp_setd(0, vfp_getd(1) * sinh(vfp_getd(0))); }
void vita_hle__Cosh(void)  { vfp_setd(0, vfp_getd(1) * cosh(vfp_getd(0))); }
void vita_hle__FSinh(void) { vfp_setf(0, vfp_getf(1) * sinhf(vfp_getf(0))); }
void vita_hle__FCosh(void) { vfp_setf(0, vfp_getf(1) * coshf(vfp_getf(0))); }

/* double _Log(double x, int baseflag): 0 natural, 1 decimal, otherwise binary. */
void vita_hle__Log(void) {
    double x = vfp_getd(0);
    vfp_setd(0, r0 == 0 ? log(x) : r0 == 1 ? log10(x) : log2(x));
}
void vita_hle__FLog(void) {
    float x = vfp_getf(0);
    vfp_setf(0, r0 == 0 ? logf(x) : r0 == 1 ? log10f(x) : log2f(x));
}

/* double _Exp(double *px, double y, short eoff): y * exp(*px) * 2^eoff, the
 * Dinkumware kernel behind exp(). The pointer is in r0, y in d0, eoff in r1. */
void vita_hle__Exp(void) {
    double x = 0;
    if (vita_mem_ptr(r0, 8)) {
        uint64_t bits = ((uint64_t)vita_read32(r0 + 4) << 32) | vita_read32(r0);
        memcpy(&x, &bits, 8);
    }
    vfp_setd(0, vfp_getd(0) * ldexp(exp(x), (short)r1));
}
void vita_hle__FExp(void) {
    float x = 0;
    if (vita_mem_ptr(r0, 4)) { uint32_t bits = vita_read32(r0); memcpy(&x, &bits, 4); }
    vfp_setf(0, vfp_getf(0) * ldexpf(expf(x), (short)r1));
}
