/* fbdemo — the smallest Vita program that draws and reads input.
 *
 * Freestanding: no newlib and no C library, so the firmware surface it needs
 * is exactly its import table and nothing arrives through a runtime it did
 * not ask for. Eight functions:
 *
 *   sceKernelAllocMemBlock, sceKernelGetMemBlockBase, sceKernelFreeMemBlock
 *   sceDisplaySetFrameBuf, sceDisplayWaitVblankStart
 *   sceCtrlSetSamplingMode, sceCtrlPeekBufferPositive
 *   sceKernelExitProcess
 *
 * It draws a gradient that cycles with the frame counter and a white square
 * the d-pad moves; START exits. That is enough to prove, end to end, that a
 * recompiled module loads, runs its own code against guest memory, reaches
 * firmware through bound imports, and puts its own pixels on a host window.
 *
 * Built with the vitasdk toolchain: see CMakeLists.txt and tools/wsl-vitasdk.sh.
 * MIT, like the rest of this repository.
 */

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>

#define WIDTH   960
#define HEIGHT  544
#define PITCH   960                              /* pixels per row            */
#define FB_SIZE 0x200000u                        /* 960*544*4 rounded up to the 256 KiB a memblock needs */

/* With no C library the compiler may still emit calls to these for aggregate
 * initialisation, so they are provided here rather than imported. */
void *memset(void *dst, int c, unsigned int n) {
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, unsigned int n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

static void draw(unsigned int *px, unsigned int frame, int bx, int by) {
    for (int y = 0; y < HEIGHT; y++) {
        unsigned int g = (unsigned int)(y * 255 / (HEIGHT - 1));
        unsigned int b = frame & 0xFF;
        for (int x = 0; x < WIDTH; x++) {
            unsigned int r = (unsigned int)(x * 255 / (WIDTH - 1));
            /* A8B8G8R8 in memory: R in the low byte. */
            px[y * PITCH + x] = 0xFF000000u | (b << 16) | (g << 8) | r;
        }
    }
    for (int y = by; y < by + 64 && y < HEIGHT; y++)
        for (int x = bx; x < bx + 64 && x < WIDTH; x++)
            px[y * PITCH + x] = 0xFFFFFFFFu;
}

int _start(SceSize args, void *argp) {
    (void)args; (void)argp;

    SceUID block = sceKernelAllocMemBlock("fbdemo_fb",
                                          SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                          FB_SIZE, NULL);
    if (block < 0) {
        sceKernelExitProcess(1);
        return 1;
    }

    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    unsigned int *px = (unsigned int *)base;

    SceDisplayFrameBuf fb;
    memset(&fb, 0, sizeof(fb));
    fb.size        = sizeof(fb);
    fb.base        = base;
    fb.pitch       = PITCH;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width       = WIDTH;
    fb.height      = HEIGHT;

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);

    int bx = (WIDTH - 64) / 2, by = (HEIGHT - 64) / 2;
    unsigned int frame = 0;

    for (;;) {
        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);

        if (pad.buttons & SCE_CTRL_START) break;
        if (pad.buttons & SCE_CTRL_LEFT  && bx > 0)            bx -= 4;
        if (pad.buttons & SCE_CTRL_RIGHT && bx < WIDTH - 64)   bx += 4;
        if (pad.buttons & SCE_CTRL_UP    && by > 0)            by -= 4;
        if (pad.buttons & SCE_CTRL_DOWN  && by < HEIGHT - 64)  by += 4;

        draw(px, frame, bx, by);
        sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
        sceDisplayWaitVblankStart();
        frame++;
    }

    sceKernelFreeMemBlock(block);
    sceKernelExitProcess(0);
    return 0;
}
