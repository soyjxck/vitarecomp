/* analyze.c — function discovery by recursive descent.
 *
 * The walk carries (address, mode) pairs rather than bare addresses. Every
 * seed records the instruction set it was reached in, because there is no way
 * to recover that from the bytes: Thumb decoded as ARM yields instructions, not
 * errors.
 */

#include "analyze.h"

#include <stdlib.h>
#include <string.h>

/* --- the visited bitmap ---------------------------------------------------- */
/* One bit per 2 bytes: the finest granularity an instruction can start on. */

static int seen_get(const vf_result *r, uint32_t off) {
    uint32_t i = off >> 1;
    return (r->seen[i >> 3] >> (i & 7)) & 1;
}

static void seen_set(vf_result *r, uint32_t off) {
    uint32_t i = off >> 1;
    r->seen[i >> 3] |= (uint8_t)(1u << (i & 7));
}

/* --- a queue of (address, mode) seeds -------------------------------------- */

typedef struct { uint32_t addr; arm_mode mode; uint16_t heur; } seed;

typedef struct { seed *v; uint32_t n, cap; } seedq;

static void sq_push(seedq *q, uint32_t addr, arm_mode mode, uint16_t heur) {
    if (q->n == q->cap) {
        q->cap = q->cap ? q->cap * 2 : 1024;
        q->v = (seed *)realloc(q->v, q->cap * sizeof(seed));
    }
    q->v[q->n].addr = addr;
    q->v[q->n].mode = mode;
    q->v[q->n].heur = heur;
    q->n++;
}

static int looks_like_text(uint16_t h);

/* Does code start here?
 *
 * The pointer tiers (relocation, shape, MOVW/MOVT) name addresses that may be
 * data as easily as functions: a string, a record, an entry of a table at an
 * odd offset. The first instruction is a weak test, because the Thumb decoder
 * reads nearly every halfword as something. A short linear decode is a strong
 * one: real code is claimed instruction after claimed instruction, and data
 * is not. Twelve instructions, stopping at the first return or unconditional
 * branch; an undefined one first, or two the emitter could not name, and it
 * is not code.
 *
 * What this costs: a function reached only through a stored pointer whose
 * first dozen instructions include two the emitter does not translate yet is
 * refused, and a call through that pointer is a dispatch miss that names the
 * address. That is the announced failure this toolkit prefers to the silent
 * one, which was ~250 roots walking a record table as Thumb and emitting
 * 20,000 traps of one undefined word. */
static int starts_like_code(const vm_image *img, uint32_t addr) {
    const uint8_t *text = img->data + img->seg_file;
    const uint32_t base = img->seg_vaddr, len = img->seg_len;
    uint32_t a = addr;
    int unclaimed = 0;
    for (int k = 0; k < 12; k++) {
        if (a < base || a + 2 > base + len) break;
        arm_insn in;
        int n = arm_decode(text, base, len, a, ARM_T32, &in);
        if (n == 0) break;
        if (in.cls == A_UNKNOWN || in.cls == A_UNDEF) {
            if (k == 0) return 0;
            unclaimed++;
        } else if (in.op == OP_NONE && in.cls != A_CALL && in.cls != A_BRANCH &&
                   in.cls != A_RETURN && in.cls != A_INDIRECT && in.cls != A_NOP) {
            unclaimed++;
        }
        if (unclaimed >= 2) return 0;
        if (in.cls == A_RETURN || in.cls == A_INDIRECT) break;
        if (in.cls == A_BRANCH && !in.conditional) break;
        a += (uint32_t)n;
    }
    return 1;
}

/* `reason` is the VF_R_* bit that named the address; the reporting tier
 * follows from it. */
/* The prologue a compiled function overwhelmingly begins with: a push, or a
 * stack adjustment. Its absence is what makes a stored pointer's target weak
 * evidence of a function. */
static int has_prologue(uint16_t h) {
    return ((h & 0xFE00) == 0xB400)          /* push            */
        || (h == 0xE92D)                     /* push.w / stmdb  */
        || ((h & 0xFF80) == 0xB080);         /* sub sp, #imm    */
}

