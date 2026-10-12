/* hle_audio.c — SceAudio: the output ports, and what the host hears of them.
 *
 * A program opens a port for blocks of so many frames at a rate, mono or
 * stereo, and hands it a block at a time (sceAudioOutOutput), a call that
 * returns when the port has room for the next. That wait is what paces a
 * program's sound thread, with or without anything to hear it.
 *
 * What is heard goes through here: each port has a queue at the host's
 * rate (48,000 frames a second, two channels), a block is brought to that
 * rate as it is queued (a straight line between two of its frames) at the
 * port's volume, and the host's sound device takes the sum of the queues
 * (vita_audio_pull) on a thread of its own. A queue has one writer, the
 * program's thread that holds the baton, and one reader, the device's
 * thread: its two ends are counters each of them alone moves.
 *
 * With no device (a run without a window, or by the virtual clock) nothing
 * is queued, and a block takes as long to hand over as it lasts.
 *
 * Calling convention: see hle.h.
 */

#include "vitarecomp/hle.h"
#include "vitarecomp/host.h"
#include "vitarecomp/recomp_rt.h"
#include "sched.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#include <intrin.h>
#define end_load(p)     ((uint32_t)_InterlockedOr((volatile long *)(p), 0))
#define end_store(p, v) ((void)_InterlockedExchange((volatile long *)(p), (long)(v)))
#else
#define end_load(p)     __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define end_store(p, v) __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
#endif

#define SCE_AUDIO_OUT_ERROR_INVALID_PORT 0x80260002u
#define SCE_AUDIO_VOLUME_0DB             32768

#define PORTS        8
#define QUEUE_FRAMES 16384u                 /* a third of a second; a power of two */
#define QUEUE_MOST   3072u                  /* a block waits while more than this is still to be heard */

typedef struct {
    int      used;
    uint32_t len, freq, channels;
    int32_t  volume[2];
    int16_t  queue[QUEUE_FRAMES * 2];
    volatile uint32_t read, written;        /* frames taken and frames put, never wrapped */
    uint32_t phase;                         /* where between two of the block's frames the next one out falls, 16.16 */
    int16_t  before[2];                     /* the frame before the block's first */
} port;

static port g_ports[PORTS];
static volatile uint32_t g_device;          /* a host device is taking what is queued */
static uint64_t g_taken, g_empty;           /* the device's thread's own count */

static port *port_of(uint32_t handle) {
    return handle >= 1 && handle <= PORTS && g_ports[handle - 1].used ? &g_ports[handle - 1] : NULL;
}

static uint32_t queued(const port *p) { return end_load(&p->written) - end_load(&p->read); }

/* A block onto a port's queue, at the host's rate. What the queue has no
 * room for is lost: the device has stopped taking it. */
static void queue_block(port *p, const int16_t *in, uint32_t frames) {
    const uint32_t step = (uint32_t)(((uint64_t)p->freq << 16) / VITA_AUDIO_RATE);
    uint32_t written = end_load(&p->written);
    const uint32_t read = end_load(&p->read);
    uint32_t phase = p->phase;                  /* 0 is the frame before the block, 1.0 its first */
    while ((phase >> 16) <= frames) {
        const uint32_t i = phase >> 16, f = phase & 0xFFFF;
        int32_t a[2], b[2];
        if (i == frames) break;                 /* between the block's last frame and the next block's first */
        for (uint32_t c = 0; c < 2; c++) {
            const uint32_t src = p->channels == 2 ? c : 0;
            a[c] = i ? in[(i - 1) * p->channels + src] : p->before[src];
            b[c] = in[i * p->channels + src];
        }
        if (written - read < QUEUE_FRAMES) {
            int16_t *out = &p->queue[(written % QUEUE_FRAMES) * 2];
            for (uint32_t c = 0; c < 2; c++) {
                int32_t s = (int32_t)(((int64_t)a[c] * (0x10000 - f) + (int64_t)b[c] * f) >> 16);
                s = (int32_t)(((int64_t)s * p->volume[c]) / SCE_AUDIO_VOLUME_0DB);
                out[c] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
            }
            written++;
        }
        phase += step;
    }
    p->phase = phase - (frames << 16);
    if (frames) for (uint32_t c = 0; c < p->channels; c++) p->before[c] = in[(frames - 1) * p->channels + c];
    end_store(&p->written, written);
}

void vita_audio_device(int present) { end_store(&g_device, present ? 1u : 0u); }

void vita_audio_pull(int16_t *out, uint32_t frames) {
    static int32_t sum[2 * 4096];
    /* VITARECOMP_AUDIO_HEARD=<file>: what the device is given, as it takes
     * it, the silence of a program that fell behind included: sixteen bits,
     * two channels, the host's rate. And <file>.gaps, a line for each time
     * the device asked and less was queued than it took: how many frames it
     * had taken until then, and how many were wanting. A program's own
     * silence is queued like any sound, and is not a gap. */
    static FILE *heard, *gaps;
    static int asked;
    int16_t *const first = out;
    const uint32_t all = frames;
    if (!asked) {
        const char *path = getenv("VITARECOMP_AUDIO_HEARD");
        asked = 1;
        if (path && *path) {
            char beside[1024];
            heard = fopen(path, "wb");
            snprintf(beside, sizeof beside, "%s.gaps", path);
            gaps = fopen(beside, "w");
        }
    }
    while (frames) {
        const uint32_t n = frames < 4096 ? frames : 4096;
        uint32_t most = 0;
        memset(sum, 0, n * 2 * sizeof sum[0]);
        for (int i = 0; i < PORTS; i++) {
            port *p = &g_ports[i];
            if (!p->used) continue;
            uint32_t read = end_load(&p->read);
            uint32_t have = end_load(&p->written) - read;
            if (have > n) have = n;
            if (have > most) most = have;
            for (uint32_t k = 0; k < have; k++, read++) {
                const int16_t *in = &p->queue[(read % QUEUE_FRAMES) * 2];
                sum[2 * k] += in[0];
                sum[2 * k + 1] += in[1];
            }
            end_store(&p->read, read);
        }
        for (uint32_t k = 0; k < n * 2; k++) out[k] = (int16_t)(sum[k] > 32767 ? 32767 : sum[k] < -32768 ? -32768 : sum[k]);
        if (gaps && most < n) { fprintf(gaps, "%llu %u\n", (unsigned long long)g_taken, n - most); fflush(gaps); }
        g_taken += n;
        g_empty += n - most;
        out += n * 2;
        frames -= n;
    }
    if (heard) { fwrite(first, 4, all, heard); fflush(heard); }
}

