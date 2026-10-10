/* test_reloc.c — the PT_SCE_RELA decoder and applier, on synthetic streams.
 *
 * Every stream here is assembled by hand from the format definitions, and the
 * expected instruction encodings are worked out from the ARM ARM rather than
 * read back from the code under test. No game data.
 */

#include "reloc.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "tests must be built with asserts enabled (NDEBUG is defined)"
#endif

static void wr32(uint8_t *b, size_t off, uint32_t v) {
    b[off + 0] = (uint8_t)(v);
    b[off + 1] = (uint8_t)(v >> 8);
    b[off + 2] = (uint8_t)(v >> 16);
    b[off + 3] = (uint8_t)(v >> 24);
}

static uint32_t rd32(const uint8_t *b, size_t off) {
    return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8)
         | ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}

/* A two-segment module: text at 0x81000000 (file 0x1000, 0x2000 bytes) and
 * data at 0x81100000 (file 0x3000, 0x1000 bytes), with one relocation stream
 * at file 0x4000. */
#define TEXT_VA 0x81000000u
#define DATA_VA 0x81100000u
#define TEXT_FO 0x1000u
#define DATA_FO 0x3000u
#define RELA_FO 0x4000u
#define IMG_SZ  0x5000u

static void make_module(vc_module *m, uint32_t rela_len) {
    memset(m, 0, sizeof(*m));
    m->kind = VC_VELF;
    m->elf.type = VC_ET_SCE_RELEXEC;
    m->phdr_count = 3;
    m->phdr[0].type = VC_PT_LOAD; m->phdr[0].offset = TEXT_FO; m->phdr[0].vaddr = TEXT_VA;
    m->phdr[0].filesz = 0x2000; m->phdr[0].memsz = 0x2000; m->phdr[0].flags = 5;
    m->phdr[1].type = VC_PT_LOAD; m->phdr[1].offset = DATA_FO; m->phdr[1].vaddr = DATA_VA;
    m->phdr[1].filesz = 0x1000; m->phdr[1].memsz = 0x1800; m->phdr[1].flags = 6;
    m->phdr[2].type = VC_PT_SCE_RELA; m->phdr[2].offset = RELA_FO; m->phdr[2].vaddr = 0;
    m->phdr[2].filesz = rela_len; m->phdr[2].memsz = 0;
}

static const vr_entry *find(const vr_table *t, uint32_t patch, uint8_t code) {
    for (uint32_t i = 0; i < t->n; i++)
        if (t->v[i].patch == patch && t->v[i].code == code) return &t->v[i];
    return NULL;
}

/* --- every format once ------------------------------------------------------ */

