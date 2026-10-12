/* test_mp4.c — the MP4 reader, on files written here box by box. */

#include "../src/mp4.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#error "the tests check with assert(): build them without NDEBUG"
#endif

/* --- writing a file ------------------------------------------------------------------ */

typedef struct { uint8_t b[8192]; size_t n; } bytes;

static void put8(bytes *o, unsigned v)  { assert(o->n < sizeof o->b); o->b[o->n++] = (uint8_t)v; }
static void put16(bytes *o, unsigned v) { put8(o, v >> 8); put8(o, v); }
static void put32(bytes *o, uint32_t v) { put16(o, v >> 16); put16(o, v & 0xFFFF); }
static void put64(bytes *o, uint64_t v) { put32(o, (uint32_t)(v >> 32)); put32(o, (uint32_t)v); }
static void zeros(bytes *o, size_t n)   { while (n--) put8(o, 0); }
static void text(bytes *o, const char *s) { while (*s) put8(o, (unsigned char)*s++); }
static void append(bytes *o, const bytes *in) { for (size_t i = 0; i < in->n; i++) put8(o, in->b[i]); }

/* `content` as a box of that name, added to `o`. */
static void box(bytes *o, const char *name, const bytes *content) {
    put32(o, (uint32_t)content->n + 8);
    text(o, name);
    append(o, content);
}

/* A table box: version and flags, a count, and the words of its rows. */
static void table(bytes *o, const char *name, uint32_t count, const uint32_t *words, size_t n) {
    bytes c = { { 0 }, 0 };
    put32(&c, 0);
    put32(&c, count);
    for (size_t i = 0; i < n; i++) put32(&c, words[i]);
    box(o, name, &c);
}

static const uint8_t SETS[] = { 1, 0x64, 0, 0x1E, 0xFF, 0xE1, 0, 4, 0x67, 0x64, 0, 0x1E, 1, 0, 2, 0x68, 0xEE };

/* A track of five pictures: the first and the last ones decoding can start
 * at, the second shown after the two that follow it, in three chunks of
 * two, two and one. `delayed` puts half a second of nothing before it. */
static void video_track(bytes *moov, uint32_t chunk[3], int delayed) {
    bytes trak = { { 0 }, 0 }, edts = { { 0 }, 0 }, mdia = { { 0 }, 0 }, minf = { { 0 }, 0 }, stbl = { { 0 }, 0 }, c = { { 0 }, 0 };
    /* the playing begins 100 units in, where the first picture is shown */
    if (delayed) { static const uint32_t rows[] = { 500, 0xFFFFFFFFu, 0x10000, 450, 100, 0x10000 }; table(&edts, "elst", 2, rows, 6); }
    else         { static const uint32_t rows[] = { 450, 100, 0x10000 }; table(&edts, "elst", 1, rows, 3); }
    box(&trak, "edts", &edts);

    put32(&c, 0); put32(&c, 0); put32(&c, 0); put32(&c, 1000); put32(&c, 450);
    put16(&c, (5u << 10) | (14u << 5) | 7u);                    /* "eng" */
    put16(&c, 0);
    box(&mdia, "mdhd", &c);
    c.n = 0; put32(&c, 0); put32(&c, 0); text(&c, "vide"); zeros(&c, 13);
    box(&mdia, "hdlr", &c);

    /* the sample entry: 78 bytes of record with the size in it, then the decoder's set-up in a box of its own */
    bytes entry = { { 0 }, 0 }, avcc = { { 0 }, 0 };
    zeros(&entry, 6); put16(&entry, 1); zeros(&entry, 16); put16(&entry, 64); put16(&entry, 48); zeros(&entry, 50);
    for (size_t i = 0; i < sizeof SETS; i++) put8(&avcc, SETS[i]);
    box(&entry, "avcC", &avcc);
    c.n = 0; put32(&c, 0); put32(&c, 1); box(&c, "avc1", &entry);
    box(&stbl, "stsd", &c);

    static const uint32_t lengths[] = { 4, 100, 1, 50 }, later[] = { 1, 100, 1, 300, 2, 0, 1, 100 };
    static const uint32_t per_chunk[] = { 1, 2, 1, 3, 1, 1 }, starts[] = { 1, 5 };
    table(&stbl, "stts", 2, lengths, 4);
    table(&stbl, "ctts", 4, later, 8);
    table(&stbl, "stsc", 2, per_chunk, 6);
    c.n = 0; put32(&c, 0); put32(&c, 0); put32(&c, 5);
    for (uint32_t k = 1; k <= 5; k++) put32(&c, 10 * k);
    box(&stbl, "stsz", &c);
    table(&stbl, "stco", 3, chunk, 3);
    table(&stbl, "stss", 2, starts, 2);
    box(&minf, "stbl", &stbl);
    box(&mdia, "minf", &minf);
    box(&trak, "mdia", &mdia);
    box(moov, "trak", &trak);
}

