/* analyze.h — function discovery.
 *
 * Recursive descent from seeds, carrying instruction-set state. Mode is the
 * part with no PSP analogue: a seed is worthless without knowing whether its
 * target is ARM or Thumb, and decoding a Thumb function as ARM produces
 * plausible instructions rather than an obvious failure.
 *
 * Where the seeds come from, in increasing order of what they find:
 *
 *   1. module_start, from .sce_module_info
 *   2. the export entry table
 *   3. a linear harvest of BL/BLX targets
 *   4. stored pointers named by the relocation stream (relocatable modules)
 *   5. pointer-shape recovery for stored function pointers
 *
 * Step 5 is a HEURISTIC and is reported separately for that reason. Step 4 is
 * the enumeration that replaces it on ET_SCE_RELEXEC modules: a relocation
 * names a word that holds an address, so there is no question of whether the
 * word is a pointer, only of whether the odd-aligned thing it points at is a
 * function or a string. On ET_SCE_EXEC modules — which is every Vita
 * launch-window title, and therefore every exclusive worth recompiling — there
 * are no relocations and shape is all there is.
 */

#ifndef ARMRECOMP_ANALYZE_H
#define ARMRECOMP_ANALYZE_H

#include "decode.h"
#include "module.h"
#include "reloc.h"

/* How a function was found. Kept on the function so a report can say which
 * tier is doing the work. */
enum {
    VF_PRINCIPLED = 0,   /* entry point, export, or a call target */
    VF_SHAPE      = 1,   /* pointer-shape heuristic               */
    VF_RELOC      = 2,   /* a relocated word with the Thumb bit   */
};

/* Why an address was seeded, as a set: a function can be named by several
 * of these, and the emitter cares which. The first five are ROOTS, places
 * control can arrive from outside the module's own direct calls: the entry
 * point, an exported symbol, or a pointer the program stores or builds. The
 * other three are reached from code already being followed, so they matter
 * only if what reaches them is itself kept. */
enum {
    VF_R_ENTRY     = 1,     /* module_start                                 */
    VF_R_EXPORT    = 2,     /* the export table                              */
    VF_R_RELOC     = 4,     /* a relocated word with the Thumb bit           */
    VF_R_SHAPE     = 8,     /* a stored word with the shape of a pointer     */
    VF_R_POINTER   = 16,    /* a MOVW/MOVT pair building a Thumb address     */
    VF_R_CALL      = 32,    /* a BL target, followed or swept                */
    VF_R_TAIL      = 64,    /* a branch target registered as a function      */
    VF_R_INTERWORK = 128,   /* a `bx pc` veneer's destination                */
    /* A stored or built pointer with no prologue at its target and no table
     * of pointers around it. Data as often as code (an OID string, a record
     * at an odd offset), so it is walked but it is not a root: it runs only
     * if code the roots reach transfers to it. */
    VF_R_WEAK_PTR  = 256,
};
#define VF_R_ROOTS (VF_R_ENTRY | VF_R_EXPORT | VF_R_RELOC | VF_R_SHAPE | VF_R_POINTER)

typedef struct {
    uint32_t addr;          /* virtual address                      */
    arm_mode mode;
    uint32_t end;           /* highest address reached from here    */
    uint32_t insns;
    uint8_t  from_heuristic;  /* one of VF_PRINCIPLED, VF_SHAPE, VF_RELOC */
    uint16_t reasons;         /* the VF_R_* bits that named this address */
} vf_func;

typedef struct {
    vf_func *funcs;
    uint32_t count;
    uint32_t cap;

    uint8_t *seen;          /* one bit per 2 bytes: instruction start  */
    uint32_t seen_bytes;

    uint32_t insns_total;   /* instructions reached                    */
    uint32_t bytes_covered;
    uint32_t seeds_entry;
    uint32_t seeds_export;
    uint32_t seeds_call;
    uint32_t seeds_shape;
    /* Of seeds_shape, how many came from the DATA segments rather than from
     * .text. Kept apart because they are found in a table of pointers rather
     * than among instructions, which is a far better place to look — merging
     * the counts would hide which sweep is doing the work. */
    uint32_t seeds_shape_data;
    uint32_t rejected_shape;
    /* Mode-switching calls whose target is not a known import stub, and so
     * were not followed into ARM. Counted rather than hidden: if a module has
     * genuine ARM code outside its stub table, this is the number that says
     * so. */
    uint32_t seeds_arm_refused;
    /* `bx pc` veneers followed into the other instruction set. */
    uint32_t seeds_interwork;
    /* Unconditional branch targets, registered as functions in case the
     * branch was a tail call. */
    uint32_t seeds_tail;
    /* TBB/TBH tables whose length the guarding compare gave, followed. */
    uint32_t jump_tables;
    /* MOVW/MOVT pairs in walked code that build a Thumb-tagged address into
     * .text: a function pointer no literal pool holds and, on a static
     * module, no relocation names. */
    uint32_t seeds_pointer;
    /* Of the relocation and MOVW/MOVT pointers, those seeded weakly. */
    uint32_t seeds_weak;
    uint32_t simd_insns;
    uint32_t indirect_sites;

    /* Relocation-backed pointers into the executable segment.
     *
     *   reloc_ptrs_code   odd (Thumb-tagged): a function pointer, or a pointer
     *                     to an odd byte of read-only data in the same segment
     *   reloc_ptrs_even   even: ARM code or data, and nothing in the entry
     *                     says which, so these are counted and not seeded
     *   seeds_reloc       of the odd ones, those seeded
     *   rejected_reloc    of the odd ones, those refused because the bytes
     *                     there read as text rather than as a prologue */
    uint32_t reloc_ptrs_code;
    uint32_t reloc_ptrs_even;
    uint32_t seeds_reloc;
    uint32_t seeds_reloc_table;   /* of seeds_reloc, those inside a pointer table */
    uint32_t rejected_reloc;
} vf_result;

/* Discover functions in the image's executable segment. `relocs` may be NULL
 * (a static module, or a caller that does not want relocation seeds). Returns
 * 0 on success. The caller frees with vf_free(). */
int vf_discover(const vm_image *img, const vm_module *mod,
                const vr_table *relocs, vf_result *out);

void vf_free(vf_result *r);

#endif /* ARMRECOMP_ANALYZE_H */
