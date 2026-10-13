/* hle_io.c — SceIofilemgr: files.
 *
 * The C runtime opens the three standard streams at start-up by name
 * ("tty0:" and friends) and writes to them through sceIoWrite; those are the
 * host's stdout and stderr. Everything else is a path under a mount the
 * host declared (vita_io_mount): "app0:" is the title's data, "ux0:" is
 * where it writes. A path under no mount is reported missing, which is
 * honest, and a program that copes with a missing file keeps running.
 */

#include "vitarecomp/hle.h"
#include "vitarecomp/host.h"
#include "vitarecomp/recomp_rt.h"
#include "hle_vfs.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#define io_seek(f, off, wh) _fseeki64(f, off, wh)
#define io_tell(f)          _ftelli64(f)
#define host_stat           _stat64
#define host_stat_t         struct __stat64
#else
#define io_seek(f, off, wh) fseeko(f, off, wh)
#define io_tell(f)          ftello(f)
#define host_stat           stat
#define host_stat_t         struct stat
#endif

#define SCE_ERROR_ERRNO_ENOENT 0x80010002u
#define SCE_ERROR_ERRNO_EBADF  0x80010009u
#define SCE_ERROR_ERRNO_EINVAL 0x80010016u

#define SCE_O_RDONLY 1
#define SCE_O_WRONLY 2
#define SCE_O_APPEND 0x100
#define SCE_O_CREAT  0x200
#define SCE_O_TRUNC  0x400

#define FD_FIRST 0x40200000u
#define MAX_FDS  32

typedef struct { int used; FILE *stream; int is_file; } fd_entry;
static fd_entry g_fds[MAX_FDS];

static int fd_index(uint32_t fd) {
    if (fd < FD_FIRST || fd >= FD_FIRST + MAX_FDS) return -1;
    int i = (int)(fd - FD_FIRST);
    return g_fds[i].used ? i : -1;
}

/* --- mounts --------------------------------------------------------------------- */

#define MAX_MOUNTS 8
static struct { char prefix[16]; char dir[512]; } g_mounts[MAX_MOUNTS];
static int g_nmounts;

void vita_io_mount(const char *prefix, const char *host_dir) {
    if (g_nmounts == MAX_MOUNTS) return;
    size_t n = strlen(prefix);
    if (n && prefix[n - 1] == ':') n--;
    if (n >= sizeof(g_mounts[0].prefix)) n = sizeof(g_mounts[0].prefix) - 1;
    memcpy(g_mounts[g_nmounts].prefix, prefix, n);
    g_mounts[g_nmounts].prefix[n] = '\0';
    snprintf(g_mounts[g_nmounts].dir, sizeof(g_mounts[0].dir), "%s", host_dir);
    g_nmounts++;
}

/* Whether a host has said this path is not to be found, whatever the
 * mounts hold: VITARECOMP_IO_HIDE names such paths as a program names them,
 * "app0:patch.bin", several with a semicolon between. A folder of a title's
 * data can hold more than the title's module was made for (an update's
 * archive beside a module from before the update), and a module that finds
 * it reads what it cannot use. Slashes after the colon are not part of the
 * name, and case is not looked at. */
static int hidden(const char *guest) {
    static const char *list;
    static int asked;
    if (!asked) { list = getenv("VITARECOMP_IO_HIDE"); asked = 1; }
    if (!list || !*list) return 0;
    const char *colon = strchr(guest, ':');
    if (!colon) return 0;
    const char *rest = colon + 1;
    while (*rest == '/' || *rest == '\\') rest++;
    for (const char *at = list; *at;) {
        const char *end = strchr(at, ';');
        if (!end) end = at + strlen(at);
        const char *c = memchr(at, ':', (size_t)(end - at));
        if (c && c - at == colon - guest) {
            const char *want = c + 1, *have = rest;
            int same = 1;
            for (ptrdiff_t k = 0; k < c - at; k++)
                if ((at[k] | 0x20) != (guest[k] | 0x20)) { same = 0; break; }
            while (want < end && (*want == '/' || *want == '\\')) want++;
            for (; same && want < end; want++, have++) {
                const char a = *want == '\\' ? '/' : *want, b = *have == '\\' ? '/' : *have;
                if ((a >= 'A' && a <= 'Z' ? a + 32 : a) != (b >= 'A' && b <= 'Z' ? b + 32 : b)) same = 0;
            }
            if (same && !*have) return 1;
        }
        at = *end ? end + 1 : end;
    }
    return 0;
}

