/* mp4.c — an MP4 file, as tracks of samples (mp4.h).
 *
 * A box is four bytes of length, four of name, and that much content, which
 * for some names is more boxes. Everything is big-endian. The ones read:
 *
 *   moov                    the tables; everything else is the samples
 *     mvhd                  the movie's units a second, which edits are in
 *     trak                  one for each track
 *       edts/elst           edits: where in the track its playing begins
 *       mdia
 *         mdhd              the track's units a second, its language
 *         hdlr              "vide" or "soun"
 *         minf/stbl
 *           stsd            the codec, a picture's size, the decoder's set-up
 *           stts            how long each sample lasts, in runs
 *           ctts            how long after its decoding each is shown, in runs
 *           stsc            how many samples each chunk holds, in runs
 *           stsz            each sample's size
 *           stco, co64      where each chunk begins in the file
 *           stss            which samples decoding may start at
 *
 * A track's samples lie in chunks, one after another within a chunk, and
 * the tables are kept the way they are written, as runs; here they are
 * laid out flat, a row for each sample, which for an hour of film is a few
 * megabytes and makes every question about a sample an index.
 */

#include "mp4.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOST_MOOV    (256u << 20)       /* a table of contents larger than this is not one */
#define MOST_TRACKS  16
#define MOST_SAMPLES (16u << 20)

typedef struct {
    mp4_track info;
    uint32_t  timescale;        /* units a second */
    int64_t   starts;           /* the unit its playing begins at: later samples' times count from here */
    uint64_t *offset;
    uint32_t *size;
    uint64_t *decode;           /* when each sample is decoded, in units; one more than there are samples, the last being the end */
    int32_t  *later;            /* how long after that it is shown */
    uint8_t  *sync;
    uint8_t  *config;
} track;

struct mp4 {
    uint32_t tracks;
    track    t[MOST_TRACKS];
};

static uint32_t be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

#define NAME(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* A run of bytes holding boxes, or one box's content. */
typedef struct { const uint8_t *p; size_t n; } span;

/* The first box named `name` in `in`: its content. Nothing (a NULL p) when
 * there is none. `after`, when given, is moved past the box found, to look
 * for the next of the name. */
static span find(span in, uint32_t name, span *after) {
    span none = { NULL, 0 };
    while (in.n >= 8) {
        uint64_t size = be32(in.p);
        size_t head = 8;
        if (size == 1) {
            if (in.n < 16) break;
            size = be64(in.p + 8);
            head = 16;
        } else if (size == 0) {
            size = in.n;
        }
        if (size < head || size > in.n) break;
        if (be32(in.p + 4) == name) {
            span found = { in.p + head, (size_t)size - head };
            if (after) { after->p = in.p + size; after->n = in.n - (size_t)size; }
            return found;
        }
        in.p += size;
        in.n -= (size_t)size;
    }
    return none;
}

/* The table of a "full box": past its version and flags and its count of
 * entries, `entry` bytes each. The count, or 0 when the box is not whole. */
static uint32_t table(span box, size_t entry, const uint8_t **rows) {
    if (!box.p || box.n < 8) return 0;
    uint32_t count = be32(box.p + 4);
    if ((uint64_t)count * entry > box.n - 8) return 0;
    *rows = box.p + 8;
    return count;
}

/* --- the decoder's set-up ---------------------------------------------------------------
 *
 * A sample entry is a box named for the codec whose content begins with a
 * fixed record (78 bytes for pictures, 28 for sound) and goes on with more
 * boxes; the one that matters holds what the decoder is given first. */

static int keep_config(track *t, const uint8_t *bytes, size_t n) {
    t->config = (uint8_t *)malloc(n ? n : 1);
    if (!t->config) return 0;
    memcpy(t->config, bytes, n);
    t->info.config = t->config;
    t->info.config_size = (uint32_t)n;
    return 1;
}

/* An MPEG-4 descriptor: a tag, a length of up to four bytes of seven bits
 * each, and that much content. */
