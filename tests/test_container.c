/* test_container.c — the container stack, on synthetic input only.
 *
 * Every blob here is built in this file. No game data, no dump, no key
 * material. The point of these tests is that the parser reports what is
 * actually in the bytes and refuses what is not — a parser that guesses is the
 * failure mode that matters, because it produces a plausible report.
 */

#include "container.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every check below is an assert(), and a Release build defines NDEBUG, which
 * compiles all of them to nothing. The suite would still print "all passed" —
 * a green run that tested nothing, which is worse than a red one. The build
 * strips NDEBUG for test targets; this guard turns any regression of that into
 * a compile error instead of a silent pass. */
#ifdef NDEBUG
#error "tests must be built with asserts enabled (NDEBUG is defined)"
#endif

static void wr32(uint8_t *b, size_t off, uint32_t v) {
    b[off + 0] = (uint8_t)(v);
    b[off + 1] = (uint8_t)(v >> 8);
    b[off + 2] = (uint8_t)(v >> 16);
    b[off + 3] = (uint8_t)(v >> 24);
}

static void wr16(uint8_t *b, size_t off, uint16_t v) {
    b[off + 0] = (uint8_t)(v);
    b[off + 1] = (uint8_t)(v >> 8);
}

static void wr64(uint8_t *b, size_t off, uint64_t v) {
    wr32(b, off, (uint32_t)v);
    wr32(b, off + 4, (uint32_t)(v >> 32));
}

/* Lay down an ELF32 ARM header at `off` with one PT_LOAD program header. */
static void put_elf(uint8_t *b, size_t off, size_t phoff, uint16_t etype) {
    b[off + 0] = 0x7F; b[off + 1] = 'E'; b[off + 2] = 'L'; b[off + 3] = 'F';
    b[off + 4] = 1;    /* ELFCLASS32 */
    b[off + 5] = 1;    /* ELFDATA2LSB */
    wr16(b, off + 0x10, etype);
    wr16(b, off + 0x12, VC_EM_ARM);
    wr32(b, off + 0x18, 0x81000000);      /* e_entry  */
    wr32(b, off + 0x1C, (uint32_t)phoff); /* e_phoff  */
    wr16(b, off + 0x2A, 0x20);            /* phentsize */
    wr16(b, off + 0x2C, 1);               /* phnum     */

    wr32(b, phoff + 0x00, VC_PT_LOAD);
    wr32(b, phoff + 0x04, 0x1000);        /* p_offset */
    wr32(b, phoff + 0x08, 0x81000000);    /* p_vaddr  */
    wr32(b, phoff + 0x10, 0x2000);        /* p_filesz */
    wr32(b, phoff + 0x14, 0x3000);        /* p_memsz  */
    wr32(b, phoff + 0x18, 5);             /* r-x      */
    wr32(b, phoff + 0x1C, 0x1000);        /* p_align  */
}

/* --- a well-formed SELF ----------------------------------------------------- */

