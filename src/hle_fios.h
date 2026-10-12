/* hle_fios.h — a file by the path a program would name, for the runtime's own reading.
 *
 * A library the runtime stands in for is sometimes handed a path and
 * expected to read the file itself (the movie player, of a movie). The
 * path means what it means to the file library: overlays move it, a
 * mounted archive answers for it, and what neither claims is a file of the
 * host's under a mount. This is that way in, without a program's handle:
 * open, read at an offset, close. Nothing is unpacked to anywhere; a file
 * inside an archive is read from the archive, the blocks a read touches.
 */

#ifndef VITARECOMP_HLE_FIOS_H
#define VITARECOMP_HLE_FIOS_H

#include <stdint.h>

typedef struct vita_fios_file vita_fios_file;

/* NULL when the path leads to no file. */
vita_fios_file *vita_fios_open(const char *guest_path);
void            vita_fios_close(vita_fios_file *f);
uint64_t        vita_fios_size(const vita_fios_file *f);

/* `length` bytes at `offset` into `dst`: how many were read, fewer only at
 * the file's end, or -1 when it could not be read. */
int64_t vita_fios_read(vita_fios_file *f, uint64_t offset, void *dst, uint32_t length);

#endif /* VITARECOMP_HLE_FIOS_H */
