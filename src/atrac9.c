/* atrac9.c — ATRAC9, the console's audio codec, decoded (atrac9.h).
 *
 * A stream is a run of superframes of one size, each one, two, four or
 * eight frames, and a frame is 64, 128 or 256 samples of every channel.
 * Four bytes of configuration travel beside the stream (in a file's header,
 * or in what a program tells the synthesiser) and say which:
 *
 *   byte 0      0xFE
 *   byte 1      the sample rate, as an index (4 bits); the channel layout
 *               (3 bits); a zero
 *   bytes 2-3   a frame's average size in bytes, less one (11 bits); the
 *               superframe's frame count, as a power of two (2 bits)
 *
 * A frame is a block for each channel or pair of channels of the layout,
 * every block starting on a byte. A block is a transform coder's. Its
 * spectrum (an MDCT, as many coefficients as the frame has samples) is cut
 * into up to thirty units, two coefficients wide at the bottom and sixteen
 * at the top. A unit has a scale factor, a power of two, and a precision,
 * the number of bits its coefficients were quantised to. The scale factors
 * are sent, in one of several ways. The precisions are not: both ends work
 * them out from the scale factors and from a gradient, a curve across the
 * units that the encoder describes in a few numbers. Then the coefficients
 * follow, from a codebook where the precision is low and as plain numbers
 * where it is not, and a second run of bits for the units whose precision
 * is more than fifteen.
 *
 * Two things save bits at the top of the spectrum. A stereo pair may share
 * it: from some unit up only one channel's coefficients are sent, and the
 * other channel's are the same ones, with a sign a unit and that channel's
 * own scale factors. And band extension leaves the top units out and has
 * the decoder make them: the units below mirrored upwards, or noise, at
 * levels sent in a few bits.
 *
 * The frames of a superframe share its bytes as they need them (only the
 * superframe's size is fixed), and a frame may take its band layout and
 * its scale factors from the frame before. The first frame of a superframe
 * takes nothing, so a stream can be entered at any superframe, with only
 * the half window of overlap missing.
 *
 * The codec has no public specification. This is written from two open
 * descriptions of it: LibAtrac9 by Alex Barney (github.com/Thealexbarney/
 * LibAtrac9), which follows the console's decoder closely, and FFmpeg's
 * libavcodec/atrac9dec.c. Where the two disagree this follows the first.
 * Where the first reads past the end of one of its own tables this does
 * not: a gradient that ends beyond the last unit is cut off there, a
 * precision is held to thirty bits, band extension on a block too narrow
 * or too wide for its table is refused, and a level kept from a frame of
 * another mode is held to its own mode's range. The tables are the
 * format's own. The long ones (atrac9_tables.h) are as both descriptions
 * list them, and the two agree entry for entry; the ones that follow a
 * rule are computed from the rule here.
 *
 * Checked against LibAtrac9's decoder: on whole files, where the two give
 * the same sixteen-bit samples to within one, and on random frames for
 * what those files do not use (band extension, the fine bits, every rate).
 * FFmpeg's decoder gives the same samples with their sign turned.
 * tests/test_atrac9.c has what can be checked without a file.
 */

#include "atrac9.h"

#include "atrac9_tables.h"

#include <math.h>
#include <string.h>

#define UNITS_MOST 30
#define PI         3.14159265358979323846

enum { MONO, STEREO, LFE };

/* --- what the format fixes --------------------------------------------------------------- */

static const uint32_t RATES[16] = {
    11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000,
    44100, 48000, 64000, 88200, 96000, 128000, 176400, 192000,
};

/* A frame's samples, as a power of two, and the most bands a block may
 * have, by rate. The upper eight rates are the "high" ones: they code no
 * coefficient from a codebook and count their bands from one, not three. */
static const uint8_t FRAME_LOG2[16] = { 6, 6, 7, 7, 7, 8, 8, 8, 6, 6, 7, 7, 7, 8, 8, 8 };
static const uint8_t BANDS_MOST[16] = { 8, 8, 12, 12, 12, 18, 18, 18, 8, 8, 12, 12, 12, 16, 16, 16 };

/* The channel layouts: mono, two monos, stereo, 5.1, 7.1, four channels. */
static const struct { uint8_t blocks, channels, type[5]; } LAYOUTS[6] = {
    { 1, 1, { MONO } },
    { 2, 2, { MONO, MONO } },
    { 1, 2, { STEREO } },
    { 4, 6, { STEREO, MONO, LFE, STEREO } },
    { 5, 8, { STEREO, MONO, LFE, STEREO, STEREO } },
    { 2, 4, { STEREO, STEREO } },
};

/* A block says how wide it is in bands; this is how many units that is. */
static const uint8_t BAND_UNITS[19] = { 0, 4, 8, 10, 12, 13, 14, 15, 16, 18, 20, 21, 22, 23, 24, 25, 26, 28, 30 };

/* Where each unit's coefficients begin. */
static const uint16_t UNIT_AT[UNITS_MOST + 1] = {
    0, 2, 4, 6, 8, 10, 12, 14, 16, 20, 24, 28, 32, 40, 48, 56,
    64, 72, 80, 88, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240, 256,
};

