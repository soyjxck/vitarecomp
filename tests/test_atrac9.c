/* test_atrac9.c — the ATRAC9 decoder, on streams made here.
 *
 * No title's file is in the repository, so the streams are written by a
 * small encoder of this file's own: a bit writer and the layout of a
 * frame, from the same description the decoder was written from. It is not
 * a codec. It allots no bits and models nothing: it writes exactly the
 * spectrum it is handed. Three kinds of check come of that.
 *
 * A round trip. A signal goes through a forward transform written here
 * from its definition, is coded with all the precision the format has, and
 * is decoded. The decoder's window, transform, overlap, steps and scale
 * factors have to give the signal back, one frame late.
 *
 * Twins. One spectrum is written two ways, and the two streams have to
 * decode to the same samples, bit for bit: coefficients from the codebooks
 * against the same coefficients plain (a stream at one of the high rates
 * has no codebooks, so it is the plain twin), scale factors sent each of
 * the ways there are against the same ones sent plainly, and a stereo
 * pair's shared top against the same top written out for both channels.
 *
 * Refusals. What is not a stream is turned away, and costs the decoder
 * nothing but the frame before.
 *
 * What the decoder makes of real files is checked elsewhere, against other
 * decoders, on files that cannot be here (`armrecomp at9` decodes one).
 */

#include "../src/atrac9.h"
#include "../src/atrac9_tables.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#error "the tests check with assert(): build them without NDEBUG"
#endif

#define PI    3.14159265358979323846
#define UNITS 30

/* --- what the format fixes, as this file needs it ---------------------------------------- */

static const int BAND_UNITS[19] = { 0, 4, 8, 10, 12, 13, 14, 15, 16, 18, 20, 21, 22, 23, 24, 25, 26, 28, 30 };
static const int UNIT_AT[UNITS + 1] = {
    0, 2, 4, 6, 8, 10, 12, 14, 16, 20, 24, 28, 32, 40, 48, 56,
    64, 72, 80, 88, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240, 256,
};
/* the third of the eight weightings a run of scale factors may be sent against */
static const int WEIGHT_2[32] = { 0, 2, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 8, 9, 12, 12, 12 };

/* How many numbers a spectrum book's symbol holds. */
static int book_values(int width, int size) { return width > 4 ? 1 : width > 2 || size == 0 ? 2 : 4; }

/* Where a book begins in the table: the six books of scale factor steps
 * (one to six bits wide), the four of distances (two to five), the
 * spectrum's first set (a width of two to seven, a unit size of 2, 4, 8 or
 * 16 coefficients) and its second (the same without the units of two). */
static const uint16_t *step_book(int width) {
    const uint16_t *at = ATRAC9_CODES;
    for (int w = 1; w < width; w++) at += (size_t)1 << w;
    return at;
}
static const uint16_t *distance_book(int width) {
    const uint16_t *at = step_book(7);
    for (int w = 2; w < width; w++) at += (size_t)1 << w;
    return at;
}
static const uint16_t *spectrum_book(int set, int width, int size) {
    const uint16_t *at = distance_book(6);
    for (int s = 0; s < 2; s++)
        for (int w = 2; w <= 7; w++)
            for (int z = s; z < 4; z++) {
                if (s == set && w == width && z == size) return at;
                at += (size_t)1 << (w * book_values(w, z));
            }
    assert(!"no such book");
    return NULL;
}

/* A coefficient of precision p is a whole number of steps of this size, and
 * its fine part a whole number of steps 65,535 times smaller. */
static double step_of(int precision) { return 2.0 / (double)((2u << precision) - 1u); }

/* --- a bit writer: most significant bit first ---------------------------------------------- */

typedef struct {
    uint8_t buf[1 << 15];
    size_t  at;                     /* in bits */
} bw;

static void put(bw *w, uint32_t v, int n) {
    for (int i = n - 1; i >= 0; i--, w->at++) {
        assert(w->at < sizeof w->buf * 8);
        if ((v >> i) & 1) w->buf[w->at >> 3] |= (uint8_t)(0x80 >> (w->at & 7));
    }
}

static void put_code(bw *w, const uint16_t *book, uint32_t symbol) {
    assert(book[symbol] & 15);      /* the book has the symbol */
    put(w, book[symbol] >> 4, book[symbol] & 15);
}

