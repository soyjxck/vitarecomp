/* reloc.h — PT_SCE_RELA, the relocation stream of an ET_SCE_RELEXEC module.
 *
 * Why discovery wants it. A relocation names a word that holds an address.
 * On a relocatable module that is every stored pointer the linker knows
 * about — vtables, callback tables, thread entries, the static-constructor
 * list — enumerated rather than recognised by shape. With the Thumb bit set,
 * such a word is a function pointer, and that is a seed with no heuristic in
 * it. Every homebrew module and every 2015+ retail title is relocatable
 * (see docs/CORPUS.md); the launch-window catalogue is not, and keeps the
 * shape heuristic as its only source.
 *
 * The load model is IDENTITY: each PT_LOAD segment is taken to live at its
 * own p_vaddr, which is the address space the decoder, the emitter and the
 * literal folder already use. Under that model the symbol value is the
 * symbol segment's p_vaddr plus the addend, and applying the stream rewrites
 * every word to the value the linker already left there — which is why
 * vr_apply reports how many bytes it CHANGED. A non-zero count means the
 * image was not linked at its p_vaddr and the bytes discovery has been
 * reading are not the bytes the program would run with. Loading at a
 * different base is a run-time decision for the loader, not a
 * translation-time one.
 *
 * The entry formats are the Vita loader's own: ten of them, selected by the
 * low four bits of the first word. Format 0 is the plain three-word form and
 * the only one vita-toolchain writes; 1 to 9 are Sony's compact forms, which
 * carry a running offset and reuse the previous entry's type pair. The field
 * layouts are functional facts of the file format, cross-checked between the
 * toolchain's writer and the emulator's reader.
 */

#ifndef ARMRECOMP_RELOC_H
#define ARMRECOMP_RELOC_H

#include "container.h"

#include <stddef.h>
#include <stdint.h>

/* ARM ELF relocation codes the stream uses. Standard numbers. */
enum {
    VR_NONE            = 0,
    VR_ABS32           = 2,
    VR_REL32           = 3,
    VR_ABS8            = 8,
    VR_THM_CALL        = 10,
    VR_CALL            = 28,
    VR_JUMP24          = 29,
    VR_THM_JUMP24      = 30,
    VR_TARGET1         = 38,
    VR_V4BX            = 40,
    VR_TARGET2         = 41,
    VR_PREL31          = 42,
    VR_MOVW_ABS_NC     = 43,
    VR_MOVT_ABS        = 44,
    VR_THM_MOVW_ABS_NC = 47,
    VR_THM_MOVT_ABS    = 48,
    VR_THM_JUMP11      = 102,   /* 16-bit B, 11-bit offset */
    VR_THM_JUMP8       = 103,   /* 16-bit conditional B, 8-bit offset */
    VR_RBASE           = 255,
};

#define VR_SEG_ABSOLUTE 0xF     /* symbol_segment meaning "no base, S = 0" */

/* One relocation: a place, a code, and the value S + A the code is applied
 * with. Compact stream entries that patch two places (a MOVW/MOVT pair)
 * produce two of these. */
typedef struct {
    uint32_t patch;     /* virtual address of the patched word or instruction */
    uint32_t value;     /* S + A under the identity load model                */
    uint8_t  format;    /* stream entry format this came from, 0..9           */
    uint8_t  code;      /* relocation code applied at `patch`                 */
    uint8_t  patch_seg; /* program header index of the patched segment        */
    uint8_t  sym_seg;   /* program header index of the symbol, or 0xF         */
} vr_entry;

typedef struct {
    vr_entry *v;
    uint32_t  n, cap;

    uint32_t streams;          /* PT_SCE_RELA program headers decoded          */
    uint32_t stream_bytes;
    uint32_t by_format[10];    /* stream ENTRIES, by format                    */
    uint32_t by_code[256];     /* relocations, by code                         */
    uint32_t bad_segment;      /* entries naming a segment that is not PT_LOAD */
    uint32_t rebase_unmatched; /* rebase entries whose word is in no segment   */
    uint32_t trailing;         /* bytes at a stream's end too short to decode  */
} vr_table;

/* Decode every PT_SCE_RELA segment of `m`. Returns 0 on success, which
 * includes a module with no relocation segments (an ET_SCE_EXEC module). A
 * stream that reaches an unknown format stops there: what was decoded stays in
 * the table, `err` names the stream and offset, and the return is non-zero. */
int vr_parse(const uint8_t *buf, size_t size, const vc_module *m,
             vr_table *out, char *err, size_t errsz);

/* Apply every decoded relocation to the file image in place, under the
 * identity load model. `changed` counts relocations whose bytes differed from
 * what was already there; `unsupported` counts codes this does not write;
 * `outside` counts patch sites past the end of their segment's file bytes
 * (.bss). Returns 0 on success. */
int vr_apply(uint8_t *buf, size_t size, const vc_module *m, const vr_table *t,
             uint32_t *changed, uint32_t *unsupported, uint32_t *outside);

void vr_free(vr_table *t);

const char *vr_code_name(uint8_t code);

#endif /* ARMRECOMP_RELOC_H */