/* A track of three frames of sound, seven bytes each, in one chunk whose
 * place is written in sixty-four bits, under a header of the wide kind. */
static void audio_track(bytes *moov, uint64_t chunk) {
    bytes trak = { { 0 }, 0 }, mdia = { { 0 }, 0 }, minf = { { 0 }, 0 }, stbl = { { 0 }, 0 }, c = { { 0 }, 0 };
    put32(&c, 0x01000000u); put64(&c, 0); put64(&c, 0); put32(&c, 48000); put64(&c, 3072);
    put16(&c, (10u << 10) | (16u << 5) | 14u);                  /* "jpn" */
    put16(&c, 0);
    box(&mdia, "mdhd", &c);
    c.n = 0; put32(&c, 0); put32(&c, 0); text(&c, "soun"); zeros(&c, 13);
    box(&mdia, "hdlr", &c);

    /* 28 bytes of record, then the stream's descriptor: the set-up is the two bytes inside the innermost */
    bytes entry = { { 0 }, 0 }, esds = { { 0 }, 0 };
    zeros(&entry, 6); put16(&entry, 1); zeros(&entry, 8); put16(&entry, 2); put16(&entry, 16); zeros(&entry, 4); put32(&entry, 48000u << 16);
    put32(&esds, 0);
    put8(&esds, 3); put8(&esds, 0x80); put8(&esds, 0x80); put8(&esds, 0x80); put8(&esds, 25);      /* a length may be spelt in four bytes */
    put16(&esds, 1); put8(&esds, 0);
    put8(&esds, 4); put8(&esds, 17); put8(&esds, 0x40); put8(&esds, 0x15); zeros(&esds, 11);
    put8(&esds, 5); put8(&esds, 2); put8(&esds, 0x11); put8(&esds, 0x90);
    put8(&esds, 6); put8(&esds, 1); put8(&esds, 2);
    box(&entry, "esds", &esds);
    c.n = 0; put32(&c, 0); put32(&c, 1); box(&c, "mp4a", &entry);
    box(&stbl, "stsd", &c);

    static const uint32_t lengths[] = { 3, 1024 }, per_chunk[] = { 1, 3, 1 };
    table(&stbl, "stts", 1, lengths, 2);
    table(&stbl, "stsc", 1, per_chunk, 3);
    c.n = 0; put32(&c, 0); put32(&c, 7); put32(&c, 3);         /* every sample the one size */
    box(&stbl, "stsz", &c);
    c.n = 0; put32(&c, 0); put32(&c, 1); put64(&c, chunk);
    box(&stbl, "co64", &c);
    box(&minf, "stbl", &stbl);
    box(&mdia, "minf", &minf);
    box(&trak, "mdia", &mdia);
    box(moov, "trak", &trak);
}

/* A whole file: the samples before the tables or after them. The samples'
 * box is written with its length in sixty-four bits. */
static bytes g_file;
static uint32_t g_samples_at;

static void write_file(int tables_first, int delayed) {
    bytes moov = { { 0 }, 0 }, c = { { 0 }, 0 }, ftyp = { { 0 }, 0 };
    g_file.n = 0;
    text(&ftyp, "mp42"); put32(&ftyp, 0); text(&ftyp, "mp42isom");
    box(&g_file, "ftyp", &ftyp);
    for (int pass = 0; pass < 2; pass++) {
        /* twice: the first time to learn how long the tables are, which says where the samples lie */
        uint32_t at = tables_first ? (uint32_t)(g_file.n + moov.n + 8 + 16) : (uint32_t)g_file.n + 16;
        uint32_t chunk[3] = { at, at + 100, at + 200 };
        g_samples_at = at;
        moov.n = 0; c.n = 0;
        put32(&c, 0); put32(&c, 0); put32(&c, 0); put32(&c, 1000); put32(&c, 450); zeros(&c, 80);
        box(&moov, "mvhd", &c);
        video_track(&moov, chunk, delayed);
        audio_track(&moov, (uint64_t)at + 300);
    }
    if (tables_first) box(&g_file, "moov", &moov);
    put32(&g_file, 1); text(&g_file, "mdat"); put64(&g_file, 16 + 400);
    for (uint32_t k = 0; k < 400; k++) put8(&g_file, k);
    if (!tables_first) box(&g_file, "moov", &moov);
}

static uint64_t g_readable;         /* how much of the file the reader will give */

static int64_t read_file(void *user, uint64_t offset, void *dst, uint32_t length) {
    (void)user;
    if (offset >= g_readable) return 0;
    if (length > g_readable - offset) length = (uint32_t)(g_readable - offset);
    memcpy(dst, g_file.b + offset, length);
    return length;
}

/* --- reading it ---------------------------------------------------------------------- */