static void test_formats(void) {
    static uint8_t img[IMG_SZ];
    memset(img, 0, sizeof(img));
    size_t r = RELA_FO;

    /* Format 0: sym seg 0, ABS32, patch seg 1, code2 ABS32 at +2 halfwords
     * (= +4 bytes), addend 0x201, offset 0x10. */
    wr32(img, r, 0u | (0u << 4) | (VR_ABS32 << 8) | (1u << 16) | ((uint32_t)VR_ABS32 << 20) | (2u << 28));
    wr32(img, r + 4, 0x201);
    wr32(img, r + 8, 0x10);
    r += 12;

    /* Format 1: sym absolute (0xF), ABS32, patch seg 1, offset 0x345 (lo) with
     * hi 0, addend 0x3FFFFF (the 22-bit maximum). */
    wr32(img, r, 1u | (0xFu << 4) | (VR_ABS32 << 8) | (1u << 16) | (0x345u << 20));
    wr32(img, r + 4, (0u) | (0x3FFFFFu << 10));
    r += 8;

    /* Format 2: sym seg 0, ABS32, offset +0x20 from the running offset
     * (0x345 -> 0x365), addend 0x100. */
    wr32(img, r, 2u | (0u << 4) | (VR_ABS32 << 8) | (0x20u << 16));
    wr32(img, r + 4, 0x100);
    r += 8;

    /* Format 3: sym seg 0, Thumb MOVW/MOVT pair, offset +0x8 (-> 0x36D),
     * second instruction 4 bytes on, addend 0x300. */
    wr32(img, r, 3u | (0u << 4) | (1u << 8) | (0x8u << 9) | (4u << 27));
    wr32(img, r + 4, 0x300);
    r += 8;

    /* Format 4: offset +0x10 (-> 0x37D), dist2 4: repeats the Thumb pair. */
    wr32(img, r, 4u | (0x10u << 4) | (4u << 27));
    r += 4;

    /* Format 5: dist1 8 (-> 0x385), dist2 4, dist3 8 (-> 0x38D), dist4 4. */
    wr32(img, r, 5u | (8u << 4) | (4u << 13) | (8u << 18) | (4u << 27));
    r += 4;

    /* Format 6: offset +0x73 (-> 0x400): rebase the word there, which holds a
     * text address. */
    wr32(img, DATA_FO + 0x400, TEXT_VA + 0x500);
    wr32(img, r, 6u | (0x73u << 4));
    r += 4;

    /* Format 8: two 4-bit word counts, 1 then 3: words at 0x404 and 0x410. */
    wr32(img, DATA_FO + 0x404, TEXT_VA + 0x600);
    wr32(img, DATA_FO + 0x410, DATA_VA + 0x10);
    wr32(img, r, 8u | (1u << 4) | (3u << 8));
    r += 4;

    /* Format 9: one 2-bit field of 1: the next word (0x414), holding an
     * address in no segment -> counted unmatched, still a pointer slot. */
    wr32(img, DATA_FO + 0x414, 0x12345678);
    wr32(img, r, 9u | (1u << 4));
    r += 4;

    vc_module m;
    make_module(&m, (uint32_t)(r - RELA_FO));

    vr_table t;
    char err[128];
    int rc = vr_parse(img, sizeof(img), &m, &t, err, sizeof(err));
    assert(rc == 0);
    assert(t.streams == 1);
    assert(t.by_format[0] == 1 && t.by_format[1] == 1 && t.by_format[2] == 1);
    assert(t.by_format[3] == 1 && t.by_format[4] == 1 && t.by_format[5] == 1);
    assert(t.by_format[6] == 1 && t.by_format[8] == 1 && t.by_format[9] == 1);
    assert(t.bad_segment == 0);
    assert(t.rebase_unmatched == 1);
    assert(t.trailing == 0);

    /* 2 + 1 + 1 + 2 + 2 + 4 + 1 + 2 + 1 = 16 relocations */
    assert(t.n == 16);

    const vr_entry *e;
    e = find(&t, DATA_VA + 0x10, VR_ABS32);
    assert(e && e->value == TEXT_VA + 0x201 && e->format == 0 && e->sym_seg == 0 && e->patch_seg == 1);
    e = find(&t, DATA_VA + 0x14, VR_ABS32);
    assert(e && e->value == TEXT_VA + 0x201 && e->format == 0);

    e = find(&t, DATA_VA + 0x345, VR_ABS32);
    assert(e && e->value == 0x3FFFFF && e->format == 1 && e->sym_seg == 0xF);

    e = find(&t, DATA_VA + 0x365, VR_ABS32);
    assert(e && e->value == TEXT_VA + 0x100 && e->format == 2);

    e = find(&t, DATA_VA + 0x36D, VR_THM_MOVW_ABS_NC);
    assert(e && e->value == TEXT_VA + 0x300 && e->format == 3);
    e = find(&t, DATA_VA + 0x371, VR_THM_MOVT_ABS);
    assert(e && e->value == TEXT_VA + 0x300 && e->format == 3);

    e = find(&t, DATA_VA + 0x37D, VR_THM_MOVW_ABS_NC);
    assert(e && e->value == TEXT_VA + 0x300 && e->format == 4);
    e = find(&t, DATA_VA + 0x381, VR_THM_MOVT_ABS);
    assert(e && e->format == 4);

    e = find(&t, DATA_VA + 0x385, VR_THM_MOVW_ABS_NC); assert(e && e->format == 5);
    e = find(&t, DATA_VA + 0x389, VR_THM_MOVT_ABS);    assert(e && e->format == 5);
    e = find(&t, DATA_VA + 0x38D, VR_THM_MOVW_ABS_NC); assert(e && e->format == 5);
    e = find(&t, DATA_VA + 0x391, VR_THM_MOVT_ABS);    assert(e && e->format == 5);

    e = find(&t, DATA_VA + 0x400, VR_ABS32);
    assert(e && e->value == TEXT_VA + 0x500 && e->format == 6 && e->sym_seg == 0);
    e = find(&t, DATA_VA + 0x404, VR_ABS32);
    assert(e && e->value == TEXT_VA + 0x600 && e->format == 8 && e->sym_seg == 0);
    e = find(&t, DATA_VA + 0x410, VR_ABS32);
    assert(e && e->value == DATA_VA + 0x10 && e->format == 8 && e->sym_seg == 1);
    e = find(&t, DATA_VA + 0x414, VR_ABS32);
    assert(e && e->value == 0x12345678 && e->format == 9 && e->sym_seg == 0xF);

    assert(t.by_code[VR_ABS32] == 8);
    assert(t.by_code[VR_THM_MOVW_ABS_NC] == 4);
    assert(t.by_code[VR_THM_MOVT_ABS] == 4);

    vr_free(&t);
    puts("  formats 0-9 decode");
}

