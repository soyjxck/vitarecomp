/* psarc.c — a PlayStation archive, read in place (psarc.h).
 *
 * The layout, every field big-endian:
 *
 *   0x00  "PSAR"
 *   0x04  version: major, minor (16 bits each); 1.4 is the one in use
 *   0x08  the compression, four letters: "zlib"
 *   0x0C  the length of the table of contents, this header included
 *   0x10  the size of one entry (30)
 *   0x14  the number of entries
 *   0x18  the block size (65536)
 *   0x1C  flags: 1 = names compare without case, 2 = names are absolute
 *   0x20  the entries: a 16-byte digest of the name, the index of the file's
 *         first block in the size table (32 bits), the file's length and
 *         its offset in the archive (40 bits each)
 *   then  the size table: for every block of every file, how many bytes it
 *         takes in the archive, in as many bytes as hold the block size
 *         (two for 65536). Zero says the block is a whole one, stored.
 *
 * Entry 0 is the manifest: the names of entries 1.., one to a line. A block
 * that holds as many bytes as it stands for is stored; any other is a zlib
 * stream. The digests are not used: the manifest gives every name, and a
 * sorted copy of it finds one.
 */

#include "psarc.h"

#include "inflate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define arc_seek(f, off) _fseeki64(f, (long long)(off), SEEK_SET)
#else
#define arc_seek(f, off) fseeko(f, (off_t)(off), SEEK_SET)
#endif

typedef struct {
    uint32_t first;     /* its first block's index in the size table */
    uint64_t size;
    uint64_t offset;
} entry;

typedef struct {
    const char *key;    /* the name, folded */
    uint32_t    index;  /* the entry */
} name_ref;

struct psarc {
    FILE     *f;
    uint32_t  block;
    uint32_t  entries;
    entry    *e;
    uint32_t  sizes_n;
    uint32_t *sizes;
    char     *manifest;     /* the names as written, NUL-separated */
    char    **names;        /* entry i's name is names[i - 1] */
    char     *folded;
    name_ref *sorted;
    uint32_t  files;
    uint8_t  *packed;       /* one block as the archive has it */
};

struct psarc_file {
    psarc   *a;
    entry    e;
    uint32_t at_block;      /* the block the cursor is at */
    uint64_t at_offset;     /* and where it begins in the archive */
    uint8_t *data;          /* the block in `have`, unpacked */
    uint32_t have;          /* its index, or ~0 for none */
    uint32_t have_len;
};

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t be40(const uint8_t *p) { return ((uint64_t)p[0] << 32) | be32(p + 1); }

static char fold(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c == '\\' ? '/' : c;
}

/* A name without its leading slashes, folded into `out`. */
static void fold_name(const char *name, char *out, size_t cap) {
    size_t n = 0;
    while (*name == '/' || *name == '\\') name++;
    for (; *name && n + 1 < cap; name++) out[n++] = fold(*name);
    out[n] = '\0';
}

static int by_key(const void *x, const void *y) { return strcmp(((const name_ref *)x)->key, ((const name_ref *)y)->key); }

/* How many bytes block `k` of a file takes in the archive, and how many it
 * stands for. */
static uint32_t block_plain(const psarc *a, const entry *e, uint32_t k) {
    const uint64_t left = e->size - (uint64_t)k * a->block;
    return left < a->block ? (uint32_t)left : a->block;
}
static uint32_t block_packed(const psarc *a, const entry *e, uint32_t k) {
    const uint32_t z = e->first + k < a->sizes_n ? a->sizes[e->first + k] : 0;
    return z ? z : block_plain(a, e, k);
}

/* A whole file into `dst`, which holds its size. 0 when it was read. */
static int read_whole(psarc *a, const entry *e, uint8_t *dst) {
    uint64_t done = 0;
    if (arc_seek(a->f, e->offset)) return -1;
    for (uint32_t k = 0; done < e->size; k++) {
        const uint32_t plain = block_plain(a, e, k), packed = block_packed(a, e, k);
        if (packed > a->block) return -1;
        if (packed == plain) {
            if (fread(dst + done, 1, plain, a->f) != plain) return -1;
        } else {
            size_t out = plain;
            if (fread(a->packed, 1, packed, a->f) != packed) return -1;
            if (inf_zlib(a->packed, packed, dst + done, &out) != INF_OK || out != plain) return -1;
        }
        done += plain;
    }
    return 0;
}

