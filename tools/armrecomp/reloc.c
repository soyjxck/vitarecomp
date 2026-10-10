/* reloc.c — decoding and applying the PT_SCE_RELA stream.
 *
 * The stream is a sequence of variable-size entries. The first word's low four
 * bits give the format and therefore the size, so the decoder never needs a
 * length field and a single bad format stops the stream rather than
 * desynchronising it. Formats 0 and 1 set the decoder's running state (patch
 * segment, offset, symbol base, addend, type pair); formats 2 to 9 advance the
 * offset and reuse whatever state they did not set themselves.
 */

#include "reloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- little-endian access --------------------------------------------------- */

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

/* --- segments --------------------------------------------------------------- */

typedef struct {
    uint32_t base;      /* p_vaddr                               */
    uint32_t memsz;
    uint32_t file;      /* p_offset                              */
    uint32_t filesz;
    int      ok;        /* a PT_LOAD program header exists here  */
} seginfo;

static seginfo seg_lookup(const vc_module *m, uint32_t idx) {
    seginfo s;
    memset(&s, 0, sizeof(s));
    if (idx < (uint32_t)m->phdr_count && m->phdr[idx].type == VC_PT_LOAD) {
        s.base   = m->phdr[idx].vaddr;
        s.memsz  = m->phdr[idx].memsz;
        s.file   = m->phdr[idx].offset;
        s.filesz = m->phdr[idx].filesz;
        s.ok     = 1;
    }
    return s;
}

/* The symbol base for a symbol_segment field: a segment's p_vaddr, or zero
 * for the absolute marker. A segment index that is not a PT_LOAD is counted;
 * the loader would have faulted on it, so a real module never carries one. */
static uint32_t sym_base(const vc_module *m, uint32_t sym_seg, vr_table *t) {
    if (sym_seg == VR_SEG_ABSOLUTE) return 0;
    seginfo s = seg_lookup(m, sym_seg);
    if (!s.ok) { t->bad_segment++; return 0; }
    return s.base;
}

/* --- the table -------------------------------------------------------------- */

static int push(vr_table *t, uint32_t patch, uint32_t value, uint8_t format,
                uint8_t code, uint8_t patch_seg, uint8_t sym_seg) {
    if (code == VR_NONE) return 0;         /* a no-op is not a relocation */
    if (t->n == t->cap) {
        uint32_t cap = t->cap ? t->cap * 2 : 4096;
        vr_entry *v = (vr_entry *)realloc(t->v, cap * sizeof(vr_entry));
        if (!v) return 1;
        t->v = v;
        t->cap = cap;
    }
    vr_entry *e = &t->v[t->n++];
    e->patch     = patch;
    e->value     = value;
    e->format    = format;
    e->code      = code;
    e->patch_seg = patch_seg;
    e->sym_seg   = sym_seg;
    t->by_code[code]++;
    return 0;
}

/* --- decoder state ---------------------------------------------------------- */

typedef struct {
    uint32_t addr;       /* base of the patch segment                     */
    uint32_t offset;     /* running offset within it                      */
    uint32_t patch_seg;
    uint32_t saddr;      /* symbol base                                   */
    uint32_t sym_seg;
    uint32_t addend;
    uint8_t  type, type2;
} rstate;

/* Formats 6 to 9 do not carry a symbol: the word already holds an absolute
 * address from the link, and the loader works out which segment it falls in
 * and rebases it. Under the identity model the result is the word itself,
 * but the entry is still a stored pointer and discovery wants it. */