/* The eight weightings a run of scale factors may be sent against. */
static const uint8_t SF_WEIGHT[8][32] = {
    { 0, 0, 0, 1, 1, 2, 2, 2, 2, 2, 2, 3, 2, 3, 3, 4, 4, 4, 4, 4, 4, 5, 5, 6, 6, 7, 7, 8, 10, 12, 12, 12 },
    { 3, 2, 2, 1, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 2, 3, 3, 4, 5, 7, 10, 10, 10 },
    { 0, 2, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 8, 9, 12, 12, 12 },
    { 0, 1, 1, 2, 2, 2, 3, 3, 3, 3, 3, 4, 4, 4, 5, 5, 5, 6, 6, 6, 6, 7, 8, 8, 10, 11, 11, 12, 13, 13, 13, 13 },
    { 0, 2, 2, 3, 3, 4, 4, 5, 4, 5, 5, 5, 5, 6, 7, 8, 8, 8, 8, 9, 9, 9, 10, 10, 11, 12, 12, 13, 13, 14, 14, 14 },
    { 1, 1, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 5, 6, 7, 7, 9, 11, 11, 11 },
    { 0, 5, 8, 10, 11, 11, 12, 12, 12, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 12, 12, 12, 12, 13, 15, 15, 15 },
    { 0, 2, 3, 4, 5, 6, 6, 7, 7, 8, 8, 8, 9, 9, 10, 10, 10, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 15, 15, 15 },
};

/* The gradient's shape between its two ends, drawn across forty-eight
 * units; a shorter gradient takes every so-manyth point of it. */
static const uint8_t GRADIENT_CURVE[48] = {
    1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 5, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15,
    16, 18, 19, 20, 21, 22, 23, 24, 25, 26, 26, 27, 27, 28, 28, 28, 29, 29, 29, 29, 30, 30, 30, 30,
};

/* Band extension, by how many units the block codes (thirteen to twenty):
 * the two units where the made-up spectrum's groups change, and which of
 * six arrangements of levels ("bands") the block uses. */
static const uint8_t BEX_GROUP[8][3] = {
    { 16, 21, 0 }, { 18, 22, 1 }, { 20, 22, 2 }, { 21, 22, 3 },
    { 21, 22, 3 }, { 23, 24, 4 }, { 23, 24, 4 }, { 24, 24, 5 },
};

/* The levels a channel sends, by mode and arrangement: how many bits each
 * is, and none where the list stops. Mode 1 sends nothing. */
static const uint8_t BEX_BITS[5][6][4] = {
    { { 0 }, { 0 }, { 0 }, { 5, 4, 3, 3 }, { 4, 4, 3, 4 }, { 4, 5 } },
    { { 0 }, { 0 }, { 0 }, { 0 },          { 0 },          { 0 }    },
    { { 0 }, { 0 }, { 0 }, { 6, 6 },       { 6, 6 },       { 6 }    },
    { { 0 }, { 0 }, { 0 }, { 4, 4 },       { 4, 4 },       { 4, 4 } },
    { { 3 }, { 3 }, { 3 }, { 0 },          { 0 },          { 0 }    },
};

/* --- what is made once ------------------------------------------------------------------- */

/* The codebooks, in the order atrac9_tables.h lists them. */
enum {
    BOOK_STEP   = 0,                /* scale factor steps, one to six bits wide */
    BOOK_DIST   = 6,                /* scale factor distances, two to five      */
    BOOK_A      = 10,               /* the spectrum's first set: four a width   */
    BOOK_B      = 34,               /* and its second: three a width            */
    BOOKS       = 52,
    BOOK_WORDS  = 25878             /* what the books' lookup tables come to    */
};

/* A book ready to read with: `longest` bits of the stream index `lookup`,
 * and the word found there is the symbol and its code's true length. A
 * symbol is `values` numbers of `width` bits each. */
typedef struct {
    const uint16_t *lookup;
    uint8_t width, values, longest;
} book;

/* The transform's constants for one frame size. */
typedef struct {
    float   window[ATRAC9_FRAME_MOST];
    float   turn[ATRAC9_FRAME_MOST / 2][2];
    uint8_t swap[ATRAC9_FRAME_MOST / 2];
} transform;

static book      g_book[BOOKS];
static uint16_t  g_lookup[BOOK_WORDS];
static transform g_transform[3];                        /* 64, 128 and 256 samples */
static float     g_root[ATRAC9_FRAME_MOST / 4][2];      /* the 128th roots of one, half way round */
static float     g_step[16], g_fine[16], g_scale[32];
static int       g_ready;

static const uint16_t *make_book(book *k, const uint16_t *codes, uint16_t **room, uint32_t width, uint32_t values) {
    const uint32_t symbols = 1u << (width * values);
    uint32_t longest = 0;
    for (uint32_t s = 0; s < symbols; s++)
        if ((codes[s] & 15u) > longest) longest = codes[s] & 15u;
    for (uint32_t s = 0; s < symbols; s++) {
        const uint32_t length = codes[s] & 15u;
        if (!length) continue;
        /* every index that begins with the code finds the symbol */
        uint16_t *at = *room + ((uint32_t)(codes[s] >> 4) << (longest - length));
        for (uint32_t i = 0; i < 1u << (longest - length); i++) at[i] = (uint16_t)(s << 4 | length);
    }
    k->lookup = *room;
    k->width = (uint8_t)width; k->values = (uint8_t)values; k->longest = (uint8_t)longest;
    *room += (size_t)1 << longest;
    return codes + symbols;
}

