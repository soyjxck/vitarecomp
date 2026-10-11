/* hle_ctrl.c — SceCtrl: the buttons and sticks.
 *
 * The program asks for a snapshot of the pad; the host says what is held
 * and where the sticks are.
 * SceCtrlData is 32 bytes: { u64 timeStamp; u32 buttons; u8 lx, ly, rx, ry;
 * u8 up, right, down, left, ltrigger, rtrigger, l1, r1, l3, r3, square,
 * cross, circle, triangle; u8 reserved[2] }. The per-button bytes are the
 * pressure-sensitive reading of a controller the Vita does not have, and are
 * left zero as the console leaves them.
 */

#include "vitarecomp/hle.h"
#include "vitarecomp/host.h"
#include "vitarecomp/recomp_rt.h"
#include "sched.h"

#define SCE_CTRL_ERROR_INVALID_ARG  0x80340001u
#define SCE_CTRL_ERROR_INVALID_MODE 0x80340022u

#define SCE_CTRL_MODE_ANALOG_WIDE 2u
#define SCE_CTRL_DATA_SIZE        32u
#define FRAME_US                  16667u

static uint32_t g_mode;        /* SCE_CTRL_MODE_DIGITAL by default */
static uint64_t g_stamp;
static uint32_t g_buttons;     /* as last read, for the trace */

/* int sceCtrlSetSamplingMode(SceCtrlPadInputMode mode): returns the previous mode */
void vita_hle_sceCtrlSetSamplingMode(void) {
    if (r0 > SCE_CTRL_MODE_ANALOG_WIDE) { r0 = SCE_CTRL_ERROR_INVALID_MODE; return; }
    uint32_t prev = g_mode;
    g_mode = r0;
    r0 = prev;
}

/* int sceCtrlPeekBufferPositive(int port, SceCtrlData *pad_data, int count)
 *
 * Positive: a bit is set while its button is held. Returns the number of
 * records written. Every record carries the same snapshot; a host that
 * samples faster than the program reads has nothing older to offer. */
void vita_hle_sceCtrlPeekBufferPositive(void) {
    uint32_t port = r0, p = r1, count = r2;
    if (port > 4 || count == 0 || count > 64 ||
        !vita_mem_ptr(p, count * SCE_CTRL_DATA_SIZE)) {
        r0 = SCE_CTRL_ERROR_INVALID_ARG;
        return;
    }

    uint32_t buttons = vita_host_buttons();
    if (buttons != g_buttons) vita_hle_trace("ctrl: buttons 0x%04X, were 0x%04X", buttons, g_buttons);
    g_buttons = buttons;
    uint8_t sticks[4] = { 128, 128, 128, 128 };
    if (g_mode) vita_host_sticks(sticks);       /* a pad in its digital mode reports them at rest */
    g_stamp += FRAME_US;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t rec = p + i * SCE_CTRL_DATA_SIZE;
        for (uint32_t k = 0; k < SCE_CTRL_DATA_SIZE; k += 4) vita_write32(rec + k, 0);
        vita_write32(rec + 0, (uint32_t)g_stamp);
        vita_write32(rec + 4, (uint32_t)(g_stamp >> 32));
        vita_write32(rec + 8, buttons);
        for (uint32_t k = 0; k < 4; k++) vita_write8(rec + 12 + k, sticks[k]);      /* lx ly rx ry */
    }
    r0 = count;
}

/* int sceCtrlReadBufferPositive(int port, SceCtrlData *pad_data, int count): the
 * blocking form waits for a fresh sample, one per frame; here the other
 * threads get a turn, which is what the wait was for. */
void vita_hle_sceCtrlReadBufferPositive(void) {
    vita_hle_sceCtrlPeekBufferPositive();
    vita_sched_yield();
}
void vita_hle_sceCtrlReadBufferPositive2(void)   { vita_hle_sceCtrlReadBufferPositive(); }
void vita_hle_sceCtrlReadBufferPositiveExt(void) { vita_hle_sceCtrlReadBufferPositive(); }
void vita_hle_sceCtrlPeekBufferPositive2(void)   { vita_hle_sceCtrlPeekBufferPositive(); }
void vita_hle_sceCtrlPeekBufferPositiveExt(void) { vita_hle_sceCtrlPeekBufferPositive(); }