static int rebase(const uint8_t *buf, size_t size, const vc_module *m,
                  rstate *st, vr_table *t, uint8_t format) {
    seginfo ps = seg_lookup(m, st->patch_seg);
    if (!ps.ok) { t->bad_segment++; return 0; }

    uint64_t fo = (uint64_t)ps.file + st->offset;
    if ((uint64_t)st->offset + 4 > ps.filesz || fo + 4 > size) {
        /* A rebase into .bss has nothing to read. Count it with the
         * unmatched ones: it is a pointer slot the file does not hold. */
        t->rebase_unmatched++;
        return 0;
    }
    uint32_t orgval = rd32(buf + fo);

    uint32_t symseg = VR_SEG_ABSOLUTE;
    for (int i = 0; i < m->phdr_count; i++) {
        if (m->phdr[i].type != VC_PT_LOAD) continue;
        if (orgval >= m->phdr[i].vaddr &&
            orgval <  m->phdr[i].vaddr + m->phdr[i].memsz) {
            symseg = (uint32_t)i;             /* last match wins, as the loader */
        }
    }
    if (symseg == VR_SEG_ABSOLUTE) t->rebase_unmatched++;
    else st->saddr = m->phdr[symseg].vaddr;
    st->sym_seg = symseg;
    st->type  = VR_ABS32;
    st->type2 = VR_NONE;

    return push(t, ps.base + st->offset, orgval, format, VR_ABS32,
                (uint8_t)st->patch_seg, (uint8_t)symseg);
}

static const uint8_t ENTRY_SIZE[10] = { 12, 8, 8, 8, 4, 4, 4, 4, 4, 4 };