static uint32_t g_seed = 20261012u;
static int random_below(int n) {
    g_seed = g_seed * 1664525u + 1013904223u;
    return (int)((g_seed >> 8) % (uint32_t)n);
}

/* --- a block, as its spectrum and the few numbers that describe it --------------------------- */

enum { PLAIN = 1 };                 /* the way to send scale factors that needs nothing else */

typedef struct {
    int  channels;                  /* 1, or 2 for a pair */
    int  units;
    int  shared_from;               /* a pair: the unit from which only one channel's spectrum is sent */
    int  primary;                   /* and which channel's */
    int  flip[UNITS];               /* and the shared units the other channel has upside down */
    int  gradient;                  /* the same for every unit: a unit's precision is its scale factor less this */
    int  sf[2][UNITS];
    int  way[2];                    /* how each channel's scale factors are sent */
    int  set[UNITS];                /* the units whose scale factor stands out: the second set of books */
    int  coarse[2][256], fine[2][256];      /* each coefficient, in its unit's steps */
} block;

static int precision_of(const block *b, int c, int u) {
    int p = b->sf[c][u] - b->gradient;
    return p < 1 ? 1 : p > 30 ? 30 : p;
}

static void put_steps(bw *w, const int *sf, int n) {
    int t[UNITS], base = 31, most = 0, width = 3;
    for (int i = 0; i < n; i++) if (sf[i] + WEIGHT_2[i] < base) base = sf[i] + WEIGHT_2[i];
    for (int i = 0; i < n; i++) { t[i] = sf[i] + WEIGHT_2[i] - base; if (t[i] > most) most = t[i]; }
    while ((1 << width) <= most) width++;
    assert(width <= 6);
    put(w, 2, 3);                   /* the weighting */
    put(w, (uint32_t)base, 5);
    put(w, (uint32_t)(width - 3), 2);
    put(w, (uint32_t)t[0], width);
    for (int i = 1; i < n; i++) put_code(w, step_book(width), (uint32_t)(t[i] - t[i - 1]) & ((1u << width) - 1u));
}

static void put_plain(bw *w, const int *sf, int n) {
    put(w, 3, 2);                   /* five bits each, and so no base */
    for (int i = 0; i < n; i++) put(w, (uint32_t)sf[i], 5);
}

static void put_distances(bw *w, const int *sf, const int *other, int n) {
    put(w, 3, 2);                   /* from the book five bits wide */
    for (int i = 0; i < n; i++) put_code(w, distance_book(5), (uint32_t)(sf[i] - other[i]) & 31u);
}

static void put_steps_over(bw *w, const int *sf, const int *other, int n) {
    int t[UNITS], base = 15, most = 0, width = 1;
    for (int i = 0; i < n; i++) if (sf[i] - other[i] < base) base = sf[i] - other[i];
    assert(base >= -16);
    for (int i = 0; i < n; i++) { t[i] = sf[i] - other[i] - base; if (t[i] > most) most = t[i]; }
    while ((1 << width) <= most) width++;
    assert(width <= 4);
    put(w, (uint32_t)(base + 16), 5);
    put(w, (uint32_t)(width - 1), 2);
    put(w, (uint32_t)t[0], width);
    for (int i = 1; i < n; i++) put_code(w, step_book(width), (uint32_t)(t[i] - t[i - 1]) & ((1u << width) - 1u));
}

/* One block of one frame. `last` is the block as the frame before had it,
 * for the ways of sending scale factors that lean on that. */
