/* emit.c — ARM to C.
 *
 * The output is meant to be read. Every instruction carries its address and
 * disassembly as a comment above the C it became, because the first thing
 * anyone does with a recompiled function that misbehaves is compare it against
 * the original.
 *
 * Two things this does NOT do, deliberately:
 *
 *   - It does not guess. An instruction whose operands the decoder did not
 *     fill becomes a named trap, never an approximation. A gap that stops the
 *     program is worth far more than one that produces a program which runs and
 *     is wrong.
 *   - It does not reorder. ARM has no delay slots, so the translation is
 *     instruction-for-instruction, and the only control flow that moves is a
 *     branch becoming a goto.
 */

#include "emit.h"

#include <stdlib.h>
#include <string.h>

#define MAX_INSNS_PER_FUNC 65536

static const char *reg_name(int r) {
    static const char *n[16] = {
        "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
        "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc"
    };
    return (r >= 0 && r < 16) ? n[r] : "?";
}

/* --- collecting one function's instructions -------------------------------- */

typedef struct {
    arm_insn *v;
    uint32_t  n, cap;
    uint32_t *labels;
    uint32_t  nlabels, lcap;
} fbody;

static void fb_push(fbody *b, const arm_insn *in) {
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 256;
        b->v = (arm_insn *)realloc(b->v, b->cap * sizeof(arm_insn));
    }
    b->v[b->n++] = *in;
}

static void fb_label(fbody *b, uint32_t addr) {
    for (uint32_t i = 0; i < b->nlabels; i++)
        if (b->labels[i] == addr) return;
    if (b->nlabels == b->lcap) {
        b->lcap = b->lcap ? b->lcap * 2 : 64;
        b->labels = (uint32_t *)realloc(b->labels, b->lcap * sizeof(uint32_t));
    }
    b->labels[b->nlabels++] = addr;
}

static int fb_has_label(const fbody *b, uint32_t addr) {
    for (uint32_t i = 0; i < b->nlabels; i++)
        if (b->labels[i] == addr) return 1;
    return 0;
}

/* Collect a function by FOLLOWING ITS CONTROL FLOW, not by sweeping its extent.
 *
 * The distinction is the difference between a usable output and a 2.4 GB one.
 * `fn->end` is the highest address the discovery walk reached, and because a
 * tail call is a branch, that walk follows branches into other functions — so
 * an extent routinely spans code and data belonging to many functions. Sweeping
 * it linearly re-emits all of that under every function whose extent covers it,
 * and the same instruction lands in the output dozens of times.
 *
 * Flow-following emits each instruction once, in exactly the places it is
 * genuinely reachable, and stops at returns. Extent is still useful as a bound;
 * it is not a body.
 */

#define VISIT_SLOTS 8192

typedef struct { uint32_t k[VISIT_SLOTS]; } visited;

static int visit_mark(visited *v, uint32_t addr) {
    uint32_t h = (addr * 2654435761u) & (VISIT_SLOTS - 1);
    for (uint32_t i = 0; i < VISIT_SLOTS; i++) {
        uint32_t s = (h + i) & (VISIT_SLOTS - 1);
        if (v->k[s] == addr) return 0;          /* already seen */
        if (v->k[s] == 0)    { v->k[s] = addr; return 1; }
    }
    return 0;                                    /* table full: stop expanding */
}