static int parse_stream(const uint8_t *buf, size_t size, const vc_module *m,
                        const uint8_t *s, uint32_t len, vr_table *t,
                        char *err, size_t errsz) {
    rstate st;
    memset(&st, 0, sizeof(st));

    uint32_t pos = 0;
    while (pos < len) {
        if (len - pos < 4) { t->trailing += len - pos; break; }
        uint32_t w0 = rd32(s + pos);
        uint32_t format = w0 & 0xF;
        if (format > 9) {
            snprintf(err, errsz, "unknown relocation entry format %u at stream offset %u",
                     format, pos);
            return 1;
        }
        if (len - pos < ENTRY_SIZE[format]) { t->trailing += len - pos; break; }
        t->by_format[format]++;

        uint32_t w1 = ENTRY_SIZE[format] >= 8  ? rd32(s + pos + 4) : 0;
        uint32_t w2 = ENTRY_SIZE[format] >= 12 ? rd32(s + pos + 8) : 0;
        int oom = 0;

        switch (format) {
        case 0: {
            uint32_t sym   = (w0 >> 4)  & 0xF;
            uint8_t  code  = (uint8_t)((w0 >> 8) & 0xFF);
            uint32_t patch = (w0 >> 16) & 0xF;
            uint8_t  code2 = (uint8_t)((w0 >> 20) & 0xFF);
            uint32_t dist2 = (w0 >> 28) & 0xF;
            uint32_t a = w1, off = w2;

            seginfo ps = seg_lookup(m, patch);
            if (!ps.ok) t->bad_segment++;
            uint32_t sb = sym_base(m, sym, t);
            uint32_t p  = ps.base + off;

            oom |= push(t, p, sb + a, 0, code, (uint8_t)patch, (uint8_t)sym);
            /* The second place is dist2 HALFWORDS away in this format alone;
             * formats 3 to 5 measure their second distance in bytes. */
            if (code2 != VR_NONE)
                oom |= push(t, p + dist2 * 2, sb + a, 0, code2, (uint8_t)patch, (uint8_t)sym);

            st.addr = ps.base; st.offset = off; st.patch_seg = patch;
            st.saddr = sb; st.sym_seg = sym; st.addend = a;
            st.type = code; st.type2 = code2;
            break;
        }
        case 1: {
            uint32_t sym   = (w0 >> 4)  & 0xF;
            uint8_t  code  = (uint8_t)((w0 >> 8) & 0xFF);
            uint32_t patch = (w0 >> 16) & 0xF;
            uint32_t off   = ((w0 >> 20) & 0xFFF) | ((w1 & 0x3FF) << 12);
            uint32_t a     = w1 >> 10;                  /* 22 bits */

            seginfo ps = seg_lookup(m, patch);
            if (!ps.ok) t->bad_segment++;
            uint32_t sb = sym_base(m, sym, t);
            uint32_t p  = ps.base + off;

            oom |= push(t, p, sb + a, 1, code, (uint8_t)patch, (uint8_t)sym);

            st.addr = ps.base; st.offset = off; st.patch_seg = patch;
            st.saddr = sb; st.sym_seg = sym; st.addend = a;
            st.type = code; st.type2 = VR_NONE;
            break;
        }
        case 2: {
            uint32_t sym  = (w0 >> 4) & 0xF;
            uint8_t  code = (uint8_t)((w0 >> 8) & 0xFF);
            uint32_t off  = (w0 >> 16) & 0xFFFF;

            st.offset += off;
            st.saddr   = sym_base(m, sym, t);
            st.sym_seg = sym;
            st.addend  = w1;
            st.type    = code;
            st.type2   = VR_NONE;

            oom |= push(t, st.addr + st.offset, st.saddr + st.addend, 2, code,
                        (uint8_t)st.patch_seg, (uint8_t)sym);
            break;
        }
        case 3: {
            uint32_t sym   = (w0 >> 4) & 0xF;
            uint32_t mode  = (w0 >> 8) & 1;             /* 0 ARM, 1 Thumb */
            uint32_t off   = (w0 >> 9) & 0x3FFFF;
            uint32_t dist2 = (w0 >> 27) & 0x1F;

            st.type  = mode ? VR_THM_MOVW_ABS_NC : VR_MOVW_ABS_NC;
            st.type2 = mode ? VR_THM_MOVT_ABS    : VR_MOVT_ABS;
            st.offset += off;
            st.saddr   = sym_base(m, sym, t);
            st.sym_seg = sym;
            st.addend  = w1 & 0x3FFFFF;

            uint32_t p = st.addr + st.offset;
            oom |= push(t, p,         st.saddr + st.addend, 3, st.type,  (uint8_t)st.patch_seg, (uint8_t)sym);
            oom |= push(t, p + dist2, st.saddr + st.addend, 3, st.type2, (uint8_t)st.patch_seg, (uint8_t)sym);
            break;
        }
        case 4: {
            uint32_t off   = (w0 >> 4) & 0x7FFFFF;
            uint32_t dist2 = (w0 >> 27) & 0x1F;

            st.offset += off;
            uint32_t p = st.addr + st.offset;
            oom |= push(t, p,         st.saddr + st.addend, 4, st.type,  (uint8_t)st.patch_seg, (uint8_t)st.sym_seg);
            oom |= push(t, p + dist2, st.saddr + st.addend, 4, st.type2, (uint8_t)st.patch_seg, (uint8_t)st.sym_seg);
            break;
        }
        case 5: {
            uint32_t dist1 = (w0 >> 4)  & 0x1FF;
            uint32_t dist2 = (w0 >> 13) & 0x1F;
            uint32_t dist3 = (w0 >> 18) & 0x1FF;
            uint32_t dist4 = (w0 >> 27) & 0x1F;

            st.offset += dist1;
            uint32_t p = st.addr + st.offset;
            oom |= push(t, p,         st.saddr + st.addend, 5, st.type,  (uint8_t)st.patch_seg, (uint8_t)st.sym_seg);
            oom |= push(t, p + dist2, st.saddr + st.addend, 5, st.type2, (uint8_t)st.patch_seg, (uint8_t)st.sym_seg);

            st.offset += dist3;
            p = st.addr + st.offset;
            oom |= push(t, p,         st.saddr + st.addend, 5, st.type,  (uint8_t)st.patch_seg, (uint8_t)st.sym_seg);
            oom |= push(t, p + dist4, st.saddr + st.addend, 5, st.type2, (uint8_t)st.patch_seg, (uint8_t)st.sym_seg);
            break;
        }
        case 6: {
            st.offset += w0 >> 4;                        /* 28 bits */
            oom |= rebase(buf, size, m, &st, t, 6);
            break;
        }
        case 7: case 8: case 9: {
            uint32_t offsets = w0 >> 4;
            uint32_t bits = format == 7 ? 7 : format == 8 ? 4 : 2;
            uint32_t mask = (1u << bits) - 1;
            /* do-while: the first field is used even when it is zero, which
             * is how the stream names the word right after the previous one. */
            do {
                st.offset += (offsets & mask) * 4;       /* fields count words */
                oom |= rebase(buf, size, m, &st, t, (uint8_t)format);
            } while ((offsets >>= bits) != 0);
            break;
        }
        }

        if (oom) { snprintf(err, errsz, "out of memory decoding relocations"); return 1; }
        pos += ENTRY_SIZE[format];
    }
    return 0;
}