static void add_func(vf_result *r, uint32_t addr, arm_mode mode, uint16_t reason) {
    if (r->count == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 4096;
        r->funcs = (vf_func *)realloc(r->funcs, r->cap * sizeof(vf_func));
    }
    vf_func *f = &r->funcs[r->count++];
    memset(f, 0, sizeof(*f));
    f->addr = addr;
    f->mode = mode;
    f->reasons = reason;
    f->from_heuristic = (reason & (VF_R_SHAPE | VF_R_WEAK_PTR)) ? VF_SHAPE
                      : (reason & VF_R_RELOC) ? VF_RELOC : VF_PRINCIPLED;
}

/* Should a mode-switching call be followed into ARM?
 *
 * Only when the target is a known import stub. The stubs are this module's ARM
 * code — placeholder bodies the loader patches — and a BLX to one is the
 * ordinary way a Thumb module calls firmware. A BLX-immediate pointing
 * anywhere else is, on this corpus, a misdecode: the tail of .text is
 * read-only data, and data decoded as Thumb produces plausible BLX encodings
 * in quantity.
 *
 * Refusing matters more here than for any other seed, because the two decoders
 * fail differently. The Thumb decoder returns UNKNOWN often enough to stop a
 * runaway walk; the ARM decoder assigns a class to every op value, so it never
 * stops on its own. One bad ARM seed does not produce one bad function, it
 * produces thousands of instructions of confident nonsense — 152,919 of them
 * before this check existed, against 1.58 million real ones. */
static int arm_seed_ok(const vm_module *mod, uint32_t target) {
    return vm_find_stub(mod, target) != NULL;
}

/* --- walking one function --------------------------------------------------- */
/*
 * Follows local control flow from `start`, marking instruction starts and
 * queueing call targets as new functions. Branch targets are followed as local
 * flow; a branch that is really a tail call therefore merges the callee into
 * this function's extent, which inflates extent without losing correctness —
 * every instruction still gets decoded exactly once, from the right mode.
 */