static void make_tables(void) {
    if (g_ready) return;

    const uint16_t *codes = ATRAC9_CODES;
    uint16_t *room = g_lookup;
    book *k = g_book;
    for (uint32_t width = 1; width <= 6; width++) codes = make_book(k++, codes, &room, width, 1);
    for (uint32_t width = 2; width <= 5; width++) codes = make_book(k++, codes, &room, width, 1);
    /* The spectrum's books: a width (two to seven bits a coefficient) and a
     * unit size (two, four, eight or sixteen coefficients) have one, and
     * the narrow widths pack two or four coefficients into a symbol. The
     * second set has none for the units of two. */
    for (uint32_t set = 0; set < 2; set++)
        for (uint32_t width = 2; width <= 7; width++)
            for (uint32_t size = set; size < 4; size++)
                codes = make_book(k++, codes, &room, width, width > 4 ? 1 : width > 2 || size == 0 ? 2 : 4);

    /* A coefficient of precision p is a whole number of steps of 2/(2^(p+1)-1);
     * the fine bits are steps 65,535 times smaller; a scale factor f is 2^(f-15). */
    for (int i = 0; i < 16; i++) {
        const double step = 2.0 / (double)((2u << i) - 1u);
        g_step[i] = (float)step;
        g_fine[i] = (float)(step / 65535.0);
    }
    for (int i = 0; i < 32; i++) g_scale[i] = (float)ldexp(1.0, i - 15);

    for (uint32_t j = 0; j < ATRAC9_FRAME_MOST / 4; j++) {
        g_root[j][0] = (float)cos(-2.0 * PI * j / (ATRAC9_FRAME_MOST / 2));
        g_root[j][1] = (float)sin(-2.0 * PI * j / (ATRAC9_FRAME_MOST / 2));
    }
    for (uint32_t size = 0; size < 3; size++) {
        transform *t = &g_transform[size];
        const uint32_t log2 = 6 + size, n = 1u << log2;
        double rise[ATRAC9_FRAME_MOST];
        /* The encoder's window rises as half a sine does, from 0 to 1 across
         * the frame; the decoder's is that divided by the two overlapping
         * halves' squares, so that what the two windows do between them to
         * each sample, summed over the two frames that hold it, is nothing. */
        for (uint32_t i = 0; i < n; i++) rise[i] = (sin(((i + 0.5) / n - 0.5) * PI) + 1.0) * 0.5;
        for (uint32_t i = 0; i < n; i++)
            t->window[i] = (float)(rise[i] / (rise[i] * rise[i] + rise[n - 1 - i] * rise[n - 1 - i]));
        for (uint32_t m = 0; m < n / 2; m++) {
            const double angle = -PI * (8.0 * m + 1.0) / (8.0 * n);
            uint32_t back = 0;
            t->turn[m][0] = (float)cos(angle);
            t->turn[m][1] = (float)sin(angle);
            for (uint32_t bit = 0; bit < log2 - 1; bit++) back |= ((m >> bit) & 1u) << (log2 - 2 - bit);
            t->swap[m] = (uint8_t)back;
        }
    }
    g_ready = 1;
}

/* --- the bits ----------------------------------------------------------------------------
 * Most significant first. Past the end of the superframe there are zeros,
 * and the place keeps counting, which is how reading too far is noticed. */

typedef struct {
    const uint8_t *p;
    uint32_t at, end;               /* in bits */
} bits;

/* The next `n` bits, 25 at most, without taking them. */
static uint32_t peek(const bits *b, uint32_t n) {
    const uint32_t byte = b->at >> 3, last = b->end >> 3;
    uint32_t w = 0;
    for (uint32_t i = 0; i < 4; i++) w = w << 8 | (byte + i < last ? b->p[byte + i] : 0u);
    return n ? (w << (b->at & 7u)) >> (32u - n) : 0u;
}

static uint32_t take(bits *b, uint32_t n) {
    const uint32_t v = peek(b, n);
    b->at += n;
    return v;
}

static int32_t as_signed(uint32_t v, uint32_t n) {
    const uint32_t top = 1u << (n - 1u);
    return (int32_t)(v ^ top) - (int32_t)top;
}

static int32_t take_signed(bits *b, uint32_t n) { return as_signed(take(b, n), n); }

static uint32_t take_symbol(bits *b, const book *k) {
    const uint16_t found = k->lookup[peek(b, k->longest)];
    b->at += found & 15u;
    return found >> 4;
}

/* --- a block's description of itself ------------------------------------------------------ */

/* What a frame says about a block before its channels, and does not keep. */
typedef struct {
    uint32_t mode, boundary;        /* of the gradient */
    uint8_t  gradient[UNITS_MOST + 1];
    uint32_t primary;               /* the channel of a pair whose spectrum is whole */
    uint8_t  flip[UNITS_MOST];      /* the shared units the other channel has upside down */
    uint32_t bex_data;              /* band extension is used in this frame */
} shape;

/* How wide the block is: its units, the unit a pair shares its spectrum
 * from, and the unit band extension reaches to. */
static int read_bands(const atrac9_decoder *d, atrac9_block *k, bits *b) {
    const uint32_t least = d->high ? 1 : 3, reach_most = d->high ? 16 : 18;
    const uint32_t bands = take(b, 4) + least;
    if (bands > BANDS_MOST[d->rate_index]) return 0;
    k->units = BAND_UNITS[bands];
    k->stereo_units = k->units;
    if (k->type == STEREO) {
        const uint32_t from = take(b, 4) + least;
        if (from > bands) return 0;
        k->stereo_units = BAND_UNITS[from];
    }
    k->extends = (uint8_t)take(b, 1);
    k->ext_units = k->units;
    if (k->extends) {
        const uint32_t reach = take(b, 4) + least;
        if (reach < bands || reach > reach_most) return 0;
        k->ext_units = BAND_UNITS[reach];
    }
    return 1;
}

/* The gradient: a value for every unit, level up to its first unit, level
 * after its last, and between them along the curve. */