static void put_block(bw *w, const block *b, const block *last, int high_rate, int first_frame) {
    int bands = 0, shared_bands = 0;
    const int least = high_rate ? 1 : 3;
    while (BAND_UNITS[bands] != b->units) bands++;
    while (BAND_UNITS[shared_bands] != b->shared_from) shared_bands++;

    put(w, first_frame ? 0 : 1, 1);
    put(w, 0, 1);                               /* a band layout of its own */
    put(w, (uint32_t)(bands - least), 4);
    if (b->channels == 2) put(w, (uint32_t)(shared_bands - least), 4);
    put(w, 0, 1);                               /* no band extension */

    put(w, 0, 2);                               /* the gradient: its first way of being sent, */
    put(w, 1, 6); put(w, 0, 6);                 /* from unit 1 to unit 1, so no slope at all, */
    put(w, (uint32_t)b->gradient, 5); put(w, (uint32_t)b->gradient, 5);     /* level before it and after */
    put(w, 0, 4);                               /* and no unit given a bit more */

    if (b->channels == 2) {
        int any = 0;
        for (int u = b->shared_from; u < b->units; u++) any |= b->flip[u];
        put(w, (uint32_t)b->primary, 1);
        put(w, (uint32_t)any, 1);
        for (int u = b->shared_from; any && u < b->units; u++) put(w, (uint32_t)b->flip[u], 1);
    }
    put(w, 0, 1);                               /* no band extension data */

    for (int c = 0; c < b->channels; c++) {
        const int coded = b->channels == 1 || c == b->primary ? b->units : b->shared_from;
        const int way = b->way[c];
        put(w, (uint32_t)way, 2);
        if (way == 0) put_steps(w, b->sf[c], b->units);
        else if (c == 0 && way == 1) put_plain(w, b->sf[c], b->units);
        else if (c == 0 && way == 2) put_distances(w, b->sf[c], last->sf[c], b->units);
        else if (c == 0) put_steps_over(w, b->sf[c], last->sf[c], b->units);
        else if (way == 1) put_distances(w, b->sf[c], b->sf[0], b->units);
        else if (way == 2) put_steps_over(w, b->sf[c], b->sf[0], b->units);
        else put_distances(w, b->sf[c], last->sf[c], b->units);

        for (int u = 0; u < coded; u++) {
            const int p = precision_of(b, c, u), width = (p > 15 ? 15 : p) + 1;
            const int at = UNIT_AT[u], n = UNIT_AT[u + 1] - at;
            if (!high_rate && width <= 7) {
                const int size = n == 2 ? 0 : n == 4 ? 1 : n == 8 ? 2 : 3, values = book_values(width, size);
                const uint16_t *book = spectrum_book(b->set[u], width, size);
                for (int i = 0; i < n; i += values) {
                    uint32_t symbol = 0;
                    for (int v = 0; v < values; v++)
                        symbol |= ((uint32_t)b->coarse[c][at + i + v] & ((1u << width) - 1u)) << (v * width);
                    put_code(w, book, symbol);
                }
            } else {
                for (int i = 0; i < n; i++) put(w, (uint32_t)b->coarse[c][at + i] & ((1u << width) - 1u), width);
            }
        }
        for (int u = 0; u < coded; u++) {
            const int p = precision_of(b, c, u);
            if (p <= 15) continue;
            for (int i = UNIT_AT[u]; i < UNIT_AT[u + 1]; i++) put(w, (uint32_t)b->fine[c][i] & ((2u << (p - 15)) - 1u), p - 15 + 1);
        }
    }
    w->at = (w->at + 7) & ~(size_t)7;
}

/* The four bytes that go beside a stream. */
static void configuration(uint8_t config[4], int rate_index, int layout, int frame_bytes, int frames_log2) {
    config[0] = 0xFE;
    config[1] = (uint8_t)(rate_index << 4 | layout << 1);
    config[2] = (uint8_t)((frame_bytes - 1) >> 3);
    config[3] = (uint8_t)(((frame_bytes - 1) & 7) << 5 | frames_log2 << 3);
}

/* --- the tests ------------------------------------------------------------------------------- */

static void test_configuration(void) {
    static const uint8_t music[4] = { 0xFE, 0x74, 0x0B, 0xF0 }, speech[4] = { 0xFE, 0x70, 0x03, 0xF0 };
    atrac9_info info;
    assert(atrac9_config(music, &info));
    assert(info.rate == 48000 && info.channels == 2 && info.frame_samples == 256 && info.frames == 4);
    assert(info.superframe_bytes == 384 && info.superframe_samples == 1024);
    assert(atrac9_config(speech, &info));
    assert(info.rate == 48000 && info.channels == 1 && info.superframe_bytes == 128 && info.superframe_samples == 1024);

    uint8_t c[4];
    configuration(c, 1, 0, 100, 0);             /* 12,000 Hz: the short frame, alone in its superframe */
    assert(atrac9_config(c, &info) && info.rate == 12000 && info.frame_samples == 64 && info.frames == 1 && info.superframe_bytes == 100);
    configuration(c, 4, 3, 2048, 3);            /* 24,000 Hz, 5.1, the largest superframe there is */
    assert(atrac9_config(c, &info) && info.rate == 24000 && info.channels == 6 && info.frame_samples == 128);
    assert(info.frames == 8 && info.superframe_bytes == ATRAC9_SUPERFRAME_BYTES_MOST && info.superframe_samples == 1024);

    memcpy(c, music, 4); c[0] = 0xFF;
    assert(!atrac9_config(c, &info));           /* not the mark */
    memcpy(c, music, 4); c[1] |= 1;
    assert(!atrac9_config(c, &info));           /* the bit that is always clear */
    configuration(c, 7, 6, 96, 2);
    assert(!atrac9_config(c, &info));           /* there are six layouts */
    static atrac9_decoder d;
    assert(!atrac9_open(&d, c));
    printf("  configurations                 ok\n");
}

