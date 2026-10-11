/* extract.c — a SELF as the ELF inside it.
 *
 * Reassembly is mechanical once the container is parsed: the plaintext ELF
 * header and program headers are copied out of the SELF, and each segment is
 * inflated into the file offset its program header declares.
 *
 * What makes it worth doing carefully is that every step has a value the file
 * itself declares, so success is checkable rather than assumed. Three
 * independent numbers have to agree: the inflated size against `p_filesz`, the
 * Adler-32 against the segment bytes, and the total against `elf_filesize`.
 *
 * It is apart from the parser because it is the one part of this that needs
 * the inflater, and apart from the tool because a game's own program does it
 * too, to the player's copy of the module, when it is set up.
 */

#include "container.h"
#include "inflate.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(int code, char *err, size_t errsz, const char *fmt, ...) {
    if (err && errsz) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, errsz, fmt, ap);
        va_end(ap);
    }
    return code;
}

int vc_extract(const uint8_t *buf, size_t size, const vc_module *m,
               uint8_t **elf, size_t *elf_size, vc_placed *placed,
               char *err, size_t errsz) {
    if (!buf || !m || !elf || !elf_size) return fail(VC_EXTRACT_FAILED, err, errsz, "null argument");
    *elf = NULL;
    *elf_size = 0;
    if (err && errsz) err[0] = '\0';

    if (m->kind != VC_SELF) return fail(VC_EXTRACT_FAILED, err, errsz, "not a SELF; nothing to extract");

    for (int i = 0; i < m->seg_count; i++)
        if (m->seg[i].encryption == VC_ENCRYPT_YES)
            return fail(VC_EXTRACT_ENCRYPTED, err, errsz, "segment %d is encrypted", i);

    size_t out_size = (size_t)m->sce.elf_filesize;
    uint8_t *out = (uint8_t *)calloc(out_size ? out_size : 1, 1);
    if (!out) return fail(VC_EXTRACT_FAILED, err, errsz, "out of memory (%zu bytes)", out_size);

    /* ELF header, then the program header table at the offset the ELF header
     * itself declares. Both are plaintext inside the SELF. */
    if (m->sce.elf_offset + 0x34 > size || out_size < 0x34) {
        free(out);
        return fail(VC_EXTRACT_FAILED, err, errsz, "ELF header past end of file");
    }
    memcpy(out, buf + m->sce.elf_offset, 0x34);

    size_t ph_bytes = (size_t)m->elf.phnum * m->elf.phentsize;
    if (m->sce.phdr_offset + ph_bytes > size || m->elf.phoff + ph_bytes > out_size) {
        free(out);
        return fail(VC_EXTRACT_FAILED, err, errsz, "program header table does not fit");
    }
    memcpy(out + m->elf.phoff, buf + m->sce.phdr_offset, ph_bytes);

    for (int i = 0; i < m->seg_count; i++) {
        uint64_t src_off = m->seg[i].offset;
        uint64_t src_len = m->seg[i].length;
        uint32_t dst_off = m->phdr[i].offset;
        uint32_t want    = m->phdr[i].filesz;

        /* SCE_RELA and SCE_VERSION segments carry a p_offset like any other,
         * but a p_filesz of 0 would mean nothing to place. */
        if (want == 0) {
            if (placed) placed[i] = VC_PLACED_SKIPPED;
            continue;
        }

        if (src_off + src_len > size) {
            free(out);
            return fail(VC_EXTRACT_FAILED, err, errsz, "segment %d source past end of file", i);
        }
        if ((uint64_t)dst_off + want > out_size) {
            free(out);
            return fail(VC_EXTRACT_FAILED, err, errsz, "segment %d does not fit in the output", i);
        }

        if (m->seg[i].compression == VC_COMPRESS_ZLIB) {
            size_t produced = want;
            inf_status st = inf_zlib(buf + src_off, (size_t)src_len, out + dst_off, &produced);
            if (st != INF_OK) {
                free(out);
                return fail(VC_EXTRACT_FAILED, err, errsz, "segment %d: %s", i, inf_strerror(st));
            }
            /* The inflated size must match what the program header declares.
             * A stream that decompresses cleanly to the wrong length means the
             * segment table and the program headers disagree, which means the
             * container is being misread. */
            if (produced != want) {
                free(out);
                return fail(VC_EXTRACT_FAILED, err, errsz, "segment %d inflated to %zu, p_filesz says %u", i, produced, want);
            }
            if (placed) placed[i] = VC_PLACED_INFLATED;
        } else {
            if (src_len != want) {
                free(out);
                return fail(VC_EXTRACT_FAILED, err, errsz, "segment %d stored length %llu != p_filesz %u", i,
                            (unsigned long long)src_len, want);
            }
            memcpy(out + dst_off, buf + src_off, want);
            if (placed) placed[i] = VC_PLACED_STORED;
        }
    }

    if (!(out[0] == 0x7F && out[1] == 'E' && out[2] == 'L' && out[3] == 'F')) {
        free(out);
        return fail(VC_EXTRACT_FAILED, err, errsz, "reassembled file is not an ELF");
    }

    *elf = out;
    *elf_size = out_size;
    return 0;
}
