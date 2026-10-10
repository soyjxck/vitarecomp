/* hello — the smallest newlib-linked Vita program that draws text.
 *
 * Where fbdemo is freestanding, this is what every real homebrew is: a C
 * `main` on top of vitasdk's newlib. The C runtime's start-up, heap, locks
 * and stdio arrive through the import table whether the program asks for
 * them or not, and that import table is the point of the sample: it is the
 * firmware surface a recompiled homebrew needs before its own code runs.
 *
 * It logs through sceClibPrintf, allocates with malloc, formats with
 * snprintf, and draws the result with a 5x7 font of its own. START exits
 * through main's return, which is newlib's exit().
 *
 * MIT, like the rest of this repository.
 */

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH   960
#define HEIGHT  544
#define PITCH   960
#define FB_SIZE 0x200000u

/* newlib's heap on the Vita is one memory block of this size. */
unsigned int _newlib_heap_size_user = 4 * 1024 * 1024;

/* --- a 5x7 font: digits, capitals, a few marks ----------------------------- */

static const uint8_t FONT[][7] = {
    /* 0-9 */
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    /* A-Z */
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, {0x07,0x02,0x02,0x02,0x12,0x12,0x0C},
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    /* 36: unknown glyph, a box */
    {0x1F,0x11,0x11,0x11,0x11,0x11,0x1F},
    /* 37: '-'   38: ':'   39: '.' */
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, {0x00,0x04,0x00,0x00,0x00,0x04,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x04},
};

static int glyph_index(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return 10 + (c - 'A');
    if (c >= 'a' && c <= 'z') return 10 + (c - 'a');
    if (c == '-') return 37;
    if (c == ':') return 38;
    if (c == '.') return 39;
    if (c == ' ') return -1;
    return 36;
}

static void draw_text(uint32_t *px, int x, int y, const char *s, uint32_t colour, int scale) {
    for (; *s; s++, x += 6 * scale) {
        int g = glyph_index(*s);
        if (g < 0) continue;
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (!(FONT[g][row] & (0x10 >> col))) continue;
                for (int dy = 0; dy < scale; dy++)
                    for (int dx = 0; dx < scale; dx++) {
                        int X = x + col * scale + dx, Y = y + row * scale + dy;
                        if (X >= 0 && X < WIDTH && Y >= 0 && Y < HEIGHT)
                            px[Y * PITCH + X] = colour;
                    }
            }
        }
    }
}

int main(void) {
    /* Two ways to say hello. printf goes into newlib's stdout buffer, which
     * on vitasdk is never flushed: the C library's internal reent (the one
     * printf uses) and the `stdout` user code sees through _impure_ptr are
     * different objects, so neither setvbuf nor fflush from here reaches
     * it, and exit() cleans up a third. That is the toolchain, not the
     * recompiler. Real homebrew logs through the firmware's own printf,
     * which is what the second line does, and what the console shows. */
    printf("hello from newlib\n");
    sceClibPrintf("hello from %s, frame %d, heap %u\n", "sceClibPrintf", 0, _newlib_heap_size_user);

    SceUID block = sceKernelAllocMemBlock("hello_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                          FB_SIZE, NULL);
    if (block < 0) {
        printf("no framebuffer: 0x%08X\n", (unsigned)block);
        return 1;
    }
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    uint32_t *px = (uint32_t *)base;

    SceDisplayFrameBuf fb;
    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(fb); fb.base = base; fb.pitch = PITCH;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = WIDTH; fb.height = HEIGHT;

    char *line = malloc(64);
    if (!line) return 2;

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);

    int frame = 0;
    for (;;) {
        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if (pad.buttons & SCE_CTRL_START) break;

        for (int i = 0; i < PITCH * HEIGHT; i++) px[i] = 0xFF302820u;   /* dark blue-grey */

        draw_text(px, 48, 48, "HELLO FROM NEWLIB", 0xFFFFFFFFu, 6);
        snprintf(line, 64, "FRAME %d  BUTTONS %08X", frame, (unsigned)pad.buttons);
        draw_text(px, 48, 160, line, 0xFF40E0FFu, 4);
        snprintf(line, 64, "HEAP %u BYTES  LINE AT %p", _newlib_heap_size_user, (void *)line);
        draw_text(px, 48, 220, line, 0xFF80FF80u, 3);
        draw_text(px, 48, 480, "START TO EXIT", 0xFFC0C0C0u, 3);

        sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
        sceDisplayWaitVblankStart();
        frame++;
    }

    sceClibPrintf("exit at frame %d\n", frame);
    free(line);
    sceKernelFreeMemBlock(block);
    return 0;
}
