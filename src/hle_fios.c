/* hle_fios.c — SceFios2: the file I/O streaming library, synchronously.
 *
 * FIOS2 is how a retail title reads its data: handles opened on paths that
 * name a mount ("app0:gamedata.bin"), reads issued as operations that
 * complete later, and a set of calls to wait on, poll and discard those
 * operations. Here every operation completes before its call returns, which
 * is a legal outcome of the asynchronous interface: an op that is already
 * done when the program first asks about it. The results are kept until the
 * program deletes the op, as the console does.
 *
 * The prototypes follow the FIOS2 interface as the SDK publishes it for this
 * platform (the same one as on the two consoles before and after it);
 * vita-headers carries only its overlay types. Where a 64-bit argument sits
 * matters more than usual: a SceFiosSize or SceFiosOffset after an odd
 * register goes to the stack, eight-byte aligned, and the register it would
 * have straddled is skipped.
 *
 * The paths resolve through the mounts the host declared (hle_io.c,
 * vita_io_mount). Calling convention: see hle.h.
 */

#include "vitarecomp/hle.h"
#include "vitarecomp/host.h"
#include "vitarecomp/recomp_rt.h"
#include "hle_vfs.h"
#include "psarc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* The archive a title ships is bigger than 2 GB, and a stat that cannot
 * report that size says the file does not exist. */
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define fios_seek(f, off, wh) _fseeki64(f, off, wh)
#define fios_tell(f)          _ftelli64(f)
#define fios_mkdir(p)         _mkdir(p)
#define host_stat             _stat64
#define host_stat_t           struct __stat64
#else
#include <dirent.h>
#include <unistd.h>
#define fios_seek(f, off, wh) fseeko(f, off, wh)
#define fios_tell(f)          ftello(f)
#define fios_mkdir(p)         mkdir(p, 0777)
#define host_stat             stat
#define host_stat_t           struct stat
#endif

#define SCE_FIOS_OK                 0
#define SCE_FIOS_ERROR_BAD_PATH     0x80280010u   /* a path no mount resolves; negative is what callers test */
#define SCE_FIOS_ERROR_BAD_FH       0x80280011u
#define SCE_FIOS_ERROR_BAD_OP       0x80280012u
#define SCE_FIOS_ERROR_BAD_ARG      0x80280013u
#define SCE_FIOS_ERROR_EOF          0x8028001Au

#define SCE_FIOS_STATUS_DIRECTORY   1
#define SCE_FIOS_STATUS_READABLE    2
#define SCE_FIOS_STATUS_WRITABLE    4

#define SCE_FIOS_O_READ   1
#define SCE_FIOS_O_WRITE  2
#define SCE_FIOS_O_APPEND 4
#define SCE_FIOS_O_CREAT  8
#define SCE_FIOS_O_TRUNC  16

#define MAX_FHS  64
#define MAX_DHS  16
#define MAX_OPS  256

typedef struct {
    int         used;
    FILE       *f;          /* a file of the host's, or NULL for one inside an archive */
    psarc_file *af;         /* the file inside an archive, and where its reads have got to */
    int64_t     pos;
    int         mount;      /* for the handle a mounted archive is known by: its mount + 1 */
    char        path[512];
    uint32_t    flags;
} fh_entry;
typedef struct { int used; char **names; int n, next; } dh_entry;
typedef struct {
    int      used;
    int32_t  error;        /* the op's result, as the sync call would return it */
    int64_t  actual;       /* bytes moved                                       */
    int64_t  requested;
    uint32_t buffer;       /* the guest buffer, for sceFiosOpGetBuffer          */
    uint32_t attr;
} op_entry;

static fh_entry g_fh[MAX_FHS];
static dh_entry g_dh[MAX_DHS];
static op_entry g_op[MAX_OPS];

static fh_entry *fh_get(uint32_t h) {
    if (h == 0 || h > MAX_FHS || !g_fh[h - 1].used) return NULL;
    return &g_fh[h - 1];
}

static dh_entry *dh_get(uint32_t h) {
    if (h == 0 || h > MAX_DHS || !g_dh[h - 1].used) return NULL;
    return &g_dh[h - 1];
}

static op_entry *op_get(uint32_t h) {
    if (h == 0 || h > MAX_OPS || !g_op[h - 1].used) return NULL;
    return &g_op[h - 1];
}

/* A finished operation, handed to the caller as the op handle. */
static uint32_t op_done(int32_t error, int64_t actual, int64_t requested, uint32_t buffer, uint32_t attr) {
    for (int i = 0; i < MAX_OPS; i++) {
        if (g_op[i].used) continue;
        g_op[i].used = 1; g_op[i].error = error; g_op[i].actual = actual;
        g_op[i].requested = requested; g_op[i].buffer = buffer; g_op[i].attr = attr;
        return (uint32_t)i + 1;
    }
    vita_trap_unimpl(0, MAX_OPS, "sceFios: too many undeleted ops");
    return 0;
}

static int guest_str(uint32_t addr, char *dst, size_t cap) {
    uint32_t avail = vita_mem_avail(addr);
    const char *s = avail ? (const char *)vita_mem_ptr(addr, 1) : NULL;
    if (!s) return 0;
    size_t n = 0;
    while (n < cap - 1 && n < avail && s[n]) { dst[n] = s[n]; n++; }
    dst[n] = '\0';
    return 1;
}

/* A 64-bit value from the stack, eight-byte aligned, at the given offset. */
static int64_t stack64(uint32_t off) {
    off = (off + 7) & ~7u;
    return (int64_t)(((uint64_t)vita_read32(sp + off + 4) << 32) | vita_read32(sp + off));
}

static void ret64(int64_t v) { r0 = (uint32_t)v; r1 = (uint32_t)((uint64_t)v >> 32); }

/* --- where a path leads -----------------------------------------------------------
 *
 * Three things stand between the path a program names and a file. An
 * overlay replaces the front of a path with another (sceFiosOverlayAdd): a
 * patch folder laid over the game's. An archive mounted at a point
 * (sceFiosArchiveMount) answers for every path under that point that it
 * holds a file for. And what neither claims is a file of the host's, under
 * the folder its mount stands for (hle_vfs.h).
 *
 * Mount points and overlay paths are compared the way the console's file
 * system compares them: without case, either slash, and a run of slashes
 * as one, so that "app0:/data" and "app0:data" are one place. */

#define MAX_MOUNTS   16
#define MAX_OVERLAYS 16