static span descriptor(span *in, unsigned *tag) {
    span none = { NULL, 0 };
    if (in->n < 2) return none;
    *tag = in->p[0];
    size_t at = 1, len = 0;
    for (int k = 0; k < 4 && at < in->n; k++) {
        uint8_t b = in->p[at++];
        len = (len << 7) | (b & 0x7Fu);
        if (!(b & 0x80u)) break;
    }
    if (len > in->n - at) return none;
    span body = { in->p + at, len };
    in->p += at + len;
    in->n -= at + len;
    return body;
}

/* The AudioSpecificConfig out of an `esds` box: inside the stream's
 * descriptor (3), inside its decoder's (4), the decoder's own (5). Whether
 * the stream is AAC is the decoder descriptor's first byte: 0x40, or one of
 * the three numbers the same coding had a standard earlier. A box that
 * does not hold all that leaves the track one of an unknown codec; 0 is for
 * running out of memory. */
static int audio_config(track *t, span esds) {
    unsigned tag;
    if (esds.n < 4) return 1;
    span in = { esds.p + 4, esds.n - 4 };
    span es = descriptor(&in, &tag);
    if (!es.p || tag != 3 || es.n < 3) return 1;
    size_t skip = 3;                                    /* its number and flags */
    if (es.p[2] & 0x80) skip += 2;                      /* the stream it depends on */
    if (es.p[2] & 0x40) skip += 1 + (skip < es.n ? es.p[skip] : 0);     /* a URL */
    if (es.p[2] & 0x20) skip += 2;                      /* the stream its clock is */
    if (skip > es.n) return 1;
    span rest = { es.p + skip, es.n - skip };
    span dec = descriptor(&rest, &tag);
    if (!dec.p || tag != 4 || dec.n < 13) return 1;
    if (dec.p[0] != 0x40 && dec.p[0] != 0x66 && dec.p[0] != 0x67 && dec.p[0] != 0x68) return 1;
    span inner = { dec.p + 13, dec.n - 13 };
    span own = descriptor(&inner, &tag);
    if (!own.p || tag != 5 || !own.n) return 1;
    t->info.codec = MP4_AAC;
    return keep_config(t, own.p, own.n);
}

static int sample_entry(track *t, span stsd) {
    if (stsd.n < 16 || !be32(stsd.p + 4)) return 1;     /* no entry: a track of nothing known */
    span entry = { stsd.p + 8, stsd.n - 8 };
    uint32_t size = be32(entry.p), name = be32(entry.p + 4);
    if (size < 16 || size > entry.n) return 1;
    const uint8_t *e = entry.p + 8;                     /* past the box's length and name */
    size_t left = size - 8;
    if (t->info.kind == MP4_VIDEO && left >= 78) {
        t->info.width = be16(e + 24);
        t->info.height = be16(e + 26);
        span boxes = { e + 78, left - 78 };
        if (name == NAME('a', 'v', 'c', '1') || name == NAME('a', 'v', 'c', '3')) {
            span avcc = find(boxes, NAME('a', 'v', 'c', 'C'), NULL);
            if (avcc.p && avcc.n >= 7) {
                t->info.codec = MP4_H264;
                return keep_config(t, avcc.p, avcc.n);
            }
        }
    } else if (t->info.kind == MP4_AUDIO && left >= 28) {
        /* the record grew twice in the format this one descends from: its version says by how much */
        uint32_t version = be16(e + 8);
        size_t fixed = version == 1 ? 28 + 16 : version == 2 ? 28 + 36 : 28;
        t->info.channels = be16(e + 16);
        t->info.rate = be32(e + 24) >> 16;
        if (name == NAME('m', 'p', '4', 'a') && left >= fixed) {
            span boxes = { e + fixed, left - fixed };
            span esds = find(boxes, NAME('e', 's', 'd', 's'), NULL);
            if (esds.p && !audio_config(t, esds)) return 0;
        }
    }
    return 1;
}

/* --- a track's tables, laid out flat --------------------------------------------------- */

