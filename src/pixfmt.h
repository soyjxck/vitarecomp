/* pixfmt.h — the console's pixel layouts, between bytes and colours.
 *
 * A texel of a texture and a pixel of a colour surface are the same thing
 * seen from two sides: a few bytes, cut into up to four fields, each field
 * a number of some kind, and a rule saying which field is red, which green,
 * which blue and which alpha. The console names its formats by listing the
 * fields from the most significant bit down ("U8U8U8U8"), and separately
 * the channels the same way ("ABGR": alpha on top, red in the low byte).
 * This file turns either kind of name into one description and converts
 * with it in both directions.
 *
 * The format values are those of the SDK's headers; the field positions
 * were taken from the vitasdk headers and the Vita3K project's notes.
 */

#ifndef VITARECOMP_PIXFMT_H
#define VITARECOMP_PIXFMT_H

#include <stdint.h>

/* What kind of number a field holds. */
enum {
    PIX_UNORM = 0,          /* unsigned, all ones is 1.0 */
    PIX_SNORM,              /* two's complement, the largest is 1.0 and nothing is below -1.0 */
    PIX_RAW,                /* an integer delivered as it is: the float carries its bits */
    PIX_F16, PIX_F32,
    PIX_F32M,               /* a float whose top bit belongs to something else */
    PIX_UF11, PIX_UF10,     /* unsigned floats: five bits of exponent, six or five of mantissa */
    PIX_SE9                 /* a 9-bit mantissa sharing the exponent in the top five bits */
};

/* Where a channel comes from: a field, or a constant. */
enum { PIX_ZERO = -1, PIX_ONE = -2 };

typedef struct {
    uint8_t bytes;                          /* one pixel: 1, 2, 3, 4 or 8 */
    uint8_t fields;
    struct { uint8_t shift, bits, kind; } f[4];
    int8_t  from[4];                        /* red, green, blue, alpha: a field number or PIX_ZERO, PIX_ONE */
    uint8_t components;                     /* how many channels the format really has: 1, 2 or 4 */
    uint8_t raw;                            /* the bits are the value: never filtered, never converted */
    uint8_t type;                           /* the kind its channels are, for callers that keep texels unconverted */
} pixfmt;

/* The texture format `format` (base and swizzle bits), when a texel is a
 * plain run of bytes. Returns 0 for the formats that are not: compressed
 * blocks, palettes, planes of luminance and chroma. */
int pixfmt_texture(uint32_t format, pixfmt *out);

/* The colour surface format `format`. Returns 0 when it is not one. */
int pixfmt_colour(uint32_t format, pixfmt *out);

/* The texture format that reads the pixels a colour surface of `format`
 * holds, for the channel orders both kinds of format can name. */
uint32_t pixfmt_colour_texture(uint32_t format);

/* One pixel's bytes as red, green, blue and alpha. */
void pixfmt_decode(const pixfmt *f, const uint8_t *pixel, float rgba[4]);

/* Set, every field is worked out afresh; clear, the fields with few values
 * are looked up in tables made by the same arithmetic. For a test to hold
 * the second way to the first. */
extern int pixfmt_plain;

/* The reverse, for the channels whose bit is set in `channels` (1 red,
 * 2 green, 4 blue, 8 alpha); the others keep what the pixel had. */
void pixfmt_encode(const pixfmt *f, uint8_t *pixel, const float rgba[4], unsigned channels);

/* The curve between a display's values and light. */
float pix_srgb_to_linear(float v);
float pix_linear_to_srgb(float v);

#endif /* VITARECOMP_PIXFMT_H */
