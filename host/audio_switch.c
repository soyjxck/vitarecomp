/* audio_switch.c — the host's sound device on the Nintendo Switch.
 *
 * The console's plain audio output takes buffers of sixteen-bit stereo at
 * 48,000 frames a second, which is the rate the runtime keeps its queues
 * at, and hands each back when it has played it. A thread keeps three small
 * buffers in flight: whenever one comes back it is filled with what the
 * runtime has queued (vita_audio_pull, silence where the program has fallen
 * behind) and given to the device again. The buffers are the system's to
 * read while they are out, so each is its own pages.
 */

#include "audio.h"

#include "vitarecomp/host.h"

#include <malloc.h>
#include <string.h>

#include <switch.h>

#define BUFFERS 3
#define FRAMES  960                             /* a fiftieth of a second */
#define BYTES   (FRAMES * 4)
#define ROUNDED ((BYTES + 0xFFF) & ~0xFFF)      /* a buffer is whole pages */

static AudioOutBuffer g_buffer[BUFFERS];
static Thread         g_thread;
static volatile int   g_stop, g_started;

static void feed(void *unused) {
    (void)unused;
    for (int i = 0; i < BUFFERS; i++) {
        vita_audio_pull((int16_t *)g_buffer[i].buffer, FRAMES);
        audoutAppendAudioOutBuffer(&g_buffer[i]);
    }
    while (!g_stop) {
        AudioOutBuffer *played = NULL;
        u32 count = 0;
        if (R_FAILED(audoutWaitPlayFinish(&played, &count, 100000000ull)) || !count || !played) continue;
        vita_audio_pull((int16_t *)played->buffer, FRAMES);
        audoutAppendAudioOutBuffer(played);
    }
}

int vita_host_audio_start(void) {
    if (g_started) return 0;
    if (R_FAILED(audoutInitialize())) return 1;
    if (R_FAILED(audoutStartAudioOut())) { audoutExit(); return 1; }
    for (int i = 0; i < BUFFERS; i++) {
        void *pages = memalign(0x1000, ROUNDED);
        if (!pages) { audoutStopAudioOut(); audoutExit(); return 1; }
        memset(pages, 0, ROUNDED);
        memset(&g_buffer[i], 0, sizeof g_buffer[i]);
        g_buffer[i].buffer = pages;
        g_buffer[i].buffer_size = ROUNDED;
        g_buffer[i].data_size = BYTES;
    }
    g_stop = 0;
    /* above the program's own threads: a late buffer is heard */
    if (R_FAILED(threadCreate(&g_thread, feed, NULL, NULL, 0x8000, 0x30, -2)) || R_FAILED(threadStart(&g_thread))) {
        audoutStopAudioOut();
        audoutExit();
        return 1;
    }
    g_started = 1;
    vita_audio_device(1);
    return 0;
}

void vita_host_audio_stop(void) {
    if (!g_started) return;
    vita_audio_device(0);
    g_stop = 1;
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
    audoutStopAudioOut();
    audoutExit();
    g_started = 0;
}