static int cmp_addr(const void *a, const void *b) {
    uint32_t x = ((const arm_insn *)a)->addr, y = ((const arm_insn *)b)->addr;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* --- the set of addresses that are actually functions ----------------------- */
/*
 * The emitter must never name a symbol it does not define. A call target only
 * becomes `vita_func_<addr>()` if discovery registered it; anything else is a
 * trap.
 *
 * This is not defensive tidying — it is the only correct answer. A linear sweep
 * decodes literal pools and tables as instructions, and some of that data
 * decodes as a BL with a plausible target. Two such targets in this module land
 * *inside the import table*, one of them 0x32 bytes into the first entry: not
 * code, never called at run time, and pure decoding artefact. Emitting calls to
 * them produced C that compiled and then failed to link.
 */

typedef struct { uint32_t *v; uint32_t n; } funcset;

static int fs_cmp(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static int fs_has(const funcset *s, uint32_t addr) {
    uint32_t lo = 0, hi = s->n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (s->v[mid] == addr) return 1;
        if (s->v[mid] <  addr) lo = mid + 1; else hi = mid;
    }
    return 0;
}

static void collect(const vm_image *img, const vf_func *fn, fbody *b) {
    const uint8_t *text = img->data + img->seg_file;
    const uint32_t base = img->seg_vaddr;

    visited seen;
    memset(&seen, 0, sizeof(seen));

    /* A function registered because something called it may have been walked
     * by discovery only briefly — its recorded extent can be zero or tiny. The
     * body is still real, so fall back to a generous bound and let control flow
     * terminate the walk, which it does at the first return. Extent is a
     * safety limit here, not the definition of the body. */
    uint32_t bound = (fn->end > fn->addr + 16) ? fn->end : fn->addr + 8192;

    uint32_t stack[512];
    int sp_ = 0;
    stack[sp_++] = fn->addr;

    while (sp_ > 0 && b->n < MAX_INSNS_PER_FUNC) {
        uint32_t a = stack[--sp_];

        /* IT state. The decoder is stateless, so the block structure is
         * applied here, where instructions are seen in order: after an IT,
         * the next one to four instructions carry its condition (or its
         * inverse, per the mask), and a return or branch among them is
         * conditional rather than the end of the block. A block boundary
         * ends an IT block too; branching into one is UNPREDICTABLE. */
        uint32_t it_first = 0, it_mask = 0;
        int it_left = 0, it_k = 0;

        for (;;) {
            if (a < base || a >= base + img->seg_len) break;
            if (a >= bound) break;                   /* extent as a bound */
            if (!visit_mark(&seen, a)) break;        /* already emitted */

            arm_insn in;
            int n = arm_decode(text, base, img->seg_len, a, fn->mode, &in);
            if (n == 0) break;

            int in_it = 0;
            if (it_left > 0) {
                it_k++;
                /* The first instruction takes the IT's condition; each later
                 * one takes it or its inverse according to the mask bit for
                 * its position — bit 3 for the second, 2 for the third, 1
                 * for the fourth — compared against the condition's low bit.
                 * Reading bit 2 for the second was the bug that made
                 * `ite ls; movls r1, #0; movhi r1, #1` set r1 to 1 on both
                 * paths, and malloc report out of memory on every call. */
                uint32_t c = it_first;
                if (it_k > 1 && (((it_mask >> (5 - it_k)) & 1) != (it_first & 1)))
                    c = it_first ^ 1;
                in.cond = (uint8_t)c;
                if (in.cls == A_BRANCH || in.cls == A_CALL ||
                    in.cls == A_RETURN || in.cls == A_INDIRECT)
                    in.conditional = 1;
                /* Inside an IT block the 16-bit data-processing forms do not
                 * set the flags, whatever their S bit would say outside one;
                 * only the compares do. A `movls` that wrote NZ would wreck
                 * the condition the next instruction of the block tests. */
                if (in.width == 2 && in.op != OP_CMP && in.op != OP_CMN && in.op != OP_TST)
                    in.sets_flags = 0;
                it_left--;
                in_it = 1;
            }
            if (in.op == OP_IT) {
                it_first = in.cond;
                it_mask  = in.it_mask;
                it_left  = (it_mask & 1) ? 4 : (it_mask & 2) ? 3 : (it_mask & 4) ? 2 : 1;
                it_k     = 0;
            }

            /* A jump table: the count comes from the compare just before it,
             * which is the last thing pushed, and travels in `imm` so the
             * emitter can read the table again. Its targets are blocks of
             * this function. */
            if (in.cls == A_INDIRECT && (in.op == OP_TBB || in.op == OP_TBH)) {
                uint32_t have = b->n < 4 ? b->n : 4;
                uint32_t count = arm_table_count(b->v + (b->n - have), have, &in);
                uint32_t tg[256];
                uint32_t nt = count ? arm_table_targets(text, base, img->seg_len, &in, count, tg, 256) : 0;
                in.imm = nt ? count : 0;
                in.has_imm = nt != 0;
                fb_push(b, &in);
                for (uint32_t i = 0; i < nt; i++) {
                    if (tg[i] < fn->addr || tg[i] >= bound) continue;
                    fb_label(b, tg[i]);
                    if (sp_ < (int)(sizeof(stack) / sizeof(stack[0])))
                        stack[sp_++] = tg[i];
                }
                break;
            }
            fb_push(b, &in);

            if (in.cls == A_RETURN && !in_it) break;
            /* A computed transfer ends the block; what follows it is reached
             * by a label if at all, and may be data. */
            if (in.cls == A_INDIRECT) break;

            /* `bound`, not `fn->end`.
             *
             * These must be the same notion of "inside this function" or the
             * walk and the labelling disagree. The walk above collects up to
             * `bound`, which falls back to a generous range when discovery
             * recorded only a tiny extent — and it records a tiny extent
             * whenever an earlier function's walk had already marked those
             * instructions seen, which is common.
             *
             * Gating labels on `fn->end` instead meant that for exactly those
             * functions, instructions were collected but branches into them got
             * no label. Each then fell through to the call-or-trap path and
             * trapped: 2,547 instructions, and every one of them an ordinary
             * intra-function branch that had already been translated correctly
             * a few lines above. */
            if (in.has_target && in.cls == A_BRANCH && !in.switches_mode &&
                in.target >= fn->addr && in.target < bound) {
                fb_label(b, in.target);
                if (sp_ < (int)(sizeof(stack) / sizeof(stack[0])))
                    stack[sp_++] = in.target;
                if (!in.conditional) break;          /* flow ends here */
            } else if (in.cls == A_BRANCH && !in.conditional) {
                break;                               /* tail call */
            }

            a += (uint32_t)n;
        }
    }

    /* Blocks were discovered out of order; emit in address order so the output
     * reads alongside a disassembly. */
    if (b->n > 1) qsort(b->v, b->n, sizeof(arm_insn), cmp_addr);

    /* Drop labels for blocks that were never collected.
     *
     * A label is recorded the moment a branch target is seen inside the
     * function's extent, but the block behind it is only reached if the walk
     * gets there — and the walk can stop short at any of three limits: the
     * visited table filling, the block stack filling, or the per-function
     * instruction cap. When that happens the label is referenced by a `goto`
     * and never placed, which is C that does not compile.
     *
     * Pruning here rather than raising the limits is the honest fix: the limits
     * exist for a reason, and a `goto` into a block we did not translate would
     * be a lie even if the compiler accepted it. Removing the label routes the
     * branch through the call-or-trap path below, which says what is actually
     * true — either the target is a function, or we cannot get there.
     *
     * This only showed up at 1,500 functions. At 100 nothing reached a limit,
     * so the output compiled and the bug was invisible. */
    uint32_t kept = 0;
    for (uint32_t i = 0; i < b->nlabels; i++) {
        int placed = 0;
        uint32_t lo = 0, hi = b->n;
        while (lo < hi) {                      /* body is sorted by address */
            uint32_t mid = lo + (hi - lo) / 2;
            if (b->v[mid].addr == b->labels[i]) { placed = 1; break; }
            if (b->v[mid].addr <  b->labels[i]) lo = mid + 1; else hi = mid;
        }
        if (placed) b->labels[kept++] = b->labels[i];
    }
    b->nlabels = kept;
}

/* --- operand rendering ------------------------------------------------------ */

/* A register operand, with r15 resolved.
 *
 * A recompiled program has no PC, so `pc` is not a variable and emitting it
 * produces C that does not compile — which is the good failure. The value is
 * known at translation time: reading PC in Thumb yields the instruction's own
 * address plus 4. The high-register ADD/MOV/CMP forms are where this actually
 * shows up, and they are common enough (position-independent address
 * computation) that leaving them to a trap would cost real coverage. */
static void reg_operand(char *dst, size_t cap, int r, const arm_insn *in) {
    if (r == 15) snprintf(dst, cap, "0x%08Xu", in->addr + 4);
    else         snprintf(dst, cap, "%s", reg_name(r));
}

/* A SOURCE register as a C expression, for the many places that want one
 * inline in an fprintf rather than in a buffer.
 *
 * The division this enforces is the one that kept getting lost: a DESTINATION
 * of r15 is a branch and is trapped by the guard in emit_insn, while a SOURCE
 * of r15 is a CONSTANT this pass knows — the instruction's own address plus
 * four, which is how the architecture defines reading the PC. Naming it "pc"
 * is never right in either case, and reg_name() is now left naming only
 * destinations.
 *
 * Rotating buffers because two sources can appear in one fprintf; the most any
 * call site uses is two. */
static const char *src_name(const arm_insn *in, int r) {
    static char buf[4][32];
    static unsigned next;
    if (r != 15) return reg_name(r);
    char *b = buf[next++ & 3];
    snprintf(b, sizeof(buf[0]), "0x%08Xu", in->addr + 4);
    return b;
}

/* The second operand of a data-processing instruction: an immediate, a plain
 * register, or a shifted register. */
static void operand2(char *dst, size_t cap, const arm_insn *in) {
    if (in->has_imm) {
        snprintf(dst, cap, "0x%X", in->imm);
        return;
    }
    if (in->rm == ARM_NO_REG) { snprintf(dst, cap, "/*?*/0"); return; }
    if (in->shift_amt == 0 && in->shift_type == SH_LSL) {
        reg_operand(dst, cap, in->rm, in);
        return;
    }
    if (in->rm == 15) { reg_operand(dst, cap, 15, in); return; }
    static const char *fn[4] = { "vita_lsl", "vita_lsr", "vita_asr", "vita_ror" };
    snprintf(dst, cap, "%s(%s, %u)", fn[in->shift_type & 3],
             src_name(in, in->rm), in->shift_amt);
}

/* The base-register update of a write-back form, emitted AFTER the access.
 * Pre-indexed ([rn, #imm]!) and post-indexed ([rn], #imm) differ only in
 * which address the access itself used; the register ends up the same. */
static void writeback(FILE *f, const arm_insn *in) {
    if (!in->writeback || !in->has_imm) return;
    if (in->rn == ARM_NO_REG || in->rn == 15) return;
    fprintf(f, "    %s = %s %c 0x%X;\n", reg_name(in->rn), reg_name(in->rn),
            in->mem_add ? '+' : '-', in->imm);
}

/* The address expression for a load or store. */
static int address(char *dst, size_t cap, const arm_insn *in) {
    if (in->rn == ARM_NO_REG) return 0;

    /* Post-indexed: the access uses the base as it stands. */
    if (in->mem_post && in->rn != 15) {
        snprintf(dst, cap, "%s", src_name(in, in->rn));
        return 1;
    }

    if (in->rn == 15) {
        /* PC-relative. A recompiled program has no PC, so it is resolved here:
         * for Thumb the base is the instruction address plus 4, aligned down to
         * a word. This is what turns a literal-pool load into a constant.
         *
         * Every caller must come through here rather than formatting `rn`
         * itself: `reg_name(15)` is "pc", and cpu.h deliberately has no such
         * variable, so a caller that builds its own address expression emits C
         * that does not compile. That is the good failure — the bad one is a
         * `pc` that exists and holds something plausible. */
        uint32_t pc = (in->addr + 4) & ~3u;
        snprintf(dst, cap, "0x%08X", in->mem_add ? pc + in->imm : pc - in->imm);
        return 1;
    }
    /* The U bit is not decoration: the T32 8-bit form encodes [rN, #-imm]
     * with the same fields as the positive one, so ignoring it reads from the
     * wrong side of the base register. */
    if (in->has_imm && in->imm)
        snprintf(dst, cap, "%s %c 0x%X", src_name(in, in->rn),
                 in->mem_add ? '+' : '-', in->imm);
    else if (in->rm != ARM_NO_REG && in->shift_amt)
        /* The T32 register-offset form scales the index: [rn, rm, lsl #n].
         * Dropping the shift read newlib's descriptor table one byte in,
         * and the pointer it produced looked like any other pointer. */
        snprintf(dst, cap, "%s + (%s << %u)", src_name(in, in->rn),
                 src_name(in, in->rm), in->shift_amt);
    else if (in->rm != ARM_NO_REG)
        snprintf(dst, cap, "%s + %s", src_name(in, in->rn), src_name(in, in->rm));
    else
        snprintf(dst, cap, "%s", src_name(in, in->rn));
    return 1;
}

/* --- one instruction --------------------------------------------------------- */

/* Mnemonics are static strings, so the pointer identifies the kind and the
 * table stays a short linear scan. */
static void note_trap(emit_stats *st, const char *what) {
    if (!what) what = "?";
    for (uint32_t i = 0; i < st->trap_kinds; i++) {
        if (st->traps[i].what == what) { st->traps[i].count++; return; }
    }
    if (st->trap_kinds < EM_MAX_TRAP_KINDS) {
        st->traps[st->trap_kinds].what  = what;
        st->traps[st->trap_kinds].count = 1;
        st->trap_kinds++;
        return;
    }
    st->traps_other++;
}

static void trap(FILE *f, const arm_insn *in, const char *why) {
    fprintf(f, "    vita_trap_unimpl(0x%08X, 0x%08X, \"%s\");\n",
            in->addr, in->raw, why);
}

/* Returns 1 if real C was emitted, 0 if a trap was. */
/* The C symbol for an imported firmware function.
 *
 * The resolved name is used when the database knows it, because the whole point
 * of binding imports is that the generated C says `vita_hle_sceGxmDraw()` where
 * it would otherwise say `vita_func_814BB75C()` and leave the reader to work
 * out that the callee is not ours at all. An unresolved NID still gets a stable
 * symbol so the output links either way. */
static void hle_symbol(char *dst, size_t cap, const nid_db *db,
                       const vm_stub *s) {
    const char *n = nid_func_name(db, s->lib_nid, s->func_nid);
    if (n) snprintf(dst, cap, "vita_hle_%s", n);
    else   snprintf(dst, cap, "vita_hle_nid_%08X", s->func_nid);
}

static int emit_insn(const vm_image *img, const vm_module *mod, const nid_db *db,
                     const funcset *fs, const fbody *b,
                     const arm_insn *in, FILE *f, emit_stats *st) {
    char o2[64], addr[64], rn_s[32];

    /* The first operand, with r15 resolved. Computed once because several
     * paths below need it and the ternary for "rn defaults to rd" is easy to
     * get inconsistently wrong when repeated. */
    reg_operand(rn_s, sizeof(rn_s),
                in->rn == ARM_NO_REG ? in->rd : in->rn, in);

    /* A call whose target is an import stub is a firmware call, and must be
     * bound before anything else looks at it. Otherwise it decodes as an
     * ordinary call to an ordinary address — the stub body is a placeholder the
     * loader overwrites at load time, so recompiling it would translate filler
     * and then "return" into whatever the filler happened to be. */
    if ((in->cls == A_CALL || in->cls == A_BRANCH) && in->has_target) {
        const vm_stub *s = vm_find_stub(mod, in->target);
        if (s) {
            char sym[96];
            hle_symbol(sym, sizeof(sym), db, s);
            /* Inside an IT block the call is conditional like anything else. */
            char guard[40] = "";
            if (in->cond != ARM_COND_AL)
                snprintf(guard, sizeof(guard), "if (vita_cond(%u)) ", in->cond);
            if (in->cls == A_CALL) fprintf(f, "    %s%s();\n", guard, sym);
            else                   fprintf(f, "    %s{ %s(); return; }\n", guard, sym);  /* tail call */
            st->import_calls++;
            return 1;
        }
    }

    /* A destination of r15 is a write to the PC, which is a BRANCH. The
     * decoder classifies the real ones as branches, so anything still carrying
     * r15 as a destination here is an encoding this emitter does not model —
     * and emitting it would print `pc`, which cpu.h deliberately does not
     * define.
     *
     * One guard rather than one per case, because one per case is what was
     * tried and it kept not holding: this is the FOURTH distinct path to reach
     * a build emitting a bare `pc` (MOV pc,rN; VLDR from a literal pool; STRD;
     * and MOVT/MOVW/ADDW). Each new translation had to remember the rule
     * independently, and eventually one did not. Checking it in the one place
     * every translation passes through is the only version that stays fixed.
     *
     * Source operands are not covered here and must not be: reg_operand()
     * resolves rm == 15 to the instruction's own address at translation time,
     * which is correct and is how PC-relative arithmetic works at all.
     *
     * Scoped in two ways, both learned by getting it wrong. `rt == 15` only
     * means the PC in a LOAD or STORE: in `VMRS APSR_nzcv, FPSCR` the same
     * field encodes "the condition flags", and a blanket rule trapped 3,827
     * working instructions. And an instruction the decoder already left as
     * OP_NONE is going to trap anyway with a more specific name than this one,
     * so it is left alone rather than relabelled. */
    int writes_pc = (in->rd == 15)
                 || ((in->cls == A_LOAD || in->cls == A_STORE)
                     && (in->rt == 15 || in->rt2 == 15));
    if (in->op != OP_NONE && writes_pc) {
        trap(f, in, "writes pc");
        note_trap(st, "writes pc");
        return 0;
    }

    /* An instruction made conditional by an IT block, or a conditional branch,
     * is wrapped rather than translated differently. Getting this wrong is
     * invisible until a condition happens to be false. */
    int guarded = 0;
    if (in->cls != A_BRANCH && in->cond != ARM_COND_AL && in->op != OP_IT) {
        fprintf(f, "    if (vita_cond(%u)) {\n", in->cond);
        guarded = 1;
    }

    int ok = 1;
    switch (in->op) {
        case OP_MOV:
            operand2(o2, sizeof(o2), in);
            fprintf(f, "    %s = %s;\n", reg_name(in->rd), o2);
            if (in->sets_flags) fprintf(f, "    vita_flags_nz(%s);\n", reg_name(in->rd));
            break;

        case OP_MOVT:
            /* Top half written, bottom half preserved. One line, and the
             * difference between a program that builds correct pointers and one
             * whose every constructed address is missing its high 16 bits. */
            fprintf(f, "    %s = (%s & 0x0000FFFFu) | 0x%08Xu;\n",
                    reg_name(in->rd), reg_name(in->rd), in->imm << 16);
            break;

        case OP_MVN:
            operand2(o2, sizeof(o2), in);
            fprintf(f, "    %s = ~(%s);\n", reg_name(in->rd), o2);
            if (in->sets_flags) fprintf(f, "    vita_flags_nz(%s);\n", reg_name(in->rd));
            break;

        case OP_ADD: case OP_SUB: {
            operand2(o2, sizeof(o2), in);
            const char *rn = rn_s;
            if (in->sets_flags) {
                /* The operands have to be captured before the destination is
                 * written, because rd and rn are frequently the same register
                 * and the flag computation needs the ORIGINAL values. */
                fprintf(f, "    { uint32_t _a = %s, _b = %s;\n", rn, o2);
                fprintf(f, "      %s = _a %c _b;\n", reg_name(in->rd),
                        in->op == OP_ADD ? '+' : '-');
                fprintf(f, "      vita_flags_%s(_a, _b, %s); }\n",
                        in->op == OP_ADD ? "add" : "sub", reg_name(in->rd));
            } else {
                fprintf(f, "    %s = %s %c %s;\n", reg_name(in->rd), rn,
                        in->op == OP_ADD ? '+' : '-', o2);
            }
            break;
        }

        case OP_ADC: case OP_SBC: {
            const char *rn = rn_s;
            operand2(o2, sizeof(o2), in);
            fprintf(f, "    { uint32_t _a = %s, _b = %s, _c = (uint32_t)flag_c;\n", rn, o2);
            if (in->op == OP_ADC) {
                fprintf(f, "      %s = _a + _b + _c;\n", reg_name(in->rd));
                if (in->sets_flags)
                    fprintf(f, "      vita_flags_adc(_a, _b, _c, %s);\n", reg_name(in->rd));
            } else {
                fprintf(f, "      %s = _a - _b - (1u - _c);\n", reg_name(in->rd));
                if (in->sets_flags)
                    fprintf(f, "      vita_flags_sub(_a, _b + (1u - _c), %s);\n", reg_name(in->rd));
            }
            fprintf(f, "    }\n");
            break;
        }

        case OP_RSB:
            /* rd = operand2 - rn. The 16-bit form is the negate, which the
             * decoder presents as operand2 == #0; the 32-bit form carries a
             * real operand2 (immediate or shifted register), and reading it as
             * the negate turned `rsb r7, r5, r5, lsl #4` into `0 - r5`. */
            operand2(o2, sizeof(o2), in);
            fprintf(f, "    { uint32_t _a = %s, _b = %s;\n", o2, src_name(in, in->rn));
            fprintf(f, "      %s = _a - _b;\n", reg_name(in->rd));
            if (in->sets_flags)
                fprintf(f, "      vita_flags_sub(_a, _b, %s);\n", reg_name(in->rd));
            fprintf(f, "    }\n");
            break;

        case OP_AND: case OP_ORR: case OP_EOR: case OP_BIC: {
            operand2(o2, sizeof(o2), in);
            const char *rn = rn_s;
            const char *ops = in->op == OP_AND ? "&" : in->op == OP_ORR ? "|" : "^";
            if (in->op == OP_BIC)
                fprintf(f, "    %s = %s & ~(%s);\n", reg_name(in->rd), rn, o2);
            else
                fprintf(f, "    %s = %s %s %s;\n", reg_name(in->rd), rn, ops, o2);
            if (in->sets_flags) fprintf(f, "    vita_flags_nz(%s);\n", reg_name(in->rd));
            break;
        }

        case OP_LSL: case OP_LSR: case OP_ASR: case OP_ROR: {
            static const char *fn2[4] = { "vita_lsl", "vita_lsr", "vita_asr", "vita_ror" };
            int which = in->op == OP_LSL ? 0 : in->op == OP_LSR ? 1
                      : in->op == OP_ASR ? 2 : 3;
            const char *src = src_name(in, in->rm == ARM_NO_REG ? in->rd : in->rm);
            if (in->has_imm)
                fprintf(f, "    %s = %s(%s, %u);\n", reg_name(in->rd),
                        fn2[which], src, in->imm);
            else
                /* Shift by register: the 16-bit form shifts rd by rm; the
                 * 32-bit form names a separate rn. Reading rd for both turned
                 * `asr.w r3, r9, ip` into a shift of r3, and a font's bit
                 * test into noise. */
                fprintf(f, "    %s = %s(%s, %s & 0xFF);\n", reg_name(in->rd),
                        fn2[which], reg_name(in->rn != ARM_NO_REG ? in->rn : in->rd),
                        src_name(in, in->rm));
            if (in->sets_flags) fprintf(f, "    vita_flags_nz(%s);\n", reg_name(in->rd));
            break;
        }

        case OP_MUL:
            /* The 16-bit form is rd = rd * rm; the 32-bit form names rn. */
            fprintf(f, "    %s = %s * %s;\n", reg_name(in->rd),
                    reg_name(in->rn != ARM_NO_REG ? in->rn : in->rd), src_name(in, in->rm));
            if (in->sets_flags) fprintf(f, "    vita_flags_nz(%s);\n", reg_name(in->rd));
            break;

        case OP_MLA:
            fprintf(f, "    %s = %s + %s * %s;\n", reg_name(in->rd), reg_name(in->ra),
                    reg_name(in->rn), reg_name(in->rm));
            break;

        case OP_MLS:
            fprintf(f, "    %s = %s - %s * %s;\n", reg_name(in->rd), reg_name(in->ra),
                    reg_name(in->rn), reg_name(in->rm));
            break;

        /* The 64-bit product goes into a temporary first: RdLo may be one of
         * the operands, and the accumulating forms read both halves before
         * either is written. */
        case OP_UMULL:
            fprintf(f, "    { uint64_t _p = (uint64_t)%s * %s;\n", reg_name(in->rn), reg_name(in->rm));
            fprintf(f, "      %s = (uint32_t)_p; %s = (uint32_t)(_p >> 32); }\n",
                    reg_name(in->rd), reg_name(in->rdhi));
            break;

        case OP_SMULL:
            fprintf(f, "    { int64_t _p = (int64_t)(int32_t)%s * (int32_t)%s;\n", reg_name(in->rn), reg_name(in->rm));
            fprintf(f, "      %s = (uint32_t)_p; %s = (uint32_t)((uint64_t)_p >> 32); }\n",
                    reg_name(in->rd), reg_name(in->rdhi));
            break;

        case OP_UMLAL:
            fprintf(f, "    { uint64_t _p = (((uint64_t)%s << 32) | %s) + (uint64_t)%s * %s;\n",
                    reg_name(in->rdhi), reg_name(in->rd), reg_name(in->rn), reg_name(in->rm));
            fprintf(f, "      %s = (uint32_t)_p; %s = (uint32_t)(_p >> 32); }\n",
                    reg_name(in->rd), reg_name(in->rdhi));
            break;

        case OP_SMLAL:
            fprintf(f, "    { int64_t _p = (int64_t)(((uint64_t)%s << 32) | %s) + (int64_t)(int32_t)%s * (int32_t)%s;\n",
                    reg_name(in->rdhi), reg_name(in->rd), reg_name(in->rn), reg_name(in->rm));
            fprintf(f, "      %s = (uint32_t)_p; %s = (uint32_t)((uint64_t)_p >> 32); }\n",
                    reg_name(in->rd), reg_name(in->rdhi));
            break;

        case OP_REV16: case OP_REVSH: case OP_RBIT: case OP_CLZ: {
            const char *fn1 = in->op == OP_REV16 ? "vita_rev16" : in->op == OP_REVSH ? "vita_revsh"
                            : in->op == OP_RBIT  ? "vita_rbit"  : "vita_clz";
            fprintf(f, "    %s = %s(%s);\n", reg_name(in->rd), fn1, reg_name(in->rm));
            break;
        }

        case OP_UADD8: case OP_USUB8: case OP_SEL: {
            const char *fn1 = in->op == OP_UADD8 ? "vita_uadd8" : in->op == OP_USUB8 ? "vita_usub8" : "vita_sel";
            fprintf(f, "    %s = %s(%s, %s);\n", reg_name(in->rd), fn1, reg_name(in->rn), reg_name(in->rm));
            break;
        }

        /* --- Advanced SIMD, lane-wise on D registers ----------------------
         *
         * A Q form is its two D halves one after the other; no lane crosses
         * the middle in any of these. */
        case OP_NEON_MOV_IMM: case OP_NEON_ORR_IMM: case OP_NEON_BIC_IMM:
            for (int q = 0; q <= in->neon_q; q++) {
                int d = in->vd + q;
                if (in->op == OP_NEON_MOV_IMM)
                    fprintf(f, "    vita_d_set(%d, 0x%016llXull);\n", d, (unsigned long long)in->imm64);
                else if (in->op == OP_NEON_ORR_IMM)
                    fprintf(f, "    vita_d_set(%d, vita_d_get(%d) | 0x%016llXull);\n", d, d, (unsigned long long)in->imm64);
                else
                    fprintf(f, "    vita_d_set(%d, vita_d_get(%d) & ~0x%016llXull);\n", d, d, (unsigned long long)in->imm64);
            }
            break;

        case OP_NEON_AND: case OP_NEON_BIC: case OP_NEON_ORR: case OP_NEON_ORN:
        case OP_NEON_EOR: case OP_NEON_BSL: case OP_NEON_BIT: case OP_NEON_BIF:
            for (int q = 0; q <= in->neon_q; q++) {
                int d = in->vd + q, n = in->vn + q, m = in->vm + q;
                switch (in->op) {
                case OP_NEON_AND: fprintf(f, "    vita_d_set(%d, vita_d_get(%d) & vita_d_get(%d));\n", d, n, m); break;
                case OP_NEON_BIC: fprintf(f, "    vita_d_set(%d, vita_d_get(%d) & ~vita_d_get(%d));\n", d, n, m); break;
                case OP_NEON_ORR: fprintf(f, "    vita_d_set(%d, vita_d_get(%d) | vita_d_get(%d));\n", d, n, m); break;
                case OP_NEON_ORN: fprintf(f, "    vita_d_set(%d, vita_d_get(%d) | ~vita_d_get(%d));\n", d, n, m); break;
                case OP_NEON_EOR: fprintf(f, "    vita_d_set(%d, vita_d_get(%d) ^ vita_d_get(%d));\n", d, n, m); break;
                case OP_NEON_BSL: fprintf(f, "    vita_d_set(%d, vita_neon_bsl(vita_d_get(%d), vita_d_get(%d), vita_d_get(%d)));\n", d, d, n, m); break;
                case OP_NEON_BIT: fprintf(f, "    vita_d_set(%d, (vita_d_get(%d) & ~vita_d_get(%d)) | (vita_d_get(%d) & vita_d_get(%d)));\n", d, d, m, n, m); break;
                default:          fprintf(f, "    vita_d_set(%d, (vita_d_get(%d) & vita_d_get(%d)) | (vita_d_get(%d) & ~vita_d_get(%d)));\n", d, d, m, n, m); break;
                }
            }
            break;

        case OP_NEON_ADD: case OP_NEON_SUB: case OP_NEON_CEQ: case OP_NEON_TST: case OP_NEON_PADD: {
            const char *fn1 = in->op == OP_NEON_ADD ? "vita_neon_add" : in->op == OP_NEON_SUB ? "vita_neon_sub"
                            : in->op == OP_NEON_CEQ ? "vita_neon_ceq" : in->op == OP_NEON_TST ? "vita_neon_tst"
                            : "vita_neon_padd";
            for (int q = 0; q <= in->neon_q; q++)
                fprintf(f, "    vita_d_set(%d, %s(vita_d_get(%d), vita_d_get(%d), %u));\n",
                        in->vd + q, fn1, in->vn + q, in->vm + q, in->neon_size);
            break;
        }

        case OP_NEON_DUP:
            for (int q = 0; q <= in->neon_q; q++)
                fprintf(f, "    vita_d_set(%d, vita_neon_dup(%s, %u));\n", in->vd + q, reg_name(in->rt), in->neon_size);
            break;

        case OP_NEON_SET32:
            fprintf(f, "    vita_d_set(%d, vita_neon_set32(vita_d_get(%d), %u, %s));\n",
                    in->vd, in->vd, in->imm, reg_name(in->rt));
            break;

        case OP_NEON_GET32:
            fprintf(f, "    %s = vita_neon_get32(vita_d_get(%d), %u);\n", reg_name(in->rt), in->vn, in->imm);
            break;

        case OP_NEON_LD1: case OP_NEON_ST1: {
            if (in->rn == 15) { ok = 0; trap(f, in, "vld1/vst1 pc base"); break; }
            const char *rn = reg_name(in->rn);
            for (uint32_t k = 0; k < in->imm; k++) {
                int d = in->vd + (int)k;
                if (in->op == OP_NEON_LD1)
                    fprintf(f, "    vita_d_set(%d, (uint64_t)vita_read32(%s + %u) | ((uint64_t)vita_read32(%s + %u) << 32));\n",
                            d, rn, k * 8, rn, k * 8 + 4);
                else {
                    fprintf(f, "    vita_write32(%s + %u, (uint32_t)vita_d_get(%d));\n", rn, k * 8, d);
                    fprintf(f, "    vita_write32(%s + %u, (uint32_t)(vita_d_get(%d) >> 32));\n", rn, k * 8 + 4, d);
                }
            }
            /* Rm = 13: advance by the bytes moved; another register: by it. */
            if (in->rm == 13)      fprintf(f, "    %s += %u;\n", rn, in->imm * 8);
            else if (in->rm != 15) fprintf(f, "    %s += %s;\n", rn, reg_name(in->rm));
            break;
        }

        case OP_NEON_DUP_LANE:
            /* The source lane is one D register whichever form this is; only
             * the destination doubles up. */
            for (int q = 0; q <= in->neon_q; q++)
                fprintf(f, "    vita_d_set(%d, vita_neon_dup((uint32_t)vita_neon_lane(vita_d_get(%d), %u, %u), %u));\n",
                        in->vd + q, in->vm, in->imm, in->neon_size, in->neon_size);
            break;

        case OP_NEON_EXT:
            if (in->neon_q) fprintf(f, "    vita_neon_ext_q(%d, %d, %d, %u);\n", in->vd, in->vn, in->vm, in->imm);
            else            fprintf(f, "    vita_d_set(%d, vita_neon_ext(vita_d_get(%d), vita_d_get(%d), %u));\n",
                                    in->vd, in->vn, in->vm, in->imm);
            break;

        case OP_NEON_SWP:
            for (int q = 0; q <= in->neon_q; q++)
                fprintf(f, "    { uint64_t _t = vita_d_get(%d); vita_d_set(%d, vita_d_get(%d)); vita_d_set(%d, _t); }\n",
                        in->vd + q, in->vd + q, in->vm + q, in->vm + q);
            break;

        case OP_NEON_MVN: case OP_NEON_CLZ: case OP_NEON_CNT: case OP_NEON_REV:
        case OP_NEON_NEG: case OP_NEON_ABS: case OP_NEON_CMP0:
        case OP_NEON_FNEG: case OP_NEON_FABS: case OP_NEON_FRECPE: case OP_NEON_FRSQRTE:
        case OP_NEON_CVT:
            for (int q = 0; q <= in->neon_q; q++) {
                int d = in->vd + q, m = in->vm + q;
                switch (in->op) {
                case OP_NEON_MVN:  fprintf(f, "    vita_d_set(%d, ~vita_d_get(%d));\n", d, m); break;
                case OP_NEON_CLZ:  fprintf(f, "    vita_d_set(%d, vita_neon_clz(vita_d_get(%d), %u));\n", d, m, in->neon_size); break;
                case OP_NEON_CNT:  fprintf(f, "    vita_d_set(%d, vita_neon_cnt(vita_d_get(%d)));\n", d, m); break;
                case OP_NEON_REV:  fprintf(f, "    vita_d_set(%d, vita_neon_rev(vita_d_get(%d), %u, %u));\n", d, m, in->neon_size, in->imm); break;
                case OP_NEON_NEG:  fprintf(f, "    vita_d_set(%d, vita_neon_sub(0, vita_d_get(%d), %u));\n", d, m, in->neon_size); break;
                case OP_NEON_ABS:  fprintf(f, "    vita_d_set(%d, vita_neon_abs(vita_d_get(%d), %u));\n", d, m, in->neon_size); break;
                case OP_NEON_CMP0:
                    if (in->neon_f) fprintf(f, "    vita_d_set(%d, vita_neon_fcmp0(vita_d_get(%d), %u));\n", d, m, in->imm);
                    else            fprintf(f, "    vita_d_set(%d, vita_neon_cmp0(vita_d_get(%d), %u, %u));\n", d, m, in->neon_size, in->imm);
                    break;
                case OP_NEON_FNEG:    fprintf(f, "    vita_d_set(%d, vita_d_get(%d) ^ 0x8000000080000000ull);\n", d, m); break;
                case OP_NEON_FABS:    fprintf(f, "    vita_d_set(%d, vita_d_get(%d) & 0x7FFFFFFF7FFFFFFFull);\n", d, m); break;
                case OP_NEON_FRECPE:  fprintf(f, "    vita_d_set(%d, vita_neon_frecpe(vita_d_get(%d)));\n", d, m); break;
                case OP_NEON_FRSQRTE: fprintf(f, "    vita_d_set(%d, vita_neon_frsqrte(vita_d_get(%d)));\n", d, m); break;
                default:              fprintf(f, "    vita_d_set(%d, vita_neon_cvt(vita_d_get(%d), %u));\n", d, m, in->imm); break;
                }
            }
            break;

        case OP_NEON_MUL: case OP_NEON_MLA: case OP_NEON_MLS:
            for (int q = 0; q <= in->neon_q; q++) {
                int d = in->vd + q, n = in->vn + q, m = in->vm + q;
                if (in->op == OP_NEON_MUL)
                    fprintf(f, "    vita_d_set(%d, vita_neon_mul(vita_d_get(%d), vita_d_get(%d), %u));\n", d, n, m, in->neon_size);
                else
                    fprintf(f, "    vita_d_set(%d, vita_neon_%s(vita_d_get(%d), vita_d_get(%d), vita_d_get(%d), %u));\n",
                            d, in->op == OP_NEON_MLA ? "mla" : "mls", d, n, m, in->neon_size);
            }
            break;

        case OP_NEON_MAX: case OP_NEON_MIN: case OP_NEON_CGT: case OP_NEON_CGE: {
            const char *fn3 = in->op == OP_NEON_MAX ? "vita_neon_max" : in->op == OP_NEON_MIN ? "vita_neon_min"
                            : in->op == OP_NEON_CGT ? "vita_neon_cgt" : "vita_neon_cge";
            for (int q = 0; q <= in->neon_q; q++)
                fprintf(f, "    vita_d_set(%d, %s(vita_d_get(%d), vita_d_get(%d), %u, %d));\n",
                        in->vd + q, fn3, in->vn + q, in->vm + q, in->neon_size, in->vfp_unsigned ? 1 : 0);
            break;
        }

        /* f32 lanes. A Q form is still two independent halves: none of these
         * crosses the middle, and VPADD has no Q form at all. */
        case OP_NEON_FADD: case OP_NEON_FSUB: case OP_NEON_FMUL: case OP_NEON_FPADD: case OP_NEON_FABD:
        case OP_NEON_FMAX: case OP_NEON_FMIN: case OP_NEON_FCEQ: case OP_NEON_FCGE: case OP_NEON_FCGT:
        case OP_NEON_FRECPS: case OP_NEON_FRSQRTS: {
            const char *fn2 = in->op == OP_NEON_FADD ? "fadd" : in->op == OP_NEON_FSUB ? "fsub"
                            : in->op == OP_NEON_FMUL ? "fmul" : in->op == OP_NEON_FPADD ? "fpadd"
                            : in->op == OP_NEON_FABD ? "fabd" : in->op == OP_NEON_FMAX ? "fmax"
                            : in->op == OP_NEON_FMIN ? "fmin" : in->op == OP_NEON_FCEQ ? "fceq"
                            : in->op == OP_NEON_FCGE ? "fcge" : in->op == OP_NEON_FCGT ? "fcgt"
                            : in->op == OP_NEON_FRECPS ? "frecps" : "frsqrts";
            for (int q = 0; q <= in->neon_q; q++)
                fprintf(f, "    vita_d_set(%d, vita_neon_%s(vita_d_get(%d), vita_d_get(%d)));\n",
                        in->vd + q, fn2, in->vn + q, in->vm + q);
            break;
        }

        case OP_NEON_FMLA: case OP_NEON_FMLS:
            for (int q = 0; q <= in->neon_q; q++)
                fprintf(f, "    vita_d_set(%d, vita_neon_%s(vita_d_get(%d), vita_d_get(%d), vita_d_get(%d)));\n",
                        in->vd + q, in->op == OP_NEON_FMLA ? "fmla" : "fmls", in->vd + q, in->vn + q, in->vm + q);
            break;

        case OP_NEON_SHR: case OP_NEON_SRA: case OP_NEON_RSHR: case OP_NEON_RSRA:
        case OP_NEON_SHL: case OP_NEON_SLI: case OP_NEON_SRI:
            for (int q = 0; q <= in->neon_q; q++) {
                int d = in->vd + q, m = in->vm + q;
                unsigned u = in->vfp_unsigned ? 1u : 0u;
                switch (in->op) {
                case OP_NEON_SHR:  fprintf(f, "    vita_d_set(%d, vita_neon_shr(vita_d_get(%d), %u, %u, %u));\n", d, m, in->neon_size, in->imm, u); break;
                case OP_NEON_RSHR: fprintf(f, "    vita_d_set(%d, vita_neon_rshr(vita_d_get(%d), %u, %u, %u));\n", d, m, in->neon_size, in->imm, u); break;
                case OP_NEON_SRA:  fprintf(f, "    vita_d_set(%d, vita_neon_add(vita_d_get(%d), vita_neon_shr(vita_d_get(%d), %u, %u, %u), %u));\n", d, d, m, in->neon_size, in->imm, u, in->neon_size); break;
                case OP_NEON_RSRA: fprintf(f, "    vita_d_set(%d, vita_neon_add(vita_d_get(%d), vita_neon_rshr(vita_d_get(%d), %u, %u, %u), %u));\n", d, d, m, in->neon_size, in->imm, u, in->neon_size); break;
                case OP_NEON_SHL:  fprintf(f, "    vita_d_set(%d, vita_neon_shl(vita_d_get(%d), %u, %u));\n", d, m, in->neon_size, in->imm); break;
                case OP_NEON_SLI:  fprintf(f, "    vita_d_set(%d, vita_neon_sli(vita_d_get(%d), vita_d_get(%d), %u, %u));\n", d, d, m, in->neon_size, in->imm); break;
                default:           fprintf(f, "    vita_d_set(%d, vita_neon_sri(vita_d_get(%d), vita_d_get(%d), %u, %u));\n", d, d, m, in->neon_size, in->imm); break;
                }
            }
            break;

        case OP_NEON_MOVL: case OP_NEON_SHLL:
            /* The source is read once: the Q destination may contain it. */
            fprintf(f, "    { uint64_t _m = vita_d_get(%d);\n", in->vm);
            fprintf(f, "      vita_d_set(%d, vita_neon_shl(vita_neon_widen((uint32_t)_m, %u, %u), %u, %u));\n",
                    in->vd, in->neon_size, in->vfp_unsigned ? 1u : 0u, in->neon_size * 2, in->imm);
            fprintf(f, "      vita_d_set(%d, vita_neon_shl(vita_neon_widen((uint32_t)(_m >> 32), %u, %u), %u, %u)); }\n",
                    in->vd + 1, in->neon_size, in->vfp_unsigned ? 1u : 0u, in->neon_size * 2, in->imm);
            break;

        case OP_NEON_MOVN: case OP_NEON_SHRN: case OP_NEON_RSHRN:
            fprintf(f, "    vita_d_set(%d, vita_neon_narrow(vita_d_get(%d), vita_d_get(%d), %u, %u, %d));\n",
                    in->vd, in->vm, in->vm + 1, in->neon_size, in->imm, in->op == OP_NEON_RSHRN ? 1 : 0);
            break;

        case OP_NEON_MUL_SCALAR: case OP_NEON_MLA_SCALAR: case OP_NEON_MLS_SCALAR:
            /* The lane is broadcast once, then it is the vector operation. */
            fprintf(f, "    { uint64_t _s = vita_neon_dup((uint32_t)vita_neon_lane(vita_d_get(%d), %u, %u), %u);\n",
                    in->vm, in->imm, in->neon_size, in->neon_size);
            for (int q = 0; q <= in->neon_q; q++) {
                int d = in->vd + q, n = in->vn + q;
                if (in->neon_f) {
                    if (in->op == OP_NEON_MUL_SCALAR)
                        fprintf(f, "      vita_d_set(%d, vita_neon_fmul(vita_d_get(%d), _s));\n", d, n);
                    else
                        fprintf(f, "      vita_d_set(%d, vita_neon_%s(vita_d_get(%d), vita_d_get(%d), _s));\n",
                                d, in->op == OP_NEON_MLA_SCALAR ? "fmla" : "fmls", d, n);
                } else {
                    if (in->op == OP_NEON_MUL_SCALAR)
                        fprintf(f, "      vita_d_set(%d, vita_neon_mul(vita_d_get(%d), _s, %u));\n", d, n, in->neon_size);
                    else
                        fprintf(f, "      vita_d_set(%d, vita_neon_%s(vita_d_get(%d), vita_d_get(%d), _s, %u));\n",
                                d, in->op == OP_NEON_MLA_SCALAR ? "mla" : "mls", d, n, in->neon_size);
                }
            }
            fprintf(f, "    }\n");
            break;

        case OP_MRC_TLS:
            if (in->rt == ARM_NO_REG || in->rt == 15) { ok = 0; break; }
            fprintf(f, "    %s = vita_tls_base();\n", reg_name(in->rt));
            break;

        case OP_VNMLA: case OP_VNMLS: {
            if (in->vd == ARM_NO_REG || in->vn == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            char c = (in->op == OP_VNMLS) ? '+' : '-';
            if (in->vfp_dp)
                fprintf(f, "    vfp_setd(%d, -vfp_getd(%d) %c (vfp_getd(%d) * vfp_getd(%d)));\n",
                        in->vd, in->vd, c, in->vn, in->vm);
            else
                fprintf(f, "    vfp_setf(%d, -vfp_getf(%d) %c (vfp_getf(%d) * vfp_getf(%d)));\n",
                        in->vd, in->vd, c, in->vn, in->vm);
            break;
        }

        case OP_LDREX: case OP_STREX: {
            if (in->rn == ARM_NO_REG || in->rn == 15 || in->rt == ARM_NO_REG || in->rt == 15) { ok = 0; break; }
            /* One guest thread at a time: nothing can touch the location
             * between the pair, so the store always succeeds. When threads
             * arrive they will be scheduled cooperatively on this same host
             * thread, and the property holds as long as no switch happens
             * between an LDREX and its STREX, which no switch point does. */
            if (in->op == OP_LDREX) {
                fprintf(f, "    %s = vita_read%u(%s + %u);\n",
                        reg_name(in->rt), in->mem_size, reg_name(in->rn), in->imm);
            } else {
                if (in->rd == ARM_NO_REG || in->rd == 15) { ok = 0; break; }
                fprintf(f, "    vita_write%u(%s + %u, (uint%u_t)%s);\n",
                        in->mem_size, reg_name(in->rn), in->imm, in->mem_size, reg_name(in->rt));
                fprintf(f, "    %s = 0;\n", reg_name(in->rd));
            }
            break;
        }

        case OP_VMOV_IMM:
            if (in->vd == ARM_NO_REG) { ok = 0; break; }
            if (in->vfp_dp) fprintf(f, "    vita_d_set(%d, 0x%016llXull);\n", in->vd, (unsigned long long)in->imm64);
            else            fprintf(f, "    vfp_setu(%d, 0x%08Xu);\n", in->vd, in->imm);
            break;

        case OP_VMOV_D_TO_C: case OP_VMOV_C_TO_D: {
            if (in->vm == ARM_NO_REG || in->rt == ARM_NO_REG || in->rt2 == ARM_NO_REG) { ok = 0; break; }
            if (in->rt == 15 || in->rt2 == 15) { ok = 0; break; }
            const char *lo = reg_name(in->rt), *hi = reg_name(in->rt2);
            if (in->op == OP_VMOV_D_TO_C) {
                if (in->vfp_dp)
                    fprintf(f, "    %s = (uint32_t)vita_d_get(%d); %s = (uint32_t)(vita_d_get(%d) >> 32);\n", lo, in->vm, hi, in->vm);
                else
                    fprintf(f, "    %s = vfp_getu(%d); %s = vfp_getu(%d);\n", lo, in->vm, hi, in->vm + 1);
            } else {
                if (in->vfp_dp)
                    fprintf(f, "    vita_d_set(%d, ((uint64_t)%s << 32) | %s);\n", in->vm, hi, lo);
                else
                    fprintf(f, "    vfp_setu(%d, %s); vfp_setu(%d, %s);\n", in->vm, lo, in->vm + 1, hi);
            }
            break;
        }

        case OP_VCVT_FF:
            if (in->vd == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            if (in->vfp_dp) fprintf(f, "    vfp_setf(%d, (float)vfp_getd(%d));\n", in->vd, in->vm);
            else            fprintf(f, "    vfp_setd(%d, (double)vfp_getf(%d));\n", in->vd, in->vm);
            break;

        case OP_CMP: case OP_CMN:
            operand2(o2, sizeof(o2), in);
            fprintf(f, "    { uint32_t _a = %s, _b = %s;\n",
                    rn_s, o2);
            fprintf(f, "      vita_flags_%s(_a, _b, _a %c _b); }\n",
                    in->op == OP_CMP ? "sub" : "add",
                    in->op == OP_CMP ? '-' : '+');
            break;

        case OP_TST:
            operand2(o2, sizeof(o2), in);
            fprintf(f, "    vita_flags_nz(%s & %s);\n",
                    rn_s, o2);
            break;

        case OP_ADR:
            fprintf(f, "    %s = 0x%08X;\n", reg_name(in->rd),
                    ((in->addr + 4) & ~3u) + in->imm);
            break;

        case OP_LDR: case OP_LDRB: case OP_LDRH: case OP_LDRSB: case OP_LDRSH: {
            if (!address(addr, sizeof(addr), in)) { ok = 0; trap(f, in, "load addr"); break; }
            /* A literal-pool load has a fixed address, so the value is known at
             * translation time and becomes a constant rather than a memory
             * access. This is also what stops the pool from being mistaken for
             * code later. */
            if (in->rn == 15) {
                uint32_t pc = (in->addr + 4) & ~3u;
                const uint8_t *p = vm_va(img, pc + in->imm, 4);
                if (p) {
                    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                               | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
                    fprintf(f, "    %s = 0x%08X;  /* literal */\n", reg_name(in->rt), v);
                    st->literals++;
                    break;
                }
            }
            const char *rd = in->op == OP_LDR   ? "vita_read32(%s)"
                           : in->op == OP_LDRB  ? "vita_read8(%s)"
                           : in->op == OP_LDRH  ? "vita_read16(%s)"
                           : in->op == OP_LDRSB ? "vita_sxtb(vita_read8(%s))"
                                                : "vita_sxth(vita_read16(%s))";
            fprintf(f, "    %s = ", reg_name(in->rt));
            fprintf(f, rd, addr);
            fprintf(f, ";\n");
            writeback(f, in);
            break;
        }

        case OP_LDRD: case OP_STRD: {
            if (in->rn == ARM_NO_REG || in->rt2 == ARM_NO_REG) { ok = 0; break; }
            /* rt/rt2 are named as variables, so neither may be the PC — there
             * is no `pc` in cpu.h to load into or store from, deliberately. */
            if (in->rt == 15 || in->rt2 == 15) { ok = 0; trap(f, in, "ldrd/strd pc"); break; }
            /* Two adjacent words. The offset is added or subtracted per the U
             * bit, and the second register is always four bytes above the
             * first regardless of direction.
             *
             * Through the shared address(), which is the only thing that knows
             * how to resolve a PC-relative base. Building the string here is
             * what put a bare `pc` in the output — the same mistake VLDR made,
             * in the third place it could be made. */
            char base[64];
            if (!address(base, sizeof(base), in)) { ok = 0; trap(f, in, "ldrd/strd addr"); break; }

            if (in->op == OP_LDRD) {
                fprintf(f, "    %s = vita_read32(%s);\n", reg_name(in->rt), base);
                fprintf(f, "    %s = vita_read32((%s) + 4);\n", reg_name(in->rt2), base);
            } else {
                fprintf(f, "    vita_write32(%s, %s);\n", base, reg_name(in->rt));
                fprintf(f, "    vita_write32((%s) + 4, %s);\n", base, reg_name(in->rt2));
            }
            writeback(f, in);
            break;
        }

        case OP_STR: case OP_STRB: case OP_STRH: {
            if (!address(addr, sizeof(addr), in)) { ok = 0; trap(f, in, "store addr"); break; }
            const char *w = in->op == OP_STR ? "vita_write32"
                          : in->op == OP_STRB ? "vita_write8" : "vita_write16";
            fprintf(f, "    %s(%s, %s);\n", w, addr, reg_name(in->rt));
            writeback(f, in);
            break;
        }

        case OP_PUSH: {
            /* ARM pushes in descending register order to descending addresses,
             * so the lowest-numbered register ends up at the lowest address.
             * Emitting the stores in ascending order with a pre-decremented sp
             * gives the same layout without simulating the order. */
            int count = 0;
            for (int i = 0; i < 16; i++) if (in->reglist & (1u << i)) count++;
            fprintf(f, "    sp -= %d;\n", count * 4);
            int slot = 0;
            for (int i = 0; i < 16; i++) {
                if (!(in->reglist & (1u << i))) continue;
                /* A register list can name the PC. Its stored value is this
                 * instruction's own address plus four, which is known here —
                 * the same rule as any other PC source, and the reason this
                 * goes through reg_operand rather than reg_name. */
                { char rv[64]; reg_operand(rv, sizeof(rv), i, in);
                  fprintf(f, "    vita_write32(sp + %d, %s);\n", slot * 4, rv); }
                slot++;
            }
            break;
        }

        /* General LDM/STM: n registers, ascending, at base (increment-after)
         * or at base - 4n (decrement-before); write-back moves the base by
         * 4n either way. A load that includes the PC is a jump, left as a
         * trap so it is counted rather than mistranslated. */
        case OP_LDM: case OP_STM: {
            if (in->rn == ARM_NO_REG || in->rn == 15) { ok = 0; break; }
            if (in->op == OP_LDM && (in->reglist & 0x8000)) { ok = 0; trap(f, in, "ldm with pc"); break; }
            int count = 0;
            for (int i = 0; i < 16; i++) if (in->reglist & (1u << i)) count++;
            const char *rn = reg_name(in->rn);
            /* The base is read once, before any register in the list is
             * loaded over it, and the loads may include it. For the
             * decrement-before forms the block starts 4n below the base. */
            fprintf(f, "    { uint32_t _b = %s;\n", rn);
            if (!in->mem_add) fprintf(f, "      _b -= %d;\n", count * 4);
            int slot = 0;
            for (int i = 0; i < 16; i++) {
                if (!(in->reglist & (1u << i))) continue;
                if (in->op == OP_LDM)
                    fprintf(f, "      %s = vita_read32(_b + %d);\n", reg_name(i), slot * 4);
                else
                    fprintf(f, "      vita_write32(_b + %d, %s);\n", slot * 4, reg_name(i));
                slot++;
            }
            /* Write-back lands the base after the block (IA) or at its
             * start (DB), which is _b + 4n and _b respectively. */
            if (in->writeback)
                fprintf(f, "      %s = _b + %d; }\n", rn, in->mem_add ? count * 4 : 0);
            else
                fprintf(f, "    }\n");
            break;
        }

        case OP_POP: {
            int slot = 0;
            for (int i = 0; i < 16; i++) {
                if (!(in->reglist & (1u << i))) continue;
                if (i == 15) { slot++; continue; }   /* PC: this is the return */
                fprintf(f, "    %s = vita_read32(sp + %d);\n", reg_name(i), slot * 4);
                slot++;
            }
            fprintf(f, "    sp += %d;\n", slot * 4);
            if (in->reglist & 0x8000) fprintf(f, "    return;\n");
            break;
        }

        case OP_SXTB: case OP_SXTH: case OP_UXTB: case OP_UXTH: {
            const char *fn3 = in->op == OP_SXTB ? "vita_sxtb"
                            : in->op == OP_SXTH ? "vita_sxth"
                            : in->op == OP_UXTB ? "vita_uxtb" : "vita_uxth";
            fprintf(f, "    %s = %s(%s);\n", reg_name(in->rd), fn3, src_name(in, in->rm));
            break;
        }

        case OP_REV:
            fprintf(f, "    %s = vita_rev(%s);\n", reg_name(in->rd), src_name(in, in->rm));
            break;

        case OP_SBFX: case OP_UBFX:
            fprintf(f, "    %s = %s(%s, %u, %u);\n", reg_name(in->rd),
                    in->op == OP_SBFX ? "vita_sbfx" : "vita_ubfx",
                    src_name(in, in->rn), in->bf_lsb, in->bf_width);
            break;

        case OP_BFI:
            fprintf(f, "    %s = vita_bfi(%s, %s, %u, %u);\n", reg_name(in->rd),
                    reg_name(in->rd), src_name(in, in->rn), in->bf_lsb, in->bf_width);
            break;

        case OP_BFC:
            fprintf(f, "    %s = vita_bfi(%s, 0, %u, %u);\n", reg_name(in->rd),
                    reg_name(in->rd), in->bf_lsb, in->bf_width);
            break;

        case OP_CBZ: case OP_CBNZ:
            if (fb_has_label(b, in->target))
                fprintf(f, "    if (%s %s 0) goto L_%08X;\n", src_name(in, in->rn),
                        in->op == OP_CBZ ? "==" : "!=", in->target);
            else
                if (fs_has(fs, in->target)) fprintf(f, "    if (%s %s 0) { vita_func_%08X(); return; }\n", src_name(in, in->rn), in->op == OP_CBZ ? "==" : "!=", in->target); else { fprintf(f, "    vita_trap_indirect(0x%08X, 0x%08X);\n", in->addr, in->target); ok = 0; }
            break;

        case OP_B:
            if (fb_has_label(b, in->target)) {
                if (in->cond == ARM_COND_AL) fprintf(f, "    goto L_%08X;\n", in->target);
                else fprintf(f, "    if (vita_cond(%u)) goto L_%08X;\n",
                             in->cond, in->target);
            } else if (in->target < img->seg_vaddr ||
                       in->target >= img->seg_vaddr + img->seg_len) {
                /* Outside the segment entirely. This is not a branch the
                 * program makes — it is a linear sweep decoding data as code,
                 * which happens wherever a literal pool or a table sits inside
                 * .text. Naming it separately keeps it out of the count of
                 * things worth implementing. */
                fprintf(f, "    vita_trap_unimpl(0x%08X, 0x%08X, \"branch out of segment\");\n",
                        in->addr, in->raw);
                note_trap(st, "branch: target off-segment");
            } else {
                /* A branch out of this function's extent is a tail call. */
                if (in->cond == ARM_COND_AL)
                    if (fs_has(fs, in->target)) fprintf(f, "    vita_func_%08X(); return;\n", in->target); else { fprintf(f, "    vita_trap_indirect(0x%08X, 0x%08X);\n", in->addr, in->target); note_trap(st, "branch: target not a function"); }
                else
                    if (fs_has(fs, in->target)) fprintf(f, "    if (vita_cond(%u)) { vita_func_%08X(); return; }\n", in->cond, in->target); else { fprintf(f, "    vita_trap_indirect(0x%08X, 0x%08X);\n", in->addr, in->target); note_trap(st, "branch: target not a function"); }
            }
            break;

        case OP_TBB: case OP_TBH: {
            uint32_t tg[256];
            uint32_t nt = in->has_imm
                        ? arm_table_targets(img->data + img->seg_file, img->seg_vaddr, img->seg_len,
                                            in, in->imm, tg, 256)
                        : 0;
            if (!nt) { ok = 0; break; }                /* no count: a trap, below */
            fprintf(f, "    switch (%s) {\n", reg_name(in->rm));
            for (uint32_t i = 0; i < nt; i++) {
                if (fb_has_label(b, tg[i]))
                    fprintf(f, "    case %u: goto L_%08X;\n", i, tg[i]);
                else
                    fprintf(f, "    case %u: vita_trap_indirect(0x%08X, 0x%08X); break;\n", i, in->addr, tg[i]);
            }
            fprintf(f, "    default: vita_trap_indirect(0x%08X, %s); break;\n", in->addr, reg_name(in->rm));
            fprintf(f, "    }\n");
            break;
        }

        case OP_BX: case OP_BLX:
            /* `bx lr` is a return, and a return in the generated C is
             * `return`: the link register is not maintained across the C
             * calls that replaced BL, so dispatching on it would look up a
             * value nothing ever set. This was reached for every leaf
             * function's return before the check existed; the first
             * newlib-linked module found it in `_init`. */
            if (in->cls == A_RETURN) { fprintf(f, "    return;\n"); break; }
            /* An indirect transfer through a register. The destination is a
             * value, so it can only be resolved while running — this is the one
             * place a recompiler genuinely needs a lookup rather than a
             * translation. BX is a tail transfer and does not come back; BLX is
             * a call and does. */
            if (in->rm == ARM_NO_REG) { ok = 0; break; }
            /* Through reg_operand, not reg_name. `bx pc` is a real (if odd)
             * instruction, and as a SOURCE the PC is a constant this pass
             * knows — resolving it is correct where naming it would not
             * compile. */
            { char rm[64]; reg_operand(rm, sizeof(rm), in->rm, in);
              if (in->op == OP_BX) fprintf(f, "    vita_dispatch(0x%08X, %s); return;\n", in->addr, rm);
              else                 fprintf(f, "    vita_dispatch(0x%08X, %s);\n", in->addr, rm); }
            break;

        /* --- scalar VFP ---------------------------------------------------
         *
         * Emitted through accessors rather than as direct array indexing, so
         * the single/double aliasing lives in one place. `dp` selects between
         * the float and double forms of everything.
         */
        case OP_VLDR: case OP_VSTR: {
            if (in->vd == ARM_NO_REG) { ok = 0; break; }
            /* Through the shared address(), not a local snprintf. VFP constants
             * live in the same literal pools as integer ones, so `rn == 15` is
             * common here — and building the expression locally emitted a bare
             * `pc`, which does not compile. */
            char a[64];
            if (!address(a, sizeof(a), in)) { ok = 0; trap(f, in, "vldr addr"); break; }

            /* A VFP literal-pool load has a fixed address, so fold it to a
             * constant exactly as the integer path does — same reason, too:
             * it keeps the pool from being read back as code later. */
            if (in->rn == 15) {
                uint32_t pc = (in->addr + 4) & ~3u;
                uint32_t at = in->mem_add ? pc + in->imm : pc - in->imm;
                const uint8_t *p = vm_va(img, at, in->vfp_dp ? 8 : 4);
                if (p && in->op == OP_VLDR) {
                    for (int w = 0; w < (in->vfp_dp ? 2 : 1); w++) {
                        uint32_t v = (uint32_t)p[w*4] | ((uint32_t)p[w*4+1] << 8)
                                   | ((uint32_t)p[w*4+2] << 16) | ((uint32_t)p[w*4+3] << 24);
                        if (in->vfp_dp)
                            fprintf(f, "    vfp_s[%d].u = 0x%08Xu;  /* literal */\n",
                                    in->vd * 2 + w, v);
                        else
                            fprintf(f, "    vfp_setu(%d, 0x%08Xu);  /* literal */\n",
                                    in->vd, v);
                    }
                    st->literals++;
                    break;
                }
            }
            if (in->op == OP_VLDR) {
                if (in->vfp_dp) {
                    fprintf(f, "    vfp_s[%d].u = vita_read32(%s);\n", in->vd * 2, a);
                    fprintf(f, "    vfp_s[%d].u = vita_read32((%s) + 4);\n", in->vd * 2 + 1, a);
                } else {
                    fprintf(f, "    vfp_setu(%d, vita_read32(%s));\n", in->vd, a);
                }
            } else {
                if (in->vfp_dp) {
                    fprintf(f, "    vita_write32(%s, vfp_s[%d].u);\n", a, in->vd * 2);
                    fprintf(f, "    vita_write32((%s) + 4, vfp_s[%d].u);\n", a, in->vd * 2 + 1);
                } else {
                    fprintf(f, "    vita_write32(%s, vfp_getu(%d));\n", a, in->vd);
                }
            }
            break;
        }

        case OP_VADD: case OP_VSUB: case OP_VMUL: case OP_VDIV: {
            if (in->vd == ARM_NO_REG || in->vn == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            char c = in->op == OP_VADD ? '+' : in->op == OP_VSUB ? '-'
                   : in->op == OP_VMUL ? '*' : '/';
            if (in->vfp_dp)
                fprintf(f, "    vfp_setd(%d, vfp_getd(%d) %c vfp_getd(%d));\n",
                        in->vd, in->vn, c, in->vm);
            else
                fprintf(f, "    vfp_setf(%d, vfp_getf(%d) %c vfp_getf(%d));\n",
                        in->vd, in->vn, c, in->vm);
            break;
        }

        case OP_VLDM: case OP_VSTM: {
            if (in->rn == ARM_NO_REG || in->vd == ARM_NO_REG) { ok = 0; break; }
            /* Registers occupy ascending addresses regardless of direction. A
             * decrement-before form (VPUSH) moves the base down by the whole
             * block first, then writes upward from there — the same shape as
             * the integer PUSH, and for the same reason. */
            uint32_t n     = in->imm;
            uint32_t words = in->vfp_dp ? n * 2 : n;
            /* This base is written back, so it is an lvalue and not a
             * source: resolving r15 to a constant here would emit an
             * assignment to one. There is no such addressing mode. */
            if (in->rn == 15) { ok = 0; trap(f, in, "vldm/vstm pc base"); break; }
            const char *rn = reg_name(in->rn);

            if (!in->mem_add) fprintf(f, "    %s -= %u;\n", rn, words * 4);
            for (uint32_t k = 0; k < words; k++) {
                int reg = (in->vfp_dp ? in->vd * 2 : in->vd) + (int)k;
                if (in->op == OP_VLDM)
                    fprintf(f, "    vfp_s[%d].u = vita_read32(%s + %u);\n", reg & 63, rn, k * 4);
                else
                    fprintf(f, "    vita_write32(%s + %u, vfp_s[%d].u);\n", rn, k * 4, reg & 63);
            }
            if (in->mem_add && in->writeback)
                fprintf(f, "    %s += %u;\n", rn, words * 4);
            break;
        }

        case OP_VMLA: case OP_VMLS: {
            if (in->vd == ARM_NO_REG || in->vn == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            char c = (in->op == OP_VMLA) ? '+' : '-';
            if (in->vfp_dp)
                fprintf(f, "    vfp_setd(%d, vfp_getd(%d) %c (vfp_getd(%d) * vfp_getd(%d)));\n",
                        in->vd, in->vd, c, in->vn, in->vm);
            else
                fprintf(f, "    vfp_setf(%d, vfp_getf(%d) %c (vfp_getf(%d) * vfp_getf(%d)));\n",
                        in->vd, in->vd, c, in->vn, in->vm);
            break;
        }

        case OP_VNMUL:
            if (in->vd == ARM_NO_REG || in->vn == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            if (in->vfp_dp)
                fprintf(f, "    vfp_setd(%d, -(vfp_getd(%d) * vfp_getd(%d)));\n", in->vd, in->vn, in->vm);
            else
                fprintf(f, "    vfp_setf(%d, -(vfp_getf(%d) * vfp_getf(%d)));\n", in->vd, in->vn, in->vm);
            break;

        case OP_VMOV:
            if (in->vd == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            if (in->vfp_dp) fprintf(f, "    vfp_setd(%d, vfp_getd(%d));\n", in->vd, in->vm);
            else            fprintf(f, "    vfp_setu(%d, vfp_getu(%d));\n", in->vd, in->vm);
            break;

        case OP_VABS:
            if (in->vd == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            /* Clearing the sign bit rather than calling fabs: it is the
             * architecture's definition, and it preserves NaN payloads where
             * a library call need not. */
            if (in->vfp_dp) fprintf(f, "    vfp_setd(%d, vfp_getd(%d) < 0.0 ? -vfp_getd(%d) : vfp_getd(%d));\n",
                                    in->vd, in->vm, in->vm, in->vm);
            else            fprintf(f, "    vfp_setu(%d, vfp_getu(%d) & 0x7FFFFFFFu);\n", in->vd, in->vm);
            break;

        case OP_VNEG:
            if (in->vd == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            if (in->vfp_dp) fprintf(f, "    vfp_setd(%d, -vfp_getd(%d));\n", in->vd, in->vm);
            else            fprintf(f, "    vfp_setu(%d, vfp_getu(%d) ^ 0x80000000u);\n", in->vd, in->vm);
            break;

        case OP_VSQRT:
            if (in->vd == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            if (in->vfp_dp) fprintf(f, "    vfp_setd(%d, sqrt(vfp_getd(%d)));\n", in->vd, in->vm);
            else            fprintf(f, "    vfp_setf(%d, (float)sqrt((double)vfp_getf(%d)));\n", in->vd, in->vm);
            break;

        case OP_VCMP:
            if (in->vd == ARM_NO_REG) { ok = 0; break; }
            /* Writes FPSCR, not the integer flags. A later VMRS moves them. */
            if (in->vm == ARM_NO_REG) {
                if (in->vfp_dp) fprintf(f, "    vfp_cmp_f64(vfp_getd(%d), 0.0);\n", in->vd);
                else            fprintf(f, "    vfp_cmp_f32(vfp_getf(%d), 0.0f);\n", in->vd);
            } else {
                if (in->vfp_dp) fprintf(f, "    vfp_cmp_f64(vfp_getd(%d), vfp_getd(%d));\n", in->vd, in->vm);
                else            fprintf(f, "    vfp_cmp_f32(vfp_getf(%d), vfp_getf(%d));\n", in->vd, in->vm);
            }
            break;

        case OP_VMRS:
            fprintf(f, "    vfp_mrs_apsr();\n");
            break;

        case OP_VMOV_TO_V:
            if (in->rt == ARM_NO_REG || in->vn == ARM_NO_REG) { ok = 0; break; }
            fprintf(f, "    vfp_setu(%d, %s);\n", in->vn, reg_name(in->rt));
            break;

        case OP_VMOV_TO_C:
            if (in->rt == ARM_NO_REG || in->vn == ARM_NO_REG) { ok = 0; break; }
            fprintf(f, "    %s = vfp_getu(%d);\n", reg_name(in->rt), in->vn);
            break;

        case OP_VCVT_I2F:
            if (in->vd == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            if (in->vfp_dp)
                fprintf(f, "    vfp_setd(%d, (double)(%s)vfp_getu(%d));\n",
                        in->vd, in->vfp_unsigned ? "uint32_t" : "int32_t", in->vm);
            else
                fprintf(f, "    vfp_setf(%d, (float)(%s)vfp_getu(%d));\n",
                        in->vd, in->vfp_unsigned ? "uint32_t" : "int32_t", in->vm);
            break;

        case OP_VCVT_F2I:
            if (in->vd == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            /* Toward zero, which is what a C cast does — the only rounding
             * mode claimed by the decoder for exactly that reason. */
            /* The cast alone is not enough, though: out of range it is
             * undefined in C and 0x80000000 on x86, where the architecture
             * saturates, and NaN converts to zero. */
            fprintf(f, "    vfp_setu(%d, (uint32_t)vita_%s_to_%s(vfp_get%s(%d)));\n",
                    in->vd, in->vfp_dp ? "f64" : "f32", in->vfp_unsigned ? "u32" : "s32",
                    in->vfp_dp ? "d" : "f", in->vm);
            break;

        case OP_NOP:
            fprintf(f, "    /* nop */\n");
            break;

        case OP_IT:
            /* The IT instruction itself emits nothing; its effect is that the
             * following instructions carry a condition, which the decoder has
             * already attached to them. */
            fprintf(f, "    /* it %s */\n", arm_cond_name(in->cond));
            break;

        default:
            ok = 0;
            break;
    }

    /* Control-flow classes the operand decoder does not cover are handled by
     * class rather than by op, so a call is still a call even when its exact
     * encoding was not decoded. */
    if (!ok) {
        if (in->cls == A_CALL && in->has_target && fs_has(fs, in->target)) {
            fprintf(f, "    vita_func_%08X();\n", in->target);
            ok = 1;
        } else if (in->cls == A_CALL && in->has_target) {
            fprintf(f, "    vita_trap_indirect(0x%08X, 0x%08X);\n", in->addr, in->target);
        } else if (in->cls == A_RETURN) {
            fprintf(f, "    return;\n");
            ok = 1;
        } else if (in->cls == A_CALL || in->cls == A_INDIRECT) {
            fprintf(f, "    vita_trap_indirect(0x%08X, 0);\n", in->addr);
            note_trap(st, "indirect transfer");
        } else if (in->cls == A_SIMD) {
            trap(f, in, "simd");
            note_trap(st, "simd/vfp");
        } else {
            trap(f, in, in->mnemonic);
            note_trap(st, in->mnemonic);
        }
    }

    if (guarded) fprintf(f, "    }\n");
    return ok;
}

/* --- the whole module -------------------------------------------------------- */

/* --- which functions can run ------------------------------------------------
 *
 * Discovery registers every address anything branches to, which is what keeps
 * a `b` that leaves its function from trapping. But most such targets are
 * blocks their own function already carries, and each one registered is a
 * second copy of everything after it. The copies cost nothing at run time and
 * everything at compile time: on the first retail title, 13 M instructions
 * were emitted for 3.3 M distinct ones, 1.1 GB of C.
 *
 * So the emitter keeps only what can be entered: the functions an entry point,
 * an export or a stored pointer names (discovery's roots), and everything they
 * reach by a call or by a branch that leaves its caller's body. A function
 * reached only by a branch its caller inlines, or only from data the linear
 * sweep read as a call, is left out; nothing can arrive at it. The dispatch
 * table lists what is emitted, so a computed transfer to anything else
 * announces itself as a miss rather than running a copy.
 */

typedef struct { uint32_t addr, idx; } addr_idx;

static int ai_cmp(const void *a, const void *b) {
    uint32_t x = ((const addr_idx *)a)->addr, y = ((const addr_idx *)b)->addr;
    return x < y ? -1 : x > y ? 1 : 0;
}

static int ai_find(const addr_idx *v, uint32_t n, uint32_t addr) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (v[mid].addr == addr) return (int)v[mid].idx;
        if (v[mid].addr <  addr) lo = mid + 1; else hi = mid;
    }
    return -1;
}

/* The functions `fn`'s body transfers to as functions: its calls, and the
 * branches that are not labels of its own. Import stubs are firmware, not
 * functions of the module. Returns how many it wrote.
 *
 * `clean` is set when the whole body would translate without a trap: every
 * instruction claimed, every branch a label, a function or an import, nothing
 * off the segment. Data read as code never manages that for long. */
static uint32_t body_refs(const vm_image *img, const vm_module *mod, const vf_func *fn,
                          const addr_idx *ai, uint32_t nai, uint32_t **out, uint32_t *cap, uint32_t at,
                          int *clean) {
    fbody b;
    memset(&b, 0, sizeof(b));
    collect(img, fn, &b);
    uint32_t n = 0;
    int ok = b.n > 0;
    for (uint32_t k = 0; k < b.n; k++) {
        const arm_insn *in = &b.v[k];
        if (in->cls == A_UNKNOWN || in->cls == A_UNDEF) ok = 0;
        else if (in->op == OP_NONE && in->cls != A_CALL && in->cls != A_BRANCH &&
                 in->cls != A_RETURN && in->cls != A_INDIRECT && in->cls != A_NOP) ok = 0;
        else if (in->rd == 15) ok = 0;
        if (!in->has_target) continue;
        if (in->cls != A_CALL && in->cls != A_BRANCH) continue;
        if (in->cls == A_BRANCH && fb_has_label(&b, in->target)) continue;
        if (vm_find_stub(mod, in->target)) continue;
        int idx = ai_find(ai, nai, in->target);
        if (idx < 0) { ok = 0; continue; }
        if (at + n == *cap) {
            *cap = *cap ? *cap * 2 : 65536;
            *out = (uint32_t *)realloc(*out, *cap * sizeof(uint32_t));
        }
        (*out)[at + n++] = (uint32_t)idx;
    }
    free(b.v);
    free(b.labels);
    *clean = ok;
    return n;
}

/* Marks keep[i] for every function reachable from a root. A weak pointer
 * target (discovery's VF_R_WEAK_PTR: a stored pointer with no prologue at
 * its target and no table around it) is a root only if its body is clean.
 * That keeps a prologue-less leaf that only a pointer names, and leaves out
 * the record table whose odd offsets a relocation happened to name. */
static uint32_t reachable(const vm_image *img, const vm_module *mod, const vf_result *disc,
                          uint8_t *keep, uint32_t *promoted) {
    uint32_t n = disc->count;
    addr_idx *ai = (addr_idx *)malloc((n ? n : 1) * sizeof(addr_idx));
    uint32_t *ref_start = (uint32_t *)malloc((n + 1) * sizeof(uint32_t));
    uint8_t *clean = (uint8_t *)calloc(n ? n : 1, 1);
    uint32_t *refs = NULL, cap = 0, total = 0;
    *promoted = 0;
    if (!ai || !ref_start || !clean) { free(ai); free(ref_start); free(clean); return 0; }
    for (uint32_t i = 0; i < n; i++) { ai[i].addr = disc->funcs[i].addr; ai[i].idx = i; }
    qsort(ai, n, sizeof(addr_idx), ai_cmp);

    for (uint32_t i = 0; i < n; i++) {
        int c = 0;
        ref_start[i] = total;
        total += body_refs(img, mod, &disc->funcs[i], ai, n, &refs, &cap, total, &c);
        clean[i] = (uint8_t)c;
    }
    ref_start[n] = total;

    uint32_t *queue = (uint32_t *)malloc((n ? n : 1) * sizeof(uint32_t));
    uint32_t head = 0, tail = 0, kept = 0;
    if (queue) {
        for (uint32_t i = 0; i < n; i++) {
            int root = (disc->funcs[i].reasons & VF_R_ROOTS) != 0;
            if (!root && (disc->funcs[i].reasons & VF_R_WEAK_PTR) && clean[i]) { root = 1; (*promoted)++; }
            if (root) { keep[i] = 1; queue[tail++] = i; }
        }
        while (head < tail) {
            uint32_t i = queue[head++];
            kept++;
            for (uint32_t r = ref_start[i]; r < ref_start[i + 1]; r++) {
                uint32_t j = refs[r];
                if (!keep[j]) { keep[j] = 1; queue[tail++] = j; }
            }
        }
    }
    free(queue);
    free(refs);
    free(ref_start);
    free(clean);
    free(ai);
    return kept;
}

/* The output's base name: `outpath` without its directory and extension, for
 * the include line each part carries. */
static void base_name(char *dst, size_t cap, const char *outpath) {
    const char *s = outpath, *p;
    for (p = outpath; *p; p++) if (*p == '/' || *p == '\\') s = p + 1;
    snprintf(dst, cap, "%s", s);
    char *dot = strrchr(dst, '.');
    if (dot && dot != dst) *dot = '\0';
}

/* Everything that every part has to see: the runtime, the imports, and
 * every function. One header rather than a copy per part, so the
 * declaration set stays one thing. */
static int write_header(const char *outpath, const vm_module *mod, const nid_db *db,
                        const vf_result *disc, const uint32_t *list, uint32_t nlist) {
    char path[1200], base[256];
    base_name(base, sizeof(base), outpath);
    snprintf(path, sizeof(path), "%s", outpath);
    char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    if (dot && (!slash || dot > slash) && (!bslash || dot > bslash)) *dot = '\0';
    strncat(path, ".h", sizeof(path) - strlen(path) - 1);
    FILE *h = fopen(path, "w");
    if (!h) return 1;
    fprintf(h, "/* Generated by armrecomp. Do not edit.\n");
    fprintf(h, " *\n");
    fprintf(h, " * module: %s -- what every part of the output declares.\n", mod->info.name);
    fprintf(h, " */\n\n");
    fprintf(h, "#ifndef VITARECOMP_GENERATED_%s_H\n#define VITARECOMP_GENERATED_%s_H\n\n", base, base);
    fprintf(h, "#include \"vitarecomp/recomp_rt.h\"\n");
    fprintf(h, "#include \"vitarecomp/dispatch.h\"\n\n");
    if (mod->stub_count) {
        fprintf(h, "/* firmware imports */\n");
        for (uint32_t i = 0; i < mod->stub_count; i++) {
            char sym[96];
            hle_symbol(sym, sizeof(sym), db, &mod->stubs[i]);
            fprintf(h, "void %s(void);\n", sym);
        }
        fprintf(h, "\n");
    }
    fprintf(h, "/* functions: the call graph has cycles, so every one is declared before any body */\n");
    for (uint32_t i = 0; i < nlist; i++)
        fprintf(h, "void vita_func_%08X(void);\n", disc->funcs[list[i]].addr);
    fprintf(h, "\n#endif\n");
    fclose(h);
    return 0;
}

/* Opens part `n` of the output beside `outpath`. */
static FILE *open_part(const char *outpath, uint32_t n, const char *base) {
    char path[1200];
    snprintf(path, sizeof(path), "%s", outpath);
    char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    if (dot && (!slash || dot > slash) && (!bslash || dot > bslash)) *dot = '\0';
    char suffix[32];
    snprintf(suffix, sizeof(suffix), "_%03u.c", n);
    strncat(path, suffix, sizeof(path) - strlen(path) - 1);
    FILE *p = fopen(path, "w");
    if (!p) return NULL;
    fprintf(p, "/* Generated by armrecomp. Do not edit. Part %u of the functions. */\n\n", n);
    fprintf(p, "#include \"%s.h\"\n\n", base);
    return p;
}

int em_emit(const vm_image *img, const vm_module *mod, const nid_db *db,
            const vf_result *disc, FILE *f, const char *outpath, const char *impdir,
            uint32_t limit, emit_stats *st) {
    memset(st, 0, sizeof(*st));

    /* Functions per part. One translation unit per module was fine at a
     * homebrew's size and is not at a retail title's: a compiler works on
     * one file for as long as it takes, and 23,000 functions in one file is
     * an hour that twelve files are five minutes of. */
    uint32_t split = 2000;
    {
        const char *e = getenv("VITARECOMP_EMIT_SPLIT");
        if (e && *e) split = (uint32_t)strtoul(e, NULL, 10);
    }
    char base[256];
    base_name(base, sizeof(base), outpath);

    /* The functions to emit, in discovery order. */
    uint8_t *keep = (uint8_t *)calloc(disc->count ? disc->count : 1, 1);
    uint32_t *list = (uint32_t *)malloc((disc->count ? disc->count : 1) * sizeof(uint32_t));
    if (!keep || !list) { free(keep); free(list); return 1; }
    uint32_t promoted = 0;
    uint32_t kept = reachable(img, mod, disc, keep, &promoted);
    uint32_t nlist = 0;
    for (uint32_t i = 0; i < disc->count; i++) if (keep[i]) list[nlist++] = i;
    (void)kept;
    st->funcs_discovered = disc->count;
    st->funcs_dropped    = disc->count - nlist;
    st->funcs_promoted   = promoted;

    uint32_t n_funcs = nlist;
    if (limit && limit < n_funcs) n_funcs = limit;

    /* Every address that is a defined symbol in this output, sorted for lookup.
     * Under --limit the un-emitted ones still get trapping stubs, so they are
     * genuinely defined and belong in the set. */
    funcset fs;
    fs.n = nlist;
    fs.v = (uint32_t *)malloc((fs.n ? fs.n : 1) * sizeof(uint32_t));
    if (!fs.v) { free(keep); free(list); return 1; }
    for (uint32_t i = 0; i < nlist; i++) fs.v[i] = disc->funcs[list[i]].addr;
    qsort(fs.v, fs.n, sizeof(uint32_t), fs_cmp);

    fprintf(f, "/* Generated by armrecomp. Do not edit.\n");
    fprintf(f, " *\n");
    fprintf(f, " * module: %s\n", mod->info.name);
    fprintf(f, " * %u functions emitted of %u discovered; the rest are reachable\n", nlist, disc->count);
    fprintf(f, " * from no entry point, export or stored pointer\n");
    fprintf(f, " */\n\n");
    /* Every import the module could call and every function, declared in one
     * header the main file and every part include. Declaring the imports
     * unconditionally, rather than only those a limited emit reaches, keeps
     * the declaration set stable: adding functions to the emit does not
     * change which symbols exist. */
    if (write_header(outpath, mod, db, disc, list, nlist)) {
        free(fs.v); free(keep); free(list);
        return 1;
    }
    fprintf(f, "#include \"%s.h\"\n\n", base);

    /* A limited emit still references functions it did not emit, so those get
     * a stub that traps. Without this the output would not link, and a silently
     * missing function is exactly the failure this project refuses. */
    if (n_funcs < nlist) {
        fprintf(f, "/* Not emitted under --limit; present so the output links. */\n");
        for (uint32_t i = n_funcs; i < nlist; i++)
            fprintf(f, "void vita_func_%08X(void) { vita_trap_unimpl(0x%08X, 0, \"not emitted\"); }\n",
                    disc->funcs[list[i]].addr, disc->funcs[list[i]].addr);
        fprintf(f, "\n");
    }

    /* The bodies, into parts. The main file keeps everything else. */
    FILE *part = NULL;
    FILE *out  = f;
    st->parts = 0;
    for (uint32_t li = 0; li < n_funcs; li++) {
        const vf_func *fn = &disc->funcs[list[li]];
        if (split && li % split == 0) {
            if (part) fclose(part);
            part = open_part(outpath, st->parts, base);
            if (part) { st->parts++; out = part; }
            else      { out = f; }
            uint32_t here = n_funcs - li < split ? n_funcs - li : split;
            if (here > st->part_funcs) st->part_funcs = here;
        }
        FILE *pf = out;
        fbody b;
        memset(&b, 0, sizeof(b));
        collect(img, fn, &b);

        fprintf(pf, "/* ---------------------------------------------------------------\n");
        fprintf(pf, " * vita_func_%08X  --  %u instructions, %u bytes%s\n",
                fn->addr, b.n, fn->end - fn->addr,
                fn->from_heuristic == VF_SHAPE ? ", discovered by shape (heuristic)"
              : fn->from_heuristic == VF_RELOC ? ", discovered by relocation" : "");
        fprintf(pf, " * ------------------------------------------------------------- */\n");
        fprintf(pf, "void vita_func_%08X(void) {\n", fn->addr);
        /* The function-entry trace: a global test and a branch while off. */
        fprintf(pf, "    vita_trace_fn(0x%08Xu);\n", fn->addr);

        /* Per-instruction PC recording, only when asked for in the emitter's
         * environment: it is a store per instruction in the output. */
        static int emit_pc = -1;
        if (emit_pc < 0) {
            const char *e = getenv("VITARECOMP_EMIT_PC");
            emit_pc = (e && *e && *e != '0') ? 1 : 0;
        }

        for (uint32_t k = 0; k < b.n; k++) {
            const arm_insn *in = &b.v[k];
            if (fb_has_label(&b, in->addr))
                fprintf(pf, "L_%08X:\n", in->addr);
            if (emit_pc)
                fprintf(pf, "    vita_trace_pc(0x%08Xu);\n", in->addr);

            fprintf(pf, "    /* %08X  %-16s */\n", in->addr, in->mnemonic);
            if (emit_insn(img, mod, db, &fs, &b, in, pf, st)) st->translated++;
            else                               st->trapped++;
            st->insns++;
        }

        fprintf(pf, "}\n\n");
        st->funcs++;

        free(b.v);
        free(b.labels);
    }
    if (part) fclose(part);
    if (!split) st->part_funcs = n_funcs;

    /* --- the HLE defaults ----------------------------------------------------
     *
     * One default implementation per import, each of which traps by name. This
     * is what makes the HLE incrementally implementable: the generated C links
     * from the first build, and every unimplemented firmware call announces
     * exactly which function the game wanted rather than failing to link or,
     * worse, silently returning zero.
     *
     * One FILE per import, for the reason in emit.h: at one archive member per
     * function, writing a real implementation and relinking is the entire
     * override procedure. No list to keep in sync, and no generated code to
     * hand-edit.
     */
    if (impdir) {
        for (uint32_t i = 0; i < mod->stub_count; i++) {
            const vm_stub *s = &mod->stubs[i];
            char sym[96];
            hle_symbol(sym, sizeof(sym), db, s);
            const char *lib = nid_lib_name(db, s->lib_nid);
            const char *fn  = nid_func_name(db, s->lib_nid, s->func_nid);

            char path[1200];
            snprintf(path, sizeof(path), "%s/%s.c", impdir, sym);
            FILE *one = fopen(path, "w");
            if (!one) continue;

            fprintf(one, "/* Generated by armrecomp. Do not edit.\n");
            fprintf(one, " *\n");
            fprintf(one, " * %s::%s  --  stub 0x%08X in module '%s'.\n",
                    lib ? lib : "?", fn ? fn : "(unresolved)", s->addr,
                    mod->info.name);
            fprintf(one, " *\n");
            fprintf(one, " * The default: trap by name. To implement it, define this symbol\n");
            fprintf(one, " * in the runtime and relink -- this file is one member of a static\n");
            fprintf(one, " * library, so the linker stops extracting it. Nothing to delete.\n");
            fprintf(one, " */\n\n");
            fprintf(one, "#include \"vitarecomp/recomp_rt.h\"\n\n");
            /* The name goes into the trap, not just the comment: at run time
             * the NID alone is a hash, and the whole reason imports are bound
             * is so a gap says which firmware function the game wanted. */
            if (fn)
                fprintf(one,
                        "void %s(void) { vita_trap_import(0x%08X, 0x%08X, \"%s::%s\"); }\n",
                        sym, s->addr, s->func_nid, lib ? lib : "?", fn);
            else
                fprintf(one,
                        "void %s(void) { vita_trap_import(0x%08X, 0x%08X, 0); }\n",
                        sym, s->addr, s->func_nid);
            fclose(one);
            st->imports_used++;
        }
    }

    /* The dispatch table: every function AND every import stub, sorted by
     * guest address, so an indirect transfer can find either at run time.
     * Functions are emitted for ALL discovered addresses — including any not
     * emitted under --limit, which have trapping stubs — because a dispatch
     * that misses should report "we never translated that" rather than "that
     * address is not a function".
     *
     * The stubs belong in the same table because a transfer to one is a
     * firmware call. Imports bind at emit time on a direct BL, but a module
     * reaches firmware through pointers too — `module_start` makes its very
     * first firmware call that way — and those never pass through the BL path.
     * Without this the dispatcher reports the game's first C++ runtime call as
     * an unresolved address, which names the wrong problem: the code is not
     * missing, it is firmware.
     *
     * One merged table rather than a second lookup: the entries have the same
     * shape already (guest address -> void(void)), both lists arrive sorted,
     * and merging keeps vita_dispatch a single binary search.
     */
    fprintf(f, "/* ---------------------------------------------------------------\n");
    fprintf(f, " * dispatch table -- %u functions + %u import stubs,\n",
            disc->count, mod->stub_count);
    fprintf(f, " * sorted by guest address\n");
    fprintf(f, " * ------------------------------------------------------------- */\n");
    fprintf(f, "static const vita_dispatch_entry vita_functions[] = {\n");
    {
        uint32_t fi = 0, si = 0;
        while (fi < fs.n || si < mod->stub_count) {
            int take_stub;
            if (fi >= fs.n)                            take_stub = 1;
            else if (si >= mod->stub_count)            take_stub = 0;
            else if (mod->stubs[si].addr < fs.v[fi])   take_stub = 1;
            else if (mod->stubs[si].addr > fs.v[fi])   take_stub = 0;
            else {
                /* Both lists claim this address. The stub wins: being in the
                 * import table is what the address MEANS, and the bytes
                 * discovery found there are the placeholder body the loader
                 * was supposed to overwrite. Translating that placeholder and
                 * returning from it is the failure this whole layer exists to
                 * prevent. */
                take_stub = 1;
                fi++;
                st->stub_shadowed++;
            }

            if (take_stub) {
                char sym[96];
                hle_symbol(sym, sizeof(sym), db, &mod->stubs[si]);
                fprintf(f, "    { 0x%08Xu, %s },\n", mod->stubs[si].addr, sym);
                si++;
            } else {
                fprintf(f, "    { 0x%08Xu, vita_func_%08X },\n", fs.v[fi], fs.v[fi]);
                fi++;
            }
        }
    }
    fprintf(f, "};\n\n");
    fprintf(f, "void vita_register_functions(void) {\n");
    fprintf(f, "    vita_dispatch_init(vita_functions,\n");
    fprintf(f, "        (uint32_t)(sizeof(vita_functions) / sizeof(vita_functions[0])));\n");
    fprintf(f, "}\n");

    /* The entry, by name. A host should not have to read .sce_module_info to
     * learn where the program starts; the module said, and discovery seeded
     * it, so the function exists. The Thumb bit is an instruction-set marker
     * and is not part of the name. */
    if (mod->have_info && mod->info.module_start != 0 &&
        mod->info.module_start != 0xFFFFFFFF) {
        uint32_t ms = img->seg_vaddr + (mod->info.module_start & 0x3FFFFFFF & ~1u);
        fprintf(f, "\n/* module_start */\n");
        fprintf(f, "const uint32_t vita_module_start_addr = 0x%08Xu;\n", ms);
        fprintf(f, "void vita_module_start(void) { vita_func_%08X(); }\n", ms);
    }

    /* The thread-local image: where it is, how much of it is initialised,
     * and how much a thread needs. The thread manager builds every thread's
     * TLS block from it once the host has passed these to vita_tls_init.
     * tls_start carries a segment index in its top two bits, as module_start
     * does. Always written, zeros when the module has none, so a host links
     * without reading .sce_module_info. */
    {
        uint32_t taddr = 0, tfile = 0, tmem = 0;
        if (mod->have_info && mod->info.tls_memsz) {
            uint32_t seg = mod->info.tls_start >> 30, off = mod->info.tls_start & 0x3FFFFFFF;
            uint32_t base = 0;
            if (seg == 0)                           base = img->seg_vaddr;
            else if (seg - 1 < img->data_seg_count) base = img->data_segs[seg - 1].vaddr;
            if (base) { taddr = base + off; tfile = mod->info.tls_filesz; tmem = mod->info.tls_memsz; }
        }
        fprintf(f, "\n/* thread-local image */\n");
        fprintf(f, "const uint32_t vita_module_tls_addr = 0x%08Xu;\n", taddr);
        fprintf(f, "const uint32_t vita_module_tls_filesz = %uu;\n", tfile);
        fprintf(f, "const uint32_t vita_module_tls_memsz = %uu;\n", tmem);
    }

    free(fs.v);
    free(list);
    free(keep);
    return 0;
}



