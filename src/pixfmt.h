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
#include <string.h>

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

/* The numbers a field of `bits` bits of a kind can stand for, in the order
 * of the field's values, where pixfmt_decode looks them up; NULL for a field
 * it works out each time. For a caller that reads many pixels of one format
 * and wants the lookups without the rest. */
const float *pixfmt_table(unsigned kind, unsigned bits);

/* Set, every field is worked out afresh; clear, the fields with few values
 * are looked up in tables made by the same arithmetic. For a test to hold
 * the second way to the first. */
extern int pixfmt_plain;

/* The reverse, for the channels whose bit is set in `channels` (1 red,
 * 2 green, 4 blue, 8 alpha); the others keep what the pixel had. */
void pixfmt_encode(const pixfmt *f, uint8_t *pixel, const float rgba[4], unsigned channels);

/* --- a pixel from four halves, by lookup --------------------------------------------------
 * A colour that arrives as four sixteen-bit floats and goes into a pixel
 * whose fields are fractions, halves or small floats: what each field gets
 * depends on one of the four halves and on nothing else, and there are
 * 65,536 halves. So the long way, each half widened, held to 0..1 if the
 * writer wants that, put on the display's curve if the surface is kept on
 * it (the three colours, not alpha), then pixfmt_encode, can be done for
 * every half once and looked up after. */
typedef struct {
    pixfmt    format;
    uint8_t   on_curve, clamped;
    uint8_t   bytes;                        /* of a pixel */
    unsigned  fields;                       /* fields that take a channel */
    uint8_t   channel[4], shift[4];         /* for each: which channel (0 red .. 3 alpha), and where the field lies */
    uint16_t *becomes[4];                   /* and what each half becomes in it */
    uint64_t  keep;                         /* the pixel's bits that are no channel's: left as they are */
} pixfmt_halves;

/* The tables for pixels of `f`, made the first time they are asked for and
 * kept; NULL when a field of `f` is not such a function of one half (a
 * whole float, a shared exponent) or there is no room for another format.
 * Not to be called from two threads at once. */
const pixfmt_halves *pixfmt_halves_for(const pixfmt *f, int on_curve, int clamped);

/* Store a colour: halves 0 and 1 are the low and high of words[0], red and
 * green; 2 and 3 of words[1], blue and alpha. All four channels are
 * written. */
static inline void pixfmt_halves_store(const pixfmt_halves *t, uint8_t *pixel, const uint32_t words[2]) {
    uint64_t bits = 0;
    if (t->keep) { memcpy(&bits, pixel, t->bytes); bits &= t->keep; }
    for (unsigned k = 0; k < t->fields; k++) {
        unsigned c = t->channel[k];
        bits |= (uint64_t)t->becomes[k][(uint16_t)(words[c >> 1] >> (16 * (c & 1)))] << t->shift[k];
    }
    memcpy(pixel, &bits, t->bytes);
}

/* The curve between a display's values and light. */
float pix_srgb_to_linear(float v);
float pix_linear_to_srgb(float v);

/* --- a picture as brightness and two colour differences -----------------------------------
 * A movie's picture is not kept as red, green and blue but as how bright
 * each pixel is ("Y") and, more coarsely, which way its colour leans:
 * towards blue (the first difference, "U" or Cb) and towards red (the
 * second, "V" or Cr), each 128 for neither. Which arithmetic makes a colour
 * of the three is a matter of standards: two sets of weights (BT.601, of
 * standard definition, and BT.709, of high) over two ranges, the studio's,
 * where brightness runs from 16 to 235 and a difference from 16 to 240, and
 * the whole 0 to 255. The four are numbered as the console numbers them
 * (SceGxmYuvProfile). */
enum { PIX_YUV_BT601 = 0, PIX_YUV_BT709, PIX_YUV_BT601_FULL, PIX_YUV_BT709_FULL };

/* A conversion as what each byte adds to each channel, in sixteen bits of
 * fraction: brightness to all three (with the half that rounds the sum),
 * the red difference to red and green, the blue to green and blue. */
typedef struct { int32_t y[256], rv[256], gu[256], gv[256], bu[256]; } pix_yuv;

/* The tables of a profile. They are made the first time any is asked for. */
const pix_yuv *pix_yuv_for(unsigned profile);

/* One pixel as a word of four bytes: red in the lowest, green, blue, and
 * an alpha of all ones. */
static inline uint32_t pix_yuv_word(const pix_yuv *t, unsigned y, unsigned u, unsigned v) {
    int32_t l = t->y[y], r = l + t->rv[v], g = l + t->gu[u] + t->gv[v], b = l + t->bu[u];
    r = r < 0 ? 0 : r >> 16;
    g = g < 0 ? 0 : g >> 16;
    b = b < 0 ? 0 : b >> 16;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | 0xFF000000u;
}

#endif /* VITARECOMP_PIXFMT_H */
