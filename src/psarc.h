/* psarc.h — a PlayStation archive, read in place.
 *
 * The archive a title ships its data in when it uses the file library's
 * dearchiver (hle_fios.c): a table of contents, a manifest of names, and
 * every file cut into blocks that are each deflated or stored. Nothing is
 * unpacked: a file is found by name and read at an offset, and only the
 * blocks a read touches are fetched and inflated.
 */

#ifndef VITARECOMP_PSARC_H
#define VITARECOMP_PSARC_H

#include <stddef.h>
#include <stdint.h>

typedef struct psarc psarc;
typedef struct psarc_file psarc_file;

/* Opens the archive at a host path. NULL with a sentence in `why`. */
psarc *psarc_open(const char *host_path, char *why, size_t why_cap);
void   psarc_close(psarc *a);

/* How many files it holds, and the name of each as the manifest has it. */
uint32_t    psarc_count(const psarc *a);
const char *psarc_name(const psarc *a, uint32_t index);

/* The file of that name, or -1: letters compare without case, either slash
 * separates, and a leading slash is ignored. */
int      psarc_find(const psarc *a, const char *name);
uint64_t psarc_size(const psarc *a, int index);

/* Whether any name begins with `dir` followed by a slash. */
int psarc_has_dir(const psarc *a, const char *dir);

/* A file opened for reading: it remembers the block it last read, so a
 * file read from front to back inflates each block once. */
psarc_file *psarc_file_open(psarc *a, int index);
void        psarc_file_close(psarc_file *f);
uint64_t    psarc_file_size(const psarc_file *f);

/* Reads `length` bytes at `offset`; returns how many were read (fewer at the
 * end of the file), or -1 when the archive itself cannot be read. */
int64_t psarc_file_read(psarc_file *f, uint64_t offset, void *dst, uint64_t length);

#endif /* VITARECOMP_PSARC_H */