static int read_gradient(const atrac9_block *k, bits *b, shape *s) {
    uint32_t from, to, low, high;
    s->mode = take(b, 2);
    if (s->mode) {
        from = take(b, 5); to = 31;
        low = take(b, 5); high = 31;
    } else {
        from = take(b, 6); to = take(b, 6) + 1;
        low = take(b, 5); high = take(b, 5);
    }
    s->boundary = take(b, 4);
    if (s->boundary > k->units || from > to || from >= 48 || to >= 48) return 0;

    const int32_t rise = (int32_t)high - (int32_t)low;
    for (uint32_t u = 0; u <= UNITS_MOST; u++) {
        int32_t g = u < to ? (int32_t)low : (int32_t)high;
        if (rise && u >= from && u < to) {
            const int32_t along = GRADIENT_CURVE[(u - from) * 48 / (to - from)];
            g = rise > 0 ? (int32_t)low + 1 + along * (rise - 1) / 31 : (int32_t)low - 1 - along * (-rise - 1) / 31;
        }
        s->gradient[u] = (uint8_t)g;
    }
    return 1;
}

/* A channel's band extension mode. A block too narrow for the modes that
 * send levels has the one that sends a single one. */
static void read_bex_mode(atrac9_channel *c, bits *b, uint32_t arrangement) {
    const uint32_t mode = take(b, 2);
    c->bex_mode = (uint8_t)(arrangement > 2 ? mode : 4);
}

static void read_bex_levels(atrac9_channel *c, bits *b, uint32_t arrangement) {
    const uint8_t *width = BEX_BITS[c->bex_mode][arrangement];
    for (uint32_t i = 0; i < 4 && width[i]; i++) c->bex_value[i] = (uint8_t)take(b, width[i]);
}

static int read_extension(atrac9_block *k, bits *b, shape *s) {
    uint32_t arrangement = 0;
    if (k->extends) {
        if (k->units < 13 || k->units > 20) return 0;
        arrangement = BEX_GROUP[k->units - 13][2];
        if (k->type == STEREO) read_bex_mode(&k->ch[1], b, arrangement);
        else b->at += 1;
    }
    s->bex_data = take(b, 1);
    if (!s->bex_data) return 1;
    if (!k->extends) {
        /* data for an extension the block does not have: its length says how much to pass over */
        b->at += 2;
        b->at += take(b, 5);
        return 1;
    }
    read_bex_mode(&k->ch[0], b, arrangement);
    const uint32_t length = take(b, 5);
    if (!length) return 1;          /* the levels stay as the last frame sent them */
    const uint32_t end = b->at + length;
    read_bex_levels(&k->ch[0], b, arrangement);
    if (k->type == STEREO) read_bex_levels(&k->ch[1], b, arrangement);
    return b->at <= end;
}

/* --- scale factors ------------------------------------------------------------------------
 * Four ways to send a channel's, and which four depends on whether it is the
 * second channel of a pair, which may lean on the first's. Every way gives
 * `n` of them, one for each unit up to where band extension reaches. */

/* Each a step from the one before, from a codebook, and the whole run
 * against one of the weightings. */
static void sf_steps(bits *b, int32_t *sf, uint32_t n) {
    const uint8_t *weight = SF_WEIGHT[take(b, 3)];
    const int32_t base = (int32_t)take(b, 5);
    const uint32_t width = take(b, 2) + 3;
    const book *k = &g_book[BOOK_STEP + width - 1];
    sf[0] = (int32_t)take(b, width);
    for (uint32_t i = 1; i < n; i++) sf[i] = (int32_t)(((uint32_t)sf[i - 1] + take_symbol(b, k)) & ((1u << width) - 1u));
    for (uint32_t i = 0; i < n; i++) sf[i] += base - weight[i];
}

/* Each as a plain number above a base. */
static void sf_plain(bits *b, int32_t *sf, uint32_t n) {
    const uint32_t width = take(b, 2) + 2;
    const int32_t base = width < 5 ? (int32_t)take(b, 5) : 0;
    for (uint32_t i = 0; i < n; i++) sf[i] = (int32_t)take(b, width) + base;
}

/* Each as its distance from the same unit of another run (the last frame's,
 * or the first channel's), from a codebook; plain where the other run is
 * shorter. */
static void sf_distances(bits *b, int32_t *sf, uint32_t n, const int32_t *other, uint32_t other_n) {
    const uint32_t width = take(b, 2) + 2;
    const book *k = &g_book[BOOK_DIST + width - 2];
    const uint32_t shared = n < other_n ? n : other_n;
    for (uint32_t i = 0; i < shared; i++) sf[i] = (other[i] + as_signed(take_symbol(b, k), width)) & 31;
    for (uint32_t i = shared; i < n; i++) sf[i] = (int32_t)take(b, 5);
}

/* Steps again, and the run they make added to the other run. */
static void sf_steps_over(bits *b, int32_t *sf, uint32_t n, const int32_t *other, uint32_t other_n) {
    const int32_t base = (int32_t)take(b, 5) - 16;
    const uint32_t width = take(b, 2) + 1;
    const book *k = &g_book[BOOK_STEP + width - 1];
    const uint32_t shared = n < other_n ? n : other_n;
    sf[0] = (int32_t)take(b, width);
    for (uint32_t i = 1; i < shared; i++) sf[i] = (int32_t)(((uint32_t)sf[i - 1] + take_symbol(b, k)) & ((1u << width) - 1u));
    for (uint32_t i = 0; i < shared; i++) sf[i] += base + other[i];
    for (uint32_t i = shared; i < n; i++) sf[i] = (int32_t)take(b, 5);
}