#define SCE_FIOS_OVERLAY_OPAQUE      0
#define SCE_FIOS_OVERLAY_TRANSLUCENT 1
#define SCE_FIOS_OVERLAY_NEWER       2
#define SCE_FIOS_OVERLAY_WRITABLE    3

typedef struct { int used; psarc *arc; char point[300]; uint32_t fh; } mount_entry;
typedef struct { int used; uint8_t type, order; char dst[300], src[300]; } overlay_entry;

static mount_entry   g_mount[MAX_MOUNTS];
static overlay_entry g_overlay[MAX_OVERLAYS];

typedef struct {
    int    kind;            /* 0 nowhere, 1 a host path, 2 a file in an archive, 3 a folder in one */
    char   host[1024];
    psarc *arc;
    int    index;
} where;

static char path_fold(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

/* A path in one spelling: forward slashes, a run of them as one, none
 * straight after the drive's colon and none at the end. "app0:/data//x/"
 * and "app0:data\x" both become "app0:data/x". */
static void path_canon(const char *in, char *out, size_t cap) {
    size_t n = 0, colon = 0;    /* colon: how much is written once the drive's colon is, 0 before */
    for (; *in && n + 1 < cap; in++) {
        const char c = *in == '\\' ? '/' : *in;
        if (c == '/' && n && (out[n - 1] == '/' || n == colon)) continue;
        out[n++] = c;
        if (c == ':' && !colon) colon = n;
    }
    while (n > 1 && out[n - 1] == '/') n--;
    out[n] = '\0';
}

/* If `path` is `prefix` or lies under it, what follows; NULL otherwise.
 * Both in the one spelling; letters compare without case. An empty prefix
 * and a lone slash are the root every path lies under. */
static const char *path_under(const char *path, const char *prefix) {
    size_t n = strlen(prefix);
    if (n == 1 && prefix[0] == '/') { prefix++; n = 0; }
    if (!n) return path[0] == '/' ? path + 1 : path;
    for (size_t i = 0; i < n; i++)
        if (path_fold(path[i]) != path_fold(prefix[i])) return NULL;
    if (!path[n]) return path + n;
    if (prefix[n - 1] == ':') return path + n;
    return path[n] == '/' ? path + n + 1 : NULL;
}

/* `rest` under `root`, in the one spelling. */
static void path_join(char *out, size_t cap, const char *root, const char *rest) {
    const size_t n = strlen(root);
    const int bare = !n || !*rest || root[n - 1] == ':' || root[n - 1] == '/';
    snprintf(out, cap, "%s%s%s", root, bare ? "" : "/", rest);
}

static int host_exists(const char *gpath, char *hpath, size_t cap) {
    host_stat_t st;
    return vita_io_resolve(gpath, hpath, cap) && host_stat(hpath, &st) == 0;
}

/* A path past the overlays: in an archive, or on the host. `must_exist`
 * zero is a path about to be written, which no archive takes. */
static void locate_plain(const char *gpath, where *w, int must_exist) {
    memset(w, 0, sizeof *w);
    for (int i = 0; i < MAX_MOUNTS && must_exist; i++) {
        if (!g_mount[i].used) continue;
        const char *rest = path_under(gpath, g_mount[i].point);
        if (!rest) continue;
        const int index = psarc_find(g_mount[i].arc, rest);
        if (index > 0) { w->kind = 2; w->arc = g_mount[i].arc; w->index = index; return; }
        if (*rest && psarc_has_dir(g_mount[i].arc, rest)) { w->kind = 3; w->arc = g_mount[i].arc; return; }
    }
    if (must_exist) {
        if (host_exists(gpath, w->host, sizeof w->host)) w->kind = 1;
    } else if (vita_io_resolve(gpath, w->host, sizeof w->host)) {
        w->kind = 1;
    }
}

/* Where a path leads, the overlays applied in their order. Returns 0 when
 * it leads nowhere. */
static int locate(const char *named, where *w, int for_write) {
    char gpath[1024], moved[1024];
    path_canon(named, gpath, sizeof gpath);
    for (int order = 0; order < 256; order++) {
        for (int i = 0; i < MAX_OVERLAYS; i++) {
            const overlay_entry *o = &g_overlay[i];
            if (!o->used || o->order != order) continue;
            const char *rest = path_under(gpath, o->dst);
            if (!rest) continue;
            path_join(moved, sizeof moved, o->src, rest);
            if (o->type == SCE_FIOS_OVERLAY_OPAQUE || (for_write && o->type == SCE_FIOS_OVERLAY_WRITABLE)) {
                locate_plain(moved, w, !for_write);
                return w->kind != 0;
            }
            if (for_write) continue;
            locate_plain(moved, w, 1);
            if (w->kind) return 1;
        }
    }
    locate_plain(gpath, w, !for_write);
    return w->kind != 0;
}

/* --- the stat the console reports -------------------------------------------------
 * SceFiosStat: { SceFiosOffset fileSize; SceFiosDate accessDate, modificationDate,
 * creationDate; SceFiosStatus statFlags; uint32_t reserved; int64 uid, gid, dev,
 * ino, mode } = 80 bytes. Dates are microseconds since 0001-01-01. */
#define FIOS_STAT_SIZE 80
#define FIOS_EPOCH_OFFSET_S 62135596800ull

static void write64(uint32_t addr, uint64_t v) { vita_write32(addr, (uint32_t)v); vita_write32(addr + 4, (uint32_t)(v >> 32)); }

static int write_stat(uint32_t out, const char *host_path) {
    host_stat_t st;
    if (host_stat(host_path, &st) != 0) return 0;
    if (!vita_mem_ptr(out, FIOS_STAT_SIZE)) return 0;
    for (uint32_t k = 0; k < FIOS_STAT_SIZE; k += 4) vita_write32(out + k, 0);
    write64(out + 0,  (uint64_t)st.st_size);
    write64(out + 8,  ((uint64_t)st.st_atime + FIOS_EPOCH_OFFSET_S) * 1000000ull);
    write64(out + 16, ((uint64_t)st.st_mtime + FIOS_EPOCH_OFFSET_S) * 1000000ull);
    write64(out + 24, ((uint64_t)st.st_ctime + FIOS_EPOCH_OFFSET_S) * 1000000ull);
    uint32_t flags = SCE_FIOS_STATUS_READABLE | SCE_FIOS_STATUS_WRITABLE;
    if (st.st_mode & S_IFDIR) flags |= SCE_FIOS_STATUS_DIRECTORY;
    vita_write32(out + 32, flags);
    write64(out + 72, (uint64_t)st.st_mode);
    return 1;
}

/* The same for what an archive holds: a size, and nothing else it knows. */
static int write_stat_packed(uint32_t out, uint64_t size, int is_dir) {
    if (!vita_mem_ptr(out, FIOS_STAT_SIZE)) return 0;
    for (uint32_t k = 0; k < FIOS_STAT_SIZE; k += 4) vita_write32(out + k, 0);
    write64(out + 0, size);
    vita_write32(out + 32, SCE_FIOS_STATUS_READABLE | (is_dir ? SCE_FIOS_STATUS_DIRECTORY : 0));
    return 1;
}

static int write_stat_where(uint32_t out, const where *w) {
    if (w->kind == 1) return write_stat(out, w->host);
    if (w->kind == 2) return write_stat_packed(out, psarc_size(w->arc, w->index), 0);
    return w->kind == 3 ? write_stat_packed(out, 0, 1) : 0;
}

/* --- set-up ---------------------------------------------------------------------- */

/* int sceFiosInitialize(const SceFiosParams *params): the thread counts and
 * storage the program offers are not needed. */
void vita_hle_sceFiosInitialize(void) {
    vita_hle_trace("sceFiosInitialize(params 0x%08X)", r0);
    r0 = SCE_FIOS_OK;
}

void vita_hle_sceFiosTerminate(void) { }

/* void sceFiosShutdownAndCancelOps(void) */
void vita_hle_sceFiosShutdownAndCancelOps(void) { }

/* SceBool sceFiosIsValidHandle(SceFiosHandle h): any of the three kinds */
void vita_hle_sceFiosIsValidHandle(void) {
    r0 = (fh_get(r0) || dh_get(r0) || op_get(r0)) ? 1u : 0u;
}

/* --- files ------------------------------------------------------------------------ */

static const char *open_mode(uint32_t flags) {
    int w = (flags & SCE_FIOS_O_WRITE) != 0, rd = (flags & SCE_FIOS_O_READ) != 0;
    if (flags & SCE_FIOS_O_APPEND) return rd ? "a+b" : "ab";
    if (w && ((flags & SCE_FIOS_O_TRUNC) || (flags & SCE_FIOS_O_CREAT))) return rd ? "w+b" : "wb";
    if (w) return "r+b";
    return "rb";
}

/* The open shared by the sync and async forms: attr, SceFiosFH *outFH, path,
 * const SceFiosOpenParams *params { openFlags:16 ... }. */
static int32_t do_open(uint32_t out_fh, uint32_t path_addr, uint32_t params) {
    char gpath[512];
    where w;
    if (!guest_str(path_addr, gpath, sizeof(gpath))) return (int32_t)SCE_FIOS_ERROR_BAD_ARG;
    if (!vita_mem_ptr(out_fh, 4)) return (int32_t)SCE_FIOS_ERROR_BAD_ARG;
    uint32_t flags = params && vita_mem_ptr(params, 4) ? (vita_read32(params) & 0xFFFF) : SCE_FIOS_O_READ;
    const int writes = (flags & (SCE_FIOS_O_WRITE | SCE_FIOS_O_APPEND | SCE_FIOS_O_CREAT | SCE_FIOS_O_TRUNC)) != 0;
    vita_write32(out_fh, 0);
    if (!locate(gpath, &w, writes) || w.kind == 3) {
        vita_hle_trace("sceFiosFHOpen(\"%s\"): not found", gpath);
        return (int32_t)SCE_FIOS_ERROR_BAD_PATH;
    }
    int i;
    for (i = 0; i < MAX_FHS && g_fh[i].used; i++) {}
    if (i == MAX_FHS) return (int32_t)SCE_FIOS_ERROR_BAD_FH;
    memset(&g_fh[i], 0, sizeof g_fh[i]);
    if (w.kind == 2) {
        g_fh[i].af = psarc_file_open(w.arc, w.index);
        if (!g_fh[i].af) return (int32_t)SCE_FIOS_ERROR_BAD_PATH;
        snprintf(g_fh[i].path, sizeof(g_fh[i].path), "%s", gpath);
    } else {
        g_fh[i].f = fopen(w.host, open_mode(flags));
        if (!g_fh[i].f) {
            vita_hle_trace("sceFiosFHOpen(\"%s\" -> \"%s\"): not found", gpath, w.host);
            return (int32_t)SCE_FIOS_ERROR_BAD_PATH;
        }
        snprintf(g_fh[i].path, sizeof(g_fh[i].path), "%s", w.host);
    }
    g_fh[i].used = 1; g_fh[i].flags = flags;
    vita_write32(out_fh, (uint32_t)i + 1);
    vita_hle_trace("sceFiosFHOpen(\"%s\", flags 0x%X) -> fh %d%s", gpath, flags, i + 1, w.kind == 2 ? ", in an archive" : "");
    return SCE_FIOS_OK;
}

/* A handle's size and position, whichever kind of file it is. */
static int64_t fh_size(fh_entry *e) {
    if (e->af) return (int64_t)psarc_file_size(e->af);
    if (!e->f) return 0;
    int64_t here = fios_tell(e->f);
    fios_seek(e->f, 0, SEEK_END);
    int64_t size = fios_tell(e->f);
    fios_seek(e->f, here, SEEK_SET);
    return size;
}
static int64_t fh_tell(fh_entry *e) { return e->f ? fios_tell(e->f) : e->pos; }

/* int sceFiosFHOpenSync(attr, SceFiosFH *outFH, const char *path, const SceFiosOpenParams *params) */
void vita_hle_sceFiosFHOpenSync(void) { r0 = (uint32_t)do_open(r1, r2, r3); }

/* SceFiosOp sceFiosFHOpen(attr, outFH, path, params) */
void vita_hle_sceFiosFHOpen(void) {
    int32_t e = do_open(r1, r2, r3);
    r0 = op_done(e, 0, 0, 0, r0);
}

static int32_t do_close(uint32_t h) {
    fh_entry *e = fh_get(h);
    if (!e) return (int32_t)SCE_FIOS_ERROR_BAD_FH;
    if (e->mount) return (int32_t)SCE_FIOS_ERROR_BAD_FH;    /* an archive is unmounted, not closed */
    if (e->f) fclose(e->f);
    psarc_file_close(e->af);
    e->used = 0;
    return SCE_FIOS_OK;
}

/* int sceFiosFHCloseSync(attr, SceFiosFH fh) */
void vita_hle_sceFiosFHCloseSync(void) { r0 = (uint32_t)do_close(r1); }
/* SceFiosOp sceFiosFHClose(attr, fh) */
void vita_hle_sceFiosFHClose(void) { r0 = op_done(do_close(r1), 0, 0, 0, r0); }

/* SceFiosSize sceFiosFHGetSize(SceFiosFH fh): 64 bits in r0:r1 */
void vita_hle_sceFiosFHGetSize(void) {
    fh_entry *e = fh_get(r0);
    if (!e) { ret64(-1); return; }
    int64_t size = fh_size(e);
    vita_hle_trace("sceFiosFHGetSize(fh %d) -> %lld", (int)r0, (long long)size);
    ret64(size);
}

/* SceFiosOffset sceFiosFHTell(SceFiosFH fh) */
void vita_hle_sceFiosFHTell(void) {
    fh_entry *e = fh_get(r0);
    ret64(e ? fh_tell(e) : -1);
}

/* SceFiosOffset sceFiosFHSeek(SceFiosFH fh, SceFiosOffset offset, SceFiosWhence whence):
 * fh in r0, the offset in r2:r3, whence on the stack. */
void vita_hle_sceFiosFHSeek(void) {
    fh_entry *e = fh_get(r0);
    int64_t off = (int64_t)(((uint64_t)r3 << 32) | r2);
    uint32_t whence = vita_read32(sp);
    if (!e) { ret64(-1); return; }
    if (!e->f) {
        const int64_t to = off + (whence == 1 ? e->pos : whence == 2 ? fh_size(e) : 0);
        if (to < 0) { ret64(-1); return; }
        e->pos = to;
    } else {
        int w = whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET;
        if (fios_seek(e->f, off, w) != 0) { ret64(-1); return; }
    }
    vita_hle_trace("sceFiosFHSeek(fh %d, %lld, %u) -> %lld", (int)r0, (long long)off, whence, (long long)fh_tell(e));
    ret64(fh_tell(e));
}

/* The read shared by the forms: at the file's position, or at `offset` when
 * pread is non-zero, into a guest buffer, `length` bytes. */
static int64_t do_read(uint32_t fh, uint32_t buf, int64_t length, int pread, int64_t offset, int32_t *err) {
    fh_entry *e = fh_get(fh);
    *err = SCE_FIOS_OK;
    if (!e) { *err = (int32_t)SCE_FIOS_ERROR_BAD_FH; return -1; }
    if (length < 0) { *err = (int32_t)SCE_FIOS_ERROR_BAD_ARG; return -1; }
    if (length == 0) return 0;
    void *p = vita_mem_ptr(buf, (uint32_t)length);
    if (!p) { vita_trap_unimpl(buf, (uint32_t)length, "sceFiosFHRead: buffer not backed"); *err = (int32_t)SCE_FIOS_ERROR_BAD_ARG; return -1; }
    if (!e->f) {
        if (!e->af) { *err = (int32_t)SCE_FIOS_ERROR_BAD_FH; return -1; }
        const int64_t n = psarc_file_read(e->af, (uint64_t)(pread ? offset : e->pos), p, (uint64_t)length);
        if (n < 0) { *err = (int32_t)SCE_FIOS_ERROR_BAD_FH; return -1; }
        if (!pread) e->pos += n;
        return n;
    }
    if (pread) {
        int64_t here = fios_tell(e->f);
        fios_seek(e->f, offset, SEEK_SET);
        size_t n = fread(p, 1, (size_t)length, e->f);
        fios_seek(e->f, here, SEEK_SET);
        return (int64_t)n;
    }
    return (int64_t)fread(p, 1, (size_t)length, e->f);
}

/* SceFiosSize sceFiosFHReadSync(attr, SceFiosFH fh, void *buf, SceFiosSize length):
 * the length is 64-bit and r3 is odd, so it is on the stack. */
void vita_hle_sceFiosFHReadSync(void) {
    int32_t err;
    int64_t n = do_read(r1, r2, stack64(0), 0, 0, &err);
    vita_hle_trace("sceFiosFHReadSync(fh %d, 0x%08X, %lld) -> %lld", (int)r1, r2, (long long)stack64(0), (long long)n);
    ret64(err ? (int64_t)(int32_t)err : n);
}

/* SceFiosOp sceFiosFHRead(attr, fh, buf, length) */
void vita_hle_sceFiosFHRead(void) {
    int32_t err;
    int64_t length = stack64(0);
    int64_t n = do_read(r1, r2, length, 0, 0, &err);
    r0 = op_done(err, n < 0 ? 0 : n, length, r2, r0);
}

/* SceFiosSize sceFiosFHPreadSync(attr, fh, buf, SceFiosSize length, SceFiosOffset offset) */
void vita_hle_sceFiosFHPreadSync(void) {
    int32_t err;
    int64_t length = stack64(0), offset = stack64(8);
    int64_t n = do_read(r1, r2, length, 1, offset, &err);
    vita_hle_trace("sceFiosFHPreadSync(fh %d, 0x%08X, %lld @ %lld) -> %lld", (int)r1, r2, (long long)length, (long long)offset, (long long)n);
    ret64(err ? (int64_t)(int32_t)err : n);
}

/* SceFiosOp sceFiosFHPread(attr, fh, buf, length, offset) */
void vita_hle_sceFiosFHPread(void) {
    int32_t err;
    int64_t length = stack64(0), offset = stack64(8);
    int64_t n = do_read(r1, r2, length, 1, offset, &err);
    vita_hle_trace("sceFiosFHPread(fh %d, 0x%08X, %lld @ %lld) -> %lld", (int)r1, r2, (long long)length, (long long)offset, (long long)n);
    r0 = op_done(err, n < 0 ? 0 : n, length, r2, r0);
}

static int64_t do_write(uint32_t fh, uint32_t buf, int64_t length, int32_t *err) {
    fh_entry *e = fh_get(fh);
    *err = SCE_FIOS_OK;
    if (!e || !e->f) { *err = (int32_t)SCE_FIOS_ERROR_BAD_FH; return -1; }
    if (length <= 0) return 0;
    const void *p = vita_mem_ptr(buf, (uint32_t)length);
    if (!p) { *err = (int32_t)SCE_FIOS_ERROR_BAD_ARG; return -1; }
    return (int64_t)fwrite(p, 1, (size_t)length, e->f);
}

/* SceFiosSize sceFiosFHWriteSync(attr, fh, const void *buf, SceFiosSize length) */
void vita_hle_sceFiosFHWriteSync(void) {
    int32_t err;
    int64_t n = do_write(r1, r2, stack64(0), &err);
    ret64(err ? (int64_t)(int32_t)err : n);
}

/* SceFiosOp sceFiosFHWrite(attr, fh, buf, length) */
void vita_hle_sceFiosFHWrite(void) {
    int32_t err;
    int64_t length = stack64(0);
    int64_t n = do_write(r1, r2, length, &err);
    r0 = op_done(err, n < 0 ? 0 : n, length, r2, r0);
}

/* int sceFiosFHStatSync(attr, SceFiosFH fh, SceFiosStat *outStatus) */
void vita_hle_sceFiosFHStatSync(void) {
    fh_entry *e = fh_get(r1);
    if (!e) { r0 = SCE_FIOS_ERROR_BAD_FH; return; }
    if (e->af) { r0 = write_stat_packed(r2, psarc_file_size(e->af), 0) ? SCE_FIOS_OK : SCE_FIOS_ERROR_BAD_ARG; return; }
    r0 = write_stat(r2, e->path) ? SCE_FIOS_OK : SCE_FIOS_ERROR_BAD_ARG;
}

/* --- paths ------------------------------------------------------------------------ */

/* Whether a path leads to a file (`dir` 0) or a folder (`dir` 1). */
static int path_is(uint32_t path_addr, int dir, char *gpath, size_t cap) {
    where w;
    gpath[0] = '\0';
    if (!guest_str(path_addr, gpath, cap) || !locate(gpath, &w, 0)) return 0;
    if (w.kind == 1) {
        host_stat_t st;
        return host_stat(w.host, &st) == 0 && ((st.st_mode & S_IFDIR) != 0) == dir;
    }
    return (w.kind == 3) == dir;
}

/* SceBool sceFiosFileExistsSync(attr, const char *path): the answer itself.
 * The asynchronous sceFiosFileExists takes a SceBool * to write it to; the
 * synchronous form returns it, and the title calls it with two arguments.
 * Read as the three-argument form, the leftover in r2 was taken for a
 * pointer and written through. */
void vita_hle_sceFiosFileExistsSync(void) {
    char gpath[512];
    const int exists = path_is(r1, 0, gpath, sizeof(gpath));
    vita_hle_trace("sceFiosFileExistsSync(\"%s\") -> %d", gpath, exists);
    r0 = (uint32_t)exists;
}

/* SceFiosOp sceFiosFileExists(attr, const char *path, SceBool *outExists) */
void vita_hle_sceFiosFileExists(void) {
    char gpath[512];
    const int exists = path_is(r1, 0, gpath, sizeof(gpath));
    if (vita_mem_ptr(r2, 4)) vita_write32(r2, (uint32_t)exists);
    r0 = op_done(SCE_FIOS_OK, 0, 0, r2, r0);
}

/* SceBool sceFiosDirectoryExistsSync(attr, const char *path) */
void vita_hle_sceFiosDirectoryExistsSync(void) {
    char gpath[512];
    r0 = (uint32_t)path_is(r1, 1, gpath, sizeof(gpath));
}

/* int sceFiosStatSync(attr, const char *path, SceFiosStat *outStatus) */
void vita_hle_sceFiosStatSync(void) {
    char gpath[512];
    where w;
    if (!guest_str(r1, gpath, sizeof(gpath)) || !locate(gpath, &w, 0)) { r0 = SCE_FIOS_ERROR_BAD_PATH; return; }
    r0 = write_stat_where(r2, &w) ? SCE_FIOS_OK : SCE_FIOS_ERROR_BAD_PATH;
    vita_hle_trace("sceFiosStatSync(\"%s\") -> 0x%08X", gpath, r0);
}

/* int sceFiosDirectoryCreateSync(attr, const char *path) */
void vita_hle_sceFiosDirectoryCreateSync(void) {
    char gpath[512];
    where w;
    if (!guest_str(r1, gpath, sizeof(gpath)) || !locate(gpath, &w, 1) || w.kind != 1) { r0 = SCE_FIOS_ERROR_BAD_PATH; return; }
    r0 = fios_mkdir(w.host) == 0 ? SCE_FIOS_OK : SCE_FIOS_ERROR_BAD_PATH;
}

/* int sceFiosFileDeleteSync(attr, const char *path) */
void vita_hle_sceFiosFileDeleteSync(void) {
    char gpath[512];
    where w;
    if (!guest_str(r1, gpath, sizeof(gpath)) || !locate(gpath, &w, 1) || w.kind != 1) { r0 = SCE_FIOS_ERROR_BAD_PATH; return; }
    r0 = remove(w.host) == 0 ? SCE_FIOS_OK : SCE_FIOS_ERROR_BAD_PATH;
}

/* --- overlays ----------------------------------------------------------------------
 *
 * SceFiosOverlay: { u8 type, order; u16 dst_len, src_len, pad; SceUID pid;
 * SceFiosOverlayID id; char dst[292], src[292] } = 0x258 bytes. A path that
 * begins with dst is looked for under src instead, by the type's policy:
 * always (opaque), when it is there (translucent, newer), or when it is
 * there and for every write (writable). "Newer" is taken as translucent:
 * which of two files is the newer is the patch's, by what a patch is. */
#define FIOS_OVERLAY_SIZE 0x258u
#define FIOS_OVERLAY_DST  16u
#define FIOS_OVERLAY_SRC  308u
#define FIOS_OVERLAY_PATH 292u

/* int sceFiosOverlayAdd(const SceFiosOverlay *pOverlay, SceFiosOverlayID *pOutID) */
void vita_hle_sceFiosOverlayAdd(void) {
    char dst[FIOS_OVERLAY_PATH + 1], src[FIOS_OVERLAY_PATH + 1];
    uint32_t in = r0, out_id = r1;
    int i;
    if (!vita_mem_ptr(in, FIOS_OVERLAY_SIZE)) { r0 = SCE_FIOS_ERROR_BAD_ARG; return; }
    for (i = 0; i < MAX_OVERLAYS && g_overlay[i].used; i++) {}
    if (i == MAX_OVERLAYS) { r0 = SCE_FIOS_ERROR_BAD_ARG; return; }
    guest_str(in + FIOS_OVERLAY_DST, dst, sizeof dst);
    guest_str(in + FIOS_OVERLAY_SRC, src, sizeof src);
    g_overlay[i].used = 1;
    g_overlay[i].type = vita_read8(in);
    g_overlay[i].order = vita_read8(in + 1);
    path_canon(dst, g_overlay[i].dst, sizeof g_overlay[i].dst);
    path_canon(src, g_overlay[i].src, sizeof g_overlay[i].src);
    if (vita_mem_ptr(out_id, 4)) vita_write32(out_id, (uint32_t)i + 1);
    vita_hle_trace("sceFiosOverlayAdd(type %u, order %u, \"%s\" is looked for under \"%s\") -> id %d",
                   g_overlay[i].type, g_overlay[i].order, dst, src, i + 1);
    r0 = SCE_FIOS_OK;
}

/* int sceFiosOverlayRemove(SceFiosOverlayID id) */
void vita_hle_sceFiosOverlayRemove(void) {
    if (r0 == 0 || r0 > MAX_OVERLAYS || !g_overlay[r0 - 1].used) { r0 = SCE_FIOS_ERROR_BAD_ARG; return; }
    g_overlay[r0 - 1].used = 0;
    r0 = SCE_FIOS_OK;
}

/* int sceFiosOverlayResolveSync(int resolveFlag, const char *pInPath, char *pOutPath,
 * size_t maxPath): the path the overlays lead to, as a path; flag 1 asks on
 * behalf of a write. What no overlay moves comes back as it was given. */
void vita_hle_sceFiosOverlayResolveSync(void) {
    char gpath[512], canon[1024], moved[1024];
    uint32_t out = r2, cap = r3;
    const int for_write = r0 == 1;
    if (!guest_str(r1, gpath, sizeof(gpath)) || !cap || !vita_mem_ptr(out, cap)) { r0 = SCE_FIOS_ERROR_BAD_ARG; return; }
    const char *answer = gpath;
    path_canon(gpath, canon, sizeof canon);
    for (int order = 0; order < 256 && answer == gpath; order++) {
        for (int i = 0; i < MAX_OVERLAYS; i++) {
            const overlay_entry *o = &g_overlay[i];
            where w;
            if (!o->used || o->order != order) continue;
            const char *rest = path_under(canon, o->dst);
            if (!rest) continue;
            path_join(moved, sizeof moved, o->src, rest);
            if (o->type != SCE_FIOS_OVERLAY_OPAQUE && !(for_write && o->type == SCE_FIOS_OVERLAY_WRITABLE)) {
                if (for_write) continue;
                locate_plain(moved, &w, 1);
                if (!w.kind) continue;
            }
            answer = moved;
            break;
        }
    }
    uint32_t n = (uint32_t)strlen(answer);
    if (n >= cap) n = cap - 1;
    for (uint32_t k = 0; k < n; k++) vita_write8(out + k, (uint8_t)answer[k]);
    vita_write8(out + n, 0);
    r0 = SCE_FIOS_OK;
}

/* --- filters and archives ----------------------------------------------------------
 *
 * An archive is read through a filter the program installs first: it names
 * the library's dearchiver (an imported function, never called from here)
 * and lends it a work buffer. Then the archive is mounted at a point, with
 * a buffer of the size the library asked for, and from then on a path under
 * that point is looked for in the archive before anywhere else. Here the
 * filter is noted and the buffers are left alone: the archive's table is
 * kept on the host (psarc.h). */

/* int sceFiosIOFilterAdd(int index, SceFiosIOFilterCallback cb, void *context) */
void vita_hle_sceFiosIOFilterAdd(void) {
    vita_hle_trace("sceFiosIOFilterAdd(%d, callback 0x%08X, context 0x%08X)", (int)r0, r1, r2);
    r0 = SCE_FIOS_OK;
}
/* int sceFiosIOFilterRemove(int index) */
void vita_hle_sceFiosIOFilterRemove(void) { r0 = SCE_FIOS_OK; }
/* int sceFiosIOFilterGetInfo(int index, SceFiosIOFilterCallback *outCb, void **outContext) */
void vita_hle_sceFiosIOFilterGetInfo(void) {
    if (vita_mem_ptr(r1, 4)) vita_write32(r1, 0);
    if (vita_mem_ptr(r2, 4)) vita_write32(r2, 0);
    r0 = SCE_FIOS_OK;
}
/* The two filters themselves: what a program passes to sceFiosIOFilterAdd.
 * Nothing calls them through the library here. */
void vita_hle_sceFiosIOFilterPsarcDearchiver(void) { }
void vita_hle_sceFiosIOFilterCache(void) { }

#define FIOS_MOUNT_BUFFER 0x10000

/* SceFiosSize sceFiosArchiveGetMountBufferSizeSync(attr, const char *archivePath,
 * const SceFiosOpenParams *params): what the mount will want lent to it, or
 * an error when there is no such archive. */
void vita_hle_sceFiosArchiveGetMountBufferSizeSync(void) {
    char gpath[512];
    where w;
    if (!guest_str(r1, gpath, sizeof(gpath)) || !locate(gpath, &w, 0) || w.kind != 1) {
        vita_hle_trace("sceFiosArchiveGetMountBufferSizeSync(\"%s\"): not found", gpath);
        ret64((int64_t)(int32_t)SCE_FIOS_ERROR_BAD_PATH);
        return;
    }
    vita_hle_trace("sceFiosArchiveGetMountBufferSizeSync(\"%s\") -> 0x%X", gpath, FIOS_MOUNT_BUFFER);
    ret64(FIOS_MOUNT_BUFFER);
}

/* The mount shared by the forms: SceFiosFH *outFH, the archive's path, the
 * mount point; the buffer (a pointer and a length, by value) and the open
 * parameters are on the stack and not needed. */
static int32_t do_mount(uint32_t out_fh, uint32_t path_addr, uint32_t point_addr) {
    char gpath[512], point[300], why[128];
    where w;
    int m, h;
    if (vita_mem_ptr(out_fh, 4)) vita_write32(out_fh, 0);
    if (!guest_str(path_addr, gpath, sizeof(gpath)) || !guest_str(point_addr, point, sizeof(point))) return (int32_t)SCE_FIOS_ERROR_BAD_ARG;
    if (!locate(gpath, &w, 0) || w.kind != 1) {
        vita_hle_trace("sceFiosArchiveMount(\"%s\" at \"%s\"): no such archive", gpath, point);
        return (int32_t)SCE_FIOS_ERROR_BAD_PATH;
    }
    for (m = 0; m < MAX_MOUNTS && g_mount[m].used; m++) {}
    for (h = 0; h < MAX_FHS && g_fh[h].used; h++) {}
    if (m == MAX_MOUNTS || h == MAX_FHS) return (int32_t)SCE_FIOS_ERROR_BAD_FH;
    psarc *arc = psarc_open(w.host, why, sizeof why);
    if (!arc) {
        vita_hle_trace("sceFiosArchiveMount(\"%s\" at \"%s\"): %s", gpath, point, why);
        return (int32_t)SCE_FIOS_ERROR_BAD_PATH;
    }
    memset(&g_fh[h], 0, sizeof g_fh[h]);
    g_fh[h].used = 1;
    g_fh[h].mount = m + 1;
    snprintf(g_fh[h].path, sizeof(g_fh[h].path), "%s", w.host);
    g_mount[m].used = 1;
    g_mount[m].arc = arc;
    g_mount[m].fh = (uint32_t)h + 1;
    path_canon(point, g_mount[m].point, sizeof g_mount[m].point);
    if (vita_mem_ptr(out_fh, 4)) vita_write32(out_fh, (uint32_t)h + 1);
    vita_hle_trace("sceFiosArchiveMount(\"%s\" at \"%s\") -> fh %d, %u files", gpath, point, h + 1, psarc_count(arc));
    return SCE_FIOS_OK;
}

/* int sceFiosArchiveMountSync(attr, SceFiosFH *outFH, const char *archivePath,
 * const char *mountPoint, SceFiosBuffer mountBuffer, const SceFiosOpenParams *params) */
void vita_hle_sceFiosArchiveMountSync(void) { r0 = (uint32_t)do_mount(r1, r2, r3); }
/* SceFiosOp sceFiosArchiveMount(the same) */
void vita_hle_sceFiosArchiveMount(void) { r0 = op_done(do_mount(r1, r2, r3), 0, 0, 0, r0); }

static int32_t do_unmount(uint32_t h) {
    fh_entry *e = fh_get(h);
    if (!e || !e->mount) return (int32_t)SCE_FIOS_ERROR_BAD_FH;
    mount_entry *m = &g_mount[e->mount - 1];
    /* a file still open in it keeps reading from an archive that is gone: close those first */
    for (int i = 0; i < MAX_FHS; i++)
        if (g_fh[i].used && g_fh[i].af) {
            vita_hle_trace("sceFiosArchiveUnmount: fh %d (\"%s\") was still open in it", i + 1, g_fh[i].path);
        }
    psarc_close(m->arc);
    m->used = 0;
    e->used = 0;
    return SCE_FIOS_OK;
}

/* int sceFiosArchiveUnmountSync(attr, SceFiosFH fh) / SceFiosOp sceFiosArchiveUnmount(attr, fh) */
void vita_hle_sceFiosArchiveUnmountSync(void) { r0 = (uint32_t)do_unmount(r1); }
void vita_hle_sceFiosArchiveUnmount(void)     { r0 = op_done(do_unmount(r1), 0, 0, 0, r0); }

/* int sceFiosPrintf(const char *fmt, ...): the library's own log line */
void vita_hle_sceFiosPrintf(void) { r0 = 0; }
/* --- directories ---------------------------------------------------------------- */

static int list_dir(const char *hpath, char ***names_out) {
    char **names = NULL;
    int n = 0, cap = 0;
#ifdef _WIN32
    char pattern[1100];
    snprintf(pattern, sizeof(pattern), "%s/*", hpath);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pattern, &fd);
    if (h == -1) return -1;
    do {
        if (strcmp(fd.name, ".") == 0 || strcmp(fd.name, "..") == 0) continue;
        if (n == cap) { cap = cap ? cap * 2 : 64; names = (char **)realloc(names, cap * sizeof(char *)); }
        names[n++] = _strdup(fd.name);
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
#else
    DIR *d = opendir(hpath);
    if (!d) return -1;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (n == cap) { cap = cap ? cap * 2 : 64; names = (char **)realloc(names, cap * sizeof(char *)); }
        names[n++] = strdup(de->d_name);
    }
    closedir(d);
#endif
    *names_out = names;
    return n;
}

