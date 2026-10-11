/* hle_movie.c — SceMp4, the movie container library: not here yet.
 *
 * A title that plays movies carries Sony's player in its own code; what it
 * imports is the layer beneath: this library to take an MP4 apart into
 * video and audio units, and the decoders to turn those into pictures and
 * sound. Neither exists here yet.
 *
 * A title does not find that out from these functions. The player loads
 * the library as a system module before it uses it, and the runtime
 * answers there that it has no such module (hle_kernel.c); the player then
 * fails to start, at once and in the call that asked, and the title goes on
 * to whatever follows the movie. That matters because the other way of
 * failing does not work: a player whose module loaded, and whose open then
 * failed, logs the failure and tells its caller nothing, and the title
 * waits for ever on a movie that never began. What is below is therefore
 * not reached by a title that asks first; it answers the ones that do not.
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
