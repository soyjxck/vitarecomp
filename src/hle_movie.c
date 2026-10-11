/* hle_movie.c — SceMp4, the movie container library: so far, one that opens
 * nothing.
 *
 * A title that plays movies carries Sony's player in its own code; what it
 * imports is the layer beneath: this library to take an MP4 apart into
 * video and audio units, and the decoders to turn those into pictures and
 * sound. Neither exists here yet. Until they do, a movie is a file that
 * cannot be opened: sceMp4OpenFile gives no handle, the title's player
 * reports the failure to the title, and the title goes on to whatever
 * comes after the movie. That is what the title does on the console when
 * the file is damaged, so it is a path its code already has.
 *
 * The imports arrive by number, since the name tables this toolkit uses do
 * not carry them; the names are the Vita3K project's.
 */

#include "vitarecomp/hle.h"
#include "vitarecomp/mem.h"
#include "vitarecomp/recomp_rt.h"

#define SCE_ERROR_ERRNO_ENOSYS 0x80010058u

/* SceMp4Handle sceMp4OpenFile(const char *path, ...): a handle, or 0 */
void vita_hle_nid_05474AF0(void) {
    const char *path = vita_mem_avail(r0) ? (const char *)vita_mem_ptr(r0, 1) : NULL;
    vita_hle_trace("sceMp4OpenFile(%.200s): no movie is opened", path ? path : "?");
    r0 = 0;
}

/* int sceMp4StartFileStreaming(handle, ...): there is never a handle to start */
void vita_hle_nid_30E49E4D(void) { r0 = SCE_ERROR_ERRNO_ENOSYS; }

/* The near-field sharing service, asked to start with a work area of the
 * title's: there is no such service on this machine, and saying so makes
 * the title give its work area back and carry on without. */
void vita_hle_nid_A412E9CA(void) { r0 = SCE_ERROR_ERRNO_ENOSYS; }
