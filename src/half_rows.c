/* half_rows.c — rows of sixteen-bit floats, converted many at a time. See half_rows.h.
 *
 * This file is the plain form and the choice between it and the fast one,
 * which lives in half_rows_avx2.c and is only built where it can be.
 */

#include "half_rows.h"
#include "vitarecomp/half.h"

#ifdef VITA_HALF_ROWS_AVX2
void vita_halves_to_floats_avx2(const uint32_t *words, int high, float *out, unsigned n);
void vita_floats_to_halves_avx2(const float *in, uint32_t *words, int high, unsigned n);

#if defined(_MSC_VER)
#include <intrin.h>
static int cpu_has_avx2_f16c(void) {
    int r[4];
    __cpuid(r, 1);
    int osxsave = (r[2] >> 27) & 1, avx = (r[2] >> 28) & 1, f16c = (r[2] >> 29) & 1;
    if (!osxsave || !avx || !f16c) return 0;
    if ((_xgetbv(0) & 6) != 6) return 0;                /* the system saves the wide registers */
    __cpuidex(r, 7, 0);
    return (r[1] >> 5) & 1;
}
#else
static int cpu_has_avx2_f16c(void) { return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("f16c"); }
#endif
#else
static int cpu_has_avx2_f16c(void) { return 0; }
#endif

static int g_fast = -1;

int vita_half_rows_fast(void) {
    if (g_fast < 0) g_fast = cpu_has_avx2_f16c();
    return g_fast;
}

void vita_halves_to_floats(const uint32_t *words, int high, float *out, unsigned n) {
#ifdef VITA_HALF_ROWS_AVX2
    if (vita_half_rows_fast()) { vita_halves_to_floats_avx2(words, high, out, n); return; }
#endif
    unsigned shift = high ? 16 : 0;
    for (unsigned i = 0; i < n; i++) out[i] = vita_f16_to_f32((uint16_t)(words[i] >> shift));
}

void vita_floats_to_halves(const float *in, uint32_t *words, int high, unsigned n) {
#ifdef VITA_HALF_ROWS_AVX2
    if (vita_half_rows_fast()) { vita_floats_to_halves_avx2(in, words, high, n); return; }
#endif
    unsigned shift = high ? 16 : 0;
    uint32_t keep = high ? 0x0000FFFFu : 0xFFFF0000u;
    for (unsigned i = 0; i < n; i++) words[i] = (words[i] & keep) | ((uint32_t)vita_f32_to_f16(in[i]) << shift);
}