int vr_parse(const uint8_t *buf, size_t size, const vc_module *m,
             vr_table *out, char *err, size_t errsz) {
    memset(out, 0, sizeof(*out));
    if (errsz) err[0] = '\0';

    int rc = 0;
    for (int i = 0; i < m->phdr_count; i++) {
        const vc_phdr *ph = &m->phdr[i];
        if (ph->type != VC_PT_SCE_RELA) continue;
        if (ph->filesz == 0) continue;
        if ((uint64_t)ph->offset + ph->filesz > size) {
            snprintf(err, errsz, "relocation segment %d extends past the file", i);
            rc = 1;
            continue;
        }
        out->streams++;
        out->stream_bytes += ph->filesz;
        if (parse_stream(buf, size, m, buf + ph->offset, ph->filesz, out, err, errsz))
            rc = 1;
    }
    return rc;
}

/* --- applying ------------------------------------------------------------- */
/*
 * Each writer below preserves every instruction bit that is not an immediate
 * field. The immediate layouts are the ARM ARM's: A1 MOVW/MOVT, T3
 * MOVW/MOVT, A1 BL, and T1 BL with its J1/J2 sign-derived bits.
 */

static void patch_thumb_mov(uint8_t *p, uint32_t v16) {
    uint16_t hi = rd16(p), lo = rd16(p + 2);
    hi = (uint16_t)((hi & ~0x040Fu) | (((v16 >> 11) & 1) << 10) | ((v16 >> 12) & 0xF));
    lo = (uint16_t)((lo & ~0x70FFu) | (((v16 >> 8) & 7) << 12) | (v16 & 0xFF));
    wr16(p, hi); wr16(p + 2, lo);
}

static void patch_arm_mov(uint8_t *p, uint32_t v16) {
    uint32_t w = rd32(p);
    w = (w & ~0x000F0FFFu) | (v16 & 0xFFF) | (((v16 >> 12) & 0xF) << 16);
    wr32(p, w);
}

static void patch_thumb_call(uint8_t *p, uint32_t v) {
    uint16_t hi = rd16(p), lo = rd16(p + 2);
    uint32_t s  = (v >> 24) & 1;
    uint32_t j1 = s ^ (~(v >> 23) & 1);
    uint32_t j2 = s ^ (~(v >> 22) & 1);
    hi = (uint16_t)((hi & ~0x07FFu) | (s << 10) | ((v >> 12) & 0x3FF));
    lo = (uint16_t)((lo & ~0x2FFFu) | (j1 << 13) | (j2 << 11) | ((v >> 1) & 0x7FF));
    wr16(p, hi); wr16(p + 2, lo);
}