/* int sceFiosDHOpenSync(attr, SceFiosDH *outDH, const char *path, SceFiosBuffer buf):
 * the buffer is passed by value, its pointer in r3 and its length on the stack,
 * and is not needed. */
void vita_hle_sceFiosDHOpenSync(void) {
    char gpath[512], hpath[1024];
    if (!vita_mem_ptr(r1, 4)) { r0 = SCE_FIOS_ERROR_BAD_ARG; return; }
    vita_write32(r1, 0);
    if (!guest_str(r2, gpath, sizeof(gpath)) || !vita_io_resolve(gpath, hpath, sizeof(hpath))) { r0 = SCE_FIOS_ERROR_BAD_PATH; return; }
    int i;
    for (i = 0; i < MAX_DHS && g_dh[i].used; i++) {}
    if (i == MAX_DHS) { r0 = SCE_FIOS_ERROR_BAD_ARG; return; }
    char **names = NULL;
    int n = list_dir(hpath, &names);
    if (n < 0) { r0 = SCE_FIOS_ERROR_BAD_PATH; return; }
    g_dh[i].used = 1; g_dh[i].names = names; g_dh[i].n = n; g_dh[i].next = 0;
    vita_write32(r1, (uint32_t)i + 1);
    vita_hle_trace("sceFiosDHOpenSync(\"%s\") -> dh %d, %d entries", gpath, i + 1, n);
    r0 = SCE_FIOS_OK;
}