static void walk(const vm_image *img, const vm_module *mod, vf_result *r,
                 seedq *q, uint32_t start, arm_mode mode, vf_func *fn) {
    const uint8_t *text = img->data + img->seg_file;
    const uint32_t base = img->seg_vaddr;
    const uint32_t len  = img->seg_len;

    /* Local block worklist. Bounded: a pathological function cannot make this
     * grow without bound because every block start is marked seen first. */
    seedq local = {0};
    sq_push(&local, start, mode, 0);

    /* Guards that branch to their table: what the walk from each such
     * target should know about the register it is about to index with. */
    struct { uint32_t addr; int8_t reg; uint32_t max; } guard[256];
    uint32_t nguard = 0;

    while (local.n) {
        seed s = local.v[--local.n];
        uint32_t a = s.addr;

        /* IT state, as in the emitter's collect(): a return or an
         * unconditional branch inside an IT block is conditional, so the
         * flow continues past it. Without this the walk stops at the first
         * `it eq; bxeq lr` and never sees the rest of the function. */
        int it_left = 0;

        /* The last few instructions in flow, for the compare that bounds a
         * jump table. */
        arm_insn prev[16];
        uint32_t nprev = 0;
        for (uint32_t g = 0; g < nguard; g++)
            if (guard[g].addr == a) { arm_table_seed(&prev[0], guard[g].reg, guard[g].max); nprev = 1; break; }

        /* MOVW halves waiting for their MOVT, per register. */
        uint32_t movw_val[16];
        uint16_t movw_ok = 0;

        for (;;) {
            if (a < base || a >= base + len) break;
            uint32_t off = a - base;
            if (seen_get(r, off)) break;          /* already decoded here */

            arm_insn in;
            int n = arm_decode(text, base, len, a, s.mode, &in);
            if (n == 0) break;

            /* A jump table whose length the guarding compare gives is a set
             * of local branches, and its targets are followed as such. One
             * without is the computed transfer it looks like. */
            if (in.cls == A_INDIRECT && (in.op == OP_TBB || in.op == OP_TBH)) {
                uint32_t count = arm_table_count(prev, nprev, &in);
                uint32_t tg[256];
                uint32_t nt = count ? arm_table_targets(text, base, len, &in, count, tg, 256) : 0;
                seen_set(r, off);
                r->insns_total++;
                r->bytes_covered += (uint32_t)n;
                fn->insns++;
                if (a + (uint32_t)n > fn->end) fn->end = a + (uint32_t)n;
                if (nt) {
                    for (uint32_t i = 0; i < nt; i++) sq_push(&local, tg[i], s.mode, 0);
                    r->jump_tables++;
                } else {
                    r->indirect_sites++;
                }
                break;
            }

            if (it_left > 0) {
                it_left--;
                if (in.cls == A_BRANCH || in.cls == A_RETURN || in.cls == A_INDIRECT)
                    in.conditional = 1;
            }
            if (in.op == OP_IT)
                it_left = (in.it_mask & 1) ? 4 : (in.it_mask & 2) ? 3 : (in.it_mask & 4) ? 2 : 1;

            seen_set(r, off);
            r->insns_total++;
            r->bytes_covered += (uint32_t)n;
            fn->insns++;
            if (a + (uint32_t)n > fn->end) fn->end = a + (uint32_t)n;
            if (in.cls == A_SIMD) r->simd_insns++;

            /* A MOVW/MOVT pair builds a 32-bit constant in a register. When
             * the constant is a Thumb-tagged address into .text it is a
             * function pointer about to be stored or called through, and
             * this is the only place to see it: no literal pool holds it,
             * and on a static module no relocation names it. The text
             * filter is the relocation seeds': an odd address can also be
             * the second byte of a string. */
            if (in.op == OP_MOVT && in.rd >= 0 && in.rd < 16 && ((movw_ok >> in.rd) & 1)) {
                uint32_t v = (movw_val[in.rd] & 0xFFFFu) | (in.imm << 16);
                movw_ok &= (uint16_t)~(1u << in.rd);
                if ((v & 1) && (v & ~1u) >= base && (v & ~1u) + 2 <= base + len) {
                    uint32_t t = v & ~1u;
                    uint16_t h = (uint16_t)(text[t - base] | ((uint16_t)text[t - base + 1] << 8));
                    if (!looks_like_text(h) && starts_like_code(img, t)) {
                        int strong = has_prologue(h);
                        sq_push(q, t, ARM_T32, strong ? VF_R_POINTER : VF_R_WEAK_PTR);
                        r->seeds_pointer++;
                        if (!strong) r->seeds_weak++;
                    }
                }
            } else if (in.op == OP_MOV && in.has_imm && in.width == 4 && !in.sets_flags &&
                       in.rd >= 0 && in.rd < 16) {
                movw_val[in.rd] = in.imm;
                movw_ok |= (uint16_t)(1u << in.rd);
            } else if (in.rd >= 0 && in.rd < 16) {
                movw_ok &= (uint16_t)~(1u << in.rd);
            }

            /* Calls seed new functions. An interworking call flips the mode of
             * the callee, which is the whole reason mode travels with a seed. */
            if (in.cls == A_CALL) {
                if (in.has_target) {
                    arm_mode cm = in.switches_mode
                                ? (s.mode == ARM_T32 ? ARM_A32 : ARM_T32)
                                : s.mode;
                    /* A mode-switching call is only followed into ARM when the
                     * target is a known import stub.
                     *
                     * The stubs are the module's ARM code — placeholder bodies
                     * the loader patches — and a BLX to one is the ordinary way
                     * a Thumb module calls firmware. A BLX-immediate pointing
                     * anywhere else is, on this corpus, a misdecode: the tail of
                     * .text is read-only data, and data decoded as Thumb
                     * produces plausible BLX encodings.
                     *
                     * Refusing matters more here than for any other seed
                     * because of an asymmetry between the decoders. The Thumb
                     * decoder returns UNKNOWN often enough to stop a runaway
                     * walk; the ARM decoder assigns a class to every op value,
                     * so it never stops on its own. One bad ARM seed does not
                     * produce one bad function, it produces thousands of
                     * instructions of confident nonsense — 152,919 of them
                     * before this check existed. */
                    if (cm == ARM_A32 && s.mode == ARM_T32 &&
                        !arm_seed_ok(mod, in.target)) {
                        r->seeds_arm_refused++;
                    } else {
                        sq_push(q, in.target, cm, VF_R_CALL);
                        r->seeds_call++;
                    }
                } else {
                    /* A register-form BLX. It is a call whose destination is
                     * computed, so it seeds nothing and the callee is reachable
                     * only through a stored pointer. Counting these separately
                     * is what makes the shape heuristic's job visible: they are
                     * the calls whose targets discovery cannot follow. */
                    r->indirect_sites++;
                }
                a += (uint32_t)n;
                continue;
            }

            if (in.cls == A_RETURN) break;

            if (in.cls == A_INDIRECT) { r->indirect_sites++; break; }

            /* A branch into the other instruction set (`bx pc`) is a transfer
             * to a function that must be decoded in that set, not local flow:
             * a function has one mode. It is accepted as an ARM seed without
             * the stub check, because it was decoded in flow from a real
             * instruction, not manufactured by a linear sweep over data. */
            if (in.cls == A_BRANCH && in.has_target && in.switches_mode) {
                sq_push(q, in.target, s.mode == ARM_T32 ? ARM_A32 : ARM_T32, VF_R_INTERWORK);
                r->seeds_interwork++;
                if (!in.conditional) break;
                a += (uint32_t)n;
                continue;
            }

            if (in.cls == A_BRANCH && in.has_target && in.conditional && nguard < 256) {
                int8_t greg; uint32_t gmax;
                if (arm_table_guard(prev, nprev, &in, &greg, &gmax)) {
                    guard[nguard].addr = in.target;
                    guard[nguard].reg  = greg;
                    guard[nguard].max  = gmax;
                    nguard++;
                }
            }
            if (nprev < 16) prev[nprev++] = in;
            else { for (uint32_t i = 1; i < 16; i++) prev[i - 1] = prev[i]; prev[15] = in; }

            if (in.cls == A_BRANCH && in.has_target) {
                sq_push(&local, in.target, s.mode, 0);
                /* A branch to before the function's own start leaves the
                 * function whatever its condition: a shared tail, or a
                 * conditional tail call. libgcc's division helpers branch
                 * back into their neighbours this way. */
                if (in.conditional && in.target < start) {
                    sq_push(q, in.target, s.mode, VF_R_TAIL);
                    r->seeds_tail++;
                }
                if (!in.conditional) {
                    /* An unconditional branch is also a possible tail call,
                     * and the function it lands in is a function whether or
                     * not anything BL's it. Registering the target as a
                     * function as well as following it locally costs a
                     * duplicate body where the branch was merely a jump; the
                     * alternative, leaving it out, costs a trap on every
                     * `b <callee>` that leaves the caller's extent — which is
                     * how newlib reaches most of its veneers. */
                    sq_push(q, in.target, s.mode, VF_R_TAIL);
                    r->seeds_tail++;
                    break;                        /* unconditional: flow ends */
                }
            }

            a += (uint32_t)n;
        }
    }

    free(local.v);
}

