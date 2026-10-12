/* hle_sas.c — SceSas: the small synthesiser, the one with numbered voices.
 *
 * Where the other synthesiser (hle_ngs.c) is a graph a program builds, this
 * one is a fixed bank of voices, the sound chip of two consoles back kept
 * as a library: a voice is given a sound (ADPCM blocks), a pitch, two
 * volumes and an envelope, keyed on and off, and once per audio block the
 * program asks for the mix of all of them (sceSasCore), `grain` stereo
 * frames of sixteen bits, which it then hands to an output port itself.
 *
 * The sound is VAG: blocks of sixteen bytes, two of header (a predictor
 * and a shift; flags that mark the loop's start, its end, and the last
 * block) and twenty-eight samples of four bits, each the error of a
 * two-tap prediction from the samples before it.
 *
 * The envelope is the old chip's too, in its two packed words
 * (sceSasSetSimpleADSR): an attack that rises to full height, a decay that
 * falls to the sustain level, a sustain that holds, rises or falls until
 * the key is released, and a release that falls to nothing, where the voice
 * ends. Each phase has a rate, seven bits of it: its upper five say how
 * many samples pass between steps (or, for the fast ones, how big a step
 * is) and its lower two trim the step; an exponential phase rises four
 * times slower in its last quarter and falls in proportion to its height.
 *
 * A pitch of 0x1000 plays a sound at one sample a frame, the mix's own
 * rate; the samples between two of the sound's are a straight line. A
 * volume of 0x1000 is the sound as it is.
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

#define ENV_MAX      0x7FFF                     /* the envelope's own height   */
#define ENV_SHIFT    15
#define SAS_ENV_MAX  0x40000000                 /* and as the library reports it */

enum { PHASE_OFF = 0, PHASE_ATTACK, PHASE_DECAY, PHASE_SUSTAIN, PHASE_RELEASE };

typedef struct {
    uint32_t data, size, loop;      /* the sound: a guest address, its length, whether it repeats */
    int32_t  pitch, left, right;
    uint32_t adsr1, adsr2;

    int      phase;                 /* PHASE_OFF: nothing to play */
    int32_t  env;
    uint32_t env_wait;              /* samples until the envelope's next step */

    uint32_t at, loop_at;           /* the next block to decode, and where the loop begins: byte offsets */
    int      ended;                 /* the sound's last block has been decoded */
    int16_t  block[28];
    uint32_t have;                  /* samples of `block` that are real */
    uint32_t pos;                   /* where in the block the voice is, 20.12 */
    int32_t  h1, h2;                /* the two samples the prediction is made from */
} voice;

static voice    g_voice[SCE_SAS_VOICES];
static uint32_t g_grain = SCE_SAS_GRAIN_DEFAULT;
static uint32_t g_buffer, g_buffer_size;

static voice *voice_of(uint32_t n) { return n < SCE_SAS_VOICES ? &g_voice[n] : NULL; }

static int32_t clamp16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }

/* --- the sound ---------------------------------------------------------------------------- */

/* The voice's next block into `block`. Zero when there is no more. */
static int next_block(voice *v) {
    static const int32_t f0[5] = { 0, 60, 115, 98, 122 }, f1[5] = { 0, 0, -52, -55, -60 };
    v->have = 0;
    if (v->ended || v->at + 16 > v->size) return 0;
    const uint8_t *b = (const uint8_t *)vita_mem_ptr(v->data + v->at, 16);
    if (!b) return 0;
    const uint32_t flags = b[1];
    uint32_t shift = b[0] & 15, filter = b[0] >> 4;
    if (flags == 7) { v->ended = 1; return 0; }       /* the mark after the last block: nothing in it */
    if (flags & 4) v->loop_at = v->at;
    if (filter > 4) filter = 0;
    if (shift > 12) shift = 9;
    for (int i = 0; i < 28; i++) {
        const uint32_t nibble = (b[2 + i / 2] >> ((i & 1) * 4)) & 15;
        int32_t s = (int16_t)(nibble << 12) >> shift;
        s += (v->h1 * f0[filter] + v->h2 * f1[filter]) >> 6;
        s = clamp16(s);
        v->h2 = v->h1;
        v->h1 = s;
        v->block[i] = (int16_t)s;
    }
    v->have = 28;
    v->at += 16;
    if (flags & 1) {
        if ((flags & 2) && v->loop) v->at = v->loop_at;
        else v->ended = 1;
    }
    return 1;
}

/* --- the envelope ------------------------------------------------------------------------- */

/* One step of a phase at `rate`: how far the height moves, and in how many
 * samples the next step comes. */
static int32_t env_step(const voice *v, uint32_t rate, int rising, int exponential, uint32_t *wait) {
    const int32_t shift = (int32_t)(rate >> 2);
    int32_t step = rising ? 7 - (int32_t)(rate & 3) : -8 + (int32_t)(rate & 3);
    uint32_t cycles = shift > 11 ? 1u << (shift - 11 > 20 ? 20 : shift - 11) : 1u;
    if (shift < 11) step *= 1 << (11 - shift);
    if (exponential && rising && v->env > 0x6000) cycles *= 4;
    if (exponential && !rising) step = (step * v->env) >> ENV_SHIFT;
    *wait = cycles;
    return step;
}