/* int sceFiosDHReadSync(attr, SceFiosDH dh, SceFiosDirEntry *outEntry)
 * SceFiosDirEntry: { SceFiosStat stat; uint16_t offsetToName, nameLength,
 * fullPathLength, pad; char fullPath[1024] }. The full path is the directory
 * as the program named it is not kept, so the entry carries the name alone
 * and offsetToName is 0. */
void vita_hle_sceFiosDHReadSync(void) {
    dh_entry *d = dh_get(r1);
    if (!d) { r0 = SCE_FIOS_ERROR_BAD_ARG; return; }
    if (d->next >= d->n) { r0 = SCE_FIOS_ERROR_EOF; return; }
    uint32_t out = r2;
    if (!vita_mem_ptr(out, FIOS_STAT_SIZE + 8 + 1024)) { r0 = SCE_FIOS_ERROR_BAD_ARG; return; }
    const char *name = d->names[d->next++];
    for (uint32_t k = 0; k < FIOS_STAT_SIZE + 8; k += 4) vita_write32(out + k, 0);
    size_t len = strlen(name);
    if (len > 1023) len = 1023;
    vita_write16(out + FIOS_STAT_SIZE + 2, (uint16_t)len);
    vita_write16(out + FIOS_STAT_SIZE + 4, (uint16_t)len);
    for (size_t k = 0; k <= len; k++) vita_write8(out + FIOS_STAT_SIZE + 8 + (uint32_t)k, (uint8_t)(k < len ? name[k] : 0));
    r0 = SCE_FIOS_OK;
}

