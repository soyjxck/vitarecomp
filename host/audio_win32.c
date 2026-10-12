/* audio_win32.c — the host's sound device on Windows: the wave output.
 *
 * The oldest of the system's ways to play samples and still the one every
 * machine has: a device is opened for a format, buffers are handed to it,
 * and it says when it has played one. Here a thread keeps a handful of
 * small buffers in flight: whenever the device has finished one, the thread
 * fills it with what the runtime has queued (vita_audio_pull, which gives
 * silence when the program has fallen behind) and hands it back. Small
 * buffers keep what is heard close behind what was played: the device is
 * never more than the handful ahead.
 */

#include "audio.h"

#include "vitarecomp/host.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BUFFERS 4
#define FRAMES  480         /* a hundredth of a second */

static HWAVEOUT g_out;
static HANDLE   g_thread, g_event;
static volatile LONG g_stop;
static WAVEHDR  g_header[BUFFERS];
static int16_t  g_samples[BUFFERS][FRAMES * 2];

static DWORD WINAPI feed(void *unused) {
    (void)unused;
    while (!g_stop) {
        for (int i = 0; i < BUFFERS && !g_stop; i++) {
            WAVEHDR *h = &g_header[i];
            if (h->dwFlags & WHDR_INQUEUE) continue;
            vita_audio_pull(g_samples[i], FRAMES);
            waveOutWrite(g_out, h, sizeof *h);
        }
        WaitForSingleObject(g_event, 20);
    }
    return 0;
}

int vita_host_audio_start(void) {
    WAVEFORMATEX format;
    if (g_out) return 0;
    memset(&format, 0, sizeof format);
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = VITA_AUDIO_RATE;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = VITA_AUDIO_RATE * 4;
    g_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!g_event) return 1;
    if (waveOutOpen(&g_out, WAVE_MAPPER, &format, (DWORD_PTR)g_event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        CloseHandle(g_event);
        g_event = NULL;
        g_out = NULL;
        return 1;
    }
    /* VITARECOMP_AUDIO=quiet: the device is fed as ever and nothing is heard,
     * for a run that is to be measured and not listened to */
    const char *how = getenv("VITARECOMP_AUDIO");
    if (how && !strcmp(how, "quiet")) waveOutSetVolume(g_out, 0);
    for (int i = 0; i < BUFFERS; i++) {
        memset(&g_header[i], 0, sizeof g_header[i]);
        g_header[i].lpData = (LPSTR)g_samples[i];
        g_header[i].dwBufferLength = sizeof g_samples[i];
        waveOutPrepareHeader(g_out, &g_header[i], sizeof g_header[i]);
    }
    g_stop = 0;
    g_thread = CreateThread(NULL, 0, feed, NULL, 0, NULL);
    if (!g_thread) {
        waveOutClose(g_out);
        CloseHandle(g_event);
        g_out = NULL;
        g_event = NULL;
        return 1;
    }
    SetThreadPriority(g_thread, THREAD_PRIORITY_TIME_CRITICAL);
    vita_audio_device(1);
    return 0;
}

void vita_host_audio_stop(void) {
    if (!g_out) return;
    vita_audio_device(0);
    InterlockedExchange(&g_stop, 1);
    SetEvent(g_event);
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    waveOutReset(g_out);
    for (int i = 0; i < BUFFERS; i++) waveOutUnprepareHeader(g_out, &g_header[i], sizeof g_header[i]);
    waveOutClose(g_out);
    CloseHandle(g_event);
    g_out = NULL;
    g_thread = g_event = NULL;
}