/* "app0:dir/file" or "app0:/dir/file" under the mount's directory, with the
 * console's separators turned into the host's. Case is kept: the dumps keep
 * the console's names, and the console's file system is case-sensitive.
 *
 * A prefix mounted more than once is an overlay, in the order given: the
 * first directory that has the file answers, which is how a title's patch
 * sits over its data on the console. A file nowhere resolves to the first
 * mount, so that a create lands somewhere definite. */
int vita_io_resolve(const char *guest, char *host, size_t cap) {
    const char *colon = strchr(guest, ':');
    if (!colon || hidden(guest)) return 0;
    size_t plen = (size_t)(colon - guest);
    int found = 0;
    for (int i = 0; i < g_nmounts; i++) {
        if (strlen(g_mounts[i].prefix) != plen) continue;
        int same = 1;
        for (size_t k = 0; k < plen; k++)
            if ((guest[k] | 0x20) != (g_mounts[i].prefix[k] | 0x20)) { same = 0; break; }
        if (!same) continue;
        const char *rest = colon + 1;
        while (*rest == '/' || *rest == '\\') rest++;
        char cand[1024];
        int n = snprintf(cand, sizeof(cand), "%s/%s", g_mounts[i].dir, rest);
        if (n < 0 || (size_t)n >= sizeof(cand) || (size_t)n >= cap) continue;
        for (char *c = cand; *c; c++) if (*c == '\\') *c = '/';
        host_stat_t st;
        if (!found) { snprintf(host, cap, "%s", cand); found = 1; }
        if (host_stat(cand, &st) == 0) { snprintf(host, cap, "%s", cand); return 1; }
    }
    return found;
}

/* A bounded copy of a guest string, for a path. */
static int guest_path(uint32_t addr, char *dst, size_t cap) {
    uint32_t avail = vita_mem_avail(addr);
    if (!avail) return 0;
    const char *s = (const char *)vita_mem_ptr(addr, 1);
    if (!s) return 0;
    size_t n = 0;
    while (n < cap - 1 && n < avail && s[n]) { dst[n] = s[n]; n++; }
    dst[n] = '\0';
    return 1;
}

static const char *open_mode(uint32_t flags) {
    int w = (flags & SCE_O_WRONLY) != 0, rd = (flags & SCE_O_RDONLY) != 0;
    if (flags & SCE_O_APPEND) return rd ? "a+b" : "ab";
    if (w && ((flags & SCE_O_TRUNC) || (flags & SCE_O_CREAT))) return rd ? "w+b" : "wb";
    if (w) return "r+b";
    return "rb";
}

/* SceUID sceIoOpen(const char *file, int flags, SceMode mode) */
void vita_hle_sceIoOpen(void) {
    char path[256];
    if (!guest_path(r0, path, sizeof(path))) { r0 = SCE_ERROR_ERRNO_EINVAL; return; }

    FILE *stream = NULL;
    int is_file = 0;
    if (strncmp(path, "tty0:", 5) == 0) stream = stdout;
    else if (strncmp(path, "tty1:", 5) == 0) stream = stderr;
    else if (strncmp(path, "tty", 3) == 0)   stream = stdout;
    else {
        char hpath[1024];
        if (!vita_io_resolve(path, hpath, sizeof(hpath))) {
            vita_hle_trace("sceIoOpen(\"%s\"): no mount for it, ENOENT", path);
            r0 = SCE_ERROR_ERRNO_ENOENT;
            return;
        }
        stream = fopen(hpath, open_mode(r1));
        if (!stream) {
            vita_hle_trace("sceIoOpen(\"%s\" -> \"%s\"): not found, ENOENT", path, hpath);
            r0 = SCE_ERROR_ERRNO_ENOENT;
            return;
        }
        is_file = 1;
    }
    int i;
    for (i = 0; i < MAX_FDS && g_fds[i].used; i++) {}
    if (i == MAX_FDS) { if (is_file) fclose(stream); r0 = SCE_ERROR_ERRNO_EINVAL; return; }
    g_fds[i].used = 1;
    g_fds[i].stream = stream;
    g_fds[i].is_file = is_file;
    r0 = FD_FIRST + (uint32_t)i;
    vita_hle_trace("sceIoOpen(\"%s\", flags 0x%X) -> 0x%08X", path, r1, r0);
}

