/* hle_sas.c — SceSas: the small synthesiser, the one with numbered voices.
 *
 * Where the other synthesiser (hle_ngs.c) is a graph a program builds, this
 * one is a fixed bank of voices: a voice is given a sound (ADPCM blocks, or
 * PCM), a pitch, two volumes and an envelope, keyed on and off, and once
 * per audio block the program asks for the mix of all of them
 * (sceSasCore), `grain` stereo frames of sixteen bits, which it then hands
 * to an output port itself.
 *
 * What is here keeps the bank and leaves out the sound, as hle_ngs.c does
 * for its own: every call is accepted and remembered, the mix is silence,
 * and a voice that was keyed on has ended by the time anyone asks, so a
 * program that waits for a sound to finish before it frees it or plays the
 * next is not kept waiting. A decoder and a mixer can replace the silence
 * without the program seeing a difference in anything but the samples.
 *
 * Calling convention: see hle.h.
 */

#include "vitarecomp/hle.h"
#include "vitarecomp/recomp_rt.h"

#include <string.h>

#define SCE_SAS_ERROR_ADDRESS       0x80420005u
#define SCE_SAS_ERROR_VOICE_INDEX   0x80420010u
#define SCE_SAS_VOICES              32
#define SCE_SAS_GRAIN_DEFAULT       256
#define SAS_MEMORY                  0x8000u     /* what the bank is said to need lent to it */

typedef struct {
    uint32_t data, size, loop;      /* the sound: a guest address, its length, whether it repeats */
    int32_t  pitch, left, right;
    uint32_t adsr1, adsr2;
    int      on;
} voice;

static voice    g_voice[SCE_SAS_VOICES];
static uint32_t g_grain = SCE_SAS_GRAIN_DEFAULT;
static uint32_t g_buffer, g_buffer_size;

static voice *voice_of(uint32_t n) { return n < SCE_SAS_VOICES ? &g_voice[n] : NULL; }

/* int sceSasGetNeededMemorySize(const char *config, SceSize *outSize) */
void vita_hle_sceSasGetNeededMemorySize(void) {
    if (!vita_mem_ptr(r1, 4)) { r0 = SCE_SAS_ERROR_ADDRESS; return; }
    vita_write32(r1, SAS_MEMORY);
    r0 = 0;
}

/* int sceSasInit(const char *config, void *buffer, SceSize size) and
 * int sceSasInitWithGrain(const char *config, SceUInt32 grain, void *buffer, SceSize size) */
void vita_hle_sceSasInit(void) {
    memset(g_voice, 0, sizeof g_voice);
    g_grain = SCE_SAS_GRAIN_DEFAULT;
    g_buffer = r1; g_buffer_size = r2;
    vita_hle_trace("sceSasInit: %u frames a block", g_grain);
    r0 = 0;
}
void vita_hle_sceSasInitWithGrain(void) {
    memset(g_voice, 0, sizeof g_voice);
    g_grain = r1 ? r1 : SCE_SAS_GRAIN_DEFAULT;
    g_buffer = r2; g_buffer_size = r3;
    vita_hle_trace("sceSasInitWithGrain: %u frames a block", g_grain);
    r0 = 0;
}

/* int sceSasExit(void **outBuffer, SceSize *outSize): the lent memory back */
void vita_hle_sceSasExit(void) {
    if (r0 && vita_mem_ptr(r0, 4)) vita_write32(r0, g_buffer);
    if (r1 && vita_mem_ptr(r1, 4)) vita_write32(r1, g_buffer_size);
    r0 = 0;
}

void vita_hle_sceSasSetGrain(void)      { if (r0) g_grain = r0; r0 = 0; }
void vita_hle_sceSasGetGrain(void)      { r0 = g_grain; }
void vita_hle_sceSasSetOutputmode(void) { r0 = 0; }
void vita_hle_sceSasGetOutputmode(void) { r0 = 0; }

/* int sceSasSetVoice(int voice, const void *vag, SceSize size, SceUInt32 loop) */
void vita_hle_sceSasSetVoice(void) {
    voice *v = voice_of(r0);
    if (!v) { r0 = SCE_SAS_ERROR_VOICE_INDEX; return; }
    v->data = r1; v->size = r2; v->loop = r3;
    r0 = 0;
}
/* int sceSasSetVoicePCM(int voice, const void *pcm, SceSize size, int loopSize) */
void vita_hle_sceSasSetVoicePCM(void) { vita_hle_sceSasSetVoice(); }
void vita_hle_sceSasSetNoise(void)    { r0 = voice_of(r0) ? 0 : SCE_SAS_ERROR_VOICE_INDEX; }

