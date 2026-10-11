/* hle_display.c — SceDisplay: the framebuffer and the vertical blank.
 *
 * The display has one job here: remember which guest pixels the program said
 * to show, and hand the host a turn once per frame. The host does the showing.
 * The checks are the console's — one pixel format, four resolutions, a pitch
 * in 64-pixel steps — so a program that passes here passes there.
 */

#include "clock.h"
#include "vitarecomp/host.h"
#include "vitarecomp/recomp_rt.h"
#include "sched.h"
#include "gxm_surfaces.h"

#define SCE_DISPLAY_ERROR_INVALID_VALUE       0x80290001u
#define SCE_DISPLAY_ERROR_INVALID_ADDR        0x80290002u
#define SCE_DISPLAY_ERROR_INVALID_PIXELFORMAT 0x80290003u
#define SCE_DISPLAY_ERROR_INVALID_PITCH       0x80290004u
#define SCE_DISPLAY_ERROR_INVALID_RESOLUTION  0x80290005u

#define SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 0u
#define SCE_DISPLAY_FRAMEBUF_SIZE        24u

static vita_framebuf g_fb;
static int           g_set;
static uint32_t      g_flips;

static int resolution_ok(uint32_t w, uint32_t h) {
    return (w == 960 && h == 544) || (w == 720 && h == 408)
        || (w == 640 && h == 368) || (w == 480 && h == 272);
}

/* int sceDisplaySetFrameBuf(const SceDisplayFrameBuf *pParam, SceDisplaySetBufSync sync)
 *
 * SceDisplayFrameBuf: { u32 size; void *base; u32 pitch; u32 pixelformat;
 *                       u32 width; u32 height }  -- 24 bytes.
 * A NULL pParam blanks the display. `sync` (immediate or next frame) makes no
 * difference to a host that presents at vblank. */
void vita_hle_sceDisplaySetFrameBuf(void) {
    uint32_t p = r0;
    if (p == 0) { g_set = 0; g_flips++; r0 = 0; return; }

    if (!vita_mem_ptr(p, SCE_DISPLAY_FRAMEBUF_SIZE)) { r0 = SCE_DISPLAY_ERROR_INVALID_ADDR; return; }
    uint32_t size  = vita_read32(p + 0);
    uint32_t base  = vita_read32(p + 4);
    uint32_t pitch = vita_read32(p + 8);
    uint32_t fmt   = vita_read32(p + 12);
    uint32_t w     = vita_read32(p + 16);
    uint32_t h     = vita_read32(p + 20);

    if (size != SCE_DISPLAY_FRAMEBUF_SIZE)       { r0 = SCE_DISPLAY_ERROR_INVALID_VALUE; return; }
    if (fmt != SCE_DISPLAY_PIXELFORMAT_A8B8G8R8) { r0 = SCE_DISPLAY_ERROR_INVALID_PIXELFORMAT; return; }
    if (!resolution_ok(w, h))                    { r0 = SCE_DISPLAY_ERROR_INVALID_RESOLUTION; return; }
    if (pitch < w || (pitch & 63))               { r0 = SCE_DISPLAY_ERROR_INVALID_PITCH; return; }
    if (!vita_mem_ptr(base, pitch * h * 4))      { r0 = SCE_DISPLAY_ERROR_INVALID_ADDR; return; }

    g_fb.base = base; g_fb.pitch = pitch; g_fb.pixelformat = fmt;
    g_fb.width = w;   g_fb.height = h;
    g_set = 1;
    g_flips++;
    r0 = 0;
}

/* int sceDisplayWaitVblankStart(void): the frame ends, and the other
 * threads get their turn before the next one starts. */
void vita_hle_sceDisplayWaitVblankStart(void) {
    vita_clock_vblank();
    vita_host_vblank();
    vita_sched_yield();
    r0 = 0;
}
void vita_hle_sceDisplayWaitVblankStartCB(void)    { vita_hle_sceDisplayWaitVblankStart(); }
void vita_hle_sceDisplayWaitSetFrameBuf(void)      { vita_hle_sceDisplayWaitVblankStart(); }
void vita_hle_sceDisplayWaitVblankStartMulti(void) { vita_hle_sceDisplayWaitVblankStart(); }
void vita_hle_sceDisplayWaitSetFrameBufMulti(void) { vita_hle_sceDisplayWaitVblankStart(); }

int vita_display_current(vita_framebuf *out) {
    if (!g_set) return 0;
    *out = g_fb;
    /* whoever asks is about to read the frame: if a graphics card drew it, it is put where it is looked for */
    uint32_t bytes = g_fb.pitch * g_fb.height * 4;
    gxm_gpu_memory_wanted(vita_mem_ptr(g_fb.base, bytes), bytes);
    return 1;
}

int vita_display_show(void *window, uint32_t *width, uint32_t *height) {
    uint32_t bytes = g_fb.pitch * g_fb.height * 4;
    if (width) *width = g_set ? g_fb.width : 0;
    if (height) *height = g_set ? g_fb.height : 0;
    if (!g_set) return gxm_gpu_show(window, NULL, 0, 0, 0);
    return gxm_gpu_show(window, vita_mem_ptr(g_fb.base, bytes), g_fb.width, g_fb.height, g_fb.pitch);
}

uint32_t vita_display_flips(void) { return g_flips; }