psarc *psarc_open(const char *host_path, char *why, size_t why_cap) {
    uint8_t head[0x20];
    uint8_t *toc = NULL;
    psarc *a = (psarc *)calloc(1, sizeof *a);
    if (why_cap) why[0] = '\0';
    if (!a) return NULL;
#define FAIL(text) do { snprintf(why, why_cap, "%s", text); psarc_close(a); free(toc); return NULL; } while (0)
    a->f = fopen(host_path, "rb");
    if (!a->f) FAIL("the file cannot be opened");
    if (fread(head, 1, sizeof head, a->f) != sizeof head || memcmp(head, "PSAR", 4)) FAIL("it does not begin with PSAR");
    if (memcmp(head + 8, "zlib", 4)) FAIL("its blocks are packed some way other than zlib");
    const uint32_t toc_len = be32(head + 0x0C), entry_size = be32(head + 0x10);
    a->entries = be32(head + 0x14);
    a->block = be32(head + 0x18);
    if (entry_size != 30 || a->entries < 1 || a->block < 0x100 || a->block > 0x1000000u
        || toc_len < 0x20 || (uint64_t)a->entries * 30 > toc_len - 0x20)
        FAIL("its table of contents makes no sense");
    toc = (uint8_t *)malloc(toc_len - 0x20);
    if (!toc || fread(toc, 1, toc_len - 0x20, a->f) != toc_len - 0x20) FAIL("its table of contents is cut short");

    uint32_t width = 1;
    while (width < 4 && ((uint64_t)1 << (8 * width)) < a->block) width++;
    a->e = (entry *)calloc(a->entries, sizeof *a->e);
    a->sizes_n = (toc_len - 0x20 - a->entries * 30) / width;
    a->sizes = (uint32_t *)calloc(a->sizes_n ? a->sizes_n : 1, sizeof *a->sizes);
    a->packed = (uint8_t *)malloc(a->block);
    if (!a->e || !a->sizes || !a->packed) FAIL("there is no memory for its table of contents");
    for (uint32_t i = 0; i < a->entries; i++) {
        const uint8_t *p = toc + (size_t)i * 30;
        a->e[i].first = be32(p + 16);
        a->e[i].size = be40(p + 20);
        a->e[i].offset = be40(p + 25);
    }
    const uint8_t *t = toc + (size_t)a->entries * 30;
    for (uint32_t i = 0; i < a->sizes_n; i++, t += width) {
        uint32_t v = 0;
        for (uint32_t b = 0; b < width; b++) v = (v << 8) | t[b];
        a->sizes[i] = v;
    }
    free(toc);
    toc = NULL;

    /* the manifest: the names of every entry after it, a line each */
    const uint64_t msize = a->e[0].size;
    if (msize > ((uint64_t)64 << 20)) FAIL("its manifest is far too long");
    a->manifest = (char *)malloc((size_t)msize + 1);
    a->folded = (char *)malloc((size_t)msize + 1);
    a->files = a->entries - 1;
    a->names = (char **)calloc(a->files ? a->files : 1, sizeof *a->names);
    a->sorted = (name_ref *)calloc(a->files ? a->files : 1, sizeof *a->sorted);
    if (!a->manifest || !a->folded || !a->names || !a->sorted) FAIL("there is no memory for its names");
    if (read_whole(a, &a->e[0], (uint8_t *)a->manifest)) FAIL("its manifest cannot be read");
    a->manifest[msize] = '\0';
    uint32_t n = 0;
    for (char *s = a->manifest; n < a->files && s <= a->manifest + msize; n++) {
        char *end = s;
        while (*end && *end != '\n') end++;
        const int last = !*end;
        *end = '\0';
        a->names[n] = s;
        if (last) { n++; break; }
        s = end + 1;
    }
    if (n != a->files) FAIL("its manifest names fewer files than it holds");
    for (uint32_t i = 0; i < a->files; i++) {
        char *key = a->folded + (a->names[i] - a->manifest);
        fold_name(a->names[i], key, strlen(a->names[i]) + 1);
        a->sorted[i].key = key;
        a->sorted[i].index = i + 1;
    }
    qsort(a->sorted, a->files, sizeof *a->sorted, by_key);
#undef FAIL
    return a;
}

void psarc_close(psarc *a) {
    if (!a) return;
    if (a->f) fclose(a->f);
    free(a->e);
    free(a->sizes);
    free(a->manifest);
    free(a->names);
    free(a->folded);
    free(a->sorted);
    free(a->packed);
    free(a);
}