static void test_tables(void) {
    char why[128];
    for (int tables_first = 0; tables_first < 2; tables_first++) {
        write_file(tables_first, 0);
        g_readable = g_file.n;
        mp4 *m = mp4_open(read_file, NULL, g_file.n, why, sizeof why);
        assert(m && !why[0] && mp4_tracks(m) == 2);

        const mp4_track *v = mp4_track_at(m, 0), *a = mp4_track_at(m, 1);
        assert(v->kind == MP4_VIDEO && v->codec == MP4_H264 && !strcmp(v->language, "eng"));
        assert(v->samples == 5 && v->width == 64 && v->height == 48 && v->duration_us == 450000);
        assert(v->config_size == sizeof SETS && !memcmp(v->config, SETS, sizeof SETS));
        /* stored, and so decoded, as first, fourth, second, third, fifth of the showing */
        static const int64_t when[5] = { 0, 300000, 100000, 200000, 400000 };
        static const uint32_t shown[5] = { 100000, 100000, 100000, 100000, 50000 };
        const uint32_t where[5] = { g_samples_at, g_samples_at + 10, g_samples_at + 100, g_samples_at + 130, g_samples_at + 200 };
        for (uint32_t k = 0; k < 5; k++) {
            mp4_sample s;
            assert(mp4_sample_at(m, 0, k, &s));
            assert(s.offset == where[k] && s.size == 10 * (k + 1) && s.time_us == when[k] && s.duration_us == shown[k]);
            assert(s.sync == (k == 0 || k == 4));
            assert(g_file.b[s.offset] == (uint8_t)(s.offset - g_samples_at));     /* and that is where the file has them */
        }

        assert(a->kind == MP4_AUDIO && a->codec == MP4_AAC && !strcmp(a->language, "jpn"));
        assert(a->samples == 3 && a->channels == 2 && a->rate == 48000 && a->duration_us == 64000);
        assert(a->config_size == 2 && a->config[0] == 0x11 && a->config[1] == 0x90);
        /* 1024 units of 48,000 a second each: a third of a microsecond is rounded, not dropped */
        static const int64_t starts[3] = { 0, 21333, 42667 };
        for (uint32_t k = 0; k < 3; k++) {
            mp4_sample s;
            assert(mp4_sample_at(m, 1, k, &s));
            assert(s.offset == g_samples_at + 300 + 7 * k && s.size == 7 && s.sync == 1 && s.time_us == starts[k]);
            assert(s.duration_us == (k == 1 ? 21334u : 21333u));
        }
        mp4_sample none;
        assert(!mp4_sample_at(m, 0, 5, &none) && !mp4_sample_at(m, 1, 3, &none) && !mp4_sample_at(m, 2, 0, &none));
        assert(!mp4_track_at(m, 2));
        mp4_close(m);
    }
    printf("  tracks, samples, their places and times ok\n");
}

static void test_edits_and_refusals(void) {
    char why[128];
    mp4_sample s;
    /* half a second of nothing before the track: every picture that much later */
    write_file(1, 1);
    g_readable = g_file.n;
    mp4 *m = mp4_open(read_file, NULL, g_file.n, why, sizeof why);
    assert(m && mp4_sample_at(m, 0, 0, &s) && s.time_us == 500000);
    assert(mp4_sample_at(m, 0, 1, &s) && s.time_us == 800000);
    mp4_close(m);

    /* a file that ends before its tables do, one that is something else, and one with no tables */
    write_file(0, 0);
    g_readable = g_file.n - 40;
    assert(!mp4_open(read_file, NULL, g_file.n, why, sizeof why) && why[0]);
    g_readable = g_file.n;
    memcpy(g_file.b + 4, "RIFF", 4);
    assert(!mp4_open(read_file, NULL, g_file.n, why, sizeof why) && !strcmp(why, "not an MP4 file"));
    write_file(0, 0);
    g_readable = g_file.n;
    for (size_t i = 0; i + 4 <= g_file.n; i++) if (!memcmp(g_file.b + i, "moov", 4)) memcpy(g_file.b + i, "free", 4);
    assert(!mp4_open(read_file, NULL, g_file.n, why, sizeof why) && why[0]);

    /* tables that name more samples than their chunks hold */
    write_file(1, 0);
    g_readable = g_file.n;
    for (size_t i = 0; i + 4 <= g_file.n; i++)
        if (!memcmp(g_file.b + i, "stsc", 4) && g_file.b[i + 11] == 2) g_file.b[i + 11] = 1;       /* the video's: only its first run */
    m = mp4_open(read_file, NULL, g_file.n, why, sizeof why);
    assert(m);                                      /* the one run goes on to the last chunk: 2, 2 and 2, which is enough */
    mp4_close(m);
    for (size_t i = 0; i + 4 <= g_file.n; i++)
        if (!memcmp(g_file.b + i, "stco", 4)) g_file.b[i + 11] = 1;                                 /* and now a single chunk of two */
    assert(!mp4_open(read_file, NULL, g_file.n, why, sizeof why) && why[0]);
    printf("  edits, and files that are refused       ok\n");
}

int main(void) {
    printf("mp4:\n");
    test_tables();
    test_edits_and_refusals();
    printf("all mp4 tests passed\n");
    return 0;
}
