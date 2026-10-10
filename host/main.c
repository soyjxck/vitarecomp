/* vita_host — load a module, bind its recompiled functions, run module_start.
 *
 * Headless. Input is scripted on the command line and frames go to files, so
 * the same binary proves a module runs on a developer's machine and in CI,
 * with nothing to click. A windowed host is the same program with the three
 * host.h functions pointed at a window; see docs/PLAN.md.
 *
 *   vita_host <module.elf> [--frames N] [--dump out.ppm[@FRAME]]
 *                          [--press BUTTON@FROM[-TO]]...
 *
 *   --frames N      stop after N vertical blanks (0: run until the program exits)
 *   --dump P[@F]    write the framebuffer as a binary PPM at vblank F, or at
 *                   exit when no frame is given
 *   --press B@F-T   hold button B (START, CROSS, UP, ...) from vblank F to T
 *
 * Guest memory is one flat region: the module where it was linked, then an
 * arena for memory blocks, a heap for SceLibc, and a stack at the top.
 */

#include "vitarecomp/cpu.h"
#include "vitarecomp/dispatch.h"
#include "vitarecomp/hle.h"
#include "vitarecomp/host.h"
#include "vitarecomp/loader.h"
#include "vitarecomp/mem.h"
#include "vitarecomp/recomp_rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* From the generated recomp_funcs.c. */
void vita_register_functions(void);
void vita_module_start(void);
extern const uint32_t vita_module_start_addr;

/* --- the address space ------------------------------------------------------ */

#define MEM_BASE    0x81000000u
#define MEM_SIZE    0x04000000u            /* 64 MiB: 0x81000000 .. 0x85000000 */
#define ARENA_BASE  0x82000000u            /* memory blocks                      */
#define ARENA_SIZE  0x01000000u
#define HEAP_BASE   0x83000000u            /* SceLibc malloc                     */
#define HEAP_SIZE   0x01000000u
#define STACK_TOP   0x85000000u            /* grows down from the end            */
#define STACK_SIZE  0x00100000u

/* --- the script ------------------------------------------------------------- */

typedef struct { uint32_t mask, from, to; } press;

static struct {
    const char *elf;
    uint32_t    frames_max;
    const char *dump_path;
    uint32_t    dump_frame;      /* 0: at exit */
    press       presses[16];
    int         press_count;
    uint32_t    frame;
} g;

static const struct { const char *name; uint32_t bit; } BUTTONS[] = {
    { "SELECT", VITA_CTRL_SELECT },   { "L3", VITA_CTRL_L3 },       { "R3", VITA_CTRL_R3 },
    { "START", VITA_CTRL_START },     { "UP", VITA_CTRL_UP },       { "RIGHT", VITA_CTRL_RIGHT },
    { "DOWN", VITA_CTRL_DOWN },       { "LEFT", VITA_CTRL_LEFT },   { "L", VITA_CTRL_LTRIGGER },
    { "R", VITA_CTRL_RTRIGGER },      { "L1", VITA_CTRL_L1 },       { "R1", VITA_CTRL_R1 },
    { "TRIANGLE", VITA_CTRL_TRIANGLE }, { "CIRCLE", VITA_CTRL_CIRCLE },
    { "CROSS", VITA_CTRL_CROSS },     { "SQUARE", VITA_CTRL_SQUARE },
};

static int parse_press(const char *s) {
    char name[16];
    unsigned from, to;
    const char *at = strchr(s, '@');
    if (!at || (size_t)(at - s) >= sizeof(name)) return 1;
    memcpy(name, s, (size_t)(at - s));
    name[at - s] = '\0';
    if (sscanf(at + 1, "%u-%u", &from, &to) == 2) {}
    else if (sscanf(at + 1, "%u", &from) == 1) to = from + 1;
    else return 1;

    for (size_t i = 0; i < sizeof(BUTTONS) / sizeof(BUTTONS[0]); i++) {
        if (strcmp(BUTTONS[i].name, name) == 0) {
            if (g.press_count == 16) return 1;
            g.presses[g.press_count].mask = BUTTONS[i].bit;
            g.presses[g.press_count].from = from;
            g.presses[g.press_count].to   = to;
            g.press_count++;
            return 0;
        }
    }
    return 1;
}

/* --- frames to files -------------------------------------------------------- */

