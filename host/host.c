/* host.c — load a module, bind its recompiled functions, run module_start.
 *
 * This is the whole of vita_host (main.c hands it its arguments) and the
 * greater part of a game's own program, which calls vita_host_main with
 * arguments it has made up itself (vitarecomp/host_main.h).
 *
 * Headless by default. Input is scripted on the command line and frames go
 * to files, so the same binary proves a module runs on a developer's machine
 * and in CI, with nothing to click. With --window it also shows each frame
 * as it is displayed, paced to sixty a second, and adds the keyboard to the
 * scripted buttons and the mouse to the scripted touches (window.h).
 *
 *   vita_host <module.elf> [--frames N] [--dump out.ppm[@FRAME]]
 *                          [--press BUTTON@FROM[-TO]]... [--touch X,Y@FROM[-TO]]...
 *                          [--window] [--window-size WxH] [--fullscreen] [--title TEXT]
 *
 *   --frames N      stop after N vertical blanks (0: run until the program exits)
 *   --dump-dir D[@N] write it into directory D every N vblanks (60 if not given)
 *   --virtual-clock  time passes a sixtieth of a second per vblank, not by the
 *                   host's clock: the same run every time, however long it takes
 *   --dump P[@F]    write the framebuffer as a binary PPM at vblank F, or at
 *                   exit when no frame is given
 *   --press B@F-T   hold button B (START, CROSS, UP, ...) from vblank F to T
 *   --touch X,Y@F-T a finger on the front panel at display pixel X,Y, likewise
 *   --stick S,X,Y@F-T the left (L) or right (R) stick held at X,Y, -100 to 100
 *   --window-size WxH the window's picture, in pixels (960x544 if not given)
 *   --fullscreen    the window over the whole screen from the start (F11 changes it)
 *   --title TEXT    what the window is called: a game's name
 *
 * Guest memory is one flat region: the module where it was linked, then an
 * arena for memory blocks, a heap for SceLibc, and a stack at the top.
 */

#include "vitarecomp/cpu.h"
#include "vitarecomp/dispatch.h"
#include "vitarecomp/hle.h"
#include "vitarecomp/host.h"
#include "vitarecomp/host_main.h"
#include "vitarecomp/loader.h"
#include "vitarecomp/mem.h"
#include "vitarecomp/recomp_rt.h"
#include "sampler.h"
#include "window.h"
#include "../src/sched.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* From the generated recomp_funcs.c. */
void vita_register_functions(void);
void vita_module_start(void);
extern const uint32_t vita_module_start_addr;
extern const uint32_t vita_module_tls_addr, vita_module_tls_filesz, vita_module_tls_memsz;

/* --- the address space ------------------------------------------------------ */

/* 160 MiB: 0x81000000 .. 0x8B000000. The module gets 16 MiB where it was
 * linked; memory blocks get 128 MiB above it, because that is where a newlib
 * program's heap lives (32 MiB by default, 192 MiB for a vita2d game) and a
 * framebuffer is 2 MiB a piece; then the SceLibc heap and the stack. */
#define MEM_BASE    0x81000000u
#define MEM_SIZE    0x1A000000u            /* 416 MiB: the console's main memory and its CDRAM, and room */
#define ARENA_BASE  0x82000000u            /* memory blocks                      */
#define ARENA_SIZE  0x18000000u
#define HEAP_BASE   0x9A000000u            /* SceLibc malloc                     */
#define HEAP_SIZE   0x00E00000u
#define KERNEL_BASE 0x9AE00000u            /* TLS slots and other kernel objects */
#define KERNEL_SIZE 0x00100000u
#define STACK_TOP   0x9B000000u            /* grows down from the end            */
#define STACK_SIZE  0x00100000u

/* --- the script ------------------------------------------------------------- */

typedef struct { uint32_t mask, from, to; } press;
typedef struct { int x, y; uint32_t from, to; } touch;
typedef struct { int right; uint8_t x, y; uint32_t from, to; } tilt;

static struct {
    const char *elf;
    uint32_t    frames_max;
    const char *dump_path;
    uint32_t    dump_frame;      /* 0: at exit */
    const char *dump_dir;        /* --dump-dir: a frame every so many vertical blanks */
    uint32_t    dump_every;
    press       presses[16];
    int         press_count;
    touch       touches[16];
    int         touch_count;
    tilt        tilts[16];
    int         tilt_count;
    uint32_t    frame;
    int         window;          /* --window: show frames and read the keyboard */
    int         window_w, window_h;      /* --window-size, or nothing */
    int         fullscreen;      /* --fullscreen */
    const char *title;           /* --title: the window's name, when it is a game's */
    uint32_t    shown_flips;     /* at the last title update */
    uint32_t    sample_from, sample_to;  /* VITARECOMP_HOST_PROFILE: the vertical blanks sampled between, if given */
    uint32_t    ran_from, ran_to;        /* VITARECOMP_COVERAGE: the vertical blanks counted between */
    const char *ran_path;
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
    vita_sampler_report(stderr);
    /* with the trace on, where every thread was left */
    const char *e = getenv("VITARECOMP_TRACE");
    if (e && *e && *e != '0') {
        fprintf(stderr, "vita_host: threads at the end:\n");
        vita_sched_dump(stderr);
    }
}