int vr_apply(uint8_t *buf, size_t size, const vc_module *m, const vr_table *t,
             uint32_t *changed, uint32_t *unsupported, uint32_t *outside) {
    *changed = *unsupported = *outside = 0;

    for (uint32_t i = 0; i < t->n; i++) {
        const vr_entry *e = &t->v[i];
        seginfo ps = seg_lookup(m, e->patch_seg);
        if (!ps.ok) { (*outside)++; continue; }

        uint32_t off  = e->patch - ps.base;
        uint32_t need = e->code == VR_ABS8 ? 1 : 4;
        uint64_t fo   = (uint64_t)ps.file + off;
        if (e->patch < ps.base || (uint64_t)off + need > ps.filesz || fo + need > size) {
            (*outside)++;
            continue;
        }

        uint8_t *p = buf + fo;
        uint8_t before[4];
        memcpy(before, p, need);

        switch (e->code) {
        case VR_NONE: case VR_V4BX: case VR_RBASE:
            break;
        case VR_ABS32: case VR_TARGET1:
            wr32(p, e->value);
            break;
        case VR_ABS8:
            p[0] = (uint8_t)e->value;
            break;
        case VR_REL32: case VR_TARGET2:
            wr32(p, e->value - e->patch);
            break;
        case VR_PREL31:
            wr32(p, (e->value - e->patch) & 0x7FFFFFFFu);
            break;
        case VR_THM_CALL:
        case VR_THM_JUMP24:     /* B.W shares BL's immediate layout; bits 15:14 and 12 are kept */
            patch_thumb_call(p, e->value - e->patch);
            break;
        case VR_THM_JUMP11: {   /* B (T2): imm11 in the low bits of one halfword */
            uint16_t h = rd16(p);
            wr16(p, (uint16_t)((h & 0xF800u) | (((e->value - e->patch) >> 1) & 0x7FF)));
            break;
        }
        case VR_THM_JUMP8: {    /* B<c> (T1): imm8 in the low byte */
            uint16_t h = rd16(p);
            wr16(p, (uint16_t)((h & 0xFF00u) | (((e->value - e->patch) >> 1) & 0xFF)));
            break;
        }
        case VR_CALL: case VR_JUMP24: {
            uint32_t w = rd32(p);
            wr32(p, (w & 0xFF000000u) | (((e->value - e->patch) >> 2) & 0xFFFFFFu));
            break;
        }
        case VR_MOVW_ABS_NC:     patch_arm_mov(p, e->value & 0xFFFF);   break;
        case VR_MOVT_ABS:        patch_arm_mov(p, e->value >> 16);      break;
        case VR_THM_MOVW_ABS_NC: patch_thumb_mov(p, e->value & 0xFFFF); break;
        case VR_THM_MOVT_ABS:    patch_thumb_mov(p, e->value >> 16);    break;
        default:
            (*unsupported)++;
            continue;
        }

        if (memcmp(before, p, need) != 0) (*changed)++;
    }
    return 0;
}

void vr_free(vr_table *t) {
    free(t->v);
    memset(t, 0, sizeof(*t));
}

const char *vr_code_name(uint8_t code) {
    switch (code) {
    case VR_NONE:            return "R_ARM_NONE";
    case VR_ABS32:           return "R_ARM_ABS32";
    case VR_REL32:           return "R_ARM_REL32";
    case VR_ABS8:            return "R_ARM_ABS8";
    case VR_THM_CALL:        return "R_ARM_THM_CALL";
    case VR_CALL:            return "R_ARM_CALL";
    case VR_JUMP24:          return "R_ARM_JUMP24";
    case VR_THM_JUMP24:      return "R_ARM_THM_JUMP24";
    case VR_THM_JUMP11:      return "R_ARM_THM_JUMP11";
    case VR_THM_JUMP8:       return "R_ARM_THM_JUMP8";
    case VR_TARGET1:         return "R_ARM_TARGET1";
    case VR_V4BX:            return "R_ARM_V4BX";
    case VR_TARGET2:         return "R_ARM_TARGET2";
    case VR_PREL31:          return "R_ARM_PREL31";
    case VR_MOVW_ABS_NC:     return "R_ARM_MOVW_ABS_NC";
    case VR_MOVT_ABS:        return "R_ARM_MOVT_ABS";
    case VR_THM_MOVW_ABS_NC: return "R_ARM_THM_MOVW_ABS_NC";
    case VR_THM_MOVT_ABS:    return "R_ARM_THM_MOVT_ABS";
    case VR_RBASE:           return "R_ARM_RBASE";
    default:                 return "R_ARM_?";
    }
}
