/* movie_none.c — movies, where there is nothing to decode them with (movie.h).
 *
 * No movie opens. A player that is told so refuses the source it was given,
 * in the call that gave it, and a title goes on to what follows the movie.
 */

#include "movie.h"

#include <stdio.h>

movie *movie_open(const movie_reader *from, char *why, size_t why_cap) {
    (void)from;
    if (why && why_cap) snprintf(why, why_cap, "this build has no movie decoder");
    return NULL;
}

void movie_close(movie *m) { (void)m; }

uint32_t movie_streams(const movie *m) { (void)m; return 0; }

const movie_stream *movie_stream_at(const movie *m, uint32_t stream) { (void)m; (void)stream; return NULL; }

int movie_next_video(movie *m, uint8_t *nv12, uint64_t *time_us, uint64_t *shown_us) {
    (void)m; (void)nv12; (void)time_us; (void)shown_us;
    return 0;
}

int movie_next_audio(movie *m, int16_t *pcm, uint32_t frames, uint64_t *time_us) {
    (void)m; (void)pcm; (void)frames; (void)time_us;
    return 0;
}

int movie_rewind(movie *m) { (void)m; return 0; }