/* int sceIoClose(SceUID fd) */
void vita_hle_sceIoClose(void) {
    int i = fd_index(r0);
    if (i < 0) { r0 = SCE_ERROR_ERRNO_EBADF; return; }
    if (g_fds[i].is_file) fclose(g_fds[i].stream);
    g_fds[i].used = 0;
    r0 = 0;
}

/* int sceIoWrite(SceUID fd, const void *data, SceSize size): bytes written */
void vita_hle_sceIoWrite(void) {
    int i = fd_index(r0);
    if (i < 0) { r0 = SCE_ERROR_ERRNO_EBADF; return; }
    uint32_t size = r2;
    if (size == 0) { r0 = 0; return; }
    const void *p = vita_mem_ptr(r1, size);
    if (!p) { r0 = SCE_ERROR_ERRNO_EINVAL; return; }
    vita_hle_trace("sceIoWrite(0x%08X, 0x%08X, %u)", r0, r1, size);
    fwrite(p, 1, size, g_fds[i].stream);
    fflush(g_fds[i].stream);
    r0 = size;
}

/* int sceIoRead(SceUID fd, void *data, SceSize size): bytes read; a console has none */
void vita_hle_sceIoRead(void) {
    int i = fd_index(r0);
    if (i < 0) { r0 = SCE_ERROR_ERRNO_EBADF; return; }
    if (!g_fds[i].is_file || r2 == 0) { r0 = 0; return; }
    void *p = vita_mem_ptr(r1, r2);
    if (!p) { r0 = SCE_ERROR_ERRNO_EINVAL; return; }
    r0 = (uint32_t)fread(p, 1, r2, g_fds[i].stream);
}

static int whence_of(uint32_t w) { return w == 1 ? SEEK_CUR : w == 2 ? SEEK_END : SEEK_SET; }

/* int sceIoLseek32(SceUID fd, int offset, int whence): the new position */
void vita_hle_sceIoLseek32(void) {
    int i = fd_index(r0);
    if (i < 0) { r0 = SCE_ERROR_ERRNO_EBADF; return; }
    if (!g_fds[i].is_file) { r0 = 0; return; }
    if (io_seek(g_fds[i].stream, (int64_t)(int32_t)r1, whence_of(r2)) != 0) { r0 = SCE_ERROR_ERRNO_EINVAL; return; }
    r0 = (uint32_t)io_tell(g_fds[i].stream);
}

/* SceOff sceIoLseek(SceUID fd, SceOff offset, int whence): the offset in r2:r3,
 * whence on the stack, a 64-bit result in r0:r1 */
void vita_hle_sceIoLseek(void) {
    int i = fd_index(r0);
    if (i < 0) { r0 = SCE_ERROR_ERRNO_EBADF; r1 = 0xFFFFFFFFu; return; }
    if (!g_fds[i].is_file) { r0 = 0; r1 = 0; return; }
    int64_t off = (int64_t)(((uint64_t)r3 << 32) | r2);
    if (io_seek(g_fds[i].stream, off, whence_of(vita_read32(sp))) != 0) { r0 = SCE_ERROR_ERRNO_EINVAL; r1 = 0xFFFFFFFFu; return; }
    int64_t pos = io_tell(g_fds[i].stream);
    r0 = (uint32_t)pos; r1 = (uint32_t)((uint64_t)pos >> 32);
}

/* SceIoStat: { SceMode st_mode; u32 st_attr; SceOff st_size; SceDateTime
 * st_ctime, st_atime, st_mtime (16 bytes each); u32 st_private[6] } = 88 bytes. */
