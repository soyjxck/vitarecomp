/* hle_io.c — SceIofilemgr: files, as far as a console needs them.
 *
 * The C runtime opens the three standard streams at start-up by name
 * ("tty0:" and friends) and writes to them through sceIoWrite. Those are the
 * host's stdout and stderr here. Everything else — real files on ux0:, app0:,
 * savedata — is the filesystem mapping milestone and is refused by name
 * until then: an open that fails with ENOENT is honest, and a program that
 * copes with a missing file is a program that keeps running.
 */

#include "vitarecomp/hle.h"
#include "vitarecomp/host.h"
#include "vitarecomp/recomp_rt.h"

#include <stdio.h>
#include <string.h>

#define SCE_ERROR_ERRNO_ENOENT 0x80010002u
#define SCE_ERROR_ERRNO_EBADF  0x80010009u
#define SCE_ERROR_ERRNO_EINVAL 0x80010016u

#define FD_FIRST 0x40200000u
#define MAX_FDS  16

typedef struct { int used; FILE *stream; } fd_entry;
static fd_entry g_fds[MAX_FDS];

static int fd_index(uint32_t fd) {
    if (fd < FD_FIRST || fd >= FD_FIRST + MAX_FDS) return -1;
    int i = (int)(fd - FD_FIRST);
    return g_fds[i].used ? i : -1;
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

/* SceUID sceIoOpen(const char *file, int flags, SceMode mode) */
void vita_hle_sceIoOpen(void) {
    char path[256];
    if (!guest_path(r0, path, sizeof(path))) { r0 = SCE_ERROR_ERRNO_EINVAL; return; }

    FILE *stream = NULL;
    if (strncmp(path, "tty0:", 5) == 0) stream = stdout;
    else if (strncmp(path, "tty1:", 5) == 0) stream = stderr;
    else if (strncmp(path, "tty", 3) == 0)   stream = stdout;

    if (!stream) {
        fprintf(stderr, "vitarecomp: sceIoOpen(\"%s\"): no filesystem yet, ENOENT\n", path);
        r0 = SCE_ERROR_ERRNO_ENOENT;
        return;
    }
    int i;
    for (i = 0; i < MAX_FDS && g_fds[i].used; i++) {}
    if (i == MAX_FDS) { r0 = SCE_ERROR_ERRNO_EINVAL; return; }
    g_fds[i].used = 1;
    g_fds[i].stream = stream;
    r0 = FD_FIRST + (uint32_t)i;
    vita_hle_trace("sceIoOpen(\"%s\", flags 0x%X) -> 0x%08X", path, r1, r0);
}

/* int sceIoClose(SceUID fd) */
void vita_hle_sceIoClose(void) {
    int i = fd_index(r0);
    if (i < 0) { r0 = SCE_ERROR_ERRNO_EBADF; return; }
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

/* int sceIoRead(SceUID fd, void *data, SceSize size): a console has nothing to read */
void vita_hle_sceIoRead(void) {
    int i = fd_index(r0);
    r0 = i < 0 ? SCE_ERROR_ERRNO_EBADF : 0;
}

/* int sceIoLseek32(SceUID fd, int offset, int whence) */
void vita_hle_sceIoLseek32(void) {
    int i = fd_index(r0);
    r0 = i < 0 ? SCE_ERROR_ERRNO_EBADF : 0;
}

/* SceOff sceIoLseek(SceUID fd, SceOff offset, int whence): 64-bit result in r0:r1 */
void vita_hle_sceIoLseek(void) {
    int i = fd_index(r0);
    if (i < 0) { r0 = SCE_ERROR_ERRNO_EBADF; r1 = 0xFFFFFFFFu; return; }
    r0 = 0; r1 = 0;
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
    for (uint32_t k = 0; k < 88; k += 4) vita_write32(r1 + k, 0);
    vita_write32(r1 + 0, 0x2000u | 0x1FFu);   /* SCE_S_IFREG | rwxrwxrwx */
    r0 = 0;
}

/* int sceIoSyncByFd(SceUID fd, int flag) */
void vita_hle_sceIoSyncByFd(void) {
    int i = fd_index(r0);
    if (i < 0) { r0 = SCE_ERROR_ERRNO_EBADF; return; }
    fflush(g_fds[i].stream);
    r0 = 0;
}