static int32_t do_dclose(uint32_t h) {
    dh_entry *d = dh_get(h);
    if (!d) return (int32_t)SCE_FIOS_ERROR_BAD_ARG;
    for (int i = 0; i < d->n; i++) free(d->names[i]);
    free(d->names);
    d->used = 0;
    return SCE_FIOS_OK;
}

/* int sceFiosDHCloseSync(attr, SceFiosDH dh) / SceFiosOp sceFiosDHClose(attr, dh) */
void vita_hle_sceFiosDHCloseSync(void) { r0 = (uint32_t)do_dclose(r1); }
void vita_hle_sceFiosDHClose(void)     { r0 = op_done(do_dclose(r1), 0, 0, 0, r0); }

/* --- operations, every one already finished ----------------------------------- */

/* int sceFiosOpSyncWait(SceFiosOp op): the op's error */
void vita_hle_sceFiosOpSyncWait(void) {
    op_entry *o = op_get(r0);
    r0 = o ? (uint32_t)o->error : SCE_FIOS_ERROR_BAD_OP;
}

/* SceFiosSize sceFiosOpSyncWaitForIO(SceFiosOp op): the bytes moved, or the error */
void vita_hle_sceFiosOpSyncWaitForIO(void) {
    op_entry *o = op_get(r0);
    if (!o) { ret64((int64_t)(int32_t)SCE_FIOS_ERROR_BAD_OP); return; }
    ret64(o->error ? (int64_t)o->error : o->actual);
}

