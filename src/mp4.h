/* mp4.h — an MP4 file, as tracks of samples.
 *
 * The container a console's movies come in (ISO/IEC 14496-12 and -14):
 * boxes within boxes that say, for each track, what its samples are coded
 * in, where in the file each one lies, how long it lasts and when it is
 * shown. This reads those tables and nothing more. A sample is handed over
 * as an offset and a size for whoever decodes it to fetch; no codec is
 * known here beyond its name and the few bytes its decoder is set up with.
 *
 * It reads the kind of file a movie player is given: the whole table of
 * samples in one `moov` box, at either end of the file. A file cut into
 * fragments (a stream's form) is refused.
 */

#ifndef VITARECOMP_MP4_H
#define VITARECOMP_MP4_H

#include <stddef.h>
#include <stdint.h>

typedef struct mp4 mp4;

/* `length` bytes of the file at `offset` into `dst`: how many were read,
 * which is fewer only where the file ends, or a negative number when it
 * could not be read. */
typedef int64_t (*mp4_read)(void *user, uint64_t offset, void *dst, uint32_t length);

enum { MP4_OTHER = 0, MP4_VIDEO, MP4_AUDIO };       /* what a track holds */
enum { MP4_UNKNOWN = 0, MP4_H264, MP4_AAC };        /* and what it is coded in */

typedef struct {
    uint8_t  kind, codec;
    char     language[4];           /* three letters as the file has them, "eng" or "und" */
    uint32_t samples;
    uint64_t duration_us;           /* from its first sample's showing to its last one's end */
    uint32_t width, height;         /* of a picture, in pixels */
    uint32_t channels, rate;        /* of sound, as the track declares them */
    /* What the decoder is told before the first sample. For H.264 the AVC
     * configuration record: the parameter sets, and how many bytes the
     * length before each unit of a sample takes. For AAC the
     * AudioSpecificConfig. */
    const uint8_t *config;
    uint32_t config_size;
} mp4_track;

typedef struct {
    uint64_t offset;                /* where its bytes are in the file */
    uint32_t size;
    int64_t  time_us;               /* when it is shown, from the start of the track's playing */
    uint32_t duration_us;           /* and for how long */
    uint8_t  sync;                  /* decoding may start here */
} mp4_sample;

/* Reads the tables of a file of `size` bytes. NULL with a sentence in `why`
 * when it is not an MP4 this can play from. `read` is not called again
 * after this returns. */
mp4 *mp4_open(mp4_read read, void *user, uint64_t size, char *why, size_t why_cap);
void mp4_close(mp4 *m);

/* The tracks, in the file's order. */
uint32_t         mp4_tracks(const mp4 *m);
const mp4_track *mp4_track_at(const mp4 *m, uint32_t track);

/* Sample `index` of a track, in the order the samples are stored, which is
 * the order they are decoded in: with pictures that lean on later ones it
 * is not the order they are shown in. Returns 0 past the last. */
int mp4_sample_at(const mp4 *m, uint32_t track, uint32_t index, mp4_sample *out);

#endif /* VITARECOMP_MP4_H */