/* `first` is the first channel's of this frame, for the second of a pair,
 * and NULL otherwise. The first frame of a superframe has no frame before
 * to lean on. */
static int read_scale_factors(const atrac9_block *k, atrac9_channel *c, bits *b, int first_frame,
                              const int32_t *first, int32_t sf[UNITS_MOST + 1]) {
    const uint32_t n = k->ext_units, way = take(b, 2);
    int32_t last[UNITS_MOST + 1];
    for (uint32_t i = 0; i <= UNITS_MOST; i++) { last[i] = c->sf_last[i]; sf[i] = 0; }

    if (way == 0) sf_steps(b, sf, n);
    else if (!first && way == 1) sf_plain(b, sf, n);
    else if (first && way == 1) sf_distances(b, sf, n, first, n);
    else if (first && way == 2) sf_steps_over(b, sf, n, first, n);
    else if (first_frame) return 0;
    else if (!first && way == 3) sf_steps_over(b, sf, n, last, k->units_last);
    else sf_distances(b, sf, n, last, k->units_last);

    for (uint32_t i = 0; i < n; i++)
        if (sf[i] < 0 || sf[i] > 31) return 0;
    for (uint32_t i = 0; i <= UNITS_MOST; i++) c->sf_last[i] = (uint8_t)sf[i];
    return 1;
}

/* --- from scale factors to bits ------------------------------------------------------------ */

/* Every unit's precision. A unit that stands above its neighbour is given
 * more, by up to five bits a side; then the gradient is taken off, the
 * result scaled by the gradient's mode, and the units below the boundary
 * given one more. What is over fifteen is the fine part. */
static void set_precisions(const shape *s, uint32_t units, const int32_t *sf, uint8_t *precision, uint8_t *fine) {
    int32_t lift[UNITS_MOST] = { 0 };
    for (uint32_t u = 1; u < units; u++) {
        const int32_t rise = sf[u] - sf[u - 1];
        if (rise > 1) lift[u] += rise - 1 < 5 ? rise - 1 : 5;
        else if (rise < -1) lift[u - 1] += -rise - 1 < 5 ? -rise - 1 : 5;
    }
    for (uint32_t u = 0; u < units; u++) {
        int32_t p = sf[u] - s->gradient[u];
        if (s->mode) {
            p += lift[u];
            if (p > 0) p = s->mode == 1 ? p / 2 : s->mode == 2 ? 3 * p / 8 : p / 4;
        }
        if (p < 1) p = 1;
        if (u < s->boundary) p++;
        if (p > 30) p = 30;
        precision[u] = (uint8_t)(p > 15 ? 15 : p);
        fine[u] = (uint8_t)(p > 15 ? p - 15 : 0);
    }
}

/* Which of the two sets of codebooks each unit's coefficients come from:
 * the second for a unit whose scale factor stands out from its neighbours',
 * a peak in the spectrum. The lowest eight units always use the first. */
static void choose_sets(uint32_t coded, const int32_t *sf, uint8_t *set) {
    int32_t mean = 0;
    memset(set, 0, UNITS_MOST);
    if (coded > 12) {
        for (uint32_t u = 0; u < 12; u++) mean += sf[u];
        mean = (mean + 6) / 12;
    }
    for (uint32_t u = 8; u < coded; u++) {
        /* the last unit's neighbour above is itself */
        const int32_t here = sf[u], below = sf[u - 1], above = sf[u + 1 < coded ? u + 1 : u];
        const int32_t lower = below < above ? below : above;
        if (here - lower >= 3 || 2 * here - below - above >= 3) set[u] = 1;
        else if (u >= 12 && here - lower >= 2 && here >= mean - (UNIT_AT[u + 1] - UNIT_AT[u] == 16 ? 1 : 0)) set[u] = 1;
    }
}

/* --- the spectrum --------------------------------------------------------------------------- */

/* The coefficients of a channel's first `coded` units, each a count of its
 * unit's steps: the coarse part of every unit, then the fine. */
static void read_spectrum(const atrac9_decoder *d, bits *b, uint32_t coded, const uint8_t *precision,
                          const uint8_t *fine, const uint8_t *set, float *spec) {
    const uint32_t book_most = d->high ? 1 : 7;
    memset(spec, 0, ATRAC9_FRAME_MOST * sizeof spec[0]);
    for (uint32_t u = 0; u < coded; u++) {
        const uint32_t at = UNIT_AT[u], n = UNIT_AT[u + 1] - at, width = precision[u] + 1u;
        const float step = g_step[precision[u]];
        if (width <= book_most) {
            const uint32_t size = n == 2 ? 0 : n == 4 ? 1 : n == 8 ? 2 : 3;
            const book *k = set[u] && size ? &g_book[BOOK_B + (width - 2) * 3 + size - 1]
                                           : &g_book[BOOK_A + (width - 2) * 4 + size];
            for (uint32_t i = 0; i < n;) {
                uint32_t symbol = take_symbol(b, k);
                for (uint32_t v = 0; v < k->values; v++, symbol >>= width)
                    spec[at + i++] = (float)as_signed(symbol & ((1u << width) - 1u), width) * step;
            }
        } else {
            for (uint32_t i = 0; i < n; i++) spec[at + i] = (float)take_signed(b, width) * step;
        }
    }
    for (uint32_t u = 0; u < coded; u++) {
        if (!fine[u]) continue;
        for (uint32_t i = UNIT_AT[u]; i < UNIT_AT[u + 1]; i++) spec[i] += (float)take_signed(b, fine[u] + 1u) * g_fine[fine[u]];
    }
}

