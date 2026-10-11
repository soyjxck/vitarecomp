/* texdec.h — the texel layouts a renderer has to undo.
 *
 * None of this is the console's own: Morton order and the block-compressed
 * formats are the same wherever they are used. Colours come out as one word
 * a texel with red in the low byte and alpha in the high one, which is the
 * order the console's A8B8G8R8 surfaces keep in memory.
 */

#ifndef VITARECOMP_TEXDEC_H
#define VITARECOMP_TEXDEC_H

#include <stdint.h>

/* Where texel (x, y) of a `width` by `height` texture lies when its texels
 * are stored in Morton order: the bits of x and y interleaved, y lowest,
 * for as many bits as the shorter side has; the longer side's remaining
 * bits above them. Sides are powers of two. */
uint32_t tex_morton_index(uint32_t x, uint32_t y, uint32_t width, uint32_t height);

/* One 4x4 block, sixteen texels row by row. BC1 is DXT1 (8 bytes, with a
 * one-bit hole), BC2 is DXT3 (16 bytes, four bits of alpha a texel), BC3 is
 * DXT5 (16 bytes, interpolated alpha), BC4 one interpolated channel in red,
 * BC5 two of them in red and green. */
void tex_decode_bc1(const uint8_t block[8], uint32_t out[16]);
void tex_decode_bc2(const uint8_t block[16], uint32_t out[16]);
void tex_decode_bc3(const uint8_t block[16], uint32_t out[16]);
void tex_decode_bc4(const uint8_t block[8], uint32_t out[16]);
void tex_decode_bc5(const uint8_t block[16], uint32_t out[16]);

/* PVRTC, of either generation (`version` 1 or 2) and either rate (`bits` 4
 * or 2 a texel). `words` is a whole level: `nx` by `ny` 64-bit words in
 * Morton order, each owning a block four texels high and four wide, or
 * eight at two bits. The texels of block (bx, by) come out row by row. A
 * block cannot be decoded alone: its colours are interpolated from the
 * words around it, and the edges of the level are each other's neighbours. */
void tex_decode_pvrtc(const uint8_t *words, uint32_t nx, uint32_t ny, uint32_t bx, uint32_t by,
                      int version, int bits, uint32_t out[32]);

#endif /* VITARECOMP_TEXDEC_H */
