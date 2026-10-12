/* window_switch.c — the host's window on the Nintendo Switch.
 *
 * A homebrew program has one window, the whole screen, and the console's
 * own controller. The frame the program shows reaches the screen one of
 * two ways. A graphics card that holds the frame draws it there for itself
 * (vita_window_native, and gpu_gl.c). Otherwise it is read from the
 * program's memory and drawn into the screen's frame buffer here (1280 by
 * 720, the console's size in the hand; docked, the console scales it),
 * keeping its shape with black beside it, the samples between four of its
 * pixels a blend of the four.
 *
 * The screen is one or the other's and never both: the console will not
 * have a frame buffer made on a window the card draws to, nor the reverse.
 * So the frame buffer is not made when the window is opened but when the
 * first frame is drawn into it, and once it is made the card is told
 * there is no window for it.
 *
 * The buttons are where a player's thumbs expect them, not where their
 * letters say: the console's right-hand button (A) is the program's
 * right-hand one (circle), the bottom one (B) its bottom one (cross), and
 * so round. L and ZL are the left shoulder, R and ZR the right. The touch
 * screen is the program's front panel.
 */

#include "window.h"

#include "vitarecomp/host.h"

#include <stddef.h>
#include <string.h>

#include <switch.h>

#define SCREEN_W 1280
#define SCREEN_H 720

static Framebuffer g_fb;
static PadState    g_pad;
static int         g_open;
static int         g_ours;      /* the screen's frame buffer has been made: the picture is drawn here */
static int         g_theirs;    /* a graphics card draws the picture in the screen for itself */
static int         g_x0, g_y0, g_w = SCREEN_W, g_h = SCREEN_H;   /* where the picture lies on the screen */
static u64         g_next_tick;

int vita_window_open(const char *title, int width, int height) {
    (void)title; (void)width; (void)height;
    if (g_open) return 0;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    hidInitializeTouchScreen();
    g_open = 1;
    return 0;
}

/* Where a frame of this size lies on the screen: the largest picture of its shape that the screen holds. */
static void place(int width, int height) {
    g_w = SCREEN_W; g_h = SCREEN_W * height / width;
    if (g_h > SCREEN_H) { g_h = SCREEN_H; g_w = SCREEN_H * width / height; }
    g_x0 = (SCREEN_W - g_w) / 2; g_y0 = (SCREEN_H - g_h) / 2;
}

void vita_window_fullscreen(void) { }
void vita_window_title(const char *text) { (void)text; }

/* The screen, for a card to draw in: for as long as it has not been taken for a picture drawn here. */
void *vita_window_native(void) { return g_open && !g_ours ? nwindowGetDefault() : NULL; }

void vita_window_drawn(int width, int height) {
    if (width > 0 && height > 0) place(width, height);          /* a finger is still placed on the picture */
    g_theirs = 1;
}

void vita_window_present(const uint8_t *pixels, int width, int height, int pitch) {
    if (!g_open || g_theirs || width <= 0 || height <= 0) return;
    if (!g_ours) {
        if (R_FAILED(framebufferCreate(&g_fb, nwindowGetDefault(), SCREEN_W, SCREEN_H, PIXEL_FORMAT_RGBA_8888, 2))) return;
        framebufferMakeLinear(&g_fb);
        g_ours = 1;
    }
    u32 stride = 0;
    uint8_t *screen = (uint8_t *)framebufferBegin(&g_fb, &stride);
    if (!screen) return;

    place(width, height);

    const uint32_t step_x = (uint32_t)(((uint64_t)width << 16) / (uint32_t)g_w);
    const uint32_t step_y = (uint32_t)(((uint64_t)height << 16) / (uint32_t)g_h);
    for (int y = 0; y < SCREEN_H; y++) {
        uint32_t *out = (uint32_t *)(screen + (size_t)y * stride);
        if (y < g_y0 || y >= g_y0 + g_h) { memset(out, 0, SCREEN_W * 4); continue; }
        const uint32_t sy = (uint32_t)(y - g_y0) * step_y;
        const uint32_t y0 = sy >> 16, y1 = y0 + 1 < (uint32_t)height ? y0 + 1 : y0, fy = (sy >> 8) & 0xFF;
        const uint32_t *row0 = (const uint32_t *)(pixels + (size_t)y0 * (size_t)pitch * 4);
        const uint32_t *row1 = (const uint32_t *)(pixels + (size_t)y1 * (size_t)pitch * 4);
        for (int x = 0; x < g_x0; x++) out[x] = 0xFF000000u;
        for (int x = g_x0 + g_w; x < SCREEN_W; x++) out[x] = 0xFF000000u;
        uint32_t sx = 0;
        for (int x = 0; x < g_w; x++, sx += step_x) {
            const uint32_t x0 = sx >> 16, x1 = x0 + 1 < (uint32_t)width ? x0 + 1 : x0, fx = (sx >> 8) & 0xFF;
            const uint32_t a = row0[x0], b = row0[x1], c = row1[x0], d = row1[x1];
            /* two channels at a time: red and blue in one word, green in the other */
            const uint32_t top_rb = ((a & 0xFF00FF) * (256 - fx) + (b & 0xFF00FF) * fx) >> 8 & 0xFF00FF;
            const uint32_t top_g  = ((a & 0x00FF00) * (256 - fx) + (b & 0x00FF00) * fx) >> 8 & 0x00FF00;
            const uint32_t bot_rb = ((c & 0xFF00FF) * (256 - fx) + (d & 0xFF00FF) * fx) >> 8 & 0xFF00FF;
            const uint32_t bot_g  = ((c & 0x00FF00) * (256 - fx) + (d & 0x00FF00) * fx) >> 8 & 0x00FF00;
            const uint32_t rb = (top_rb * (256 - fy) + bot_rb * fy) >> 8 & 0xFF00FF;
            const uint32_t g  = (top_g * (256 - fy) + bot_g * fy) >> 8 & 0x00FF00;
            out[g_x0 + x] = 0xFF000000u | rb | g;
        }
    }
    framebufferEnd(&g_fb);
}

