/* hle_vfs.h — guest paths to host paths.
 *
 * A guest path names a mount and a path under it: "app0:gamedata.bin",
 * "ux0:data/save.bin". The host declares where each mount lives
 * (vita_io_mount, host.h); the file HLEs resolve through here and refuse
 * what nothing maps, which is how a program is told a file is missing
 * rather than handed a host path it should never have seen.
 */

#ifndef VITARECOMP_HLE_VFS_H
#define VITARECOMP_HLE_VFS_H

#include <stddef.h>

/* Resolves `guest` into `host` (NUL-terminated within `cap`). Returns 1 when
 * a mount matched, 0 otherwise. */
int vita_io_resolve(const char *guest, char *host, size_t cap);

#endif /* VITARECOMP_HLE_VFS_H */