void vita_audio_counts(uint64_t *taken, uint64_t *empty) { *taken = g_taken; *empty = g_empty; }

/* int sceAudioOutOpenPort(SceAudioOutPortType type, int len, int freq, SceAudioOutMode mode) */
void vita_hle_sceAudioOutOpenPort(void) {
    int i;
    for (i = 0; i < PORTS && g_ports[i].used; i++) {}
    if (i == PORTS) { r0 = SCE_AUDIO_OUT_ERROR_INVALID_PORT; return; }
    port *p = &g_ports[i];
    memset(p, 0, sizeof *p);
    p->len = r1 ? r1 : 256;
    p->freq = r2 ? r2 : VITA_AUDIO_RATE;
    p->channels = r3 ? 2 : 1;
    p->volume[0] = p->volume[1] = SCE_AUDIO_VOLUME_0DB;
    p->phase = 0x10000;
    p->used = 1;
    vita_hle_trace("sceAudioOutOpenPort(type %u, len %u, %u Hz, mode %u) -> %d", r0, r1, r2, r3, i + 1);
    r0 = (uint32_t)i + 1;
}

/* int sceAudioOutOutput(int port, const void *buf): returns when the port
 * has room for the block after this one */
void vita_hle_sceAudioOutOutput(void) {
    port *p = port_of(r0);
    if (!p) { r0 = SCE_AUDIO_OUT_ERROR_INVALID_PORT; return; }
    const uint32_t handle = r0;
    const int16_t *samples = r1 ? (const int16_t *)vita_mem_ptr(r1, p->len * p->channels * 2) : NULL;

    /* VITARECOMP_AUDIO_DUMP=<folder>: what each port is given, as it is given
     * (sixteen bits, the port's rate, one channel or two), a file a port */
    static const char *dump;
    static int asked;
    if (!asked) { dump = getenv("VITARECOMP_AUDIO_DUMP"); asked = 1; }
    if (dump && *dump && samples) {
        char path[1024];
        snprintf(path, sizeof path, "%s/port%u-%uHz-%uch.pcm", dump, handle, p->freq, p->channels);
        FILE *f = fopen(path, "ab");
        if (f) { fwrite(samples, 1, p->len * p->channels * 2, f); fclose(f); }
    }

    if (!end_load(&g_device)) {
        const uint32_t us = (uint32_t)((uint64_t)p->len * 1000000ull / p->freq);
        vita_sched_delay(us ? us : 1000);
        r0 = 0;
        return;
    }
    if (samples) queue_block(p, samples, p->len);
    /* the device takes it at its own pace; a queue that stays full is a device that stopped */
    for (int waited = 0; queued(p) > QUEUE_MOST && waited < 500 && port_of(handle) == p; waited++) vita_sched_delay(1000);
    r0 = 0;
}

void vita_hle_sceAudioOutReleasePort(void) {
    port *p = port_of(r0);
    if (p) p->used = 0;
    r0 = 0;
}

/* int sceAudioOutSetVolume(int port, SceAudioOutChannelFlag ch, int *vol):
 * bit 0 of `ch` is the left channel and bit 1 the right; 32768 is as it is */
void vita_hle_sceAudioOutSetVolume(void) {
    port *p = port_of(r0);
    if (!p) { r0 = SCE_AUDIO_OUT_ERROR_INVALID_PORT; return; }
    if (vita_mem_ptr(r2, 8)) {
        if (r1 & 1) p->volume[0] = (int32_t)vita_read32(r2);
        if (r1 & 2) p->volume[1] = (int32_t)vita_read32(r2 + 4);
    }
    r0 = 0;
}

/* int sceAudioOutSetConfig(int port, SceSize len, int freq, SceAudioOutMode mode): a negative one is left as it was */
void vita_hle_sceAudioOutSetConfig(void) {
    port *p = port_of(r0);
    if (!p) { r0 = SCE_AUDIO_OUT_ERROR_INVALID_PORT; return; }
    if ((int32_t)r1 > 0) p->len = r1;
    if ((int32_t)r2 > 0) p->freq = r2;
    if ((int32_t)r3 >= 0) p->channels = r3 ? 2 : 1;
    r0 = 0;
}

/* int sceAudioOutGetRestSample(int port): frames of the port's own still to be heard */
void vita_hle_sceAudioOutGetRestSample(void) {
    const port *p = port_of(r0);
    r0 = p ? (uint32_t)((uint64_t)queued(p) * p->freq / VITA_AUDIO_RATE) : SCE_AUDIO_OUT_ERROR_INVALID_PORT;
}
void vita_hle_sceAudioOutGetAdopt(void) { r0 = 1; }
