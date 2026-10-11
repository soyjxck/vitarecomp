/* hle_ngs.c — SceNgs, the audio synthesiser, without the sound.
 *
 * The console's synthesiser is a graph: racks of voices, each voice a chain
 * of modules (a sample player, an envelope, filters), patched output to
 * input up to a master buss and mixed once per update. A title's sound
 * engine drives it and reads it back. It asks for a voice's parameter block,
 * writes it, asks again later and expects to find what it wrote. It asks
 * how far a voice has played, and waits for the callback that says one has
 * finished. So the graph's bookkeeping is all here, and what is missing is
 * only the signal: a voice plays for as long as its sample lasts at the
 * rate it was given, and makes no sound.
 *
 * The first version of this handed out a fresh 64-byte record for every
 * voice handle and every parameter lock. A retail sound engine locks
 * parameters about a million times while it starts, each lock found an
 * empty block where its settings should have been, and the records filled
 * the whole memory arena.
 *
 * Nothing here allocates. A rack lives in the block the title gives
 * sceNgsRackInit, of the size sceNgsRackGetRequiredMemorySize asked for,
 * and its voice handles, patch handles and parameter blocks are addresses
 * inside it. What the handles mean is kept on the host; the records in
 * guest memory only say which rack and which slot.
 *
 * The voice definitions' module chains, the parameter structures' sizes and
 * the ATRAC9 section arithmetic are the console's interface, as Vita3K's
 * ngs/ documents it.
 */

#include "vitarecomp/cpu.h"
#include "vitarecomp/dispatch.h"
#include "vitarecomp/hle.h"
#include "vitarecomp/mem.h"
#include "vitarecomp/vfp.h"
#include "hle_sysmem.h"

#include <stdlib.h>
#include <string.h>

#define NGS_ERROR                 0x804A0001u
#define NGS_ERROR_INVALID_PARAM   0x804A0002u
#define NGS_ERROR_RESOURCE_LOCKED 0x804A0008u
#define NGS_ERROR_INVALID_HANDLE  0x804A000Cu
#define NGS_ERROR_PATCH_NOT_AVAIL 0x804A000Eu
#define NGS_ERROR_INVALID_STATE   0x804A0010u
#define NGS_ERROR_INVALID_BUFFER  0x804A0013u

#define ST_ACTIVE   0x01u
#define ST_PAUSED   0x20u
#define ST_KEY_OFF  0x40u

#define MAX_SYSTEMS 4
#define MAX_RACKS   128
#define MAX_MODULES 10
#define REC         16u             /* a handle's record in guest memory */

#define MAGIC_SYSTEM 0x5353474Eu    /* "NGSS" */
#define MAGIC_RACK   0x5253474Eu    /* "NGSR" */
#define MAGIC_VOICE  0x5653474Eu    /* "NGSV" */
#define MAGIC_PATCH  0x5053474Eu    /* "NGSP" */
#define MAGIC_DEF    0x4453474Eu    /* "NGSD" */

/* --- what the console defines -----------------------------------------------------
 * A module's id, and the sizes of its parameter and state structures. */

#define M_ATRAC9  0x5CAA
#define M_INPUT   0x5CE0            /* the mixer a voice's inputs arrive at */
#define M_COMP    0x5CE1
#define M_DIST    0x5CE2
#define M_ENV     0x5CE3
#define M_FILTER  0x5CE4
#define M_PAUSER  0x5CE5
#define M_PLAYER  0x5CE6
#define M_REVERB  0x5CE7
#define M_GEN     0x5CE8
#define M_MIXER   0x5CE9
#define M_PITCH   0x5CEA
#define M_DELAY   0x5CEB
#define M_EQ      0x5CEC
#define M_OUTPUT  0x0000

typedef struct { uint16_t id, params, state; } module_kind;

static const module_kind KINDS[] = {
    { M_ATRAC9, 96, 24 }, { M_INPUT,  0,  0 }, { M_COMP,  40, 16 }, { M_DIST,   32,  0 },
    { M_ENV,    72, 20 }, { M_FILTER, 28, 0 }, { M_PAUSER, 16, 0 }, { M_PLAYER, 84, 24 },
    { M_REVERB, 76,  0 }, { M_GEN,    32, 0 }, { M_MIXER,  24, 0 }, { M_PITCH,  12,  0 },
    { M_DELAY, 128,  0 }, { M_EQ,     88, 0 }, { M_OUTPUT,  0, 0 },
};

static const module_kind *kind_of(uint16_t id) {
    for (size_t i = 0; i < sizeof(KINDS) / sizeof(KINDS[0]); i++)
        if (KINDS[i].id == id) return &KINDS[i];
    return &KINDS[sizeof(KINDS) / sizeof(KINDS[0]) - 1];
}

/* Every module's block holds at least the descriptor a lock writes into. */
static uint32_t params_size(uint16_t id) {
    uint32_t n = kind_of(id)->params;
    return n < 8 ? 8 : (n + 7u) & ~7u;
}

typedef struct { const char *name; uint8_t nmods, outputs; uint16_t mods[MAX_MODULES]; } voice_def;

enum {
    D_ATRAC9, D_COMPRESSOR, D_SIDE_CHAIN, D_DELAY, D_DISTORTION, D_ENVELOPE, D_EQ, D_MASTER,
    D_MIXER, D_PAUSER, D_PITCH, D_REVERB, D_SAS, D_SCREAM_ATRAC9, D_SCREAM, D_SIMPLE_ATRAC9,
    D_SIMPLE, D_TEMPLATE1, D_COUNT
};