/* The codec's own noise: four words of state, seeded once a channel from
 * three of the scale factors it has when noise is first wanted. */
static float noise(atrac9_channel *c, const int32_t *sf) {
    if (!c->noise_seeded) {
        const uint32_t seed = (543u * (uint32_t)(sf[8] + sf[12] + sf[15] + 1)) & 0xFFFFu;
        const uint32_t start = 0x4D93u * (seed ^ (seed >> 14));
        for (uint32_t i = 0; i < 4; i++) c->noise[i] = (uint16_t)(3u - i - start);
        c->noise_seeded = 1;
    }
    const uint16_t t = (uint16_t)(c->noise[3] ^ (c->noise[3] << 5));
    c->noise[3] = c->noise[2];
    c->noise[2] = c->noise[1];
    c->noise[1] = c->noise[0];
    c->noise[0] = (uint16_t)(t ^ c->noise[0] ^ ((t ^ (c->noise[0] >> 5)) >> 4));
    return (float)(c->noise[0] / 65535.0 * 2.0 - 1.0);
}

static void scale_units(float *spec, uint32_t from, uint32_t to, const float *level) {
    for (uint32_t u = from; u < to; u++)
        for (uint32_t i = UNIT_AT[u]; i < UNIT_AT[u + 1]; i++) spec[i] *= level[u - from];
}

/* Band extension: the spectrum above the block's last unit, which was not
 * sent. Three groups of it are each the mirror of what lies just below
 * the group, and the mode says what is done to that: scaled unit by unit,
 * with the top unit noise (mode 0); all noise, at scale factors that were
 * sent for the purpose (1); scaled group by group (2 and 4); or scaled by a
 * level that falls or climbs from one coefficient to the next (3). */
static void extend_bands(const atrac9_block *k, atrac9_channel *c, const int32_t *sf, float *spec) {
    const uint8_t *group = BEX_GROUP[k->units - 13];
    const uint32_t a = k->units, to = group[1] > 22 ? group[1] : 22;
    const uint32_t edge[4] = { UNIT_AT[a], UNIT_AT[group[0]], UNIT_AT[group[1]], UNIT_AT[to] };
    uint32_t value[4];
    float level[UNITS_MOST] = { 0 };

    /* a level kept from a frame of another mode is held to this mode's range */
    for (uint32_t i = 0; i < 4; i++) {
        const uint32_t most = (1u << BEX_BITS[c->bex_mode][group[2]][i]) - 1u;
        value[i] = c->bex_value[i] < most ? c->bex_value[i] : most;
    }

    for (uint32_t g = 0; g < 3; g++)
        for (uint32_t i = 0; edge[g] + i < edge[g + 1]; i++) spec[edge[g] + i] = spec[edge[g] - 1 - i];

    switch (c->bex_mode) {
    case 0: {
        if (group[2] == 3) {
            level[0] = BEX_SCALE_3[0][value[0]]; level[1] = BEX_SCALE_3[1][value[0]]; level[2] = BEX_SCALE_3[2][value[1]];
            level[3] = BEX_SCALE_3[3][value[2]]; level[4] = BEX_SCALE_3[4][value[3]];
        } else if (group[2] == 4) {
            level[0] = BEX_SCALE_4[0][value[0]]; level[1] = BEX_SCALE_4[1][value[0]]; level[2] = BEX_SCALE_4[2][value[1]];
            level[3] = BEX_SCALE_4[3][value[2]]; level[4] = BEX_SCALE_4[4][value[3]];
        } else {
            level[0] = BEX_SCALE_5[0][value[0]]; level[1] = BEX_SCALE_5[1][value[1]]; level[2] = BEX_SCALE_5[2][value[1]];
        }
        level[to - a - 1] = g_scale[sf[a]];
        for (uint32_t i = UNIT_AT[to - 1]; i < UNIT_AT[to]; i++) spec[i] = noise(c, sf);
        scale_units(spec, a, to, level);
        break;
    }
    case 1:
        for (uint32_t u = a; u < to; u++) level[u - a] = g_scale[sf[u]];
        for (uint32_t i = edge[0]; i < edge[3]; i++) spec[i] = noise(c, sf);
        scale_units(spec, a, to, level);
        break;
    case 2:
        for (uint32_t g = 0; g < 2; g++)
            for (uint32_t i = edge[g]; i < edge[g + 1]; i++) spec[i] *= BEX_LEVEL[value[g]];
        break;
    case 3: {
        const float rate = (float)pow(2.0, BEX_SLOPE_RATE[value[1]]);
        float now = BEX_SLOPE_FROM[value[0]];
        for (uint32_t i = edge[0]; i < edge[3]; i++) { now *= rate; spec[i] *= now; }
        break;
    }
    default: {
        static const float FALL[3] = { 0.7079468f, 0.5011902f, 0.3548279f };
        for (uint32_t g = 0; g < 3; g++)
            for (uint32_t i = edge[g]; i < edge[g + 1]; i++) spec[i] *= FALL[g] * BEX_WHOLE[value[0]];
        break;
    }
    }
}

/* --- from spectrum to samples ---------------------------------------------------------------- */

/* A fast Fourier transform of `m` complex numbers already in bit-reversed
 * order, in place. */
static void fourier(float (*z)[2], uint32_t m) {
    for (uint32_t span = 1; span < m; span *= 2) {
        const uint32_t stride = (ATRAC9_FRAME_MOST / 4) / span;
        for (uint32_t i = 0; i < m; i += 2 * span) {
            for (uint32_t j = 0; j < span; j++) {
                const float wr = g_root[j * stride][0], wi = g_root[j * stride][1];
                float *lo = z[i + j], *hi = z[i + j + span];
                const float tr = hi[0] * wr - hi[1] * wi, ti = hi[0] * wi + hi[1] * wr;
                hi[0] = lo[0] - tr; hi[1] = lo[1] - ti;
                lo[0] += tr; lo[1] += ti;
            }
        }
    }
}