static void le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void test_file(void) {
    static const uint8_t CODEC[16] = { 0xD2, 0x42, 0xE1, 0x47, 0xBA, 0x36, 0x8D, 0x4D, 0x88, 0xFC, 0x61, 0x65, 0x4F, 0x8C, 0x83, 0x6C };
    uint8_t f[12 + 60 + 20 + 68 + 8 + 768];
    atrac9_file at;
    memset(f, 0, sizeof f);
    memcpy(f, "RIFF", 4); le32(f + 4, sizeof f - 8); memcpy(f + 8, "WAVE", 4);
    uint8_t *fmt = f + 12;
    memcpy(fmt, "fmt ", 4); le32(fmt + 4, 52);
    fmt[8] = 0xFE; fmt[9] = 0xFF; fmt[10] = 2; le32(fmt + 12, 48000); fmt[20] = 0x80; fmt[21] = 1;
    memcpy(fmt + 8 + 0x18, CODEC, 16);
    fmt[8 + 0x2C] = 0xFE; fmt[8 + 0x2D] = 0x74; fmt[8 + 0x2E] = 0x0B; fmt[8 + 0x2F] = 0xF0;
    uint8_t *fact = fmt + 60;
    memcpy(fact, "fact", 4); le32(fact + 4, 12); le32(fact + 8, 1500); le32(fact + 12, 256); le32(fact + 16, 256);
    uint8_t *smpl = fact + 20;
    memcpy(smpl, "smpl", 4); le32(smpl + 4, 60); le32(smpl + 8 + 0x1C, 1); le32(smpl + 8 + 0x2C, 300); le32(smpl + 8 + 0x30, 1400);
    uint8_t *data = smpl + 68;
    memcpy(data, "data", 4); le32(data + 4, 768);

    assert(atrac9_file_open(f, sizeof f, &at));
    assert(!memcmp(at.config, "\xFE\x74\x0B\xF0", 4));
    assert(at.data == sizeof f - 768 && at.data_bytes == 768);
    assert(at.samples == 1500 && at.delay == 256);
    assert(at.loops && at.loop_start == 300 && at.loop_end == 1400);

    assert(atrac9_file_open(f, sizeof f - 384, &at) && at.data_bytes == 384);  /* cut short: what is there */
    assert(!atrac9_file_open(f, 100, &at));                 /* no data at all */
    fmt[8 + 0x18] ^= 1;
    assert(!atrac9_file_open(f, sizeof f, &at));            /* another codec's identifier */
    fmt[8 + 0x18] ^= 1;
    f[0] = 'X';
    assert(!atrac9_file_open(f, sizeof f, &at));
    printf("  the file's header              ok\n");
}

/* The window an encoder cuts a frame with, rising across the frame. */
static double rise(int i, int n) { return (sin(((i + 0.5) / n - 0.5) * PI) + 1.0) * 0.5; }

/* A frame's spectrum, from the frame before (x[0..n)) and the frame itself
 * (x[n..2n)): the two windowed, folded onto n numbers so that what one
 * frame folds wrongly the next folds back, and those transformed. */