/* --- pointer-shape recovery ------------------------------------------------- */
/*
 * With no relocations, stored function pointers can only be recognised by what
 * they look like. A candidate word must land inside the executable segment, be
 * correctly aligned, and decode as something. That is a heuristic and is
 * reported as one — it will both miss real pointers and admit false ones.
 *
 * The Thumb bit is the one genuinely reliable signal here: a stored pointer to
 * Thumb code has bit 0 set, because that is how the hardware switches mode on
 * an indirect branch. A word with bit 0 set that otherwise looks like a code
 * address is far more likely to be a real function pointer than a coincidence.
 */

/* Does this word have the SHAPE of a pointer to Thumb code? Cheap, and
 * deliberately says nothing about whether a function is there. */
static int code_ptr_shape(const vm_image *img, const uint8_t *p, uint32_t *out) {
    uint32_t w = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
               | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    if (!(w & 1)) return 0;                       /* Thumb pointers only */
    uint32_t t = w & ~1u;
    if (t < img->seg_vaddr || t >= img->seg_vaddr + img->seg_len) return 0;
    if ((t - img->seg_vaddr) & 1) return 0;
    *out = t;
    return 1;
}

/* How many consecutive words from `off` have that shape.
 *
 * This is the evidence that turns a guess into a reading. Four adjacent words
 * that are all Thumb-tagged, all correctly aligned, and all land inside a
 * 5.6 MB window of a 4 GB space do not happen by accident — the odds are
 * around 1 in 10^13. Such a run is a POINTER TABLE, and knowing that is worth
 * more than any property of the individual entries. */