/* out[k] = the sum over i of in[i] cos(pi/n (i + 1/2)(k + 1/2)): the cosine
 * transform an MDCT is made of, by way of a Fourier transform half its
 * length. The even inputs and the odd ones, backwards, are paired into
 * complex numbers; each is turned by its place's share of the half-sample
 * offsets before the transform and again after. */
static void cosine_transform(const transform *t, uint32_t n, const float *in, float *out) {
    float z[ATRAC9_FRAME_MOST / 2][2];
    const uint32_t half = n / 2;
    for (uint32_t m = 0; m < half; m++) {
        const float a = in[2 * m], b = in[n - 1 - 2 * m], c = t->turn[m][0], s = t->turn[m][1];
        float *to = z[t->swap[m]];
        to[0] = a * c - b * s;
        to[1] = a * s + b * c;
    }
    fourier(z, half);
    for (uint32_t m = 0; m < half; m++) {
        const float c = t->turn[m][0], s = t->turn[m][1];
        out[2 * m] = z[m][0] * c - z[m][1] * s;
        out[n - 1 - 2 * m] = -(z[m][0] * s + z[m][1] * c);
    }
}

/* A frame of one channel. The transform of the spectrum, unfolded, is two
 * frames long: its first frame, windowed, completes what the last call left
 * in `overlap`, and its second is left there for the next. */
static void to_samples(const transform *t, uint32_t n, const float *spec, float *overlap, float *out, uint32_t stride) {
    float d[ATRAC9_FRAME_MOST];
    const uint32_t half = n / 2;
    cosine_transform(t, n, spec, d);
    for (uint32_t i = 0; i < half; i++) {
        out[i * stride] = overlap[i] + t->window[i] * d[half + i];
        out[(half + i) * stride] = overlap[half + i] - t->window[half + i] * d[n - 1 - i];
        overlap[i] = -t->window[n - 1 - i] * d[half - 1 - i];
        overlap[half + i] = -t->window[half - 1 - i] * d[i];
    }
}

/* --- a block, a frame, a superframe ----------------------------------------------------------- */

/* One block of one frame: its channels' samples to `out`, `stride` apart. */
static int decode_block(const atrac9_decoder *d, atrac9_block *k, bits *b, int first_frame, float *out, uint32_t stride) {
    float spec[2][ATRAC9_FRAME_MOST];
    int32_t sf[2][UNITS_MOST + 1];
    uint8_t precision[UNITS_MOST], fine[UNITS_MOST], set[UNITS_MOST];
    const uint32_t n = d->info.frame_samples;
    const uint32_t first = !take(b, 1), reuse = take(b, 1);
    if (first != (uint32_t)first_frame) return 0;

    if (k->type == LFE) {
        /* the effects channel: two units, each with a plain scale factor, at
         * one of two precisions */
        const uint32_t width = reuse ? 9 : 5;
        sf[0][0] = (int32_t)take(b, 5);
        sf[0][1] = (int32_t)take(b, 5);
        memset(spec[0], 0, sizeof spec[0]);
        for (uint32_t u = 0; u < 2; u++)
            for (uint32_t i = UNIT_AT[u]; i < UNIT_AT[u + 1]; i++)
                spec[0][i] = (float)take_signed(b, width) * g_step[width - 1] * g_scale[sf[0][u]];
    } else {
        shape s;
        memset(&s, 0, sizeof s);
        if (first && reuse) return 0;
        if (!reuse && !read_bands(d, k, b)) return 0;
        if (!k->units || !read_gradient(k, b, &s)) return 0;
        if (k->type == STEREO) {
            s.primary = take(b, 1);
            if (take(b, 1))
                for (uint32_t u = k->stereo_units; u < k->units; u++) s.flip[u] = (uint8_t)take(b, 1);
        }
        if (!read_extension(k, b, &s)) return 0;

        for (uint32_t c = 0; c < k->channels; c++) {
            const uint32_t coded = c == s.primary ? k->units : k->stereo_units;
            if (!read_scale_factors(k, &k->ch[c], b, first_frame, c ? sf[0] : NULL, sf[c])) return 0;
            set_precisions(&s, k->units, sf[c], precision, fine);
            if (d->high) memset(set, 0, sizeof set); else choose_sets(coded, sf[c], set);
            read_spectrum(d, b, coded, precision, fine, set, spec[c]);
        }
        k->units_last = k->extends ? k->ext_units : k->units;

        /* the shared top of a pair's spectrum, before either channel's scale factors */
        for (uint32_t u = k->stereo_units; k->type == STEREO && u < k->units; u++)
            for (uint32_t i = UNIT_AT[u]; i < UNIT_AT[u + 1]; i++)
                spec[!s.primary][i] = s.flip[u] ? -spec[s.primary][i] : spec[s.primary][i];

        for (uint32_t c = 0; c < k->channels; c++) {
            for (uint32_t u = 0; u < k->units; u++)
                for (uint32_t i = UNIT_AT[u]; i < UNIT_AT[u + 1]; i++) spec[c][i] *= g_scale[sf[c][u]];
            if (k->extends && s.bex_data) extend_bands(k, &k->ch[c], sf[c], spec[c]);
        }
    }

    for (uint32_t c = 0; c < k->channels; c++)
        to_samples(&g_transform[d->size], n, spec[c], k->ch[c].overlap, out + c, stride);
    return 1;
}

