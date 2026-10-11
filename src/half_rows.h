/* half_rows.h — rows of sixteen-bit floats, converted many at a time.
 *
 * A shader register holds two halves in a word, and the batched interpreter
 * keeps a register as a row of words, one a pixel. Reading one component of
 * such a register for every pixel is taking the same half out of every word
 * and widening it; writing is the reverse, leaving the other half of each
 * word alone. Where the processor has instructions for the conversion they
 * do eight at once; the answers are the same as half.h's, one at a time.
 */

#ifndef VITARECOMP_HALF_ROWS_H
#define VITARECOMP_HALF_ROWS_H

#include <stdint.h>

/* out[i] = the low (high: the upper) half of words[i], as a float. */
void vita_halves_to_floats(const uint32_t *words, int high, float *out, unsigned n);

/* The low (high: the upper) half of words[i] = in[i], rounded to nearest
 * even; the other half is kept. */
void vita_floats_to_halves(const float *in, uint32_t *words, int high, unsigned n);

/* Whether the fast forms are in use on this processor: for the curious. */
int vita_half_rows_fast(void);

#endif /* VITARECOMP_HALF_ROWS_H */