static void test_self_roundtrip(void) {
    static uint8_t b[0x1000];
    memset(b, 0, sizeof(b));

    const uint64_t appinfo_off = 0x80;
    const uint64_t elf_off     = 0xA0;
    const uint64_t phdr_off    = 0x100;
    const uint64_t seg_off     = 0x180;

    wr32(b, 0x00, VC_SCE_MAGIC);
    wr32(b, 0x04, 3);            /* version    */
    wr16(b, 0x08, 0x00C0);       /* sdk_type   */
    wr16(b, 0x0A, 1);            /* SELF       */
    wr32(b, 0x0C, 0x600);        /* metadata   */
    wr64(b, 0x10, 0x1000);       /* header_len     */
    wr64(b, 0x18, 0x35BE70);     /* elf_filesize   */
    /* 0x20 and 0x28 are the two fields Vita has and PS3 does not. Writing them
     * explicitly is the point of this test: an earlier revision used the PS3
     * layout in BOTH the parser and this blob, so the test agreed with the bug
     * and passed. Synthetic data cannot catch a misread structure when it was
     * built from the same misreading — only the real module did. */
    wr64(b, 0x20, 0x19A2A2);     /* self_filesize  (Vita-only) */
    wr64(b, 0x28, 0);            /* padding        (Vita-only) */
    wr64(b, 0x30, 4);            /* self_offset    */
    wr64(b, 0x38, appinfo_off);
    wr64(b, 0x40, elf_off);
    wr64(b, 0x48, phdr_off);
    wr64(b, 0x50, 0);            /* shdr_offset    */
    wr64(b, 0x58, seg_off);

    wr64(b, appinfo_off + 0x00, 0x2F00000000000001ULL); /* auth_id   */
    wr32(b, appinfo_off + 0x08, 0x1000000);             /* vendor_id */
    wr32(b, appinfo_off + 0x0C, 4);                     /* self_type = app */

    put_elf(b, (size_t)elf_off, (size_t)phdr_off, VC_ET_SCE_RELEXEC);

    /* One segment: zlib-compressed AND encrypted. Note the inverted encoding —
     * this is the assertion that catches a parser that assumed 1 == true. */
    wr64(b, seg_off + 0x00, 0x1000);
    wr64(b, seg_off + 0x08, 0x2000);
    wr64(b, seg_off + 0x10, VC_COMPRESS_ZLIB);
    wr64(b, seg_off + 0x18, VC_ENCRYPT_YES);

    vc_module m;
    char err[256];
    int rc = vc_parse(b, sizeof(b), &m, err, sizeof(err));

    assert(rc == 0);
    assert(m.kind == VC_SELF);
    assert(m.sce.version == 3);
    assert(m.sce.elf_filesize == 0x35BE70);
    assert(m.sce.self_filesize == 0x19A2A2);
    assert(m.sce.self_offset == 4);
    /* The landmark check: elf_offset must actually point at ELF magic. This is
     * the assertion that would have failed under the PS3 layout, where the
     * field at 0x30 reads as a plausible-looking 4. */
    assert(m.sce.elf_offset == elf_off);
    assert(b[m.sce.elf_offset] == 0x7F && b[m.sce.elf_offset + 1] == 'E');
    assert(m.have_appinfo);
    assert(m.appinfo.self_type == 4);
    assert(m.have_elf);
    assert(m.elf.type == VC_ET_SCE_RELEXEC);
    assert(m.elf.machine == VC_EM_ARM);
    assert(m.elf.entry == 0x81000000);
    assert(m.phdr_count == 1);
    assert(m.phdr[0].type == VC_PT_LOAD);
    assert(m.phdr[0].flags == 5);
    assert(m.seg_count == 1);
    assert(m.seg[0].compression == VC_COMPRESS_ZLIB);
    assert(m.seg[0].encryption == VC_ENCRYPT_YES);

    printf("  self roundtrip                 ok\n");
}

/* --- a plain ELF ------------------------------------------------------------ */

static void test_plain_elf(void) {
    static uint8_t b[0x400];
    memset(b, 0, sizeof(b));

    put_elf(b, 0, 0x100, VC_ET_EXEC);

    vc_module m;
    char err[256];
    assert(vc_parse(b, sizeof(b), &m, err, sizeof(err)) == 0);
    assert(m.kind == VC_ELF);
    assert(m.elf.type == VC_ET_EXEC);
    assert(m.phdr_count == 1);
    assert(m.phdr[0].vaddr == 0x81000000);

    /* e_type ET_SCE_RELEXEC must classify as velf, not plain ELF. */
    memset(b, 0, sizeof(b));
    put_elf(b, 0, 0x100, VC_ET_SCE_RELEXEC);
    assert(vc_parse(b, sizeof(b), &m, err, sizeof(err)) == 0);
    assert(m.kind == VC_VELF);

    printf("  plain elf / velf               ok\n");
}

/* --- refusing malformed input ----------------------------------------------- */
/*
 * Each of these would, in a parser without bounds checks, read past the end of
 * the buffer and report something plausible.
 */