int atrac9_config(const uint8_t config[4], atrac9_info *info) {
    const uint32_t rate = config[1] >> 4, layout = (config[1] >> 1) & 7u;
    if (config[0] != 0xFE || (config[1] & 1u) || layout >= 6) return 0;
    const uint32_t frame_bytes = (((uint32_t)config[2] << 3) | (config[3] >> 5)) + 1u;
    const uint32_t frames_log2 = (config[3] >> 3) & 3u;
    info->rate = RATES[rate];
    info->channels = LAYOUTS[layout].channels;
    info->frame_samples = 1u << FRAME_LOG2[rate];
    info->frames = 1u << frames_log2;
    info->superframe_bytes = frame_bytes << frames_log2;
    info->superframe_samples = info->frame_samples << frames_log2;
    return 1;
}

int atrac9_open(atrac9_decoder *d, const uint8_t config[4]) {
    memset(d, 0, sizeof *d);
    if (!atrac9_config(config, &d->info)) return 0;
    const uint32_t layout = (config[1] >> 1) & 7u;
    d->rate_index = config[1] >> 4;
    d->high = d->rate_index > 7;
    d->size = (uint8_t)(FRAME_LOG2[d->rate_index] - 6);
    d->blocks = LAYOUTS[layout].blocks;
    for (uint32_t i = 0; i < d->blocks; i++) {
        d->block[i].type = LAYOUTS[layout].type[i];
        d->block[i].channels = d->block[i].type == STEREO ? 2 : 1;
    }
    make_tables();
    return 1;
}

void atrac9_reset(atrac9_decoder *d) {
    for (uint32_t i = 0; i < d->blocks; i++) {
        const uint8_t type = d->block[i].type, channels = d->block[i].channels;
        memset(&d->block[i], 0, sizeof d->block[i]);
        d->block[i].type = type;
        d->block[i].channels = channels;
    }
}

int atrac9_decode(atrac9_decoder *d, const uint8_t *in, float *out) {
    const uint32_t stride = d->info.channels;
    bits b = { in, 0, d->info.superframe_bytes * 8u };
    int good = 1;
    for (uint32_t f = 0; good && f < d->info.frames; f++) {
        float *frame = out + (size_t)f * d->info.frame_samples * stride;
        uint32_t channel = 0;
        for (uint32_t i = 0; good && i < d->blocks; i++) {
            good = decode_block(d, &d->block[i], &b, f == 0, frame + channel, stride);
            channel += d->block[i].channels;
            b.at = (b.at + 7u) & ~7u;
        }
    }
    if (good && b.at <= b.end) return 1;
    memset(out, 0, (size_t)d->info.superframe_samples * stride * sizeof out[0]);
    atrac9_reset(d);
    return 0;
}

void atrac9_pcm16(const float *in, int16_t *out, size_t count) {
    for (size_t i = 0; i < count; i++) {
        const float v = floorf(in[i] + 0.5f);
        out[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int32_t)v);
    }
}

/* --- the file --------------------------------------------------------------------------------
 * RIFF, as a .wav is: "RIFF", a length, "WAVE", then chunks of a four-letter
 * name, a length and that many bytes, padded to an even count.
 *
 *   "fmt "  the extensible wave format (tag 0xFFFE): the usual fields, a
 *           sub-format identifier at 0x18 that names the codec, a version at
 *           0x28 and the configuration at 0x2C
 *   "fact"  the sound's length in samples, and (third word) how many decoded
 *           samples come before its first: the decoder's own delay
 *   "smpl"  the sampler chunk: a count of loops at 0x1C and the first loop's
 *           start and end at 0x2C and 0x30
 *   "data"  the superframes
 */

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int atrac9_file_open(const uint8_t *file, size_t size, atrac9_file *out) {
    static const uint8_t CODEC[16] = { 0xD2, 0x42, 0xE1, 0x47, 0xBA, 0x36, 0x8D, 0x4D, 0x88, 0xFC, 0x61, 0x65, 0x4F, 0x8C, 0x83, 0x6C };
    int have_format = 0;
    memset(out, 0, sizeof *out);
    if (size < 12 || memcmp(file, "RIFF", 4) || memcmp(file + 8, "WAVE", 4)) return 0;
    for (size_t at = 12; at + 8 <= size;) {
        const uint8_t *chunk = file + at + 8;
        const size_t room = size - at - 8;
        size_t length = le32(file + at + 4);
        if (!memcmp(file + at, "data", 4)) {
            out->data = at + 8;
            out->data_bytes = length < room ? length : room;
            break;
        }
        if (length > room) return 0;
        if (!memcmp(file + at, "fmt ", 4)) {
            if (length < 0x30 || (chunk[0] | chunk[1] << 8) != 0xFFFE || memcmp(chunk + 0x18, CODEC, 16)) return 0;
            memcpy(out->config, chunk + 0x2C, 4);
            have_format = 1;
        } else if (!memcmp(file + at, "fact", 4) && length >= 4) {
            out->samples = le32(chunk);
            if (length >= 12) out->delay = le32(chunk + 8);
        } else if (!memcmp(file + at, "smpl", 4) && length >= 0x34 && le32(chunk + 0x1C)) {
            out->loops = 1;
            out->loop_start = le32(chunk + 0x2C);
            out->loop_end = le32(chunk + 0x30);
        }
        at += 8 + length + (length & 1);
    }
    atrac9_info info;
    return have_format && out->data && atrac9_config(out->config, &info);
}