static void write_iostat(uint32_t out, uint32_t mode, int64_t size) {
    for (uint32_t k = 0; k < 88; k += 4) vita_write32(out + k, 0);
    vita_write32(out + 0, mode);
    vita_write32(out + 8, (uint32_t)size);
    vita_write32(out + 12, (uint32_t)((uint64_t)size >> 32));
}

/* int sceIoGetstat(const char *file, SceIoStat *stat) */
void vita_hle_sceIoGetstat(void) {
    char path[256], hpath[1024];
    host_stat_t st;
    if (!guest_path(r0, path, sizeof(path)) || !vita_mem_ptr(r1, 88)) { r0 = SCE_ERROR_ERRNO_EINVAL; return; }
    if (!vita_io_resolve(path, hpath, sizeof(hpath)) || host_stat(hpath, &st) != 0) {
        vita_hle_trace("sceIoGetstat(\"%s\"): ENOENT", path);
        r0 = SCE_ERROR_ERRNO_ENOENT;
        return;
    }
    uint32_t mode = (st.st_mode & S_IFDIR) ? (0x1000u | 0x1FFu) : (0x2000u | 0x1FFu);
    write_iostat(r1, mode, (int64_t)st.st_size);
    vita_hle_trace("sceIoGetstat(\"%s\") -> size %lld", path, (long long)st.st_size);
    r0 = 0;
}

/* int sceIoRename(const char *oldname, const char *newname) */
void vita_hle_sceIoRename(void) {
    char a[256], b[256], ha[1024], hb[1024];
    if (!guest_path(r0, a, sizeof(a)) || !guest_path(r1, b, sizeof(b))) { r0 = SCE_ERROR_ERRNO_EINVAL; return; }
    if (!vita_io_resolve(a, ha, sizeof(ha)) || !vita_io_resolve(b, hb, sizeof(hb))) { r0 = SCE_ERROR_ERRNO_ENOENT; return; }
    r0 = rename(ha, hb) == 0 ? 0 : SCE_ERROR_ERRNO_ENOENT;
}

/* int sceIoRemove(const char *file) */
void vita_hle_sceIoRemove(void) {
    char a[256], ha[1024];
    if (!guest_path(r0, a, sizeof(a))) { r0 = SCE_ERROR_ERRNO_EINVAL; return; }
    if (!vita_io_resolve(a, ha, sizeof(ha))) { r0 = SCE_ERROR_ERRNO_ENOENT; return; }
    r0 = remove(ha) == 0 ? 0 : SCE_ERROR_ERRNO_ENOENT;
}

/* int sceIoGetstatByFd(SceUID fd, SceIoStat *stat)
 *
 * SceIoStat: { SceMode st_mode; u32 st_attr; SceOff st_size; SceDateTime
 * st_ctime, st_atime, st_mtime (16 bytes each); u32 st_private[6] } = 88
 * bytes. A console stream reports as a zero-length regular file, which is
 * what newlib's fstat needs to decide how to buffer it. */
void vita_hle_sceIoGetstatByFd(void) {
    int i = fd_index(r0);
    if (i < 0) { r0 = SCE_ERROR_ERRNO_EBADF; return; }
    if (!vita_mem_ptr(r1, 88)) { r0 = SCE_ERROR_ERRNO_EINVAL; return; }
    int64_t size = 0;
    if (g_fds[i].is_file) {
        int64_t here = io_tell(g_fds[i].stream);
        io_seek(g_fds[i].stream, 0, SEEK_END);
        size = io_tell(g_fds[i].stream);
        io_seek(g_fds[i].stream, here, SEEK_SET);
    }
    write_iostat(r1, 0x2000u | 0x1FFu, size);   /* SCE_S_IFREG | rwxrwxrwx */
    r0 = 0;
}

/* int sceIoSyncByFd(SceUID fd, int flag) */
void vita_hle_sceIoSyncByFd(void) {
    int i = fd_index(r0);
    if (i < 0) { r0 = SCE_ERROR_ERRNO_EBADF; return; }
    fflush(g_fds[i].stream);
    r0 = 0;
}