/* The envelope one sample on. Zero when the voice has run out. */
static int env_advance(voice *v) {
    if (v->env_wait && --v->env_wait) return 1;
    const uint32_t a = v->adsr1, b = v->adsr2;
    switch (v->phase) {
    case PHASE_ATTACK:
        v->env += env_step(v, (a >> 8) & 0x7F, 1, (a >> 15) & 1, &v->env_wait);
        if (v->env >= ENV_MAX) { v->env = ENV_MAX; v->phase = PHASE_DECAY; v->env_wait = 0; }
        return 1;
    case PHASE_DECAY: {
        const int32_t level = (int32_t)((a & 15) + 1) * 0x800;
        v->env += env_step(v, ((a >> 4) & 15) * 4, 0, 1, &v->env_wait);
        if (v->env <= level) { v->env = v->env < 0 ? 0 : v->env; v->phase = PHASE_SUSTAIN; v->env_wait = 0; }
        return 1;
    }
    case PHASE_SUSTAIN:
        v->env += env_step(v, (b >> 6) & 0x7F, !((b >> 14) & 1), (b >> 15) & 1, &v->env_wait);
        if (v->env > ENV_MAX) v->env = ENV_MAX;
        if (v->env < 0) v->env = 0;
        return 1;
    case PHASE_RELEASE:
        v->env += env_step(v, (b & 31) * 4, 0, (b >> 5) & 1, &v->env_wait);
        if (v->env <= 0) { v->env = 0; v->phase = PHASE_OFF; return 0; }
        return 1;
    default:
        return 0;
    }
}

/* --- the mix ------------------------------------------------------------------------------ */

/* The voices added into `acc`, `grain` frames of left and right. */
static void mix(int32_t *acc) {
    for (int n = 0; n < SCE_SAS_VOICES; n++) {
        voice *v = &g_voice[n];
        if (v->phase == PHASE_OFF) continue;
        for (uint32_t k = 0; k < g_grain; k++) {
            while ((v->pos >> 12) >= v->have) {
                const uint32_t over = v->have ? v->pos - (v->have << 12) : 0;
                if (!next_block(v)) { v->phase = PHASE_OFF; break; }
                v->pos = over;
            }
            if (v->phase == PHASE_OFF || !env_advance(v)) break;
            const uint32_t i = v->pos >> 12, frac = v->pos & 0xFFF;
            const int32_t s0 = v->block[i];
            const int32_t s1 = i + 1 < v->have ? v->block[i + 1] : s0;
            const int32_t s = ((s0 * (int32_t)(0x1000 - frac) + s1 * (int32_t)frac) >> 12) * v->env >> ENV_SHIFT;
            acc[2 * k] += (s * v->left) >> 12;
            acc[2 * k + 1] += (s * v->right) >> 12;
            v->pos += (uint32_t)(v->pitch > 0 ? v->pitch : 0);
        }
    }
}

/* --- the library -------------------------------------------------------------------------- */

/* int sceSasGetNeededMemorySize(const char *config, SceSize *outSize) */
void vita_hle_sceSasGetNeededMemorySize(void) {
    if (!vita_mem_ptr(r1, 4)) { r0 = SCE_SAS_ERROR_ADDRESS; return; }
    vita_write32(r1, SAS_MEMORY);
    r0 = 0;
}

static void bank_reset(void) {
    memset(g_voice, 0, sizeof g_voice);
    for (int i = 0; i < SCE_SAS_VOICES; i++) {
        g_voice[i].pitch = 0x1000;
        g_voice[i].adsr1 = 0x00FF;      /* at once to full height, and held */
        g_voice[i].adsr2 = 0x1FC0;
    }
}

/* int sceSasInit(const char *config, void *buffer, SceSize size) and
 * int sceSasInitWithGrain(const char *config, SceUInt32 grain, void *buffer, SceSize size) */