#define SHAPE_RUN_MIN 4

static uint32_t shape_run_len(const vm_image *img, const uint8_t *scan,
                              uint32_t scan_len, uint32_t off) {
    uint32_t n = 0, t;
    while (off + 4 <= scan_len && code_ptr_shape(img, scan + off, &t)) {
        n++;
        off += 4;
    }
    return n;
}

/* Sweep one region for words that look like pointers INTO the executable
 * segment. The region being scanned and the region being pointed at are two
 * different things: candidates come from wherever the module stores pointers,
 * but a valid target is always code. */
static uint32_t shape_scan_region(const vm_image *img, vf_result *r, seedq *q,
                                  const uint8_t *scan, uint32_t scan_len) {
    const uint8_t *text = img->data + img->seg_file;
    const uint32_t base = img->seg_vaddr;
    const uint32_t len  = img->seg_len;
    uint32_t found = 0;
    uint32_t run_left = 0;      /* words remaining in the current table */

    for (uint32_t off = 0; off + 4 <= scan_len; off += 4) {
        uint32_t t;

        /* Only measure when not already inside a table, so a long run costs
         * one measurement rather than one per entry. A run too short to
         * qualify is at most SHAPE_RUN_MIN words, so re-measuring across it is
         * bounded and cheap. */
        if (run_left == 0) {
            uint32_t n = shape_run_len(img, scan, scan_len, off);
            if (n >= SHAPE_RUN_MIN) run_left = n;
        }
        int in_table = run_left > 0;
        if (run_left) run_left--;

        if (!code_ptr_shape(img, scan + off, &t)) continue;

        /* Do not re-seed something already decoded: that is not a discovery. */
        if (seen_get(r, t - base)) continue;

        arm_insn in;
        if (arm_decode(text, base, len, t, ARM_T32, &in) == 0) { r->rejected_shape++; continue; }
        if (in.cls == A_UNKNOWN || in.cls == A_UNDEF) { r->rejected_shape++; continue; }

        /* "Decodes as something" is far too weak a filter to be worth much:
         * the Thumb decoder almost never returns unknown, so nearly every
         * candidate passes and the heuristic degenerates into "any word with
         * bit 0 set that points into .text". In several megabytes of data that
         * is mostly coincidence.
         *
         * Requiring a function PROLOGUE is a real filter. Compiled Thumb
         * functions overwhelmingly begin by pushing registers — PUSH (0xB4xx /
         * 0xB5xx) or PUSH.W / STMDB (0xE92D). A leaf function that pushes
         * nothing will be missed, which is why the two tiers are counted and
         * reported separately rather than merged into one number.
         *
         * INSIDE A TABLE the filter is not just unnecessary, it is wrong. A
         * word in a run has already been established as a pointer by its
         * neighbours, so demanding a prologue of it only throws away the
         * functions that genuinely lack one. On this corpus's first title that
         * is 47 of the 551 static constructors, and they are exactly the kind
         * you would expect: leaves that start with `movw` because they store a
         * constant and save nothing, and one that is a bare `bx lr`. Each one
         * missed is a run-time trap in a table the program walks at startup. */
        uint16_t h = (uint16_t)(text[t - base] | ((uint16_t)text[t - base + 1] << 8));
        int prologue = ((h & 0xFE00) == 0xB400)          /* push            */
                    || (h == 0xE92D)                     /* push.w / stmdb  */
                    || ((h & 0xFF80) == 0xB080);         /* sub sp, #imm    */

        if (!prologue && !in_table) { r->rejected_shape++; continue; }
        if (!starts_like_code(img, t)) { r->rejected_shape++; continue; }

        sq_push(q, t, ARM_T32, VF_R_SHAPE);
        r->seeds_shape++;
        found++;
    }
    return found;
}

static void shape_scan(const vm_image *img, vf_result *r, seedq *q) {
    shape_scan_region(img, r, q, img->data + img->seg_file, img->seg_len);

    /* The data segments, which is where a C++ module keeps the table of
     * static constructors. Nothing in .text points at them: the only reference
     * to a constructor is its slot in that table, so a sweep of the executable
     * segment alone finds none of them — on this corpus's first title, all 551,
     * including the module's very first function.
     *
     * Counted separately from the .text sweep. They are the same heuristic run
     * over different bytes, but a table of pointers is a much richer place to
     * find pointers than a code segment is, and merging the two numbers would
     * hide which one is doing the work. */
    for (uint32_t i = 0; i < img->data_seg_count; i++) {
        const vm_seg *s = &img->data_segs[i];
        if ((uint64_t)s->file + s->len > img->size) continue;
        r->seeds_shape_data +=
            shape_scan_region(img, r, q, img->data + s->file, s->len);
    }
}

