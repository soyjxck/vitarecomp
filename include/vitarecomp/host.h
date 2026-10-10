/* host.h — the contract between the HLE and the program that presents it.
 *
 * The HLE bodies in src/hle_*.c implement firmware semantics against guest
 * memory and know nothing about windows, gamepads or clocks. Those are the
 * host's, and the HLE reaches them through the three functions below. A host
 * is therefore small: a headless one that scripts its input and writes frames
 * to files is a hundred lines, and is what CI runs.
 */

#ifndef VITARECOMP_HOST_H
#define VITARECOMP_HOST_H

#include <stdint.h>

/* --- supplied by the host, called by the HLE ------------------------------- */

/* sceDisplayWaitVblankStart: present whatever vita_display_current() names,
 * poll input, and pace to 60 Hz if the host cares to. Once per frame. */
void vita_host_vblank(void);

/* The buttons held right now, as a VITA_CTRL_* mask. */
uint32_t vita_host_buttons(void);

/* sceKernelExitProcess. Must not return. */
void vita_host_exit(int status);

/* --- supplied by the HLE, called by the host ------------------------------- */

/* Where sceKernelAllocMemBlock carves blocks from. Must lie inside the region
 * given to vita_mem_init. */
void vita_memblock_init(uint32_t base, uint32_t size);

typedef struct {
    uint32_t base;         /* guest address of the pixels                 */
    uint32_t pitch;        /* in pixels                                    */
    uint32_t pixelformat;  /* SCE_DISPLAY_PIXELFORMAT_*: 0 is A8B8G8R8    */
    uint32_t width, height;
} vita_framebuf;

/* The framebuffer the program last set. Returns 0 when none is set, or it was
 * set to NULL, which blanks the display. */
int vita_display_current(vita_framebuf *out);

/* sceDisplaySetFrameBuf calls so far. */
uint32_t vita_display_flips(void);

/* Button bits, for hosts that map keys. The Vita's own values. */
#define VITA_CTRL_SELECT    0x00000001u
#define VITA_CTRL_L3        0x00000002u
#define VITA_CTRL_R3        0x00000004u
#define VITA_CTRL_START     0x00000008u
#define VITA_CTRL_UP        0x00000010u
#define VITA_CTRL_RIGHT     0x00000020u
#define VITA_CTRL_DOWN      0x00000040u
#define VITA_CTRL_LEFT      0x00000080u
#define VITA_CTRL_LTRIGGER  0x00000100u
#define VITA_CTRL_RTRIGGER  0x00000200u
#define VITA_CTRL_L1        0x00000400u
#define VITA_CTRL_R1        0x00000800u
#define VITA_CTRL_TRIANGLE  0x00001000u
#define VITA_CTRL_CIRCLE    0x00002000u
#define VITA_CTRL_CROSS     0x00004000u
#define VITA_CTRL_SQUARE    0x00008000u

#endif /* VITARECOMP_HOST_H */