/* int sceFiosOpWait(SceFiosOp op) */
void vita_hle_sceFiosOpWait(void) { vita_hle_sceFiosOpSyncWait(); }

/* SceBool sceFiosOpIsDone(SceFiosOp op) */
void vita_hle_sceFiosOpIsDone(void) { r0 = op_get(r0) ? 1u : 0u; }

/* SceBool sceFiosOpIsCancelled(SceFiosOp op) */
void vita_hle_sceFiosOpIsCancelled(void) { r0 = 0; }

/* void sceFiosOpCancel(SceFiosOp op): already done */
void vita_hle_sceFiosOpCancel(void) { }

/* int sceFiosOpGetError(SceFiosOp op) */
void vita_hle_sceFiosOpGetError(void) {
    op_entry *o = op_get(r0);
    r0 = o ? (uint32_t)o->error : SCE_FIOS_ERROR_BAD_OP;
}

/* SceFiosSize sceFiosOpGetActualCount(SceFiosOp op) */
void vita_hle_sceFiosOpGetActualCount(void) {
    op_entry *o = op_get(r0);
    ret64(o ? o->actual : 0);
}

/* SceFiosSize sceFiosOpGetRequestCount(SceFiosOp op) */
void vita_hle_sceFiosOpGetRequestCount(void) {
    op_entry *o = op_get(r0);
    ret64(o ? o->requested : 0);
}

