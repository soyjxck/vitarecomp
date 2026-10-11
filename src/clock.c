/* clock.c — the one clock everything in the runtime reads. See clock.h. */

#include "clock.h"
#include "vitarecomp/host.h"

#include <threads.h>
#include <time.h>

#define VBLANKS_PER_SECOND 60ull
#define VIRTUAL_EPOCH_S    1767225600ull       /* 2026-01-01T00:00:00Z */

static int      g_virtual;
static uint64_t g_now;                          /* the virtual clock, in microseconds */

void vita_clock_virtual(void) { g_virtual = 1; }

static uint64_t host_unix_us(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (uint64_t)t.tv_sec * 1000000ull + (uint64_t)t.tv_nsec / 1000ull;
}

uint64_t vita_clock_us(void) {
    static uint64_t start;
    if (g_virtual) return g_now++;
    uint64_t now = host_unix_us();
    if (!start) start = now;
    return now - start;
}

uint64_t vita_clock_unix_us(void) {
    return g_virtual ? VIRTUAL_EPOCH_S * 1000000ull + g_now : host_unix_us();
}

void vita_clock_wait(uint64_t us) {
    if (g_virtual) { g_now += us; return; }
    struct timespec ts = { (time_t)(us / 1000000ull), (long)((us % 1000000ull) * 1000ull) };
    thrd_sleep(&ts, NULL);
}

/* The virtual clock goes to the start of the next sixtieth of a second. */
void vita_clock_vblank(void) {
    if (!g_virtual) return;
    uint64_t frame = g_now * VBLANKS_PER_SECOND / 1000000ull + 1;
    g_now = (frame * 1000000ull + VBLANKS_PER_SECOND - 1) / VBLANKS_PER_SECOND;
}
