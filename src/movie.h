/* movie.h — a movie file, as pictures and sound.
 *
 * What a movie player (hle_avplayer.c) needs of a file, and nothing of how
 * it is had: the streams the file holds, the next picture with the time it
 * is to be shown at, the next stretch of sound. Behind it is whatever the
 * platform has to decode with, one file to a platform:
 *
 *   movie_mf.c     Windows: the system's own H.264 and AAC decoders (Media
 *                  Foundation), fed from an MP4 read by mp4.c
 *   movie_none.c   no decoder: no movie can be opened, and a player says so
 *                  where a title asks
 *
 * The file's bytes come through a reader the caller supplies, because where
 * a title keeps its movies is the caller's to know: inside an archive, or
 * behind functions of the title's own.
 *
 * A picture is handed over in the layout the console's own player hands
 * out, which is also the layout hardware decoders leave pictures in: a
 * plane of brightness, one byte a pixel, `width` bytes a row and `height`
 * rows from the top; then a plane of colour at half the size each way,
 * (height + 1) / 2 rows of (width + 1) / 2 pairs of bytes, each pair the
 * blue difference and then the red difference of a two-by-two block of
 * pixels ("NV12"). The numbers are the stream's own, unconverted: what
 * range they span and which matrix turns them into red, green and blue is
 * for whoever draws them (the console leaves it to the texture format a
 * title picks).
 *
 * Sound is sixteen-bit samples, the channels of a frame side by side, at
 * the stream's own rate.
 *
 * Nothing here is safe to call from two threads at once for one movie; a
 * movie may be used from one thread now and another later.
 */

#ifndef VITARECOMP_MOVIE_H
#define VITARECOMP_MOVIE_H

#include <stddef.h>
#include <stdint.h>

typedef struct movie movie;

/* Where a movie's bytes come from. `read` puts `length` bytes of the file
 * at `offset` into `dst` and returns how many it read: `length`, or fewer
 * only where the file ends, or a negative number when it could not read.
 * It is called only from inside the functions below, on the thread that
 * called them, so it may do whatever that thread may: run a title's code. */
typedef struct {
    void    *user;
    uint64_t size;                  /* of the file, in bytes */
    int64_t (*read)(void *user, uint64_t offset, void *dst, uint32_t length);
} movie_reader;

enum { MOVIE_VIDEO = 0, MOVIE_AUDIO, MOVIE_OTHER };

typedef struct {
    int      kind;                  /* MOVIE_VIDEO .. */
    int      decoded;               /* this is the stream of its kind the movie decodes: the first it can */
    char     language[4];           /* three letters: "eng", or "und" when the file does not say */
    uint64_t duration_us;
    uint32_t width, height;         /* of a picture */
    uint32_t channels, rate;        /* of sound, as it comes out */
} movie_stream;

/* How many bytes a picture of that size takes in the layout above. */
static inline size_t movie_picture_bytes(uint32_t width, uint32_t height) {
    return (size_t)width * height + (size_t)((width + 1) / 2) * 2 * ((height + 1) / 2);
}

/* Opens a movie: reads what the file says of itself and makes ready the
 * decoders for its first stream of pictures and its first of sound. NULL,
 * with a sentence in `why`, when the file is not a movie this can play, or
 * there is nothing here to decode with. The reader is copied; what it
 * points at must last until movie_close. */
movie *movie_open(const movie_reader *from, char *why, size_t why_cap);
void   movie_close(movie *m);

/* The streams, in the file's order. */
uint32_t            movie_streams(const movie *m);
const movie_stream *movie_stream_at(const movie *m, uint32_t stream);

/* The next picture, in the order pictures are shown, into `nv12`, which
 * holds movie_picture_bytes of its stream's size. `time_us` is when it is
 * due, in microseconds from the movie's start, and `shown_us` for how long
 * (0 when the file does not say). There is no skipping one: a picture that
 * is already late still has to be decoded, since those after it are built
 * on it. Returns 1, 0 when there are no more, -1 when decoding failed. */
int movie_next_video(movie *m, uint8_t *nv12, uint64_t *time_us, uint64_t *shown_us);

/* The next sound, up to `frames` frames of it into `pcm` (which holds that
 * many times the stream's channels). `time_us` is when the first of them is
 * due. Returns how many frames it gave, 0 when there is no more, -1 when
 * decoding failed. */
int movie_next_audio(movie *m, int16_t *pcm, uint32_t frames, uint64_t *time_us);

/* Back to the start, pictures and sound both. Returns 0 when that failed. */
int movie_rewind(movie *m);

#endif /* VITARECOMP_MOVIE_H */