/* void *sceFiosOpGetBuffer(SceFiosOp op) */
void vita_hle_sceFiosOpGetBuffer(void) {
    op_entry *o = op_get(r0);
    r0 = o ? o->buffer : 0;
}

/* const SceFiosOpAttr *sceFiosOpGetAttr(SceFiosOp op) */
void vita_hle_sceFiosOpGetAttr(void) {
    op_entry *o = op_get(r0);
    r0 = o ? o->attr : 0;
}

/* void sceFiosOpDelete(SceFiosOp op) */
void vita_hle_sceFiosOpDelete(void) {
    op_entry *o = op_get(r0);
    if (o) o->used = 0;
}

/* int sceFiosGetAllFHs(SceFiosFH *outArray, int arraySize): the count, and
 * as many handles as fit. The same for directories and ops. */
void vita_hle_sceFiosGetAllFHs(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < MAX_FHS; i++) if (g_fh[i].used) { if (n < r1 && vita_mem_ptr(r0 + n * 4, 4)) vita_write32(r0 + n * 4, i + 1); n++; }
    r0 = n;
}
void vita_hle_sceFiosGetAllDHs(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < MAX_DHS; i++) if (g_dh[i].used) { if (n < r1 && vita_mem_ptr(r0 + n * 4, 4)) vita_write32(r0 + n * 4, i + 1); n++; }
    r0 = n;
}
void vita_hle_sceFiosGetAllOps(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < MAX_OPS; i++) if (g_op[i].used) { if (n < r1 && vita_mem_ptr(r0 + n * 4, 4)) vita_write32(r0 + n * 4, i + 1); n++; }
    r0 = n;
}