uint32_t psarc_count(const psarc *a) { return a->files; }
const char *psarc_name(const psarc *a, uint32_t index) { return index < a->files ? a->names[index] : NULL; }

int psarc_find(const psarc *a, const char *name) {
    char key[1024];
    fold_name(name, key, sizeof key);
    uint32_t lo = 0, hi = a->files;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        const int c = strcmp(a->sorted[mid].key, key);
        if (!c) return (int)a->sorted[mid].index;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return -1;
}

uint64_t psarc_size(const psarc *a, int index) {
    return index > 0 && (uint32_t)index < a->entries ? a->e[index].size : 0;
}

int psarc_has_dir(const psarc *a, const char *dir) {
    char key[1024];
    fold_name(dir, key, sizeof key - 1);
    size_t n = strlen(key);
    while (n && key[n - 1] == '/') n--;
    if (!n) return a->files != 0;
    key[n++] = '/';
    key[n] = '\0';
    uint32_t lo = 0, hi = a->files;
    while (lo < hi) {   /* the first name not below the key */
        const uint32_t mid = lo + (hi - lo) / 2;
        if (strcmp(a->sorted[mid].key, key) < 0) lo = mid + 1; else hi = mid;
    }
    return lo < a->files && !strncmp(a->sorted[lo].key, key, n);
}

psarc_file *psarc_file_open(psarc *a, int index) {
    if (index <= 0 || (uint32_t)index >= a->entries) return NULL;
    psarc_file *f = (psarc_file *)calloc(1, sizeof *f);
    if (!f) return NULL;
    f->a = a;
    f->e = a->e[index];
    f->at_offset = f->e.offset;
    f->have = ~0u;
    f->data = (uint8_t *)malloc(a->block);
    if (!f->data) { free(f); return NULL; }
    return f;
}

void psarc_file_close(psarc_file *f) {
    if (!f) return;
    free(f->data);
    free(f);
}

uint64_t psarc_file_size(const psarc_file *f) { return f->e.size; }

/* The cursor to block `k`: forwards from where it is, or from the file's
 * first block when `k` is behind it. */
static void seek_block(psarc_file *f, uint32_t k) {
    if (k < f->at_block) { f->at_block = 0; f->at_offset = f->e.offset; }
    while (f->at_block < k) {
        f->at_offset += block_packed(f->a, &f->e, f->at_block);
        f->at_block++;
    }
}

/* Block `k` into `dst`, which holds a block. Its length, or -1. */
static int64_t fetch_block(psarc_file *f, uint32_t k, uint8_t *dst) {
    psarc *a = f->a;
    seek_block(f, k);
    const uint32_t plain = block_plain(a, &f->e, k), packed = block_packed(a, &f->e, k);
    if (packed > a->block || arc_seek(a->f, f->at_offset)) return -1;
    if (packed == plain) return fread(dst, 1, plain, a->f) == plain ? (int64_t)plain : -1;
    size_t out = plain;
    if (fread(a->packed, 1, packed, a->f) != packed) return -1;
    if (inf_zlib(a->packed, packed, dst, &out) != INF_OK || out != plain) return -1;
    return (int64_t)plain;
}

int64_t psarc_file_read(psarc_file *f, uint64_t offset, void *dst, uint64_t length) {
    psarc *a = f->a;
    uint8_t *out = (uint8_t *)dst;
    uint64_t done = 0;
    if (offset >= f->e.size) return 0;
    if (length > f->e.size - offset) length = f->e.size - offset;
    while (done < length) {
        const uint32_t k = (uint32_t)((offset + done) / a->block);
        const uint32_t in = (uint32_t)((offset + done) % a->block);
        const uint32_t plain = block_plain(a, &f->e, k);
        if (!in && length - done >= plain) {    /* a whole block: straight to the caller */
            if (fetch_block(f, k, out + done) < 0) return -1;
            done += plain;
            continue;
        }
        if (f->have != k) {
            const int64_t got = fetch_block(f, k, f->data);
            if (got < 0) { f->have = ~0u; return -1; }
            f->have = k;
            f->have_len = (uint32_t)got;
        }
        uint32_t take = f->have_len - in;
        if (take > length - done) take = (uint32_t)(length - done);
        memcpy(out + done, f->data + in, take);
        done += take;
    }
    return (int64_t)done;
}