static int lay_out(track *t, span stbl, char *why, size_t why_cap) {
    const uint8_t *durations, *offsets, *chunks, *places, *starts, *sizes = NULL;
    span stsz = find(stbl, NAME('s', 't', 's', 'z'), NULL);
    span stco = find(stbl, NAME('s', 't', 'c', 'o'), NULL), co64 = find(stbl, NAME('c', 'o', '6', '4'), NULL);
    uint32_t each = 0, n = 0;
    if (stsz.p && stsz.n >= 12) {
        each = be32(stsz.p + 4);
        n = be32(stsz.p + 8);
        sizes = stsz.p + 12;
        if (!each && (uint64_t)n * 4 > stsz.n - 12) n = 0;
    }
    if (n > MOST_SAMPLES) { snprintf(why, why_cap, "a track of %u samples", n); return 0; }
    t->info.samples = n;
    t->offset = (uint64_t *)calloc((size_t)n + 1, sizeof *t->offset);
    t->size = (uint32_t *)calloc((size_t)n + 1, sizeof *t->size);
    t->decode = (uint64_t *)calloc((size_t)n + 1, sizeof *t->decode);
    t->later = (int32_t *)calloc((size_t)n + 1, sizeof *t->later);
    t->sync = (uint8_t *)calloc((size_t)n + 1, 1);
    if (!t->offset || !t->size || !t->decode || !t->later || !t->sync) { snprintf(why, why_cap, "no memory for %u samples", n); return 0; }
    for (uint32_t i = 0; i < n; i++) t->size[i] = each ? each : be32(sizes + 4 * (size_t)i);

    /* when: each run of the first table is so many samples of one length */
    uint32_t runs = table(find(stbl, NAME('s', 't', 't', 's'), NULL), 8, &durations), i = 0;
    uint64_t at = 0;
    for (uint32_t r = 0; r < runs && i < n; r++) {
        uint32_t count = be32(durations + 8 * (size_t)r), length = be32(durations + 8 * (size_t)r + 4);
        for (; count && i < n; count--, i++) { t->decode[i] = at; at += length; }
    }
    for (; i < n; i++) t->decode[i] = at;               /* a table that ends early: the rest are of no length */
    t->decode[n] = at;
    runs = table(find(stbl, NAME('c', 't', 't', 's'), NULL), 8, &offsets);
    i = 0;
    for (uint32_t r = 0; r < runs && i < n; r++) {
        uint32_t count = be32(offsets + 8 * (size_t)r);
        int32_t later = (int32_t)be32(offsets + 8 * (size_t)r + 4);
        for (; count && i < n; count--, i++) t->later[i] = later;
    }

    /* where: the chunks' places, and how many samples each run of chunks holds */
    uint32_t wide = co64.p ? 8 : 4;
    uint32_t nchunks = table(co64.p ? co64 : stco, wide, &places);
    uint32_t nruns = table(find(stbl, NAME('s', 't', 's', 'c'), NULL), 12, &chunks);
    i = 0;
    for (uint32_t r = 0; r < nruns && i < n; r++) {
        uint32_t first = be32(chunks + 12 * (size_t)r), per = be32(chunks + 12 * (size_t)r + 4);
        uint32_t next = r + 1 < nruns ? be32(chunks + 12 * (size_t)(r + 1)) : nchunks + 1;
        if (!first || next > nchunks + 1) next = nchunks + 1;
        for (uint32_t c = first; first && c < next && i < n; c++) {
            uint64_t place = wide == 8 ? be64(places + 8 * (size_t)(c - 1)) : be32(places + 4 * (size_t)(c - 1));
            for (uint32_t k = 0; k < per && i < n; k++, i++) { t->offset[i] = place; place += t->size[i]; }
        }
    }
    if (i < n) { snprintf(why, why_cap, "a track whose chunks hold %u of its %u samples", i, n); return 0; }

    /* where decoding may start: every sample, unless a table names the ones */
    uint32_t nsync = table(find(stbl, NAME('s', 't', 's', 's'), NULL), 4, &starts);
    if (!find(stbl, NAME('s', 't', 's', 's'), NULL).p) memset(t->sync, 1, n);
    for (uint32_t k = 0; k < nsync; k++) {
        uint32_t number = be32(starts + 4 * (size_t)k);
        if (number && number <= n) t->sync[number - 1] = 1;
    }
    return 1;
}

/* Units of `scale` a second as microseconds, to the nearest. */
static int64_t in_us(int64_t units, uint32_t scale) {
    if (!scale) return 0;
    int64_t whole = units / (int64_t)scale, rest = units % (int64_t)scale;
    if (rest < 0) { whole--; rest += scale; }
    return whole * 1000000 + (rest * 1000000 + (int64_t)scale / 2) / (int64_t)scale;
}