static void finish(int status) {
    if (g.dump_path && g.dump_frame == 0) write_ppm(g.dump_path);
    summary();
    exit(status);
}

/* --touch X,Y@FROM[-TO]: a finger on the front panel at display pixel X,Y. */
static int parse_touch(const char *s) {
    int x, y;
    unsigned from, to;
    if (sscanf(s, "%d,%d@%u-%u", &x, &y, &from, &to) == 4) {}
    else if (sscanf(s, "%d,%d@%u", &x, &y, &from) == 3) to = from + 1;
    else return 1;
    if (g.touch_count == 16 || x < 0 || x > 959 || y < 0 || y > 543) return 1;
    g.touches[g.touch_count].x = x * 2 + 1;
    g.touches[g.touch_count].y = y * 2 + 1;
    g.touches[g.touch_count].from = from;
    g.touches[g.touch_count].to = to;
    g.touch_count++;
    return 0;
}

/* --stick L|R,X,Y@FROM[-TO]: the left or right stick held at X,Y, each from
 * -100 (left, up) to 100 (right, down). */
static int parse_stick(const char *s) {
    char which;
    int x, y;
    unsigned from, to;
    if (sscanf(s, "%c,%d,%d@%u-%u", &which, &x, &y, &from, &to) == 5) {}
    else if (sscanf(s, "%c,%d,%d@%u", &which, &x, &y, &from) == 4) to = from + 1;
    else return 1;
    if (g.tilt_count == 16 || (which != 'L' && which != 'R') || x < -100 || x > 100 || y < -100 || y > 100) return 1;
    g.tilts[g.tilt_count].right = which == 'R';
    g.tilts[g.tilt_count].x = (uint8_t)(128 + x * 127 / 100);
    g.tilts[g.tilt_count].y = (uint8_t)(128 + y * 127 / 100);
    g.tilts[g.tilt_count].from = from;
    g.tilts[g.tilt_count].to = to;
    g.tilt_count++;
    return 0;
}

/* The window's turn at a vertical blank: the frame the program is showing,
 * a title that says how fast it is flipping, and the wait for the next
 * sixtieth of a second. Closing the window ends the run. */
static void window_vblank(void) {
    vita_framebuf fb;
    if (vita_display_current(&fb)) {
        const uint8_t *px = (const uint8_t *)vita_mem_ptr(fb.base, fb.pitch * fb.height * 4);
        if (px) vita_window_present(px, (int)fb.width, (int)fb.height, (int)fb.pitch);
    }
    if (g.frame % 60 == 0) {
        char title[160];
        uint32_t flips = vita_display_flips();
        if (g.title) snprintf(title, sizeof(title), "%s | %u frames a second", g.title, flips - g.shown_flips);
        else snprintf(title, sizeof(title), "vita_host | %s | %u flips a second | vblank %u",
                      g.elf, flips - g.shown_flips, g.frame);
        g.shown_flips = flips;
        vita_window_title(title);
    }
    vita_window_pace();
    if (vita_window_closed()) {
        fprintf(stderr, "vita_host: window closed at vblank %u\n", g.frame);
        finish(0);
    }
}

void vita_host_vblank(void) {
    g.frame++;
    if (g.window) window_vblank();
    if (g.sample_to) vita_sampler_run(g.frame >= g.sample_from && g.frame < g.sample_to);
    if (g.ran_to && g.frame == g.ran_from) vita_trace_fns = 2;
    if (g.ran_to && g.frame == g.ran_to) {
        FILE *f = fopen(g.ran_path, "w");
        vita_trace_fns = 0;
        if (f) { vita_coverage_write(f); fclose(f); }
        fprintf(stderr, "vita_host: %s %s (functions entered in vblanks %u to %u)\n",
                f ? "wrote" : "cannot write", g.ran_path, g.ran_from, g.ran_to);
    }
    if (g.dump_path && g.frame == g.dump_frame) write_ppm(g.dump_path);
    if (g.dump_dir && g.frame % g.dump_every == 0) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/vblank-%06u.ppm", g.dump_dir, g.frame);
        write_ppm(path);
    }
    if (g.frames_max && g.frame >= g.frames_max) {
        fprintf(stderr, "vita_host: frame limit %u reached\n", g.frames_max);
        finish(0);
    }
}

