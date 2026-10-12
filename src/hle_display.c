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
#include "gpu_tween.h"

#include <stdlib.h>

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
static uint32_t      g_vcount;       /* blanks so far                          */
static uint32_t      g_set_vcount;   /* and how many when the buffer was last set */

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
    g_set_vcount = g_vcount;
    gpu_tween_flip();               /* what was drawn up to here is a frame */
    r0 = 0;
}

/* int sceDisplayWaitVblankStart(void): the frame ends, and the other
 * threads get their turn before the next one starts. */
static void vblank_passed(void);

void vita_hle_sceDisplayWaitVblankStart(void) {
    vita_clock_vblank();
    vblank_passed();
    vita_host_vblank();
    vita_sched_yield();
    r0 = 0;
}
void vita_hle_sceDisplayWaitSetFrameBuf(void)      { vita_hle_sceDisplayWaitVblankStart(); }
void vita_hle_sceDisplayWaitVblankStartMulti(void) { vita_hle_sceDisplayWaitVblankStart(); }
void vita_hle_sceDisplayWaitSetFrameBufMulti(void) { vita_hle_sceDisplayWaitVblankStart(); }

/* The ...CB forms, which also let the waiting thread's callbacks run. These
 * count: sceDisplayWaitVblankStartMultiCB(n) is n blanks from now, and the
 * SetFrameBuf forms wait for the n-th blank after the frame buffer was last
 * set, which is how a program holds itself to thirty frames a second
 * however quickly it drew. A blank that has already come is not waited for
 * again: a program that took its time comes straight back. Once for each
 * blank, though: here a blank only comes when somebody waits for one, and a
 * loop on such a wait with no new frame set would otherwise never see the
 * next. */
static void wait_blanks(uint32_t n) {
    if (n > 60) n = 60;
    while (n--) vita_hle_sceDisplayWaitVblankStart();
    vita_callbacks_run();
    r0 = 0;
}
static uint32_t since_set(uint32_t n) {
    /* VITARECOMP_DISPLAY_INTERVAL=n: every such wait is for n blanks,
     * whatever the program asked for: 1 lets a program that holds itself
     * to thirty frames a second show sixty. Whether it then also runs at
     * twice its speed is the program's affair. */
    static int asked, forced;
    static uint32_t passed_at = ~0u;    /* the blank at which a wait last came straight back */
    if (!asked) {
        const char *e = getenv("VITARECOMP_DISPLAY_INTERVAL");
        forced = e ? atoi(e) : 0;
        asked = 1;
    }
    if (forced > 0) n = (uint32_t)forced;
    if (n < 1) n = 1;
    const uint32_t due = g_set_vcount + n;
    if (due > g_vcount) return due - g_vcount;
    if (passed_at == g_vcount) return 1;
    passed_at = g_vcount;
    return 0;
}
void vita_hle_sceDisplayWaitVblankStartCB(void)      { wait_blanks(1); }
void vita_hle_sceDisplayWaitVblankStartMultiCB(void) { wait_blanks(r0 ? r0 : 1); }
void vita_hle_sceDisplayWaitSetFrameBufCB(void)      { wait_blanks(since_set(1)); }
void vita_hle_sceDisplayWaitSetFrameBufMultiCB(void) { wait_blanks(since_set(r0)); }

/* int sceDisplayRegisterVblankStartCallback(SceUID cb) / Unregister: the
 * callbacks that get a notice at every blank. */
#define MAX_VBLANK_CBS 8
static uint32_t g_vblank_cb[MAX_VBLANK_CBS];

void vita_hle_sceDisplayRegisterVblankStartCallback(void) {
    for (int i = 0; i < MAX_VBLANK_CBS; i++)
        if (!g_vblank_cb[i]) { g_vblank_cb[i] = r0; r0 = 0; return; }
    r0 = SCE_DISPLAY_ERROR_INVALID_VALUE;
}
void vita_hle_sceDisplayUnregisterVblankStartCallback(void) {
    for (int i = 0; i < MAX_VBLANK_CBS; i++)
        if (g_vblank_cb[i] == r0) g_vblank_cb[i] = 0;
    r0 = 0;
}

/* int sceDisplayGetVcount(void): blanks since the start, sixteen bits of them */
void vita_hle_sceDisplayGetVcount(void) { r0 = g_vcount & 0xFFFFu; }

#define BLANK_US 16667u
static uint64_t g_clock_blank;      /* the blank the clock stood in when the callbacks last had their notice */

static void vblank_passed(void) {
    g_vcount++;
    g_clock_blank = vita_clock_us() / BLANK_US;
    for (int i = 0; i < MAX_VBLANK_CBS; i++)
        if (g_vblank_cb[i]) vita_callback_notify(g_vblank_cb[i], 0);
}

/* The console's display blanks sixty times a second whether or not anybody
 * waits for it, and a program may count on its blank callback while it
 * sleeps another way (a loop of short delays, until the callback has done
 * what was asked of it). Here a blank is otherwise made by the wait for
 * one, so a wait that lets callbacks run asks this first: if the clock has
 * moved into another sixtieth since the callbacks last heard, they hear
 * now. */
void vita_display_poll(void) {
    const uint64_t now = vita_clock_us() / BLANK_US;
    if (now <= g_clock_blank) return;
    g_clock_blank = now;
    for (int i = 0; i < MAX_VBLANK_CBS; i++)
        if (g_vblank_cb[i]) vita_callback_notify(g_vblank_cb[i], 0);
}

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

int vita_display_on_the_beat(void) { return gpu_tween_on(); }

uint32_t vita_display_flips(void) { return g_flips; }
