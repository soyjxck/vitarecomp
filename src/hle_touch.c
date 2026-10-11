/* hle_touch.c — SceTouch: the two touch panels.
 *
 * The program asks for the panel's reports as it asks for the pad; the host
 * says whether the front panel is touched and where. There is one finger at
 * most, and nothing ever touches the back.
 *
 * SceTouchData is 0x90 bytes: { u64 timeStamp; u32 status; u32 reportNum;
 * SceTouchReport report[8] }, and a report is 16: { u8 id, force; s16 x, y;
 * u8 reserved[8]; u16 info }. A panel is 1920 units across and 1088 down,
 * two to a display pixel. A contact keeps its id for as long as it lasts
 * and the next contact has the next one, which is how a title tells a
 * finger that moved from one that was lifted and put down.
 */

#include "vitarecomp/host.h"
#include "vitarecomp/mem.h"
#include "vitarecomp/recomp_rt.h"
#include "clock.h"
#include "sched.h"

#define SCE_TOUCH_ERROR_INVALID_ARG 0x80350001u
#define SCE_TOUCH_DATA_SIZE         0x90u
#define PORTS                       2u

static uint32_t g_sampling[PORTS];
static uint8_t  g_id;
static int      g_down;

static void zero(uint32_t addr, uint32_t size) {
    for (uint32_t k = 0; k < size; k += 4) vita_write32(addr + k, 0);
}

/* int sceTouchPeek(SceUInt32 port, SceTouchData *pData, SceUInt32 nBufs): the
 * number of buffers filled. Every buffer carries the same snapshot. A panel
 * that has not been told to sample reports nothing. */
void vita_hle_sceTouchPeek(void) {
    uint32_t port = r0, p = r1, count = r2;
    if (port >= PORTS || count == 0 || count > 64 || !vita_mem_ptr(p, count * SCE_TOUCH_DATA_SIZE)) {
        r0 = SCE_TOUCH_ERROR_INVALID_ARG;
        return;
    }
    int x = 0, y = 0, down = port == 0 && g_sampling[0] && vita_host_touch(&x, &y);
    if (port == 0) {
        if (down && !g_down) g_id = (uint8_t)((g_id + 1) & 0x7F);
        g_down = down;
    }
    uint64_t stamp = vita_clock_us();
    for (uint32_t i = 0; i < count; i++) {
        uint32_t rec = p + i * SCE_TOUCH_DATA_SIZE;
        zero(rec, SCE_TOUCH_DATA_SIZE);
        vita_write32(rec + 0, (uint32_t)stamp);
        vita_write32(rec + 4, (uint32_t)(stamp >> 32));
        if (!down) continue;
        vita_write32(rec + 12, 1);
        vita_write8(rec + 16, g_id);
        vita_write8(rec + 17, 128);                         /* the force a panel that does not measure it reports */
        vita_write16(rec + 18, (uint16_t)x);
        vita_write16(rec + 20, (uint16_t)y);
    }
    r0 = count;
}

/* int sceTouchRead(port, pData, nBufs): the blocking form waits for a fresh
 * sample; here the other threads get a turn, which is what the wait was for. */
void vita_hle_sceTouchRead(void) {
    vita_hle_sceTouchPeek();
    vita_sched_yield();
}

/* int sceTouchSetSamplingState(SceUInt32 port, SceTouchSamplingState state) */
void vita_hle_sceTouchSetSamplingState(void) {
    if (r0 >= PORTS || r1 > 1) { r0 = SCE_TOUCH_ERROR_INVALID_ARG; return; }
    g_sampling[r0] = r1;
    r0 = 0;
}

/* int sceTouchGetSamplingState(SceUInt32 port, SceTouchSamplingState *pState) */
void vita_hle_sceTouchGetSamplingState(void) {
    if (r0 >= PORTS || !vita_mem_ptr(r1, 4)) { r0 = SCE_TOUCH_ERROR_INVALID_ARG; return; }
    vita_write32(r1, g_sampling[r0]);
    r0 = 0;
}

/* int sceTouchGetPanelInfo(SceUInt32 port, SceTouchPanelInfo *pPanelInfo):
 * 0x30 bytes: the active area and the part of it over the display, each as
 * s16 min x, min y, max x, max y, then u8 min and max force. The back panel
 * is the same width and not as tall, and sits behind the middle of the
 * display. */
void vita_hle_sceTouchGetPanelInfo(void) {
    uint32_t port = r0, p = r1;
    if (port >= PORTS || !vita_mem_ptr(p, 0x30)) { r0 = SCE_TOUCH_ERROR_INVALID_ARG; return; }
    zero(p, 0x30);
    vita_write16(p + 0, 0);
    vita_write16(p + 2, port ? 108 : 0);
    vita_write16(p + 4, 1919);
    vita_write16(p + 6, port ? 889 : 1087);
    vita_write16(p + 8, 0);
    vita_write16(p + 10, 0);
    vita_write16(p + 12, 1919);
    vita_write16(p + 14, 1087);
    vita_write8(p + 16, 1);
    vita_write8(p + 17, 128);
    r0 = 0;
}