/* --- an unknown format stops the stream, keeps what came before ------------ */

static void test_unknown_format(void) {
    static uint8_t img[IMG_SZ];
    memset(img, 0, sizeof(img));
    size_t r = RELA_FO;

    wr32(img, r, 0u | (0u << 4) | (VR_ABS32 << 8) | (1u << 16));
    wr32(img, r + 4, 0x40);
    wr32(img, r + 8, 0x0);
    r += 12;
    wr32(img, r, 0xAu);            /* format 10: does not exist */
    r += 4;

    vc_module m;
    make_module(&m, (uint32_t)(r - RELA_FO));

    vr_table t;
    char err[128];
    int rc = vr_parse(img, sizeof(img), &m, &t, err, sizeof(err));
    assert(rc != 0);
    assert(strstr(err, "format 10") != NULL);
    assert(t.n == 1);
    assert(t.v[0].patch == DATA_VA && t.v[0].value == TEXT_VA + 0x40);
    vr_free(&t);

    /* No relocation segment at all is success with an empty table. */
    make_module(&m, 0);
    m.phdr_count = 2;
    rc = vr_parse(img, sizeof(img), &m, &t, err, sizeof(err));
    assert(rc == 0 && t.n == 0 && t.streams == 0);
    vr_free(&t);

    puts("  unknown format stops, missing stream is empty");
}

/* --- applying: the instruction encodings ------------------------------------ */