/* --- relocation-backed pointers --------------------------------------------- */
/*
 * On a relocatable module the stream names every word the linker filled with
 * an address. Whether such a word IS a pointer is settled; what remains is
 * what it points at. Into the executable segment, with the Thumb bit set,
 * there are two candidates: a Thumb function, or an odd byte of the read-only
 * data that shares the segment — a string, most often, since strings are
 * byte-packed and half of them start on an odd address.
 *
 * So the filter is narrower than shape's. A target that begins with a
 * prologue is taken. One that does not, and whose first two bytes are both
 * printable ASCII, is refused as text: a function whose first halfword is two
 * printable bytes exists (`bx lr` is "pG") but a string that starts with two
 * printable bytes is the common case. Both counts are reported.
 *
 * Inside a TABLE the text filter is not applied, for the reason shape_scan
 * gives: a run of adjacent relocated words that all hold Thumb-tagged .text
 * addresses is a table of function pointers, and its neighbours have already
 * settled what each word is. A string table would hold even and odd pointers
 * alike. Relocations arrive in address order, so a run is consecutive entries
 * whose patch addresses step by one word.
 */

#define RELOC_RUN_MIN 4

static int looks_like_text(uint16_t h) {
    uint8_t a = (uint8_t)h, b = (uint8_t)(h >> 8);
    return a >= 0x20 && a < 0x7F && b >= 0x20 && b < 0x7F;
}

/* A relocated word holding a Thumb-tagged address into .text. */
static int table_word(const vm_image *img, const vr_entry *e) {
    if (e->code != VR_ABS32 && e->code != VR_TARGET1) return 0;
    if (!(e->value & 1)) return 0;
    uint32_t t = e->value & ~1u;
    return t >= img->seg_vaddr && t < img->seg_vaddr + img->seg_len;
}

static void reloc_seeds(const vm_image *img, const vr_table *relocs,
                        vf_result *r, seedq *q) {
    const uint8_t *text = img->data + img->seg_file;
    const uint32_t base = img->seg_vaddr;
    const uint32_t len  = img->seg_len;

    uint8_t *in_table = (uint8_t *)calloc(relocs->n, 1);
    if (in_table) {
        uint32_t i = 0;
        while (i < relocs->n) {
            if (!table_word(img, &relocs->v[i])) { i++; continue; }
            uint32_t j = i + 1;
            while (j < relocs->n && table_word(img, &relocs->v[j]) &&
                   relocs->v[j].patch == relocs->v[j - 1].patch + 4)
                j++;
            if (j - i >= RELOC_RUN_MIN) memset(in_table + i, 1, j - i);
            i = j;
        }
    }

    for (uint32_t i = 0; i < relocs->n; i++) {
        const vr_entry *e = &relocs->v[i];

        /* A word holding an address, or the MOVW half of a pair that builds
         * one in a register. The MOVT half carries the same value and would
         * count the same pointer twice. */
        if (e->code != VR_ABS32 && e->code != VR_TARGET1 &&
            e->code != VR_MOVW_ABS_NC && e->code != VR_THM_MOVW_ABS_NC)
            continue;

        uint32_t v = e->value;
        uint32_t t = v & ~1u;
        if (t < base || t >= base + len) continue;

        if (!(v & 1)) { r->reloc_ptrs_even++; continue; }
        r->reloc_ptrs_code++;

        if (t + 2 > base + len) { r->rejected_reloc++; continue; }

        arm_insn in;
        if (arm_decode(text, base, len, t, ARM_T32, &in) == 0 ||
            in.cls == A_UNKNOWN || in.cls == A_UNDEF) {
            r->rejected_reloc++;
            continue;
        }

        uint16_t h = (uint16_t)(text[t - base] | ((uint16_t)text[t - base + 1] << 8));
        int prologue = ((h & 0xFE00) == 0xB400)          /* push            */
                    || (h == 0xE92D)                     /* push.w / stmdb  */
                    || ((h & 0xFF80) == 0xB080);         /* sub sp, #imm    */
        int tabled = in_table && in_table[i];
        if (!prologue && !tabled && looks_like_text(h)) { r->rejected_reloc++; continue; }
        if (!starts_like_code(img, t)) { r->rejected_reloc++; continue; }

        /* Neither a prologue nor a table around it: as likely a record at an
         * odd offset as a leaf function. Walked, not a root. */
        int strong = prologue || tabled;
        sq_push(q, t, ARM_T32, strong ? VF_R_RELOC : VF_R_WEAK_PTR);
        r->seeds_reloc++;
        if (tabled)  r->seeds_reloc_table++;
        if (!strong) r->seeds_weak++;
    }

    free(in_table);
}