static void forward(const double *x, int n, double *spectrum) {
    double a[512], v[256];
    const int half = n / 2;
    for (int i = 0; i < n; i++) {
        a[i] = rise(i, n) * x[i];
        a[2 * n - 1 - i] = rise(i, n) * x[2 * n - 1 - i];
    }
    for (int i = 0; i < half; i++) {
        v[i] = -a[n + half + i] - a[n + half - 1 - i];
        v[half + i] = a[i] - a[n - 1 - i];
    }
    for (int k = 0; k < n; k++) {
        double sum = 0;
        for (int j = 0; j < n; j++) sum += v[j] * cos(PI / n * (j + 0.5) * (k + 0.5));
        spectrum[k] = sum * 2.0 / n;
    }
}

/* A spectrum into a block, each unit at the scale factor that just holds
 * it and with every bit that scale factor's precision gives. */
static void quantise(block *b, int c, const double *spectrum) {
    for (int u = 0; u < b->units; u++) {
        double most = 0;
        int sf = 8;                             /* at least the precision that is sent plain */
        for (int i = UNIT_AT[u]; i < UNIT_AT[u + 1]; i++) if (fabs(spectrum[i]) > most) most = fabs(spectrum[i]);
        while (sf < 31 && ldexp(1.0, sf - 15) < most * 1.01) sf++;
        b->sf[c][u] = sf;
        const int p = precision_of(b, c, u), coarse = p > 15 ? 15 : p;
        for (int i = UNIT_AT[u]; i < UNIT_AT[u + 1]; i++) {
            const double t = spectrum[i] / ldexp(1.0, sf - 15);
            double q = floor(t / step_of(coarse) + 0.5);
            if (q > (double)((1 << coarse) - 1)) q = (double)((1 << coarse) - 1);
            if (q < -(double)(1 << coarse)) q = -(double)(1 << coarse);
            b->coarse[c][i] = (int)q;
            b->fine[c][i] = 0;
            if (p > 15) {
                const double left = t - q * step_of(coarse), fine_step = step_of(p - 15) / 65535.0;
                double qf = floor(left / fine_step + 0.5);
                if (qf > (double)((1 << (p - 15)) - 1)) qf = (double)((1 << (p - 15)) - 1);
                if (qf < -(double)(1 << (p - 15))) qf = -(double)(1 << (p - 15));
                b->fine[c][i] = (int)qf;
            }
        }
    }
}

/* A signal coded and decoded, at one frame size and one superframe length. */
static void round_trip(int rate_index, int bands, int frames_log2, const char *what) {
    enum { FRAMES = 16 };
    static bw w;
    static atrac9_decoder d;
    static double x[(FRAMES + 2) * 256];
    static float out[FRAMES * 256];
    uint8_t config[4];
    atrac9_info info;
    const int frame_bytes = 1200, frames = 1 << frames_log2;
    configuration(config, rate_index, 0, frame_bytes, frames_log2);
    assert(atrac9_config(config, &info) && atrac9_open(&d, config));
    const int n = (int)info.frame_samples;

    /* three tones that fit no frame, and a step in the middle: silence before the first sample */
    memset(x, 0, sizeof x);
    for (int i = 0; i < FRAMES * n; i++)
        x[n + i] = 6000.0 * sin(0.0371 * i) + 2500.0 * sin(0.61 * i + 1.0) + 900.0 * sin(2.4 * i) + (i > 5 * n ? 1500.0 : 0.0);

    memset(&w, 0, sizeof w);
    for (int f = 0; f < FRAMES; f++) {
        static block b;
        double spectrum[256];
        memset(&b, 0, sizeof b);
        b.channels = 1; b.units = BAND_UNITS[bands]; b.shared_from = b.units; b.way[0] = PLAIN;
        forward(x + (size_t)f * n, n, spectrum);
        quantise(&b, 0, spectrum);
        if (f % frames == 0) w.at = (size_t)(f / frames) * info.superframe_bytes * 8;
        put_block(&w, &b, NULL, 0, f % frames == 0);
        assert(w.at <= (size_t)(f / frames + 1) * info.superframe_bytes * 8);
    }
    for (int s = 0; s < FRAMES / frames; s++)
        assert(atrac9_decode(&d, w.buf + (size_t)s * info.superframe_bytes, out + (size_t)s * info.superframe_samples));

    /* the frame that comes out of a call is the one that went into the call before */
    double noise = 0, signal = 0, worst = 0;
    for (int i = 0; i < FRAMES * n; i++) {
        const double e = (double)out[i] - x[i];
        if (fabs(e) > worst) worst = fabs(e);
        noise += e * e;
        signal += x[i] * x[i];
    }
    const double snr = 10.0 * log10(signal / noise);
    printf("  round trip, %-18s ok   (worst sample off by %.4f, %.0f dB)\n", what, worst, snr);
    assert(worst < 0.25 && snr > 90.0);
}