static const voice_def DEFS[D_COUNT] = {
    [D_ATRAC9]        = { "atrac9 voice",          10, 4, { M_ATRAC9, M_GEN, M_MIXER, M_EQ, M_ENV, M_DIST, M_EQ, M_EQ, M_EQ, M_EQ } },
    [D_COMPRESSOR]    = { "compressor buss",        2, 1, { M_INPUT, M_COMP } },
    [D_SIDE_CHAIN]    = { "side-chain compressor",  3, 1, { M_INPUT, M_INPUT, M_COMP } },
    [D_DELAY]         = { "delay buss",             2, 1, { M_INPUT, M_DELAY } },
    [D_DISTORTION]    = { "distortion buss",        2, 1, { M_INPUT, M_DIST } },
    [D_ENVELOPE]      = { "envelope buss",          2, 1, { M_INPUT, M_ENV } },
    [D_EQ]            = { "eq buss",                2, 1, { M_INPUT, M_EQ } },
    [D_MASTER]        = { "master buss",            2, 1, { M_INPUT, M_OUTPUT } },
    [D_MIXER]         = { "mixer buss",             1, 1, { M_INPUT } },
    [D_PAUSER]        = { "pauser buss",            2, 1, { M_INPUT, M_PAUSER } },
    [D_PITCH]         = { "pitch-shift buss",       2, 1, { M_INPUT, M_PITCH } },
    [D_REVERB]        = { "reverb buss",            2, 1, { M_INPUT, M_REVERB } },
    [D_SAS]           = { "sas voice",              2, 1, { M_PLAYER, M_ENV } },
    [D_SCREAM_ATRAC9] = { "scream atrac9 voice",    6, 2, { M_ATRAC9, M_ENV, M_DIST, M_EQ, M_FILTER, M_FILTER } },
    [D_SCREAM]        = { "scream voice",           6, 2, { M_PLAYER, M_ENV, M_DIST, M_EQ, M_FILTER, M_FILTER } },
    [D_SIMPLE_ATRAC9] = { "simple atrac9 voice",    6, 2, { M_ATRAC9, M_EQ, M_ENV, M_PAUSER, M_FILTER, M_FILTER } },
    [D_SIMPLE]        = { "simple voice",           6, 2, { M_PLAYER, M_EQ, M_ENV, M_PAUSER, M_FILTER, M_FILTER } },
    [D_TEMPLATE1]     = { "template 1 voice",      10, 4, { M_PLAYER, M_GEN, M_MIXER, M_EQ, M_ENV, M_DIST, M_EQ, M_EQ, M_EQ, M_EQ } },
};

/* --- the graph, on the host --------------------------------------------------------- */

typedef struct {
    int      used;
    uint32_t dst;                   /* the destination voice's handle */
    int32_t  out_index, sub_index, in_index;
    float    vol[2][2];             /* [output channel][input channel] */
} patch;

typedef struct {
    uint32_t state;                 /* the console's state bits */
    uint32_t bypass, locked;        /* a bit per module */
    uint32_t finished_cb, finished_user;
    uint32_t updates;               /* since the voice was last started */
    int32_t  st[6];                 /* the player's state: byte position, buffer, samples
                                       and bytes since key-on, samples and bytes in total */
    int32_t  loops;                 /* of the buffer being played */
    double   carry;                 /* the fraction of a byte not yet consumed */
} voice;

typedef struct {
    int              used, sys, releasing;
    uint32_t         base;          /* the title's block: this is the rack's handle */
    const voice_def *def;
    uint32_t         voices, channels, per_output, user, release_cb;
    uint32_t         voice_recs, patch_recs, params, params_each;
    uint32_t         param_off[MAX_MODULES];
    voice           *v;
    patch           *p;             /* voices x outputs x per_output */
} rack;

typedef struct { int used; uint32_t handle, granularity, rate; } ngs_system;

static ngs_system g_sys[MAX_SYSTEMS];
static rack       g_rack[MAX_RACKS];
static uint32_t   g_defs;           /* the definitions' records in guest memory */