/* --- entry point ------------------------------------------------------------ */

int vf_discover(const vm_image *img, const vm_module *mod,
                const vr_table *relocs, vf_result *out) {
    memset(out, 0, sizeof(*out));

    out->seen_bytes = (img->seg_len / 2 + 7) / 8 + 1;
    out->seen = (uint8_t *)calloc(out->seen_bytes, 1);
    if (!out->seen) return 1;

    seedq q = {0};
    const uint32_t base = img->seg_vaddr;

    /* 1. module_start. Segment-relative, top two bits naming the segment.
     *
     *    The low bit is the Thumb bit when the toolchain sets it, and
     *    vita-elf-create (every homebrew module) does: VitaShell's is 0x8231,
     *    the fbdemo sample's 0x25. Seeding the odd address decodes from one
     *    byte in and produces a function of confident nonsense, which is what
     *    happened before this was masked. SNC-built retail modules leave the
     *    bit clear, and the corpus is overwhelmingly Thumb, so a clear bit is
     *    still seeded as Thumb. */
    if (mod->have_info && mod->info.module_start != 0xFFFFFFFF) {
        uint32_t ms = mod->info.module_start & 0x3FFFFFFF;
        sq_push(&q, base + (ms & ~1u), ARM_T32, VF_R_ENTRY);
        out->seeds_entry++;
    }

    /* 2. Export entry tables. Each export names a real function. */
    if (mod->have_info) {
        uint32_t p = mod->info.export_top;
        while (p + 0x20 <= mod->info.export_end) {
            const uint8_t *e = vm_ptr(img, p, 0x20);
            if (!e) break;
            uint16_t size  = (uint16_t)(e[0] | (e[1] << 8));
            uint16_t nfunc = (uint16_t)(e[6] | (e[7] << 8));
            uint32_t etab  = (uint32_t)e[0x1C] | ((uint32_t)e[0x1D] << 8)
                           | ((uint32_t)e[0x1E] << 16) | ((uint32_t)e[0x1F] << 24);
            if (size < 0x20) break;

            const uint8_t *t = vm_va(img, etab, (uint32_t)nfunc * 4);
            for (uint32_t i = 0; t && i < nfunc; i++) {
                uint32_t v = (uint32_t)t[i*4] | ((uint32_t)t[i*4+1] << 8)
                           | ((uint32_t)t[i*4+2] << 16) | ((uint32_t)t[i*4+3] << 24);
                if (v < base || v >= base + img->seg_len) continue;
                sq_push(&q, v & ~1u, (v & 1) ? ARM_T32 : ARM_A32, VF_R_EXPORT);
                out->seeds_export++;
            }
            p += size;
        }
    }

    /* 3. Linear harvest of call targets. Recursive descent alone stalls almost
     *    immediately — the same lesson psprecomp learned when descent from the
     *    entry point found 47 instructions. */
    {
        const uint8_t *text = img->data + img->seg_file;
        for (uint32_t off = 0; off + 4 <= img->seg_len; off += 2) {
            arm_insn in;
            if (arm_decode(text, base, img->seg_len, base + off, ARM_T32, &in) == 0) continue;
            if (in.cls == A_CALL && in.has_target &&
                in.target >= base && in.target < base + img->seg_len) {
                /* Same rule as the descent above. This sweep decodes every
                 * halfword of .text including its data, so it is the site that
                 * manufactures bogus mode switches in bulk. */
                if (in.switches_mode && !arm_seed_ok(mod, in.target)) {
                    out->seeds_arm_refused++;
                    continue;
                }
                sq_push(&q, in.target, in.switches_mode ? ARM_A32 : ARM_T32, VF_R_CALL);
            }
        }
    }

    /* A separate registry of function ENTRIES, distinct from the map of decoded
     * instructions.
     *
     * These must not be conflated. `seen` records "an instruction was decoded
     * here" and exists to stop the walk repeating work. Whether an address is a
     * FUNCTION is a different question, and the answer is yes whenever
     * something calls it — even if another function's flow walk happened to
     * wander through it first, which is common where code falls through into
     * what is also a call target.
     *
     * Gating registration on `seen` meant such addresses were never registered,
     * while the emitter still emitted calls to them: the generated C compiled
     * and then failed to link, naming symbols that were never defined. */
    uint8_t *fseen = (uint8_t *)calloc(out->seen_bytes, 1);
    if (!fseen) { free(q.v); return 1; }
    /* Function index + 1 per halfword of .text, so a seed that names an
     * address already registered can add its reason to the record. */
    uint32_t *fidx = (uint32_t *)calloc(img->seg_len / 2 + 1, sizeof(uint32_t));
    if (!fidx) { free(fseen); free(q.v); return 1; }

    #define FN_MARK(a) (fseen[(((a) - base) >> 1) >> 3] |= (uint8_t)(1u << ((((a) - base) >> 1) & 7)))
    #define FN_TEST(a) ((fseen[(((a) - base) >> 1) >> 3] >> ((((a) - base) >> 1) & 7)) & 1)

    uint32_t head = 0;
    while (head < q.n) {
        seed s = q.v[head++];
        if (s.addr < base || s.addr >= base + img->seg_len) continue;
        if (FN_TEST(s.addr)) {
            out->funcs[fidx[(s.addr - base) >> 1] - 1].reasons |= s.heur;
            continue;
        }

        FN_MARK(s.addr);
        add_func(out, s.addr, s.mode, s.heur);
        fidx[(s.addr - base) >> 1] = out->count;
        walk(img, mod, out, &q, s.addr, s.mode, &out->funcs[out->count - 1]);
    }

    /* 4. Relocation-backed pointers, after the calls have been followed, so
     *    the count says what the stream ADDED: a function reachable by BL is
     *    credited to the call that reaches it, and one found only because a
     *    relocated word names it is credited here. Enumeration where the
     *    module is relocatable; nothing where it is not. */
    uint32_t before = q.n;
    if (relocs && relocs->n) reloc_seeds(img, relocs, out, &q);
    head = before;
    while (head < q.n) {
        seed s = q.v[head++];
        if (s.addr < base || s.addr >= base + img->seg_len) continue;
        if (FN_TEST(s.addr)) {
            out->funcs[fidx[(s.addr - base) >> 1] - 1].reasons |= s.heur;
            continue;
        }

        FN_MARK(s.addr);
        add_func(out, s.addr, s.mode, s.heur);
        fidx[(s.addr - base) >> 1] = out->count;
        walk(img, mod, out, &q, s.addr, s.mode, &out->funcs[out->count - 1]);
    }

    /* 5. Shape recovery, after everything principled has been exhausted, so it
     *    only ever adds what the reliable seeds could not reach. */
    before = q.n;
    shape_scan(img, out, &q);
    head = before;
    while (head < q.n) {
        seed s = q.v[head++];
        if (s.addr < base || s.addr >= base + img->seg_len) continue;
        if (FN_TEST(s.addr)) {
            out->funcs[fidx[(s.addr - base) >> 1] - 1].reasons |= s.heur;
            continue;
        }

        FN_MARK(s.addr);
        add_func(out, s.addr, s.mode, s.heur);
        fidx[(s.addr - base) >> 1] = out->count;
        walk(img, mod, out, &q, s.addr, s.mode, &out->funcs[out->count - 1]);
    }

    #undef FN_MARK
    #undef FN_TEST

    free(fidx);
    free(fseen);
    free(q.v);
    return 0;
}

void vf_free(vf_result *r) {
    free(r->funcs);
    free(r->seen);
    memset(r, 0, sizeof(*r));
}