/* A block of numbers to code, that the books have codes for: a width of
 * w bits holds -(2^(w-1) - 1) to 2^(w-1) - 1, and not every group of them
 * is in a book. */
static void random_spectrum(block *b, int c) {
    for (int u = 0; u < b->units; u++) {
        const int p = precision_of(b, c, u), coarse = p > 15 ? 15 : p, width = coarse + 1;
        const int at = UNIT_AT[u], n = UNIT_AT[u + 1] - at, most = (1 << coarse) - 1;
        const int size = n == 2 ? 0 : n == 4 ? 1 : n == 8 ? 2 : 3, values = width <= 7 ? book_values(width, size) : 1;
        for (int i = 0; i < n; i += values) {
            for (;;) {
                uint32_t symbol = 0;
                for (int v = 0; v < values; v++) {
                    b->coarse[c][at + i + v] = random_below(2 * most + 1) - most;
                    symbol |= ((uint32_t)b->coarse[c][at + i + v] & ((1u << width) - 1u)) << (v * width);
                }
                if (width > 7 || (spectrum_book(b->set[u], width, size)[symbol] & 15)) break;
            }
        }
        /* the fine part as far below nothing as above, so that a spectrum can be turned upside down */
        for (int i = at; i < at + n; i++) b->fine[c][i] = p > 15 ? random_below((2 << (p - 15)) - 1) - ((1 << (p - 15)) - 1) : 0;
    }
}

/* Two streams decoded, a superframe of each at a time, and their samples compared. */
static void same(const uint8_t *config_a, const bw *a, const uint8_t *config_b, const bw *b, int superframes) {
    static atrac9_decoder da, db;
    static float oa[2 * 2048], ob[2 * 2048];
    assert(atrac9_open(&da, config_a) && atrac9_open(&db, config_b));
    assert(da.info.channels == db.info.channels && da.info.superframe_samples == db.info.superframe_samples);
    for (int s = 0; s < superframes; s++) {
        assert(atrac9_decode(&da, a->buf + (size_t)s * da.info.superframe_bytes, oa));
        assert(atrac9_decode(&db, b->buf + (size_t)s * db.info.superframe_bytes, ob));
        double energy = 0;
        for (uint32_t i = 0; i < da.info.superframe_samples * da.info.channels; i++) energy += (double)oa[i] * oa[i];
        assert(energy > 0);         /* nothing compared with nothing proves nothing */
        assert(!memcmp(oa, ob, sizeof(float) * da.info.superframe_samples * da.info.channels));
    }
}

/* Every book of both sets: frames whose units are all at one precision, from
 * one bit to six, coded with the books at 48,000 Hz and plain at 128,000 Hz,
 * where the frame is the same size and nothing is ever coded with a book. */
static void test_codebooks(void) {
    static bw low, high;
    uint8_t config_low[4], config_high[4];
    const int frame_bytes = 640;
    int frame = 0;
    configuration(config_low, 7, 0, frame_bytes, 0);
    configuration(config_high, 13, 0, frame_bytes, 0);
    memset(&low, 0, sizeof low);
    memset(&high, 0, sizeof high);
    for (int peaks = 0; peaks < 2; peaks++) {
        for (int p = 1; p <= 6; p++, frame++) {
            static block b;
            memset(&b, 0, sizeof b);
            b.channels = 1; b.units = 24; b.shared_from = 24; b.way[0] = PLAIN;
            for (int u = 0; u < b.units; u++) b.sf[0][u] = 20;
            b.gradient = 20 - p;
            if (peaks) {
                /* a unit three above both its neighbours takes the second set: one of each size that has one */
                static const int AT[3] = { 9, 14, 22 };
                for (int k = 0; k < 3; k++) { b.sf[0][AT[k]] = 23; b.set[AT[k]] = 1; }
                b.gradient = 23 - p;
            }
            random_spectrum(&b, 0);
            low.at = high.at = (size_t)frame * frame_bytes * 8;
            put_block(&low, &b, NULL, 0, 1);
            put_block(&high, &b, NULL, 1, 1);
            assert(low.at <= (size_t)(frame + 1) * frame_bytes * 8 && high.at <= (size_t)(frame + 1) * frame_bytes * 8);
        }
    }
    same(config_low, &low, config_high, &high, frame);
    printf("  codebooks against plain        ok\n");
}