static void test_rejects_malformed(void) {
    vc_module m;
    char err[256];

    /* Truncated: SCE magic but nothing after it. */
    static uint8_t tiny[0x10];
    memset(tiny, 0, sizeof(tiny));
    wr32(tiny, 0, VC_SCE_MAGIC);
    assert(vc_parse(tiny, sizeof(tiny), &m, err, sizeof(err)) != 0);
    assert(err[0] != '\0');

    /* Offsets that point past the end of the file. */
    static uint8_t bad[0x200];
    memset(bad, 0, sizeof(bad));
    wr32(bad, 0x00, VC_SCE_MAGIC);
    wr32(bad, 0x04, 3);
    wr64(bad, 0x30, 0xFFFFFFFFULL);   /* elf_offset way past the end */
    assert(vc_parse(bad, sizeof(bad), &m, err, sizeof(err)) != 0);

    /* Wrong architecture: a valid ELF that is not ARM. */
    static uint8_t notarm[0x400];
    memset(notarm, 0, sizeof(notarm));
    put_elf(notarm, 0, 0x100, VC_ET_EXEC);
    wr16(notarm, 0x12, 8 /* EM_MIPS */);
    assert(vc_parse(notarm, sizeof(notarm), &m, err, sizeof(err)) != 0);

    /* e_phentsize == 0 would make the program-header loop spin in place. */
    static uint8_t zero_ph[0x400];
    memset(zero_ph, 0, sizeof(zero_ph));
    put_elf(zero_ph, 0, 0x100, VC_ET_EXEC);
    wr16(zero_ph, 0x2A, 0);
    assert(vc_parse(zero_ph, sizeof(zero_ph), &m, err, sizeof(err)) != 0);

    /* Not a container at all — the PFS case. A NoNpDrm eboot is ciphertext
     * from byte zero, so it must be refused by name rather than parsed. */
    static uint8_t noise[0x400];
    for (size_t i = 0; i < sizeof(noise); i++) noise[i] = (uint8_t)(i * 37 + 11);
    assert(vc_parse(noise, sizeof(noise), &m, err, sizeof(err)) != 0);
    assert(strstr(err, "PFS") != NULL);

    printf("  rejects malformed input        ok\n");
}

/* --- a SELF as the ELF inside it -------------------------------------------- */

/* A SELF of one segment, 0x2000 bytes of it at file offset 0x1000 of an ELF
 * 0x3000 long: as a zlib stream of one stored block, or as it is. */
static size_t make_self(uint8_t *b, size_t cap, int zlib, int encrypted) {
    const uint64_t elf_off = 0xA0, phdr_off = 0x100, seg_off = 0x180, data_off = 0x1000;
    size_t n = 0x2000, at = (size_t)data_off;
    memset(b, 0, cap);
    wr32(b, 0x00, VC_SCE_MAGIC);
    wr32(b, 0x04, 3);
    wr16(b, 0x08, 0x00C0);
    wr16(b, 0x0A, 1);
    wr32(b, 0x0C, 0x600);
    wr64(b, 0x10, 0x1000);
    wr64(b, 0x18, 0x3000);       /* elf_filesize */
    wr64(b, 0x30, 4);
    wr64(b, 0x38, 0x80);
    wr64(b, 0x40, elf_off);
    wr64(b, 0x48, phdr_off);
    wr64(b, 0x58, seg_off);
    wr32(b, 0x80 + 0x0C, 4);
    put_elf(b, (size_t)elf_off, (size_t)phdr_off, VC_ET_SCE_RELEXEC);

    if (zlib) {
        uint32_t a = 1, s2 = 0;
        b[at++] = 0x78; b[at++] = 0x01;                     /* a zlib header */
        b[at++] = 0x01;                                     /* the last block, stored */
        wr16(b, at, (uint16_t)n); wr16(b, at + 2, (uint16_t)~n);
        at += 4;
        for (size_t i = 0; i < n; i++) {
            b[at + i] = (uint8_t)(i * 7 + (i >> 5));
            a = (a + b[at + i]) % 65521u;
            s2 = (s2 + a) % 65521u;
        }
        at += n;
        b[at++] = (uint8_t)(s2 >> 8); b[at++] = (uint8_t)s2;        /* Adler-32, high byte first */
        b[at++] = (uint8_t)(a >> 8);  b[at++] = (uint8_t)a;
    } else {
        for (size_t i = 0; i < n; i++) b[at + i] = (uint8_t)(i * 7 + (i >> 5));
        at += n;
    }
    wr64(b, seg_off + 0x00, data_off);
    wr64(b, seg_off + 0x08, at - data_off);
    wr64(b, seg_off + 0x10, zlib ? VC_COMPRESS_ZLIB : VC_COMPRESS_NONE);
    wr64(b, seg_off + 0x18, encrypted ? VC_ENCRYPT_YES : VC_ENCRYPT_NO);
    assert(at <= cap);
    return at;
}