uint32_t vita_window_buttons(void) {
    static const struct { u64 from; uint32_t to; } map[] = {
        { HidNpadButton_A, VITA_CTRL_CIRCLE },      { HidNpadButton_B, VITA_CTRL_CROSS },
        { HidNpadButton_X, VITA_CTRL_TRIANGLE },    { HidNpadButton_Y, VITA_CTRL_SQUARE },
        { HidNpadButton_L, VITA_CTRL_LTRIGGER },    { HidNpadButton_ZL, VITA_CTRL_LTRIGGER },
        { HidNpadButton_R, VITA_CTRL_RTRIGGER },    { HidNpadButton_ZR, VITA_CTRL_RTRIGGER },
        { HidNpadButton_Plus, VITA_CTRL_START },    { HidNpadButton_Minus, VITA_CTRL_SELECT },
        { HidNpadButton_Up, VITA_CTRL_UP },         { HidNpadButton_Down, VITA_CTRL_DOWN },
        { HidNpadButton_Left, VITA_CTRL_LEFT },     { HidNpadButton_Right, VITA_CTRL_RIGHT },
        { HidNpadButton_StickL, VITA_CTRL_L3 },     { HidNpadButton_StickR, VITA_CTRL_R3 },
    };
    if (!g_open) return 0;
    padUpdate(&g_pad);
    const u64 held = padGetButtons(&g_pad);
    uint32_t m = 0;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (held & map[i].from) m |= map[i].to;
    return m;
}

void vita_window_sticks(uint8_t xy[4]) {
    xy[0] = xy[1] = xy[2] = xy[3] = 128;
    if (!g_open) return;
    for (int s = 0; s < 2; s++) {
        const HidAnalogStickState st = padGetStickPos(&g_pad, s);
        int x = 128 + st.x * 127 / JOYSTICK_MAX, y = 128 - st.y * 127 / JOYSTICK_MAX;     /* up is towards 0 */
        xy[s * 2]     = (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
        xy[s * 2 + 1] = (uint8_t)(y < 0 ? 0 : y > 255 ? 255 : y);
    }
}

int vita_window_touch(int *x, int *y) {
    HidTouchScreenState state;
    if (!g_open || !hidGetTouchScreenStates(&state, 1) || state.count < 1) return 0;
    const int px = (int)state.touches[0].x - g_x0, py = (int)state.touches[0].y - g_y0;
    if (px < 0 || py < 0 || px >= g_w || py >= g_h) return 0;
    *x = px * 1920 / g_w;
    *y = py * 1088 / g_h;
    return 1;
}

int vita_window_closed(void) { return g_open && !appletMainLoop(); }

/* A sixtieth of a second since the last call, by the console's tick. */
uint32_t vita_window_pace_left(void) {
    const u64 now = armGetSystemTick();
    if (!g_next_tick || now >= g_next_tick) return 0;
    return (uint32_t)(armTicksToNs(g_next_tick - now) / 1000u);
}

void vita_window_pace(void) {
    const u64 period = armNsToTicks(1000000000ull / 60);
    u64 now = armGetSystemTick();
    if (g_next_tick && now < g_next_tick) {
        svcSleepThread((s64)armTicksToNs(g_next_tick - now));
        now = armGetSystemTick();
    }
    /* a frame that came late starts the count again from where it came */
    g_next_tick = (g_next_tick && now < g_next_tick + period ? g_next_tick : now) + period;
}
