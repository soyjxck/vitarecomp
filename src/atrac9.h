/* atrac9.h — ATRAC9, the console's audio codec, decoded.
 *
 * What a title's music and speech are stored as: a stream of superframes,
 * every one the same number of bytes and the same number of samples, and
 * four bytes of configuration beside the stream that say what those numbers
 * are. A decoder is opened with the configuration and given one superframe
 * at a time. It needs nothing else: a title that streams a file hands the
 * synthesiser (hle_ngs.c) superframes out of the middle of it.
 *
 * Written rather than vendored, like the toolkit's inflate: the format is a
 * fully described algorithm and a table of codebooks (atrac9.c says where
 * the description comes from).
 */

#ifndef VITARECOMP_ATRAC9_H
#define VITARECOMP_ATRAC9_H

#include <stddef.h>
#include <stdint.h>

#define ATRAC9_CHANNELS_MOST 8
#define ATRAC9_FRAME_MOST    256            /* samples of one channel in a frame */
#define ATRAC9_FRAMES_MOST   8              /* frames in a superframe */
#define ATRAC9_SUPERFRAME_BYTES_MOST (2048 * ATRAC9_FRAMES_MOST)

/* What the four bytes of configuration say. */
typedef struct {
    uint32_t rate;                  /* samples a second */
    uint32_t channels;
    uint32_t frame_samples;         /* 64, 128 or 256 */
    uint32_t frames;                /* in a superframe: 1, 2, 4 or 8 */
    uint32_t superframe_bytes;      /* what one atrac9_decode takes */
    uint32_t superframe_samples;    /* and what it gives, of each channel */
} atrac9_info;

/* Reads a configuration. Zero when the bytes are not one. */
int atrac9_config(const uint8_t config[4], atrac9_info *info);

/* A decoder's memory of the stream: for each channel the half window still
 * to be heard and the last frame's scale factors, for each block the band
 * layout a frame may take over from the one before. Its fields are the
 * decoder's own; it is laid out here so that it can be kept wherever the
 * rest of a voice is. */
typedef struct {
    float    overlap[ATRAC9_FRAME_MOST];
    uint8_t  sf_last[31];
    uint8_t  bex_mode, bex_value[4];
    uint8_t  noise_seeded;
    uint16_t noise[4];
} atrac9_channel;

typedef struct {
    uint8_t  type, channels;
    uint8_t  units, stereo_units, ext_units, extends, units_last;
    atrac9_channel ch[2];
} atrac9_block;

typedef struct {
    atrac9_info  info;
    uint8_t      rate_index, high, size, blocks;
    atrac9_block block[5];
} atrac9_decoder;

/* A decoder at the start of a stream of that configuration. Zero when the
 * configuration is not one. */
int atrac9_open(atrac9_decoder *d, const uint8_t config[4]);

/* One superframe, `info.superframe_bytes` of it at `in`, to
 * `info.superframe_samples` frames at `out`: the channels interleaved, in
 * the scale of sixteen-bit samples (a full-scale tone peaks near 32768) and
 * not yet clipped to it. The first frame of a stream comes out faded in:
 * each frame's first half is completed by the one before.
 * Returns zero when the superframe is not a valid one. `out` is then silence
 * and the decoder is as it is after atrac9_open. */
int atrac9_decode(atrac9_decoder *d, const uint8_t *in, float *out);

/* Forgets the stream so far, as a seek does: the next superframe fades in. */
void atrac9_reset(atrac9_decoder *d);

/* Samples from atrac9_decode's scale to sixteen bits: rounded to the
 * nearest, and clipped. */
void atrac9_pcm16(const float *in, int16_t *out, size_t count);

/* The file ATRAC9 comes in: a RIFF "WAVE" whose format block is the
 * extensible kind with the codec's own identifier and the configuration. */
typedef struct {
    uint8_t  config[4];
    size_t   data, data_bytes;      /* where the superframes are */
    uint32_t samples;               /* how long the sound is */
    uint32_t delay;                 /* decoded samples that come before its first */
    uint32_t loops;                 /* whether it names a loop, and the loop: */
    uint32_t loop_start, loop_end;  /* counted from the first decoded sample, the delay included */
} atrac9_file;

/* Reads such a file's header. Zero when it is not one. */
int atrac9_file_open(const uint8_t *file, size_t size, atrac9_file *out);

#endif /* VITARECOMP_ATRAC9_H */