uint32_t vita_host_buttons(void) {
    uint32_t m = 0;
    for (int i = 0; i < g.press_count; i++)
        if (g.frame >= g.presses[i].from && g.frame < g.presses[i].to) m |= g.presses[i].mask;
    if (g.window) m |= vita_window_buttons();
    return m;
}

void vita_host_sticks(uint8_t xy[4]) {
    xy[0] = xy[1] = xy[2] = xy[3] = 128;
    if (g.window) vita_window_sticks(xy);
    for (int i = 0; i < g.tilt_count; i++)
        if (g.frame >= g.tilts[i].from && g.frame < g.tilts[i].to) {
            xy[g.tilts[i].right ? 2 : 0] = g.tilts[i].x;
            xy[g.tilts[i].right ? 3 : 1] = g.tilts[i].y;
        }
}

int vita_host_touch(int *x, int *y) {
    for (int i = 0; i < g.touch_count; i++)
        if (g.frame >= g.touches[i].from && g.frame < g.touches[i].to) {
            *x = g.touches[i].x;
            *y = g.touches[i].y;
            return 1;
        }
    return g.window ? vita_window_touch(x, y) : 0;
}

void vita_host_exit(int status) {
    fprintf(stderr, "vita_host: sceKernelExitProcess(%d) at vblank %u\n", status, g.frame);
    finish(status);
}

/* --- main ------------------------------------------------------------------- */

static int usage(void) {
    fputs("usage: vita_host <module.elf> [--frames N] [--dump out.ppm[@FRAME]] [--dump-dir DIR[@EVERY]]\n"
          "                 [--press BUTTON@FROM[-TO]]... [--touch X,Y@FROM[-TO]]... [--virtual-clock]\n"
          "                 [--stick L|R,X,Y@FROM[-TO]]...\n"
          "                 [--syms module.sym] [--trace-fns]\n"
          "                 [--window] [--window-size WxH] [--fullscreen] [--title TEXT]\n"
          "  --touch X,Y@F-T a finger on the front panel at display pixel X,Y (of 960 by 544)\n"
          "                 from vblank F to T\n"
          "  --stick S,X,Y@F-T the left (L) or right (R) stick at X,Y, each -100 to 100\n"
          "  --window       show each frame in a window at sixty a second. Arrows: the pad;\n"
          "                 W A S D and I J K L: the sticks; X C Z V: cross circle square\n"
          "                 triangle; Q E: the shoulders; Enter: START; Shift: SELECT;\n"
          "                 the left mouse button is a finger on the screen; F11: the whole\n"
          "                 screen and back\n"
          "  --window-size WxH the size of the window's picture (960x544)\n"
          "  --fullscreen   the window over the whole screen from the start\n"
          "  --title TEXT   the window's name\n"
          "  --mount P=DIR  where the guest mount P (app0, ux0, ...) lives on the host\n"
          "  --syms F       names from an `nm -n` listing, for traces\n"
          "  --trace-fns    log every function entry (VITARECOMP_TRACE=1 logs traps and bad accesses;\n"
          "                 VITARECOMP_PROFILE=N prints every N seconds where the title's time went,\n"
          "                 VITARECOMP_HOST_PROFILE=MS[@FROM-TO] at the end where the host's did,\n"
          "                 VITARECOMP_COVERAGE=FROM-TO@FILE writes which functions ran between\n"
          "                 two vertical blanks, for comparing two runs)\n"
          "  --trace-insns LO-HI  log registers at each instruction in [LO, HI]; needs C emitted\n"
          "                 with VITARECOMP_EMIT_PC=1, which also names the PC of bad accesses\n",
          stderr);
    return 2;
}