static void test_apply(void) {
    static uint8_t img[IMG_SZ];
    memset(img, 0, sizeof(img));
    size_t r = RELA_FO;

    /* All patches land in text here so the instruction forms can be checked.
     * Each is a format 0 entry: sym absolute, patch seg 0. */
    struct { uint32_t off, code, addend; } ents[] = {
        { 0x000, VR_ABS32,           0x81001234 },
        { 0x010, VR_REL32,           0x81001000 },   /* -> 0x81001000 - 0x81000010 */
        { 0x020, VR_PREL31,          0x81001000 },
        { 0x100, VR_THM_CALL,        0x81001100 },   /* BL +0x1000 */
        { 0x200, VR_CALL,            0x81001200 },   /* BL +0x1000 */
        { 0x300, VR_THM_MOVW_ABS_NC, 0x81001234 },
        { 0x304, VR_THM_MOVT_ABS,    0x81001234 },
        { 0x400, VR_MOVW_ABS_NC,     0x81001234 },
        { 0x404, VR_MOVT_ABS,        0x81001234 },
        { 0x500, VR_ABS8,            0x000000AB },
        { 0x600, VR_THM_JUMP11,      0x81000700 },   /* b.n +0x100 */
        { 0x610, VR_THM_JUMP24,      0x81001610 },   /* b.w +0x1000 */
    };
    const int N = (int)(sizeof(ents) / sizeof(ents[0]));
    for (int i = 0; i < N; i++) {
        wr32(img, r, 0u | (0xFu << 4) | (ents[i].code << 8) | (0u << 16));
        wr32(img, r + 4, ents[i].addend);
        wr32(img, r + 8, ents[i].off);
        r += 12;
    }

    /* Instruction bits before patching: the forms with zero immediates. */
    wr32(img, TEXT_FO + 0x100, 0xF000F000u);   /* bl (Thumb) with every immediate field zero: upper F000, lower F000 */
    wr32(img, TEXT_FO + 0x200, 0xEB000000u);   /* bl +0 (ARM)             */
    wr32(img, TEXT_FO + 0x300, 0x0000F240u);   /* movw r0, #0  (F240 0000) */
    wr32(img, TEXT_FO + 0x304, 0x0000F2C0u);   /* movt r0, #0  (F2C0 0000) */
    wr32(img, TEXT_FO + 0x400, 0xE3000000u);   /* movw r0, #0             */
    wr32(img, TEXT_FO + 0x404, 0xE3400000u);   /* movt r0, #0             */
    wr32(img, TEXT_FO + 0x600, 0x0000E000u);   /* b.n +0 (T2), halfword   */
    wr32(img, TEXT_FO + 0x610, 0x9000F000u);   /* b.w (T4) with zero immediates: upper F000, lower 9000 */

    vc_module m;
    make_module(&m, (uint32_t)(r - RELA_FO));

    vr_table t;
    char err[128];
    assert(vr_parse(img, sizeof(img), &m, &t, err, sizeof(err)) == 0);
    assert(t.n == (uint32_t)N);

    uint32_t changed, unsupported, outside;
    assert(vr_apply(img, sizeof(img), &m, &t, &changed, &unsupported, &outside) == 0);
    assert(unsupported == 0 && outside == 0);
    assert(changed == (uint32_t)N);

    assert(rd32(img, TEXT_FO + 0x000) == 0x81001234u);
    assert(rd32(img, TEXT_FO + 0x010) == 0x00000FF0u);
    assert(rd32(img, TEXT_FO + 0x020) == 0x00000FE0u);

    /* BL +0x1000, T1: imm10 = 1, imm11 = 0, S = 0, J1 = J2 = 1 -> F001 F800 */
    assert(rd32(img, TEXT_FO + 0x100) == 0xF800F001u);
    /* BL +0x1000, A1: imm24 = 0x400 */
    assert(rd32(img, TEXT_FO + 0x200) == 0xEB000400u);
    /* movw r0, #0x1234 (T3) = F241 2034; movt r0, #0x8100 = F2C8 1000 */
    assert(rd32(img, TEXT_FO + 0x300) == 0x2034F241u);
    assert(rd32(img, TEXT_FO + 0x304) == 0x1000F2C8u);
    /* movw r0, #0x1234 (A1): imm4 = 1, imm12 = 0x234 -> E301 0234;
     * movt r0, #0x8100: imm4 = 8, imm12 = 0x100 -> E348 0100 */
    assert(rd32(img, TEXT_FO + 0x400) == 0xE3010234u);
    assert(rd32(img, TEXT_FO + 0x404) == 0xE3480100u);
    assert(img[TEXT_FO + 0x500] == 0xAB);
    /* b.n +0x100 (T2): imm11 = 0x80 -> E080 */
    assert(img[TEXT_FO + 0x600] == 0x80 && img[TEXT_FO + 0x601] == 0xE0);
    /* b.w +0x1000 (T4): same immediate split as BL, bit 12 of the lower
     * halfword stays 1 and bit 14 stays 0 -> F001 B800 */
    assert(rd32(img, TEXT_FO + 0x610) == 0xB800F001u);

    /* Applying again changes nothing: the image is now at its link state. */
    assert(vr_apply(img, sizeof(img), &m, &t, &changed, &unsupported, &outside) == 0);
    assert(changed == 0);

    vr_free(&t);
    puts("  apply: ABS32/REL32/PREL31/BL/MOVW/MOVT/ABS8 encodings");
}

/* --- a patch into .bss is counted, not written -------------------------------- */

static void test_outside(void) {
    static uint8_t img[IMG_SZ];
    memset(img, 0, sizeof(img));
    size_t r = RELA_FO;

    /* data seg filesz is 0x1000, memsz 0x1800: offset 0x1400 is .bss */
    wr32(img, r, 0u | (0u << 4) | (VR_ABS32 << 8) | (1u << 16));
    wr32(img, r + 4, 0x0);
    wr32(img, r + 8, 0x1400);
    r += 12;

    vc_module m;
    make_module(&m, (uint32_t)(r - RELA_FO));

    vr_table t;
    char err[128];
    assert(vr_parse(img, sizeof(img), &m, &t, err, sizeof(err)) == 0);
    assert(t.n == 1 && t.v[0].patch == DATA_VA + 0x1400);

    uint32_t changed, unsupported, outside;
    assert(vr_apply(img, sizeof(img), &m, &t, &changed, &unsupported, &outside) == 0);
    assert(changed == 0 && outside == 1);
    vr_free(&t);
    puts("  patch beyond the file is counted, not written");
}

int main(void) {
    puts("test_reloc:");
    test_formats();
    test_unknown_format();
    test_apply();
    test_outside();
    puts("all passed");
    return 0;
}
