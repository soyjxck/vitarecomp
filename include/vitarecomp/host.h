/* host.h — the contract between the HLE and the program that presents it.
 *
 * The HLE bodies in src/hle_*.c implement firmware semantics against guest
 * memory and know nothing about windows, gamepads or clocks. Those are the
 * host's, and the HLE reaches them through the five functions below. A host
 * is therefore small: a headless one that scripts its input and writes frames
 * to files is a hundred lines, and is what CI runs.
 */

#ifndef VITARECOMP_HOST_H
#define VITARECOMP_HOST_H

#include <stdint.h>
#include <stdio.h>

/* --- supplied by the host, called by the HLE ------------------------------- */

/* sceDisplayWaitVblankStart: present whatever vita_display_current() names,
 * poll input, and pace to 60 Hz if the host cares to. Once per frame. */
void vita_host_vblank(void);

/* The buttons held right now, as a VITA_CTRL_* mask. */
uint32_t vita_host_buttons(void);

/* Whether something touches the front panel right now, and where, in the
 * panel's own units: 1920 across and 1088 down, two to a display pixel. */
int vita_host_touch(int *x, int *y);

/* Where the two sticks are: left x, left y, right x, right y, each 0 to 255
 * with 128 at rest, x growing to the right and y downwards. */
void vita_host_sticks(uint8_t xy[4]);

/* sceKernelExitProcess. Must not return. */
void vita_host_exit(int status);

/* --- supplied by the HLE, called by the host ------------------------------- */

/* Where sceKernelAllocMemBlock carves blocks from. Must lie inside the region
 * given to vita_mem_init. */
void vita_memblock_init(uint32_t base, uint32_t size);

/* A small region for objects the kernel keeps on the program's behalf —
 * thread-local storage slots, and whatever follows. Inside the region given
 * to vita_mem_init, and not where the program will look for anything. */
void vita_kernel_init(uint32_t base, uint32_t size);

/* Where a guest mount lives on the host: "app0" to the title's data
 * directory, "ux0" to somewhere writable. Paths the program opens resolve
 * through these; one nothing maps is reported missing. The same prefix
 * mounted twice is an overlay, first one wins: a patch over the data. */
void vita_io_mount(const char *prefix, const char *host_dir);

/* The main thread's stack, so the kernel can describe it: a program's own
 * crash handler walks frames until it leaves the stack it was told about. */
void vita_main_stack_init(uint32_t base, uint32_t size);

/* The module's thread-local image, as the generated C publishes it in
 * vita_module_tls_addr/filesz/memsz. Every thread's TLS block is built from
 * it: 0x800 bytes of kernel slots, then a copy of the image, which is what
 * TPIDRURO points at. Zeros for a module without one. */
void vita_tls_init(uint32_t image, uint32_t filesz, uint32_t memsz);

/* The variables the module imports (vita_var_imports, from the generated C):
 * each is built in guest memory, or given a zeroed page when the runtime has
 * no implementation, and its address written where the image expects it.
 * After the module is loaded and the memory-block arena exists. */
void vita_vars_bind(void);

typedef struct {
    uint32_t base;         /* guest address of the pixels                 */
    uint32_t pitch;        /* in pixels                                    */
    uint32_t pixelformat;  /* SCE_DISPLAY_PIXELFORMAT_*: 0 is A8B8G8R8    */
    uint32_t width, height;
} vita_framebuf;

/* The framebuffer the program last set. Returns 0 when none is set, or it was
 * set to NULL, which blanks the display. */
int vita_display_current(vita_framebuf *out);

/* The same framebuffer shown in a window of the host's, by the graphics
 * card when it is the card that drew it: the frame is on the card already,
 * and vita_display_current would fetch it into memory for the host to send
 * back. `window` is the window as the system names it, an HWND on Windows.
 * Returns 1 when it was shown, with the frame's size (nothing by nothing
 * when the display is blank, and the window black); the window is then the
 * card's and the host draws nothing in it. Returns 0 when there is no card
 * drawing: the host shows vita_display_current as it would have. */
int vita_display_show(void *window, uint32_t *width, uint32_t *height);

/* sceDisplaySetFrameBuf calls so far. */
uint32_t vita_display_flips(void);

/* Sound. What the program plays through its output ports is kept at one
 * rate, two channels of sixteen bits (hle_audio.c). A host with a sound
 * device says so, and from then on a played block is queued for it and the
 * program's sound thread is paced by what the device has taken; the device
 * takes the sum of the ports with vita_audio_pull, on its own thread, which
 * always gives the frames asked for and makes up what is missing with
 * silence. With no device a block takes as long to play as it lasts and is
 * not kept. */
#define VITA_AUDIO_RATE 48000u
void vita_audio_device(int present);
void vita_audio_pull(int16_t *out, uint32_t frames);

/* For the host's summary: frames the device has taken, and how many of
 * them had nothing queued and went out as silence. */
void vita_audio_counts(uint64_t *taken, uint64_t *empty);

/* Time that moves only with what the program does, so that a run repeats
 * exactly: a sixtieth of a second at each vertical blank, the length of a
 * wait when every thread waits. Called before the program starts; without
 * it the clock is the host's. */
void vita_clock_virtual(void);

/* --- bring-up aids ------------------------------------------------------------ */

/* Names for addresses, from an `nm -n` listing (symbols.c). Optional: a
 * retail module has none. */
int         vita_symbols_load(const char *path);
uint32_t    vita_symbols_count(void);
const char *vita_symbol_name(uint32_t addr, uint32_t *off);

/* Function-entry tracing. The generated C calls vita_trace_fn at the start
 * of every function; it prints only while vita_trace_fns is set, naming the
 * function when symbols are loaded. */
extern int vita_trace_fns;
void vita_trace_fn(uint32_t addr);

/* With vita_trace_fns at 2 the entries are counted and not printed. This
 * writes "address count" for each function entered since the last call, in
 * address order, and starts the count again. */
void vita_coverage_write(FILE *f);

/* A sampling view of where the program is (profile.c): every `period_s`
 * seconds, the functions the samples of the recorded guest address fell in
 * and which guest thread was running. Needs C emitted with
 * VITARECOMP_EMIT_PC=1. */
void vita_profile_start(uint32_t period_s);

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