int vita_host_main(int argc, char **argv) {
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
        } else if (strcmp(argv[i], "--dump-dir") == 0 && i + 1 < argc) {
            char *spec = argv[++i];
            char *at = strrchr(spec, '@');
            g.dump_every = 60;
            if (at) { *at = '\0'; g.dump_every = (uint32_t)strtoul(at + 1, NULL, 0); }
            if (!g.dump_every) g.dump_every = 1;
            g.dump_dir = spec;
        } else if (strcmp(argv[i], "--press") == 0 && i + 1 < argc) {
            if (parse_press(argv[++i])) { fprintf(stderr, "vita_host: bad --press %s\n", argv[i]); return 2; }
        } else if (strcmp(argv[i], "--stick") == 0 && i + 1 < argc) {
            if (parse_stick(argv[++i])) { fprintf(stderr, "vita_host: bad --stick %s\n", argv[i]); return 2; }
        } else if (strcmp(argv[i], "--touch") == 0 && i + 1 < argc) {
            if (parse_touch(argv[++i])) { fprintf(stderr, "vita_host: bad --touch %s\n", argv[i]); return 2; }
        } else if (strcmp(argv[i], "--syms") == 0 && i + 1 < argc) {
            if (vita_symbols_load(argv[++i])) { fprintf(stderr, "vita_host: cannot read %s\n", argv[i]); return 2; }
            fprintf(stderr, "vita_host: %u symbols from %s\n", vita_symbols_count(), argv[i]);
        } else if (strcmp(argv[i], "--mount") == 0 && i + 1 < argc) {
            char *spec = argv[++i];
            char *eq = strchr(spec, '=');
            if (!eq) { fprintf(stderr, "vita_host: --mount wants PREFIX=DIR\n"); return 2; }
            *eq = '\0';
            vita_io_mount(spec, eq + 1);
        } else if (strcmp(argv[i], "--trace-fns") == 0) {
            vita_trace_fns = 1;
        } else if (strcmp(argv[i], "--virtual-clock") == 0) {
            vita_clock_virtual();
        } else if (strcmp(argv[i], "--window") == 0) {
            g.window = 1;
        } else if (strcmp(argv[i], "--window-size") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &g.window_w, &g.window_h) != 2 || g.window_w < 96 || g.window_h < 54 ||
                g.window_w > 16384 || g.window_h > 16384) {
                fprintf(stderr, "vita_host: bad --window-size %s\n", argv[i]);
                return 2;
            }
        } else if (strcmp(argv[i], "--fullscreen") == 0) {
            g.fullscreen = 1;
        } else if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
            g.title = argv[++i];
        } else if (strcmp(argv[i], "--trace-insns") == 0 && i + 1 < argc) {
            unsigned lo, hi;
            if (sscanf(argv[++i], "%x-%x", &lo, &hi) != 2) { fprintf(stderr, "vita_host: bad --trace-insns window\n"); return 2; }
            vita_trace_insns = 1;
            vita_trace_lo = lo;
            vita_trace_hi = hi;
        } else {
            return usage();
        }
    }

    if (g.window && vita_window_open(g.title ? g.title : "vita_host", g.window_w ? g.window_w : 960,
                                     g.window_h ? g.window_h : 544)) {
        fprintf(stderr, "vita_host: this build has no window; running headless\n");
        g.window = 0;
    }
    if (g.window && g.fullscreen) vita_window_fullscreen();

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
    vita_kernel_init(KERNEL_BASE, KERNEL_SIZE);
    vita_tls_init(vita_module_tls_addr, vita_module_tls_filesz, vita_module_tls_memsz);
    vita_vars_bind();
    vita_main_stack_init(STACK_TOP - STACK_SIZE, STACK_SIZE);
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

    /* VITARECOMP_COVERAGE=FROM-TO@FILE: the functions entered between two
     * vertical blanks, each with its count, written to FILE. */
    {
        const char *e = getenv("VITARECOMP_COVERAGE");
        unsigned from = 0, to = 0;
        const char *at = e ? strchr(e, '@') : NULL;
        if (at && sscanf(e, "%u-%u", &from, &to) == 2 && to > from) {
            g.ran_from = from; g.ran_to = to; g.ran_path = at + 1;
        }
    }

    /* VITARECOMP_HOST_PROFILE=MS[@FROM-TO]: the host's own threads sampled
     * every MS milliseconds, between two vertical blanks if given. */
    {
        const char *e = getenv("VITARECOMP_HOST_PROFILE");
        unsigned ms = 0, from = 0, to = 0;
        if (e && sscanf(e, "%u@%u-%u", &ms, &from, &to) >= 1 && ms) {
            if (vita_sampler_start(ms)) fprintf(stderr, "vita_host: this build has no sampler\n");
            else if (to > from) { g.sample_from = from; g.sample_to = to; }
            else vita_sampler_run(1);
        }
    }

    /* VITARECOMP_PROFILE=N: every N seconds, where the samples fell. */
    {
        const char *e = getenv("VITARECOMP_PROFILE");
        int period = e ? atoi(e) : 0;
        if (period > 0) vita_profile_start((uint32_t)period);
    }

    vita_module_start();

    fprintf(stderr, "vita_host: module_start returned r0=%u at vblank %u\n", r0, g.frame);
    finish(0);
    return 0;
}