static int read_track(mp4 *m, span trak, uint32_t movie_scale, char *why, size_t why_cap) {
    track *t = &m->t[m->tracks];
    memset(t, 0, sizeof *t);
    span mdia = find(trak, NAME('m', 'd', 'i', 'a'), NULL);
    span mdhd = find(mdia, NAME('m', 'd', 'h', 'd'), NULL), hdlr = find(mdia, NAME('h', 'd', 'l', 'r'), NULL);
    span stbl = find(find(mdia, NAME('m', 'i', 'n', 'f'), NULL), NAME('s', 't', 'b', 'l'), NULL);
    if (!mdhd.p || mdhd.n < 24 || !stbl.p) return 1;    /* not a track of samples: passed over */
    m->tracks++;

    /* the header comes in two widths; the language is three letters of five bits, each less 0x60 */
    int wide = mdhd.p[0] == 1;
    if (wide && mdhd.n < 36) { snprintf(why, why_cap, "a track header cut short"); return 0; }
    t->timescale = be32(mdhd.p + (wide ? 20 : 12));
    uint32_t packed = be16(mdhd.p + (wide ? 32 : 20));
    for (int k = 0; k < 3; k++) {
        uint32_t five = (packed >> (10 - 5 * k)) & 31;
        t->info.language[k] = five ? (char)(five + 0x60) : '\0';
    }
    if (!t->info.language[0]) memcpy(t->info.language, "und", 4);
    if (!t->timescale) { snprintf(why, why_cap, "a track with no units of time"); return 0; }
    if (hdlr.p && hdlr.n >= 12) {
        uint32_t kind = be32(hdlr.p + 8);
        t->info.kind = kind == NAME('v', 'i', 'd', 'e') ? MP4_VIDEO : kind == NAME('s', 'o', 'u', 'n') ? MP4_AUDIO : MP4_OTHER;
    }
    if (!sample_entry(t, find(stbl, NAME('s', 't', 's', 'd'), NULL))) { snprintf(why, why_cap, "no memory for a decoder's set-up"); return 0; }
    if (!lay_out(t, stbl, why, why_cap)) return 0;

    /* Edits. A list of pieces of the track to play, of which two shapes
     * matter: a first piece of nothing, which delays the track, and a piece
     * that begins some way in, which is how a track whose first picture is
     * shown later than it is decoded is brought back to start at nothing. */
    const uint8_t *edits;
    span elst = find(find(trak, NAME('e', 'd', 't', 's'), NULL), NAME('e', 'l', 's', 't'), NULL);
    int v1 = elst.p && elst.n && elst.p[0] == 1;
    uint32_t count = table(elst, v1 ? 20 : 12, &edits);
    int64_t delay = 0;
    for (uint32_t k = 0; k < count; k++) {
        const uint8_t *e = edits + (size_t)k * (v1 ? 20 : 12);
        int64_t length = v1 ? (int64_t)be64(e) : (int64_t)be32(e);
        int64_t from = v1 ? (int64_t)be64(e + 8) : (int64_t)(int32_t)be32(e + 4);
        if (from < 0) { delay += length; continue; }
        t->starts = from;
        break;
    }
    if (delay && movie_scale) t->starts -= delay * (int64_t)t->timescale / (int64_t)movie_scale;

    int64_t end = (int64_t)t->decode[t->info.samples] - t->starts;
    if (t->info.samples) end += t->later[0];            /* the first shown is that much after the first decoded */
    t->info.duration_us = end > 0 ? (uint64_t)in_us(end, t->timescale) : 0;
    return 1;
}

/* --- the file -------------------------------------------------------------------------- */