void vita_hle_sceSasInit(void) {
    bank_reset();
    g_grain = SCE_SAS_GRAIN_DEFAULT;
    g_buffer = r1; g_buffer_size = r2;
    vita_hle_trace("sceSasInit: %u frames a block", g_grain);
    r0 = 0;
}
void vita_hle_sceSasInitWithGrain(void) {
    bank_reset();
    g_grain = r1 && r1 <= 4096 ? r1 : SCE_SAS_GRAIN_DEFAULT;
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

void vita_hle_sceSasSetGrain(void)      { if (r0 && r0 <= 4096) g_grain = r0; r0 = 0; }
void vita_hle_sceSasGetGrain(void)      { r0 = g_grain; }
void vita_hle_sceSasSetOutputmode(void) { r0 = 0; }
void vita_hle_sceSasGetOutputmode(void) { r0 = 0; }

/* int sceSasSetVoice(int voice, const void *vag, SceSize size, SceUInt32 loop) */
void vita_hle_sceSasSetVoice(void) {
    voice *v = voice_of(r0);
    if (!v) { r0 = SCE_SAS_ERROR_VOICE_INDEX; return; }
    v->data = r1; v->size = r2; v->loop = r3;
    v->phase = PHASE_OFF;
    r0 = 0;
}
/* int sceSasSetVoicePCM(int voice, const void *pcm, SceSize size, int loopSize) and
 * int sceSasSetNoise(int voice, SceUInt32 clock): taken, and not played yet */
void vita_hle_sceSasSetVoicePCM(void) {
    voice *v = voice_of(r0);
    if (!v) { r0 = SCE_SAS_ERROR_VOICE_INDEX; return; }
    v->data = 0; v->size = 0; v->phase = PHASE_OFF;
    r0 = 0;
}
void vita_hle_sceSasSetNoise(void) { r0 = voice_of(r0) ? 0 : SCE_SAS_ERROR_VOICE_INDEX; }

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

/* int sceSasSetSimpleADSR(int voice, SceUInt16 adsr1, SceUInt16 adsr2) */
void vita_hle_sceSasSetSimpleADSR(void) {
    voice *v = voice_of(r0);
    if (!v) { r0 = SCE_SAS_ERROR_VOICE_INDEX; return; }
    v->adsr1 = r1 & 0xFFFF; v->adsr2 = r2 & 0xFFFF;
    r0 = 0;
}
void vita_hle_sceSasSetADSR(void)     { r0 = voice_of(r0) ? 0 : SCE_SAS_ERROR_VOICE_INDEX; }
void vita_hle_sceSasSetADSRmode(void) { r0 = voice_of(r0) ? 0 : SCE_SAS_ERROR_VOICE_INDEX; }
void vita_hle_sceSasSetSL(void)       { r0 = voice_of(r0) ? 0 : SCE_SAS_ERROR_VOICE_INDEX; }

/* int sceSasSetKeyOn(int voice): the sound from its start, the envelope from nothing */
void vita_hle_sceSasSetKeyOn(void) {
    voice *v = voice_of(r0);
    if (!v) { r0 = SCE_SAS_ERROR_VOICE_INDEX; return; }
    v->phase = v->data && v->size >= 16 ? PHASE_ATTACK : PHASE_OFF;
    v->env = 0; v->env_wait = 0;
    v->at = 0; v->loop_at = 0; v->ended = 0;
    v->have = 0; v->pos = 0; v->h1 = v->h2 = 0;
    r0 = 0;
}
/* int sceSasSetKeyOff(int voice): the release begins */
void vita_hle_sceSasSetKeyOff(void) {
    voice *v = voice_of(r0);
    if (!v) { r0 = SCE_SAS_ERROR_VOICE_INDEX; return; }
    if (v->phase != PHASE_OFF) { v->phase = PHASE_RELEASE; v->env_wait = 0; }
    r0 = 0;
}
void vita_hle_sceSasSetPause(void)      { r0 = 0; }
void vita_hle_sceSasGetPauseState(void) { r0 = 0; }

/* int sceSasGetEndState(int voice): 1 when the voice has nothing left to play */
void vita_hle_sceSasGetEndState(void) {
    const voice *v = voice_of(r0);
    r0 = v ? (v->phase == PHASE_OFF ? 1u : 0u) : SCE_SAS_ERROR_VOICE_INDEX;
}
/* int sceSasGetEnvelope(int voice): the envelope's height */
void vita_hle_sceSasGetEnvelope(void) {
    const voice *v = voice_of(r0);
    r0 = v ? (uint32_t)v->env * (SAS_ENV_MAX / (ENV_MAX + 1)) : SCE_SAS_ERROR_VOICE_INDEX;
}

/* int sceSasCore(SceInt16 *out): the block's mix, `grain` stereo frames */
void vita_hle_sceSasCore(void) {
    static int32_t acc[2 * 4096];
    int16_t *out = (int16_t *)vita_mem_ptr(r0, g_grain * 4);
    if (!out) { r0 = SCE_SAS_ERROR_ADDRESS; return; }
    memset(acc, 0, g_grain * 2 * sizeof acc[0]);
    mix(acc);
    for (uint32_t k = 0; k < g_grain * 2; k++) out[k] = (int16_t)clamp16(acc[k]);
    r0 = 0;
}

/* int sceSasCoreWithMix(SceInt16 *inOut, int left, int right): the mix added
 * to what the buffer holds, that scaled by the two volumes (0x1000 is as it is) */
void vita_hle_sceSasCoreWithMix(void) {
    static int32_t acc[2 * 4096];
    int16_t *io = (int16_t *)vita_mem_ptr(r0, g_grain * 4);
    const int32_t left = (int32_t)r1, right = (int32_t)r2;
    if (!io) { r0 = SCE_SAS_ERROR_ADDRESS; return; }
    for (uint32_t k = 0; k < g_grain; k++) {
        acc[2 * k]     = (io[2 * k] * left) >> 12;
        acc[2 * k + 1] = (io[2 * k + 1] * right) >> 12;
    }
    mix(acc);
    for (uint32_t k = 0; k < g_grain * 2; k++) io[k] = (int16_t)clamp16(acc[k]);
    r0 = 0;
}