static int write_ppm(const char *path) {
    vita_framebuf fb;
    if (!vita_display_current(&fb)) {
        fprintf(stderr, "vita_host: no framebuffer set at vblank %u; nothing to dump\n", g.frame);
        return 1;
    }
    const uint8_t *px = (const uint8_t *)vita_mem_ptr(fb.base, fb.pitch * fb.height * 4);
    if (!px) return 1;

    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "vita_host: cannot write %s\n", path); return 1; }
    fprintf(f, "P6\n%u %u\n255\n", fb.width, fb.height);
    for (uint32_t y = 0; y < fb.height; y++) {
        for (uint32_t x = 0; x < fb.width; x++) {
            /* A8B8G8R8: R, G, B, A in memory order. */
            const uint8_t *p = px + ((size_t)y * fb.pitch + x) * 4;
            fputc(p[0], f); fputc(p[1], f); fputc(p[2], f);
        }
    }
    fclose(f);
    fprintf(stderr, "vita_host: wrote %s (%ux%u, vblank %u)\n", path, fb.width, fb.height, g.frame);
    return 0;
}

/* --- the host side of host.h ------------------------------------------------ */

static void summary(void) {
    fprintf(stderr, "vita_host: %u vblanks, %u flips, %u traps, dispatch %u hits / %u misses, "
                    "%u bad accesses, heap %u/%u\n",
            g.frame, vita_display_flips(), vita_trap_count(),
            vita_dispatch_hits(), vita_dispatch_misses(), vita_mem_bad_access,
            vita_heap_used(), vita_heap_size());
}

static void finish(int status) {
    if (g.dump_path && g.dump_frame == 0) write_ppm(g.dump_path);
    summary();
    exit(status);
}

void vita_host_vblank(void) {
    g.frame++;
    if (g.dump_path && g.frame == g.dump_frame) write_ppm(g.dump_path);
    if (g.frames_max && g.frame >= g.frames_max) {
        fprintf(stderr, "vita_host: frame limit %u reached\n", g.frames_max);
        finish(0);
    }
}

uint32_t vita_host_buttons(void) {
    uint32_t m = 0;
    for (int i = 0; i < g.press_count; i++)
        if (g.frame >= g.presses[i].from && g.frame < g.presses[i].to) m |= g.presses[i].mask;
    return m;
}

void vita_host_exit(int status) {
    fprintf(stderr, "vita_host: sceKernelExitProcess(%d) at vblank %u\n", status, g.frame);
    finish(status);
}

/* --- main ------------------------------------------------------------------- */

static int usage(void) {
    fputs("usage: vita_host <module.elf> [--frames N] [--dump out.ppm[@FRAME]] [--press BUTTON@FROM[-TO]]...\n", stderr);
    return 2;
}

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    g.elf = argv[1];
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            g.frames_max = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            char *spec = argv[++i];
            char *at = strrchr(spec, '@');
            if (at) { *at = '\0'; g.dump_frame = (uint32_t)strtoul(at + 1, NULL, 0); }
            g.dump_path = spec;
        } else if (strcmp(argv[i], "--press") == 0 && i + 1 < argc) {
            if (parse_press(argv[++i])) { fprintf(stderr, "vita_host: bad --press %s\n", argv[i]); return 2; }
        } else {
            return usage();
        }
    }

    vita_load_info info;
    if (vita_probe_elf(g.elf, &info)) {
        fprintf(stderr, "vita_host: cannot read %s as an ELF\n", g.elf);
        return 1;
    }
    if (info.lowest < MEM_BASE || info.highest > ARENA_BASE) {
        fprintf(stderr, "vita_host: module spans 0x%08X..0x%08X; this host maps 0x%08X..0x%08X for it\n",
                info.lowest, info.highest, MEM_BASE, ARENA_BASE);
        return 1;
    }

    if (vita_mem_init(MEM_BASE, MEM_SIZE)) {
        fprintf(stderr, "vita_host: cannot reserve %u MiB of guest memory\n", MEM_SIZE >> 20);
        return 1;
    }
    if (vita_load_elf(g.elf, &info)) {
        fprintf(stderr, "vita_host: cannot load %s\n", g.elf);
        return 1;
    }
    vita_memblock_init(ARENA_BASE, ARENA_SIZE);
    vita_heap_init(HEAP_BASE, HEAP_SIZE);
    vita_register_functions();

    fprintf(stderr, "vita_host: %s loaded, %d segments, 0x%08X..0x%08X; module_start 0x%08X\n",
            g.elf, info.segments, info.lowest, info.highest, vita_module_start_addr);

    /* module_start(SceSize args, const void *argp): no arguments. The link
     * register is what a return lands on; the generated C returns normally,
     * so it only has to be a value that would be recognised if it ever
     * showed up in a trap. */
    sp = STACK_TOP - 16;
    lr = 0xDEADC0DEu;
    r0 = 0;
    r1 = 0;
    (void)STACK_SIZE;

    vita_module_start();

    fprintf(stderr, "vita_host: module_start returned r0=%u at vblank %u\n", r0, g.frame);
    finish(0);
    return 0;
}