/* Scale factors that differ from unit to unit, and from the run they are
 * made from (the frame before's, or the pair's other channel's) by little
 * enough that every way of sending them can say so. */
static void wander(block *b, int c, const int *from) {
    for (int u = 0; u < b->units; u++) {
        const int sf = from ? from[u] + random_below(7) - 3 : 16 + random_below(9);
        b->sf[c][u] = sf < 9 ? 9 : sf > 28 ? 28 : sf;
    }
}

/* Each way of sending scale factors against the same ones sent the plain
 * way (or, for the second channel of a pair, which has no plain way, as
 * steps): superframes of four frames, the first leaning on nothing. */
static void test_scale_factors(void) {
    /* by frame: the first (or only) channel's way and the second's, in the stream under test */
    static const int FIRST[4] = { 0, 2, 3, 1 }, SECOND[4] = { 1, 2, 3, 0 };
    for (int channels = 1; channels <= 2; channels++) {
        static bw tested, plain;
        static block b[2];
        uint8_t config[4];
        const int frame_bytes = 1400, superframes = 3;
        configuration(config, 7, channels == 2 ? 2 : 0, frame_bytes, 2);
        memset(&tested, 0, sizeof tested);
        memset(&plain, 0, sizeof plain);
        for (int f = 0; f < superframes * 4; f++) {
            block *now = &b[f & 1], *last = f % 4 ? &b[(f + 1) & 1] : NULL;
            memset(now, 0, sizeof *now);
            now->channels = channels; now->units = 24; now->shared_from = 24;
            wander(now, 0, last ? last->sf[0] : NULL);
            if (channels == 2) wander(now, 1, now->sf[0]);
            for (int c = 0; c < channels; c++) random_spectrum(now, c);
            if (f % 4 == 0) tested.at = plain.at = (size_t)(f / 4) * frame_bytes * 4 * 8;
            now->way[0] = FIRST[f % 4]; now->way[1] = SECOND[f % 4];
            put_block(&tested, now, last, 0, f % 4 == 0);
            now->way[0] = PLAIN; now->way[1] = 0;
            put_block(&plain, now, last, 0, f % 4 == 0);
            assert(tested.at <= (size_t)(f / 4 + 1) * frame_bytes * 4 * 8 && plain.at <= (size_t)(f / 4 + 1) * frame_bytes * 4 * 8);
        }
        same(config, &tested, config, &plain, superframes);
    }
    printf("  scale factors, every way       ok\n");
}

/* A pair that shares the top of its spectrum against the pair with that top
 * written for both channels: led by the first channel, then by the second. */
static void test_shared_top(void) {
    for (int primary = 0; primary < 2; primary++) {
        static bw shared, whole;
        static block b;
        uint8_t config[4];
        const int frame_bytes = 1400, frames = 6, other = !primary;
        configuration(config, 7, 2, frame_bytes, 0);
        memset(&shared, 0, sizeof shared);
        memset(&whole, 0, sizeof whole);
        for (int f = 0; f < frames; f++) {
            memset(&b, 0, sizeof b);
            b.channels = 2; b.units = 24; b.primary = primary; b.way[0] = PLAIN; b.way[1] = 0;
            for (int c = 0; c < 2; c++) wander(&b, c, NULL);
            for (int c = 0; c < 2; c++) random_spectrum(&b, c);
            /* above unit 16 the other channel is the leading one's spectrum, some units of it upside down;
             * with the leading channel's scale factors there, its numbers are the same numbers */
            for (int u = 16; u < b.units; u++) {
                b.flip[u] = random_below(2);
                b.sf[other][u] = b.sf[primary][u];
                for (int i = UNIT_AT[u]; i < UNIT_AT[u + 1]; i++) {
                    b.coarse[other][i] = b.flip[u] ? -b.coarse[primary][i] : b.coarse[primary][i];
                    b.fine[other][i] = b.flip[u] ? -b.fine[primary][i] : b.fine[primary][i];
                }
            }
            shared.at = whole.at = (size_t)f * frame_bytes * 8;
            b.shared_from = 16;
            put_block(&shared, &b, NULL, 0, 1);
            b.shared_from = 24;
            memset(b.flip, 0, sizeof b.flip);
            put_block(&whole, &b, NULL, 0, 1);
            assert(shared.at <= (size_t)(f + 1) * frame_bytes * 8 && whole.at <= (size_t)(f + 1) * frame_bytes * 8);
        }
        same(config, &shared, config, &whole, frames);
    }
    printf("  a pair's shared top            ok\n");
}