/* --- dates -------------------------------------------------------------------------
 * SceFiosDate is microseconds since 0001-01-01; SceDateTime is
 * { u16 year, month, day, hour, minute, second; u32 microsecond }. */

static int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + doe - 719468;      /* days since 1970-01-01 */
}

/* SceFiosDate sceFiosDateFromSceDateTime(const SceDateTime *pDateTime) */
void vita_hle_sceFiosDateFromSceDateTime(void) {
    if (!vita_mem_ptr(r0, 16)) { ret64(0); return; }
    int y = vita_read16(r0), mo = vita_read16(r0 + 2), d = vita_read16(r0 + 4);
    int h = vita_read16(r0 + 6), mi = vita_read16(r0 + 8), s = vita_read16(r0 + 10);
    uint32_t us = vita_read32(r0 + 12);
    int64_t secs = (days_from_civil(y, mo, d) + (int64_t)FIOS_EPOCH_OFFSET_S / 86400) * 86400 + h * 3600 + mi * 60 + s;
    ret64(secs * 1000000 + us);
}

/* SceDateTime *sceFiosDateToSceDateTime(SceFiosDate date, SceDateTime *pDateTime):
 * the date in r0:r1, the output in r2. */
void vita_hle_sceFiosDateToSceDateTime(void) {
    uint64_t date = ((uint64_t)r1 << 32) | r0;
    uint32_t out = r2;
    if (!vita_mem_ptr(out, 16)) { r0 = 0; return; }
    uint64_t secs = date / 1000000ull;
    uint32_t us = (uint32_t)(date % 1000000ull);
    int64_t days = (int64_t)(secs / 86400) - (int64_t)(FIOS_EPOCH_OFFSET_S / 86400);
    uint32_t rem = (uint32_t)(secs % 86400);
    /* civil from days since 1970-01-01 */
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    vita_write16(out + 0, (uint16_t)y);
    vita_write16(out + 2, (uint16_t)m);
    vita_write16(out + 4, (uint16_t)d);
    vita_write16(out + 6, (uint16_t)(rem / 3600));
    vita_write16(out + 8, (uint16_t)(rem / 60 % 60));
    vita_write16(out + 10, (uint16_t)(rem % 60));
    vita_write32(out + 12, us);
    r0 = out;
}
