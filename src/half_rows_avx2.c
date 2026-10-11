/* half_rows_avx2.c — the conversions of half_rows.h, eight at a time.
 *
 * Built only for x86-64 and with the processor's wider instructions turned
 * on for this file alone; half_rows.c calls into it only after asking the
 * processor whether it has them.
 */

#include "half_rows.h"
#include "vitarecomp/half.h"

#include <immintrin.h>

void vita_halves_to_floats_avx2(const uint32_t *words, int high, float *out, unsigned n) {
    unsigned i = 0, shift = high ? 16 : 0;
    const __m128i count = _mm_cvtsi32_si128((int)shift);
    const __m256i low = _mm256_set1_epi32(0xFFFF);
    for (; i + 8 <= n; i += 8) {
        __m256i w = _mm256_loadu_si256((const __m256i *)(words + i));
        w = _mm256_and_si256(_mm256_srl_epi32(w, count), low);
        /* eight 32-bit lanes, each a half: narrow them to eight 16-bit ones */
        __m256i packed = _mm256_permute4x64_epi64(_mm256_packus_epi32(w, w), 0xD8);
        _mm256_storeu_ps(out + i, _mm256_cvtph_ps(_mm256_castsi256_si128(packed)));
    }
    for (; i < n; i++) out[i] = vita_f16_to_f32((uint16_t)(words[i] >> shift));
}

void vita_floats_to_halves_avx2(const float *in, uint32_t *words, int high, unsigned n) {
    unsigned i = 0, shift = high ? 16 : 0;
    uint32_t keep = high ? 0x0000FFFFu : 0xFFFF0000u;
    const __m128i count = _mm_cvtsi32_si128((int)shift);
    const __m256i kept = _mm256_set1_epi32((int)keep);
    for (; i + 8 <= n; i += 8) {
        __m128i h = _mm256_cvtps_ph(_mm256_loadu_ps(in + i), _MM_FROUND_TO_NEAREST_INT);
        __m256i wide = _mm256_sll_epi32(_mm256_cvtepu16_epi32(h), count);
        __m256i w = _mm256_loadu_si256((const __m256i *)(words + i));
        _mm256_storeu_si256((__m256i *)(words + i), _mm256_or_si256(_mm256_and_si256(w, kept), wide));
    }
    for (; i < n; i++) words[i] = (words[i] & keep) | ((uint32_t)vita_f32_to_f16(in[i]) << shift);
}