static void test_refusals(void) {
    static bw w;
    static block b;
    static atrac9_decoder d, fresh;
    static float out[1024], again[1024];
    uint8_t config[4], bad[400];
    const int frame_bytes = 400;
    configuration(config, 7, 0, frame_bytes, 0);
    memset(&b, 0, sizeof b);
    b.channels = 1; b.units = 24; b.shared_from = 24; b.way[0] = PLAIN;
    for (int u = 0; u < b.units; u++) b.sf[0][u] = 20;
    b.gradient = 14;
    random_spectrum(&b, 0);
    memset(&w, 0, sizeof w);
    put_block(&w, &b, NULL, 0, 1);
    assert(w.at <= (size_t)frame_bytes * 8);

    assert(atrac9_open(&d, config));
    assert(atrac9_decode(&d, w.buf, out));
    assert(atrac9_decode(&d, w.buf, out));

    /* a frame that says it is not its superframe's first, where it is */
    memcpy(bad, w.buf, sizeof bad);
    bad[0] |= 0x80;
    out[5] = 1.0f;
    assert(!atrac9_decode(&d, bad, out));
    for (int i = 0; i < 256; i++) assert(out[i] == 0.0f);

    /* a stream that is only ones */
    memset(bad, 0xFF, sizeof bad);
    assert(!atrac9_decode(&d, bad, out));

    /* more bands than the rate has: the frame's fourteen at 12,000 Hz, which has eight */
    configuration(config, 1, 0, frame_bytes, 0);
    assert(atrac9_open(&fresh, config));
    assert(!atrac9_decode(&fresh, w.buf, out));

    /* a frame cut off: what it reads past its end is not there */
    configuration(config, 7, 0, 40, 0);
    assert(atrac9_open(&fresh, config));
    assert(!atrac9_decode(&fresh, w.buf, out));

    /* after a refusal the decoder is as new: the next frame comes out as a first frame does */
    configuration(config, 7, 0, frame_bytes, 0);
    assert(atrac9_open(&fresh, config));
    assert(atrac9_decode(&d, w.buf, out) && atrac9_decode(&fresh, w.buf, again));
    assert(!memcmp(out, again, 256 * sizeof(float)));

    /* and a reset is the same as a refusal, without the frame of silence */
    assert(atrac9_decode(&d, w.buf, out));
    atrac9_reset(&d);
    assert(atrac9_decode(&d, w.buf, out));
    assert(!memcmp(out, again, 256 * sizeof(float)));
    printf("  refusals                       ok\n");
}

static void test_pcm16(void) {
    static const float in[8] = { 0.0f, 0.49f, 0.5f, -0.5f, -0.51f, 40000.0f, -40000.0f, 1234.4f };
    static const int16_t want[8] = { 0, 0, 1, 0, -1, 32767, -32768, 1234 };
    int16_t out[8];
    atrac9_pcm16(in, out, 8);
    assert(!memcmp(out, want, sizeof want));
    printf("  to sixteen bits                ok\n");
}

int main(void) {
    /* the band extension's levels are in the tables this file shares with the decoder, and no test here reaches them */
    (void)BEX_SCALE_3; (void)BEX_SCALE_4; (void)BEX_SCALE_5; (void)BEX_LEVEL; (void)BEX_SLOPE_FROM; (void)BEX_SLOPE_RATE; (void)BEX_WHOLE;
    printf("atrac9:\n");
    test_configuration();
    test_file();
    test_pcm16();
    round_trip(1, 8, 0, "64 samples a frame");
    round_trip(4, 12, 1, "128, in pairs");
    round_trip(7, 18, 2, "256, in fours");
    test_codebooks();
    test_scale_factors();
    test_shared_top();
    test_refusals();
    printf("all atrac9 tests passed\n");
    return 0;
}