/* int sceSasSetPitch(int voice, int pitch): 0x1000 is the sound's own rate */
void vita_hle_sceSasSetPitch(void) {
    voice *v = voice_of(r0);
    if (!v) { r0 = SCE_SAS_ERROR_VOICE_INDEX; return; }
    v->pitch = (int32_t)r1;
    r0 = 0;
}

/* int sceSasSetVolume(int voice, int left, int right, int wetLeft, int wetRight) */
void vita_hle_sceSasSetVolume(void) {
    voice *v = voice_of(r0);
    if (!v) { r0 = SCE_SAS_ERROR_VOICE_INDEX; return; }
    v->left = (int32_t)r1; v->right = (int32_t)r2;
    r0 = 0;
}

/* int sceSasSetSimpleADSR(int voice, SceUInt16 adsr1, SceUInt16 adsr2) and the long forms */
void vita_hle_sceSasSetSimpleADSR(void) {
    voice *v = voice_of(r0);
    if (!v) { r0 = SCE_SAS_ERROR_VOICE_INDEX; return; }
    v->adsr1 = r1 & 0xFFFF; v->adsr2 = r2 & 0xFFFF;
    r0 = 0;
}
void vita_hle_sceSasSetADSR(void)     { r0 = voice_of(r0) ? 0 : SCE_SAS_ERROR_VOICE_INDEX; }
void vita_hle_sceSasSetADSRmode(void) { r0 = voice_of(r0) ? 0 : SCE_SAS_ERROR_VOICE_INDEX; }
void vita_hle_sceSasSetSL(void)       { r0 = voice_of(r0) ? 0 : SCE_SAS_ERROR_VOICE_INDEX; }

/* int sceSasSetKeyOn(int voice) / sceSasSetKeyOff(int voice) */
void vita_hle_sceSasSetKeyOn(void) {
    voice *v = voice_of(r0);
    if (!v) { r0 = SCE_SAS_ERROR_VOICE_INDEX; return; }
    v->on = 1;
    r0 = 0;
}
void vita_hle_sceSasSetKeyOff(void) {
    voice *v = voice_of(r0);
    if (!v) { r0 = SCE_SAS_ERROR_VOICE_INDEX; return; }
    v->on = 0;
    r0 = 0;
}
void vita_hle_sceSasSetPause(void)      { r0 = 0; }
void vita_hle_sceSasGetPauseState(void) { r0 = 0; }

/* int sceSasGetEndState(int voice): 1 when the voice has nothing left to play */
void vita_hle_sceSasGetEndState(void) { r0 = voice_of(r0) ? 1u : SCE_SAS_ERROR_VOICE_INDEX; }
/* int sceSasGetEnvelope(int voice): the envelope's height, 0 for a silent voice */
void vita_hle_sceSasGetEnvelope(void) { r0 = voice_of(r0) ? 0u : SCE_SAS_ERROR_VOICE_INDEX; }

/* int sceSasCore(SceInt16 *out): the block's mix, `grain` stereo frames */
void vita_hle_sceSasCore(void) {
    void *out = vita_mem_ptr(r0, g_grain * 4);
    if (!out) { r0 = SCE_SAS_ERROR_ADDRESS; return; }
    memset(out, 0, g_grain * 4);
    for (int i = 0; i < SCE_SAS_VOICES; i++) g_voice[i].on = 0;
    r0 = 0;
}

/* int sceSasCoreWithMix(SceInt16 *inOut, int left, int right): the mix added
 * to what the buffer holds, that scaled by the two volumes (0x1000 is as it is) */
void vita_hle_sceSasCoreWithMix(void) {
    int16_t *io = (int16_t *)vita_mem_ptr(r0, g_grain * 4);
    const int32_t left = (int32_t)r1, right = (int32_t)r2;
    if (!io) { r0 = SCE_SAS_ERROR_ADDRESS; return; }
    for (uint32_t k = 0; k < g_grain; k++) {
        io[2 * k]     = (int16_t)((io[2 * k] * left) >> 12);
        io[2 * k + 1] = (int16_t)((io[2 * k + 1] * right) >> 12);
    }
    for (int i = 0; i < SCE_SAS_VOICES; i++) g_voice[i].on = 0;
    r0 = 0;
}