static void test_extract(void) {
    static uint8_t b[0x4000];
    vc_module m;
    char err[256];
    uint8_t *elf;
    size_t elf_size;
    vc_placed placed[VC_MAX_SEGMENTS];

    for (int zlib = 0; zlib < 2; zlib++) {
        size_t size = make_self(b, sizeof(b), zlib, 0);
        assert(vc_parse(b, size, &m, err, sizeof(err)) == 0);
        assert(vc_extract(b, size, &m, &elf, &elf_size, placed, err, sizeof(err)) == 0);
        assert(elf && elf_size == 0x3000);
        assert(elf[0] == 0x7F && elf[1] == 'E' && elf[2] == 'L' && elf[3] == 'F');
        assert(memcmp(elf + 0x100, b + 0x100, 0x20) == 0);          /* the program header, where e_phoff says */
        assert(placed[0] == (zlib ? VC_PLACED_INFLATED : VC_PLACED_STORED));
        for (size_t i = 0; i < 0x2000; i++) assert(elf[0x1000 + i] == (uint8_t)(i * 7 + (i >> 5)));
        free(elf);
    }

    /* one byte of the stream wrong: the Adler-32 says so, and nothing comes back */
    {
        size_t size = make_self(b, sizeof(b), 1, 0);
        b[0x1000 + 7 + 0x800] ^= 0x10;
        assert(vc_parse(b, size, &m, err, sizeof(err)) == 0);
        assert(vc_extract(b, size, &m, &elf, &elf_size, NULL, err, sizeof(err)) == VC_EXTRACT_FAILED);
        assert(elf == NULL && elf_size == 0 && err[0]);
    }
    /* a stored segment that is not the length its program header says */
    {
        size_t size = make_self(b, sizeof(b), 0, 0);
        wr64(b, 0x180 + 0x08, 0x1FFF);
        assert(vc_parse(b, size, &m, err, sizeof(err)) == 0);
        assert(vc_extract(b, size, &m, &elf, &elf_size, NULL, err, sizeof(err)) == VC_EXTRACT_FAILED);
    }
    /* encrypted: named as that, and left alone */
    {
        size_t size = make_self(b, sizeof(b), 1, 1);
        assert(vc_parse(b, size, &m, err, sizeof(err)) == 0);
        assert(vc_extract(b, size, &m, &elf, &elf_size, NULL, err, sizeof(err)) == VC_EXTRACT_ENCRYPTED);
        assert(elf == NULL);
    }
    /* a plain ELF has nothing to extract */
    {
        static uint8_t e[0x400];
        memset(e, 0, sizeof(e));
        put_elf(e, 0, 0x34, VC_ET_SCE_RELEXEC);
        assert(vc_parse(e, sizeof(e), &m, err, sizeof(err)) == 0);
        assert(vc_extract(e, sizeof(e), &m, &elf, &elf_size, NULL, err, sizeof(err)) == VC_EXTRACT_FAILED);
    }

    printf("  extract                        ok\n");
}

int main(void) {
    printf("container:\n");
    test_self_roundtrip();
    test_plain_elf();
    test_rejects_malformed();
    test_extract();
    printf("all container tests passed\n");
    return 0;
}