mp4 *mp4_open(mp4_read read, void *user, uint64_t size, char *why, size_t why_cap) {
    uint8_t head[16];
    uint64_t at = 0, moov_at = 0, moov_size = 0;
    char none[1];
    if (!why || !why_cap) { why = none; why_cap = sizeof none; }
    why[0] = '\0';

    /* the boxes of the file itself, one header at a time, until the table of contents */
    int first = 1;
    while (at + 8 <= size) {
        if (read(user, at, head, 8) != 8) { snprintf(why, why_cap, "the file could not be read at %llu", (unsigned long long)at); return NULL; }
        uint64_t length = be32(head);
        uint32_t name = be32(head + 4), header = 8;
        if (length == 1) {
            if (at + 16 > size || read(user, at + 8, head + 8, 8) != 8) break;
            length = be64(head + 8);
            header = 16;
        } else if (length == 0) {
            length = size - at;
        }
        if (first && name != NAME('f', 't', 'y', 'p') && name != NAME('m', 'o', 'o', 'v') && name != NAME('m', 'd', 'a', 't') &&
            name != NAME('f', 'r', 'e', 'e') && name != NAME('s', 'k', 'i', 'p') && name != NAME('w', 'i', 'd', 'e')) {
            snprintf(why, why_cap, "not an MP4 file");
            return NULL;
        }
        first = 0;
        if (length < header || length > size - at) break;
        if (name == NAME('m', 'o', 'o', 'v')) { moov_at = at + header; moov_size = length - header; break; }
        at += length;
    }
    if (!moov_size) { snprintf(why, why_cap, "an MP4 file with no table of its samples"); return NULL; }
    if (moov_size > MOST_MOOV) { snprintf(why, why_cap, "a table of samples of %llu bytes", (unsigned long long)moov_size); return NULL; }

    uint8_t *moov = (uint8_t *)malloc((size_t)moov_size);
    mp4 *m = (mp4 *)calloc(1, sizeof *m);
    if (!moov || !m) { free(moov); free(m); snprintf(why, why_cap, "no memory for the table of samples"); return NULL; }
    for (uint64_t done = 0; done < moov_size; ) {
        uint32_t want = moov_size - done > (1u << 20) ? (1u << 20) : (uint32_t)(moov_size - done);
        if (read(user, moov_at + done, moov + done, want) != (int64_t)want) {
            snprintf(why, why_cap, "the table of samples could not be read");
            free(moov); free(m);
            return NULL;
        }
        done += want;
    }

    span all = { moov, (size_t)moov_size }, mvhd = find(all, NAME('m', 'v', 'h', 'd'), NULL), rest = all;
    uint32_t movie_scale = mvhd.p && mvhd.n >= 32 ? be32(mvhd.p + (mvhd.p[0] == 1 ? 20 : 12)) : 0;
    int ok = 1;
    while (ok && m->tracks < MOST_TRACKS) {
        span trak = find(rest, NAME('t', 'r', 'a', 'k'), &rest);
        if (!trak.p) break;
        ok = read_track(m, trak, movie_scale, why, why_cap);
    }
    uint32_t samples = 0;
    for (uint32_t k = 0; k < m->tracks; k++) samples += m->t[k].info.samples;
    if (ok && !samples) {
        snprintf(why, why_cap, find(all, NAME('m', 'v', 'e', 'x'), NULL).p ? "an MP4 file in fragments" : "an MP4 file with no samples");
        ok = 0;
    }
    free(moov);
    if (!ok) { mp4_close(m); return NULL; }
    return m;
}

void mp4_close(mp4 *m) {
    if (!m) return;
    for (uint32_t k = 0; k < MOST_TRACKS; k++) {
        track *t = &m->t[k];
        free(t->offset); free(t->size); free(t->decode); free(t->later); free(t->sync); free(t->config);
    }
    free(m);
}

uint32_t mp4_tracks(const mp4 *m) { return m->tracks; }

const mp4_track *mp4_track_at(const mp4 *m, uint32_t k) { return k < m->tracks ? &m->t[k].info : NULL; }

int mp4_sample_at(const mp4 *m, uint32_t k, uint32_t index, mp4_sample *out) {
    if (k >= m->tracks || index >= m->t[k].info.samples) return 0;
    const track *t = &m->t[k];
    int64_t decoded = (int64_t)t->decode[index] - t->starts, ends = (int64_t)t->decode[index + 1] - t->starts;
    out->offset = t->offset[index];
    out->size = t->size[index];
    out->time_us = in_us(decoded + t->later[index], t->timescale);
    out->duration_us = (uint32_t)(in_us(ends, t->timescale) - in_us(decoded, t->timescale));
    out->sync = t->sync[index];
    return 1;
}