static float readf(uint32_t addr) {
    uint32_t u = vita_read32(addr);
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

static void writef(uint32_t addr, float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    vita_write32(addr, u);
}

/* An argument the caller may leave null: asked about only when it is not,
 * so that leaving it out is not reported as a bad access. */
static int given(uint32_t addr, uint32_t len) {
    return addr && vita_mem_ptr(addr, len);
}

static void record(uint32_t addr, uint32_t magic, uint32_t a, uint32_t b) {
    vita_write32(addr, magic);
    vita_write32(addr + 4, a);
    vita_write32(addr + 8, b);
    vita_write32(addr + 12, 0);
}

static int system_of(uint32_t handle) {
    for (int i = 0; i < MAX_SYSTEMS; i++)
        if (g_sys[i].used && g_sys[i].handle == handle) return i;
    return -1;
}

static rack *rack_of(uint32_t handle) {
    if (!vita_mem_ptr(handle, REC) || vita_read32(handle) != MAGIC_RACK) return NULL;
    uint32_t i = vita_read32(handle + 4);
    return i < MAX_RACKS && g_rack[i].used && g_rack[i].base == handle ? &g_rack[i] : NULL;
}

/* A voice handle is the address of its record; the record names the rack
 * and the index, and is believed only if it lies where that rack put it. */
static voice *voice_of(uint32_t handle, rack **rk, uint32_t *index) {
    if (!vita_mem_ptr(handle, REC) || vita_read32(handle) != MAGIC_VOICE) return NULL;
    uint32_t r = vita_read32(handle + 4), i = vita_read32(handle + 8);
    if (r >= MAX_RACKS || !g_rack[r].used || i >= g_rack[r].voices) return NULL;
    if (g_rack[r].voice_recs + i * REC != handle) return NULL;
    if (rk) *rk = &g_rack[r];
    if (index) *index = i;
    return &g_rack[r].v[i];
}

static uint32_t voice_handle(const rack *rk, uint32_t i) { return rk->voice_recs + i * REC; }

static uint32_t slots_each(const rack *rk) { return rk->def->outputs * rk->per_output; }

static patch *patch_of(uint32_t handle, rack **rk, uint32_t *slot) {
    if (!vita_mem_ptr(handle, REC) || vita_read32(handle) != MAGIC_PATCH) return NULL;
    uint32_t r = vita_read32(handle + 4), s = vita_read32(handle + 8);
    if (r >= MAX_RACKS || !g_rack[r].used || s >= g_rack[r].voices * slots_each(&g_rack[r])) return NULL;
    if (g_rack[r].patch_recs + s * REC != handle) return NULL;
    if (rk) *rk = &g_rack[r];
    if (slot) *slot = s;
    return &g_rack[r].p[s];
}

static uint32_t params_at(const rack *rk, uint32_t index, uint32_t module) {
    return rk->params + index * rk->params_each + rk->param_off[module];
}

/* Where a rack of this description puts its parts, and how much it needs. */
static uint32_t layout(const voice_def *def, uint32_t voices, uint32_t per_output, rack *out) {
    uint32_t each = 0, off[MAX_MODULES];
    for (uint32_t m = 0; m < def->nmods; m++) { off[m] = each; each += params_size(def->mods[m]); }
    uint32_t voice_recs = 0x40;
    uint32_t patch_recs = voice_recs + voices * REC;
    uint32_t params     = patch_recs + voices * def->outputs * per_output * REC;
    uint32_t total      = (params + voices * each + 15u) & ~15u;
    if (out) {
        out->voice_recs = voice_recs; out->patch_recs = patch_recs;
        out->params = params; out->params_each = each;
        memcpy(out->param_off, off, sizeof(off));
    }
    return total;
}

static const voice_def *def_of(uint32_t handle) {
    if (!g_defs || handle < g_defs || handle >= g_defs + D_COUNT * REC || ((handle - g_defs) % REC)) return NULL;
    return &DEFS[(handle - g_defs) / REC];
}

static uint32_t def_handle(int which) {
    if (!g_defs) {
        g_defs = vita_sysmem_reserve(0x1000);
        for (uint32_t i = 0; g_defs && i < D_COUNT; i++) record(g_defs + i * REC, MAGIC_DEF, i, 0);
    }
    return g_defs ? g_defs + (uint32_t)which * REC : 0;
}

/* --- callbacks -------------------------------------------------------------------------
 * void callback(const SceNgsCallbackInfo *info), with the record on the
 * calling thread's stack for the length of the call. */
static void call_back(uint32_t fn, uint32_t voice_h, uint32_t rack_h, uint32_t module_id,
                      uint32_t reason, uint32_t user) {
    if (!fn) return;
    uint32_t saved = sp;
    sp = (sp - 32u) & ~7u;
    vita_write32(sp, voice_h);
    vita_write32(sp + 4, rack_h);
    vita_write32(sp + 8, module_id);
    vita_write32(sp + 12, reason);
    vita_write32(sp + 16, 0);
    vita_write32(sp + 20, 0);
    vita_write32(sp + 24, user);
    r0 = sp;
    vita_dispatch(0, fn);
    sp = saved;
}

/* --- the system --------------------------------------------------------------------- */

/* SceInt32 sceNgsSystemGetRequiredMemorySize(const SceNgsSystemInitParams *params, SceSize *size) */
void vita_hle_sceNgsSystemGetRequiredMemorySize(void) {
    if (!vita_mem_ptr(r1, 4)) { r0 = NGS_ERROR_INVALID_PARAM; return; }
    vita_write32(r1, 0x1000);
    r0 = 0;
}

/* SceInt32 sceNgsSystemInit(void *memory, SceSize size, const SceNgsSystemInitParams *params,
 *                           SceNgsHSynSystem *handle)
 * params: { max_racks, max_voices, granularity, sample_rate, reserved } */
void vita_hle_sceNgsSystemInit(void) {
    uint32_t mem = r0, size = r1, params = r2, out = r3;
    if (!vita_mem_ptr(out, 4)) { r0 = NGS_ERROR_INVALID_PARAM; return; }
    if (!mem) mem = vita_sysmem_reserve(0x1000);
    if (!vita_mem_ptr(mem, REC)) { r0 = NGS_ERROR_INVALID_BUFFER; return; }
    int i;
    for (i = 0; i < MAX_SYSTEMS && g_sys[i].used; i++) {}
    if (i == MAX_SYSTEMS) { r0 = NGS_ERROR; return; }
    g_sys[i].used = 1;
    g_sys[i].handle = mem;
    g_sys[i].granularity = 512;
    g_sys[i].rate = 48000;
    if (given(params, 20)) {
        uint32_t g = vita_read32(params + 8), hz = vita_read32(params + 12);
        if (g >= 64 && g <= 512) g_sys[i].granularity = g;
        if (hz) g_sys[i].rate = hz;
    }
    record(mem, MAGIC_SYSTEM, (uint32_t)i, 0);
    vita_write32(out, mem);
    vita_hle_trace("sceNgsSystemInit(0x%08X, 0x%X): %u samples an update at %u Hz", mem, size,
                   g_sys[i].granularity, g_sys[i].rate);
    r0 = 0;
}

static void rack_free(rack *rk) {
    free(rk->v);
    free(rk->p);
    memset(rk, 0, sizeof(*rk));
}

/* SceInt32 sceNgsSystemRelease(SceNgsHSynSystem handle) */
void vita_hle_sceNgsSystemRelease(void) {
    int s = system_of(r0);
    if (s < 0) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    for (int i = 0; i < MAX_RACKS; i++)
        if (g_rack[i].used && g_rack[i].sys == s) rack_free(&g_rack[i]);
    g_sys[s].used = 0;
    r0 = 0;
}

void vita_hle_sceNgsSystemSetFlags(void)              { r0 = 0; }
void vita_hle_sceNgsSystemLock(void)                  { r0 = 0; }
void vita_hle_sceNgsSystemUnlock(void)                { r0 = 0; }
void vita_hle_sceNgsSystemSetParamErrorCallback(void) { r0 = 0; }

/* --- playing, in time and in silence ------------------------------------------------- */

/* How a sample player's parameters lie: the PCM player and the ATRAC9
 * player share a shape and differ in their offsets. */
typedef struct { uint32_t buf_size, freq, scalar, limit, channels; } player_layout;

static const player_layout PCM_LAYOUT = { 12, 56, 60, 68, 76 };
static const player_layout AT9_LAYOUT = { 16, 72, 76, 84, 88 };

/* An ATRAC9 stream's bytes and samples per frame, from the configuration
 * word in its header. Returns 0 when the word is not one. */
static int at9_frame(uint32_t config, uint32_t *bytes, uint32_t *samples) {
    if ((config & 0xFF) != 0xFE) return 0;
    uint32_t rate_index = (config >> 12) & 0xF;
    *bytes = ((((config >> 16) & 0xFF) << 3) | ((config >> 29) & 7)) + 1;
    *samples = rate_index == 1 ? 64 : rate_index == 4 ? 128 : rate_index == 7 ? 256 : 0;
    return *samples != 0;
}

static void voice_start(const rack *rk, uint32_t index, voice *v) {
    v->state = ST_ACTIVE;
    v->updates = 0;
    v->loops = 0;
    v->carry = 0;
    v->st[0] = v->st[1] = v->st[2] = v->st[3] = 0;
    if (rk->def->mods[0] == M_PLAYER) {
        uint32_t p = params_at(rk, index, 0);
        int32_t start = (int32_t)vita_read32(p + 72);
        int8_t  first = (int8_t)vita_read8(p + 81);
        if (start > 0) v->st[0] = start;
        if (first > 0 && first < 4) v->st[1] = first;
    }
}

static void voice_finish(rack *rk, uint32_t index) {
    voice *v = &rk->v[index];
    v->state = 0;
    call_back(v->finished_cb, voice_handle(rk, index), rk->base, rk->def->mods[0], 0, v->finished_user);
}

/* One update's worth of a voice's sample: the bytes it would have decoded.
 * Returns 1 when the voice has reached the end of its data. */
static int voice_advance(const ngs_system *sys, const rack *rk, uint32_t index, voice *v) {
    uint16_t kind = rk->def->mods[0];
    if (kind != M_PLAYER && kind != M_ATRAC9) return 0;      /* a buss plays until it is told to stop */
    const player_layout *L = kind == M_PLAYER ? &PCM_LAYOUT : &AT9_LAYOUT;
    uint32_t p = params_at(rk, index, 0);

    float freq = readf(p + L->freq), scalar = readf(p + L->scalar);
    if (!(freq > 0.0f)) freq = (float)sys->rate;
    if (!(scalar > 0.0f)) scalar = 1.0f;
    double samples = (double)sys->granularity * (double)freq * (double)scalar / (double)sys->rate;

    uint32_t channels = vita_read8(p + L->channels);
    if (channels != 1 && channels != 2) channels = 1;
    double per_sample;
    if (kind == M_PLAYER) {
        /* 16-bit PCM, or ADPCM in 16-byte blocks of 28 samples */
        per_sample = vita_read8(p + 79) == 1 ? channels * 16.0 / 28.0 : channels * 2.0;
    } else {
        uint32_t fb, fs;
        if (!at9_frame(vita_read32(p + 92), &fb, &fs)) return 1;
        per_sample = (double)fb / (double)fs;
    }

    int32_t limit = (int32_t)vita_read32(p + L->limit);
    double  want  = samples * per_sample + v->carry;
    int64_t bytes = (int64_t)want;
    v->carry = want - (double)bytes;
    v->st[2] += (int32_t)samples;
    v->st[4] += (int32_t)samples;

    int ended = 0;
    for (int guard = 0; bytes > 0 && guard < 64; guard++) {
        uint32_t cur = (uint32_t)v->st[1] & 3u;
        uint32_t b = p + 8 + cur * L->buf_size;
        uint32_t data = vita_read32(b);
        int32_t  size = (int32_t)vita_read32(b + 4);
        int16_t  loop = (int16_t)vita_read16(b + 8), next = (int16_t)vita_read16(b + 10);
        if (!data || size <= 0) { ended = 1; break; }
        int64_t left = (int64_t)size - v->st[0];
        int64_t take = left < bytes ? left : bytes;
        if (take < 0) take = 0;
        v->st[0] += (int32_t)take;
        v->st[3] += (int32_t)take;
        v->st[5] += (int32_t)take;
        bytes -= take;
        if (v->st[0] < size) break;
        v->st[0] = 0;
        if (loop < 0 || v->loops < loop) { v->loops++; continue; }     /* -1 loops for ever */
        v->loops = 0;
        if (next < 0 || next > 3) { ended = 1; break; }
        v->st[1] = next;
    }
    if (limit > 0 && v->st[2] >= limit) ended = 1;
    return ended;
}

/* SceInt32 sceNgsSystemUpdate(SceNgsHSynSystem handle): one block of audio.
 * Voices move on by the block's samples, the ones that reach their end or
 * were keyed off finish, and their callbacks run here, on the thread that
 * asked for the update, as they do on the console. */
void vita_hle_sceNgsSystemUpdate(void) {
    int s = system_of(r0);
    if (s < 0) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    for (int i = 0; i < MAX_RACKS; i++) {
        rack *rk = &g_rack[i];
        if (!rk->used || rk->sys != s) continue;
        for (uint32_t k = 0; rk->used && k < rk->voices; k++) {
            voice *v = &rk->v[k];
            if (!(v->state & ST_ACTIVE) || (v->state & ST_PAUSED)) continue;
            v->updates++;
            if ((v->state & ST_KEY_OFF) || voice_advance(&g_sys[s], rk, k, v))
                voice_finish(rk, k);        /* may run the title's code, which may change anything */
        }
        if (rk->used && rk->releasing) {
            uint32_t cb = rk->release_cb, base = rk->base, user = rk->user;
            rack_free(rk);
            call_back(cb, 0, base, 0, 0, user);
        }
    }
    r0 = 0;
}

/* --- racks -------------------------------------------------------------------------------
 * SceNgsRackDescription: { definition, voices, channels per voice,
 * max patches per input, patches per output, user data } */

/* SceInt32 sceNgsRackGetRequiredMemorySize(SceNgsHSynSystem, const SceNgsRackDescription *, SceSize *) */
void vita_hle_sceNgsRackGetRequiredMemorySize(void) {
    uint32_t desc = r1, out = r2;
    if (!vita_mem_ptr(desc, 24) || !vita_mem_ptr(out, 4)) { r0 = NGS_ERROR_INVALID_PARAM; return; }
    const voice_def *def = def_of(vita_read32(desc));
    uint32_t voices = vita_read32(desc + 4), per_output = vita_read32(desc + 16);
    if (!def || !voices || voices > 4096 || per_output > 64) { r0 = NGS_ERROR_INVALID_PARAM; return; }
    vita_write32(out, layout(def, voices, per_output, NULL));
    r0 = 0;
}

/* SceInt32 sceNgsRackInit(SceNgsHSynSystem, const SceNgsBufferInfo *, const SceNgsRackDescription *,
 *                         SceNgsHRack *handle) */
void vita_hle_sceNgsRackInit(void) {
    uint32_t sys_h = r0, buf = r1, desc = r2, out = r3;
    int s = system_of(sys_h);
    if (s < 0) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    if (!vita_mem_ptr(buf, 8) || !vita_mem_ptr(desc, 24) || !vita_mem_ptr(out, 4)) { r0 = NGS_ERROR_INVALID_PARAM; return; }
    const voice_def *def = def_of(vita_read32(desc));
    uint32_t voices = vita_read32(desc + 4), per_output = vita_read32(desc + 16);
    if (!def || !voices || voices > 4096 || per_output > 64) { r0 = NGS_ERROR_INVALID_PARAM; return; }

    int i;
    for (i = 0; i < MAX_RACKS && g_rack[i].used; i++) {}
    if (i == MAX_RACKS) { r0 = NGS_ERROR; return; }
    rack *rk = &g_rack[i];
    memset(rk, 0, sizeof(*rk));
    uint32_t base = vita_read32(buf), size = vita_read32(buf + 4);
    uint32_t need = layout(def, voices, per_output, rk);
    void *block = vita_mem_ptr(base, need);
    if (!block || size < need) {
        vita_hle_trace("sceNgsRackInit: the block at 0x%08X holds 0x%X of the 0x%X a rack of %u %s needs",
                       base, size, need, voices, def->name);
        r0 = NGS_ERROR_INVALID_BUFFER;
        return;
    }
    rk->v = calloc(voices, sizeof(voice));
    rk->p = calloc((size_t)voices * def->outputs * per_output + 1, sizeof(patch));
    if (!rk->v || !rk->p) { rack_free(rk); r0 = NGS_ERROR; return; }

    rk->used = 1;
    rk->sys = s;
    rk->base = base;
    rk->def = def;
    rk->voices = voices;
    rk->channels = vita_read32(desc + 8);
    rk->per_output = per_output;
    rk->user = vita_read32(desc + 20);
    rk->voice_recs += base;
    rk->patch_recs += base;
    rk->params += base;

    memset(block, 0, need);
    record(base, MAGIC_RACK, (uint32_t)i, 0);
    for (uint32_t k = 0; k < voices; k++) record(rk->voice_recs + k * REC, MAGIC_VOICE, (uint32_t)i, k);
    for (uint32_t k = 0; k < voices * slots_each(rk); k++) record(rk->patch_recs + k * REC, MAGIC_PATCH, (uint32_t)i, k);
    vita_write32(out, base);
    vita_hle_trace("sceNgsRackInit: %u %s, %u channels, at 0x%08X+0x%X", voices, def->name, rk->channels, base, need);
    r0 = 0;
}

/* SceInt32 sceNgsRackRelease(SceNgsHRack, SceNgsRackReleaseCallbackFunc): the
 * rack goes at the next update, and the callback says when it has. */
void vita_hle_sceNgsRackRelease(void) {
    rack *rk = rack_of(r0);
    if (!rk) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    rk->releasing = 1;
    rk->release_cb = r1;
    for (uint32_t k = 0; k < rk->voices; k++) rk->v[k].state = 0;
    r0 = 0;
}

void vita_hle_sceNgsRackSetParamErrorCallback(void) { r0 = 0; }

/* SceInt32 sceNgsRackGetVoiceHandle(SceNgsHRack, SceUInt32 index, SceNgsHVoice *handle): the same
 * handle for the same voice, every time it is asked for. */
void vita_hle_sceNgsRackGetVoiceHandle(void) {
    rack *rk = rack_of(r0);
    if (!rk) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    if (r1 >= rk->voices || !vita_mem_ptr(r2, 4)) { r0 = NGS_ERROR_INVALID_PARAM; return; }
    vita_write32(r2, voice_handle(rk, r1));
    r0 = 0;
}

/* --- voices ------------------------------------------------------------------------------ */

/* The records of a parameter block: { module index, channel } and then a
 * descriptor { id, size } whose size covers itself and the module's data.
 * Returns how many named a module the voice does not have. */
static int apply_params(const rack *rk, uint32_t index, uint32_t at, uint32_t size) {
    int errors = 0;
    uint32_t end = at + size;
    while (at + 16 <= end && vita_mem_ptr(at, 16)) {
        uint32_t module = vita_read32(at), len = vita_read32(at + 12);
        if (len < 8) break;
        if (module < rk->def->nmods) {
            uint32_t cap = params_size(rk->def->mods[module]), n = len < cap ? len : cap;
            const void *src = vita_mem_ptr(at + 8, n);
            void *dst = vita_mem_ptr(params_at(rk, index, module), n);
            if (src && dst) memcpy(dst, src, n); else errors++;
        } else {
            errors++;
        }
        at += 8 + len;
    }
    return errors;
}

/* SceNgsVoicePreset: { name offset, name length, data offset, data size,
 * bypass offset, bypass count }, the offsets from the preset's own start. */
static int apply_preset(const rack *rk, uint32_t index, voice *v, uint32_t preset) {
    if (!vita_mem_ptr(preset, 24)) return 1;
    uint32_t data = vita_read32(preset + 8), data_size = vita_read32(preset + 12);
    uint32_t flags = vita_read32(preset + 16), count = vita_read32(preset + 20);
    int errors = data ? apply_params(rk, index, preset + data, data_size) : 0;
    for (uint32_t i = 0; flags && i < count && i < 64; i++) {
        if (!vita_mem_ptr(preset + flags + 4 * i, 4)) { errors++; break; }
        uint32_t module = vita_read32(preset + flags + 4 * i);
        if (module < rk->def->nmods) v->bypass |= 1u << module; else errors++;
    }
    return errors;
}

/* Every patch that leaves this voice or arrives at it. */
static void unroute(rack *rk, uint32_t index) {
    uint32_t each = slots_each(rk), self = voice_handle(rk, index);
    for (uint32_t s = 0; s < each; s++) rk->p[index * each + s].used = 0;
    for (int i = 0; i < MAX_RACKS; i++) {
        rack *o = &g_rack[i];
        if (!o->used) continue;
        for (uint32_t s = 0; s < o->voices * slots_each(o); s++)
            if (o->p[s].used && o->p[s].dst == self) o->p[s].used = 0;
    }
}

/* SceInt32 sceNgsVoiceInit(SceNgsHVoice, const SceNgsVoicePreset *, SceUInt32 flags)
 * flags: 1 routing, 2 preset, 4 callbacks; 0 and 7 also make the voice available. */
void vita_hle_sceNgsVoiceInit(void) {
    rack *rk; uint32_t index;
    voice *v = voice_of(r0, &rk, &index);
    uint32_t preset = r1, flags = r2;
    if (!v) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    if (v->state & ST_ACTIVE) { r0 = NGS_ERROR_INVALID_STATE; return; }
    if (flags == 0 || flags == 7) v->state = 0;
    if (flags & 1) unroute(rk, index);
    if (flags & 2) {
        void *block = vita_mem_ptr(params_at(rk, index, 0), rk->params_each);
        if (block) memset(block, 0, rk->params_each);
        v->bypass = 0;
        if (preset && apply_preset(rk, index, v, preset)) { r0 = NGS_ERROR; return; }
    }
    if (flags & 4) v->finished_cb = v->finished_user = 0;
    r0 = 0;
}

/* SceInt32 sceNgsVoicePlay(SceNgsHVoice) */
void vita_hle_sceNgsVoicePlay(void) {
    rack *rk; uint32_t index;
    voice *v = voice_of(r0, &rk, &index);
    if (!v) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    voice_start(rk, index, v);
    r0 = 0;
}

/* SceInt32 sceNgsVoiceKeyOff(SceNgsHVoice): the release begins; with no
 * signal to fade, the voice finishes at the next update. */
void vita_hle_sceNgsVoiceKeyOff(void) {
    voice *v = voice_of(r0, NULL, NULL);
    if (!v) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    if (v->state & ST_ACTIVE) v->state |= ST_KEY_OFF;
    r0 = 0;
}

/* SceInt32 sceNgsVoiceKill(SceNgsHVoice): stopped at once, and no callback. */
void vita_hle_sceNgsVoiceKill(void) {
    voice *v = voice_of(r0, NULL, NULL);
    if (!v) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    v->state = 0;
    r0 = 0;
}

void vita_hle_sceNgsVoicePause(void) {
    voice *v = voice_of(r0, NULL, NULL);
    if (!v) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    if (v->state & ST_PAUSED) { r0 = NGS_ERROR_INVALID_STATE; return; }
    v->state |= ST_PAUSED;
    r0 = 0;
}

void vita_hle_sceNgsVoiceResume(void) {
    voice *v = voice_of(r0, NULL, NULL);
    if (!v) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    v->state &= ~ST_PAUSED;
    r0 = 0;
}

/* SceInt32 sceNgsVoiceSetFinishedCallback(SceNgsHVoice, SceNgsCallbackFunc, void *user) */
void vita_hle_sceNgsVoiceSetFinishedCallback(void) {
    voice *v = voice_of(r0, NULL, NULL);
    if (!v) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    v->finished_cb = r1;
    v->finished_user = r2;
    r0 = 0;
}

void vita_hle_sceNgsVoiceSetModuleCallback(void) {
    r0 = voice_of(r0, NULL, NULL) ? 0 : NGS_ERROR_INVALID_HANDLE;
}

/* SceInt32 sceNgsVoiceSetParamsBlock(SceNgsHVoice, const SceNgsModuleParamHeader *, SceSize,
 *                                    SceInt32 *errors) */
void vita_hle_sceNgsVoiceSetParamsBlock(void) {
    rack *rk; uint32_t index;
    voice *v = voice_of(r0, &rk, &index);
    if (!v) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    int errors = apply_params(rk, index, r1, r2);
    if (given(r3, 4)) vita_write32(r3, (uint32_t)errors);
    r0 = errors ? NGS_ERROR : 0;
}

/* SceInt32 sceNgsVoiceSetPreset(SceNgsHVoice, const SceNgsVoicePreset *) */
void vita_hle_sceNgsVoiceSetPreset(void) {
    rack *rk; uint32_t index;
    voice *v = voice_of(r0, &rk, &index);
    if (!v) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    r0 = apply_preset(rk, index, v, r1) ? NGS_ERROR : 0;
}

/* SceInt32 sceNgsVoiceLockParams(SceNgsHVoice, SceUInt32 module, SceNgsParamsID id,
 *                                SceNgsBufferInfo *buffer)
 * The module's own block, the same one each time, holding whatever was
 * last written to it; the lock stamps it with the interface asked for. */
void vita_hle_sceNgsVoiceLockParams(void) {
    rack *rk; uint32_t index;
    voice *v = voice_of(r0, &rk, &index);
    uint32_t module = r1, id = r2, out = r3;
    if (!v) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    if (module >= rk->def->nmods || !vita_mem_ptr(out, 8)) { r0 = NGS_ERROR_INVALID_PARAM; return; }
    if (v->locked & (1u << module)) { r0 = NGS_ERROR_RESOURCE_LOCKED; return; }
    v->locked |= 1u << module;
    uint32_t at = params_at(rk, index, module);
    vita_write32(at, id);
    vita_write32(out, at);
    vita_write32(out + 4, params_size(rk->def->mods[module]));
    r0 = 0;
}

/* SceInt32 sceNgsVoiceUnlockParams(SceNgsHVoice, SceUInt32 module) */
void vita_hle_sceNgsVoiceUnlockParams(void) {
    rack *rk;
    voice *v = voice_of(r0, &rk, NULL);
    if (!v) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    if (r1 >= rk->def->nmods) { r0 = NGS_ERROR_INVALID_PARAM; return; }
    v->locked &= ~(1u << r1);
    r0 = 0;
}

/* SceInt32 sceNgsVoiceBypassModule(SceNgsHVoice, SceUInt32 module, SceUInt32 flag): 2 bypasses */
void vita_hle_sceNgsVoiceBypassModule(void) {
    rack *rk;
    voice *v = voice_of(r0, &rk, NULL);
    if (!v) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    if (r1 >= rk->def->nmods) { r0 = NGS_ERROR_INVALID_PARAM; return; }
    if (r2 & 2u) v->bypass |= 1u << r1; else v->bypass &= ~(1u << r1);
    r0 = 0;
}

void vita_hle_sceNgsVoiceGetModuleBypass(void) {
    rack *rk;
    voice *v = voice_of(r0, &rk, NULL);
    if (!v) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    if (r1 >= rk->def->nmods || !vita_mem_ptr(r2, 4)) { r0 = NGS_ERROR_INVALID_PARAM; return; }
    vita_write32(r2, (v->bypass >> r1) & 1u ? 2u : 0u);
    r0 = 0;
}

void vita_hle_sceNgsVoiceGetModuleType(void) {
    rack *rk;
    voice *v = voice_of(r0, &rk, NULL);
    if (!v) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    if (r1 >= rk->def->nmods || !vita_mem_ptr(r2, 4)) { r0 = NGS_ERROR_INVALID_PARAM; return; }
    vita_write32(r2, rk->def->mods[r1]);
    r0 = 0;
}

/* SceInt32 sceNgsVoiceGetStateData(SceNgsHVoice, SceUInt32 module, void *mem, SceSize size)
 * The players report where they are; the other modules' states are levels
 * and envelopes of a signal that is not there, and read as zero. */
void vita_hle_sceNgsVoiceGetStateData(void) {
    rack *rk;
    voice *v = voice_of(r0, &rk, NULL);
    uint32_t module = r1, mem = r2, size = r3;
    if (!v) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    if (module >= rk->def->nmods) { r0 = NGS_ERROR_INVALID_PARAM; return; }
    void *dst = size && mem ? vita_mem_ptr(mem, size) : NULL;
    if (dst) {
        memset(dst, 0, size);
        uint16_t kind = rk->def->mods[module];
        if (kind == M_PLAYER || kind == M_ATRAC9)
            for (uint32_t i = 0; i < 6 && 4 * i + 4 <= size; i++) vita_write32(mem + 4 * i, (uint32_t)v->st[i]);
    }
    r0 = 0;
}

/* SceInt32 sceNgsVoiceGetInfo(SceNgsHVoice, SceNgsVoiceInfo *info)
 * { state, modules, inputs, outputs, patches per output, updates } */
void vita_hle_sceNgsVoiceGetInfo(void) {
    rack *rk;
    voice *v = voice_of(r0, &rk, NULL);
    if (!v) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    if (!vita_mem_ptr(r1, 24)) { r0 = NGS_ERROR_INVALID_PARAM; return; }
    uint32_t inputs = 0;
    for (uint32_t m = 0; m < rk->def->nmods; m++) inputs += rk->def->mods[m] == M_INPUT;
    vita_write32(r1, v->state);
    vita_write32(r1 + 4, rk->def->nmods);
    vita_write32(r1 + 8, inputs);
    vita_write32(r1 + 12, rk->def->outputs);
    vita_write32(r1 + 16, rk->per_output);
    vita_write32(r1 + 20, v->updates);
    r0 = 0;
}

/* SceInt32 sceNgsVoiceGetParamsOutOfRange(SceNgsHVoice, SceUInt32 module, char *message): none are */
void vita_hle_sceNgsVoiceGetParamsOutOfRange(void) {
    if (given(r2, 1)) vita_write8(r2, 0);
    r0 = 0;
}

/* --- patches ------------------------------------------------------------------------------ */

/* SceInt32 sceNgsPatchCreateRouting(const SceNgsPatchSetupInfo *, SceNgsHPatch *handle)
 * { source voice, output, slot or -1 for the first free, destination voice, input } */
void vita_hle_sceNgsPatchCreateRouting(void) {
    uint32_t info = r0, out = r1;
    if (!vita_mem_ptr(info, 20) || !vita_mem_ptr(out, 4)) { r0 = NGS_ERROR_INVALID_PARAM; return; }
    rack *rk; uint32_t index;
    if (!voice_of(vita_read32(info), &rk, &index)) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    uint32_t dst = vita_read32(info + 12);
    if (!voice_of(dst, NULL, NULL)) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    int32_t output = (int32_t)vita_read32(info + 4), sub = (int32_t)vita_read32(info + 8);
    if (output < 0 || (uint32_t)output >= rk->def->outputs || sub >= (int32_t)rk->per_output) { r0 = NGS_ERROR_INVALID_PARAM; return; }
    uint32_t first = index * slots_each(rk) + (uint32_t)output * rk->per_output;
    if (sub < 0)
        for (sub = 0; (uint32_t)sub < rk->per_output && rk->p[first + (uint32_t)sub].used; sub++) {}
    if ((uint32_t)sub >= rk->per_output) { r0 = NGS_ERROR_PATCH_NOT_AVAIL; return; }
    patch *p = &rk->p[first + (uint32_t)sub];
    memset(p, 0, sizeof(*p));
    p->used = 1;
    p->dst = dst;
    p->out_index = output;
    p->sub_index = sub;
    p->in_index = (int32_t)vita_read32(info + 16);
    p->vol[0][0] = p->vol[1][1] = 1.0f;
    vita_write32(out, rk->patch_recs + (first + (uint32_t)sub) * REC);
    r0 = 0;
}

void vita_hle_sceNgsPatchRemoveRouting(void) {
    patch *p = patch_of(r0, NULL, NULL);
    if (!p) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    p->used = 0;
    r0 = 0;
}

/* SceInt32 sceNgsPatchGetInfo(SceNgsHPatch, SceNgsPatchRouteInfo *, SceNgsPatchSetupInfo *) */
void vita_hle_sceNgsPatchGetInfo(void) {
    rack *rk, *drk = NULL; uint32_t slot;
    patch *p = patch_of(r0, &rk, &slot);
    uint32_t route = r1, setup = r2;
    if (!p || !p->used) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    (void)voice_of(p->dst, &drk, NULL);
    if (given(route, 24)) {
        vita_write32(route, rk->channels);
        vita_write32(route + 4, drk ? drk->channels : rk->channels);
        for (uint32_t i = 0; i < 4; i++) writef(route + 8 + 4 * i, p->vol[i / 2][i % 2]);
    }
    if (given(setup, 20)) {
        vita_write32(setup, voice_handle(rk, slot / slots_each(rk)));
        vita_write32(setup + 4, (uint32_t)p->out_index);
        vita_write32(setup + 8, (uint32_t)p->sub_index);
        vita_write32(setup + 12, p->dst);
        vita_write32(setup + 16, (uint32_t)p->in_index);
    }
    r0 = 0;
}

/* SceInt32 sceNgsVoiceGetOutputPatch(SceNgsHVoice, SceInt32 output, SceInt32 slot, SceNgsHPatch *) */
void vita_hle_sceNgsVoiceGetOutputPatch(void) {
    rack *rk; uint32_t index;
    if (!voice_of(r0, &rk, &index)) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    if (r1 >= rk->def->outputs || r2 >= rk->per_output || !vita_mem_ptr(r3, 4)) { r0 = NGS_ERROR_INVALID_PARAM; return; }
    uint32_t s = index * slots_each(rk) + r1 * rk->per_output + r2;
    vita_write32(r3, rk->p[s].used ? rk->patch_recs + s * REC : 0);
    r0 = rk->p[s].used ? 0 : NGS_ERROR_PATCH_NOT_AVAIL;
}

/* SceInt32 sceNgsVoicePatchSetVolume(SceNgsHPatch, SceInt32 out, SceInt32 in, SceFloat32 volume):
 * the float arrives in s0 */
void vita_hle_sceNgsVoicePatchSetVolume(void) {
    patch *p = patch_of(r0, NULL, NULL);
    if (!p) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    if (r1 > 1 || r2 > 1) { r0 = NGS_ERROR_INVALID_PARAM; return; }
    p->vol[r1][r2] = vfp_getf(0);
    r0 = 0;
}

/* SceInt32 sceNgsVoicePatchSetVolumes(SceNgsHPatch, SceInt32 out, const SceFloat32 *volumes, SceInt32 count) */
void vita_hle_sceNgsVoicePatchSetVolumes(void) {
    patch *p = patch_of(r0, NULL, NULL);
    if (!p) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    if (r1 > 1 || !vita_mem_ptr(r2, 4 * (r3 > 2 ? 2 : r3))) { r0 = NGS_ERROR_INVALID_PARAM; return; }
    for (uint32_t i = 0; i < r3 && i < 2; i++) p->vol[r1][i] = readf(r2 + 4 * i);
    r0 = 0;
}

/* SceInt32 sceNgsVoicePatchSetVolumesMatrix(SceNgsHPatch, const SceNgsVolumeMatrix *) */
void vita_hle_sceNgsVoicePatchSetVolumesMatrix(void) {
    patch *p = patch_of(r0, NULL, NULL);
    if (!p) { r0 = NGS_ERROR_INVALID_HANDLE; return; }
    if (!vita_mem_ptr(r1, 16)) { r0 = NGS_ERROR_INVALID_PARAM; return; }
    for (uint32_t i = 0; i < 4; i++) p->vol[i / 2][i % 2] = readf(r1 + 4 * i);
    r0 = 0;
}

/* --- samples ------------------------------------------------------------------------------ */

/* SceInt32 sceNgsAT9GetSectionDetails(SceInt32 start_sample, SceInt32 samples, SceInt32 config,
 *                                     SceNgsAT9SkipBufferInfo *info)
 * { start byte, bytes, start skip (16), end skip (16), super packet }: the
 * packets that hold a run of samples, and how many decoded samples to drop
 * at each end. A decoder is always one frame behind its input, so the run
 * is found a frame later than its own position. */
void vita_hle_sceNgsAT9GetSectionDetails(void) {
    uint32_t start = r0, count = r1, config = r2, info = r3;
    uint32_t frame_bytes, frame_samples;
    if (!vita_mem_ptr(info, 16)) { r0 = NGS_ERROR_INVALID_PARAM; return; }
    if (!at9_frame(config, &frame_bytes, &frame_samples)) { r0 = NGS_ERROR; return; }
    int super = (config >> 27) & 3u ? 1 : 0;
    uint32_t unit_bytes = super ? frame_bytes * 4 : frame_bytes;
    uint32_t unit_samples = super ? frame_samples * 4 : frame_samples;
    uint32_t first_byte, bytes, skip_start, skip_end;

    if (super) {
        start += frame_samples;
        uint32_t first = start / unit_samples;
        uint32_t last = (start + count + unit_samples - 1) / unit_samples;
        first_byte = first * unit_bytes;
        bytes = (last - first) * unit_bytes;
        skip_start = start - first * unit_samples;
        skip_end = last * unit_samples - (start + count);
        if (skip_start < frame_samples && first > 0) {
            /* the decoder needs the packet before it to be warm */
            first_byte -= unit_bytes;
            bytes += unit_bytes;
            skip_start += unit_samples;
        }
    } else {
        uint32_t first = start / unit_samples;
        skip_start = start + frame_samples - first * unit_samples;
        uint32_t frames = (skip_start + count + unit_samples - 1) / unit_samples;
        first_byte = first * unit_bytes;
        bytes = frames * unit_bytes;
        skip_end = frames * unit_samples - (skip_start + count);
    }
    vita_write32(info, first_byte);
    vita_write32(info + 4, bytes);
    vita_write16(info + 8, (uint16_t)skip_start);
    vita_write16(info + 10, (uint16_t)skip_end);
    vita_write32(info + 12, (uint32_t)super);
    r0 = 0;
}

void vita_hle_sceNgsModuleGetNumPresets(void) { if (given(r2, 4)) vita_write32(r2, 0); r0 = 0; }
void vita_hle_sceNgsModuleGetPreset(void)     { r0 = NGS_ERROR_INVALID_PARAM; }

/* --- the voice definitions ------------------------------------------------------------------ */

void vita_hle_sceNgsVoiceDefGetAtrac9Voice(void)             { r0 = def_handle(D_ATRAC9); }
void vita_hle_sceNgsVoiceDefGetCompressorBuss(void)          { r0 = def_handle(D_COMPRESSOR); }
void vita_hle_sceNgsVoiceDefGetCompressorSideChainBuss(void) { r0 = def_handle(D_SIDE_CHAIN); }
void vita_hle_sceNgsVoiceDefGetDelayBuss(void)               { r0 = def_handle(D_DELAY); }
void vita_hle_sceNgsVoiceDefGetDistortionBuss(void)          { r0 = def_handle(D_DISTORTION); }
void vita_hle_sceNgsVoiceDefGetEnvelopeBuss(void)            { r0 = def_handle(D_ENVELOPE); }
void vita_hle_sceNgsVoiceDefGetEqBuss(void)                  { r0 = def_handle(D_EQ); }
void vita_hle_sceNgsVoiceDefGetMasterBuss(void)              { r0 = def_handle(D_MASTER); }
void vita_hle_sceNgsVoiceDefGetMixerBuss(void)               { r0 = def_handle(D_MIXER); }
void vita_hle_sceNgsVoiceDefGetPauserBuss(void)              { r0 = def_handle(D_PAUSER); }
void vita_hle_sceNgsVoiceDefGetPitchShiftBuss(void)          { r0 = def_handle(D_PITCH); }
void vita_hle_sceNgsVoiceDefGetReverbBuss(void)              { r0 = def_handle(D_REVERB); }
void vita_hle_sceNgsVoiceDefGetSasEmuVoice(void)             { r0 = def_handle(D_SAS); }
void vita_hle_sceNgsVoiceDefGetScreamAtrac9Voice(void)       { r0 = def_handle(D_SCREAM_ATRAC9); }
void vita_hle_sceNgsVoiceDefGetScreamVoice(void)             { r0 = def_handle(D_SCREAM); }
void vita_hle_sceNgsVoiceDefGetSimpleAtrac9Voice(void)       { r0 = def_handle(D_SIMPLE_ATRAC9); }
void vita_hle_sceNgsVoiceDefGetSimpleVoice(void)             { r0 = def_handle(D_SIMPLE); }
void vita_hle_sceNgsVoiceDefGetTemplate1(void)               { r0 = def_handle(D_TEMPLATE1); }
