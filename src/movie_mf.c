/* movie_mf.c — movies, decoded by Windows (movie.h).
 *
 * The file is taken apart here (mp4.c) and its samples are given to the two
 * decoders every Windows carries as part of Media Foundation: H.264 for the
 * pictures, AAC for the sound. Nothing is shipped for it and nothing else
 * of Media Foundation is used: no player, no reader of files, no threads of
 * its own making the calls. That is deliberate. The file's bytes have to
 * come through the caller's reader on the caller's thread (it may be a
 * title's own code that fetches them), and a decoder that is handed one
 * sample and asked for one picture does exactly that and nothing behind
 * anyone's back, so a run by the virtual clock decodes the same pictures
 * at the same moments every time.
 *
 * A decoder is a "transform": samples go in (ProcessInput) and come out
 * (ProcessOutput), and it says when it wants more of the one before it can
 * give more of the other. The objects are COM objects, used from C through
 * the macros the headers generate (IMFTransform_ProcessInput and so on),
 * and each call returns an HRESULT, negative for a failure.
 *
 * What the decoders want that an MP4 does not hold as it stands:
 *
 *   H.264   A sample in an MP4 is a run of units each behind its length;
 *           the decoder wants each behind the start code 00 00 00 01
 *           ("Annex B"), and the two parameter sets, which an MP4 keeps
 *           apart in its configuration record, in the stream before the
 *           picture that needs them. The pictures come out in the order
 *           they are shown, which is not the order their samples went in
 *           where a picture leans on a later one. The times the decoder
 *           puts on them are not relied on: now and again it loses one or
 *           makes one up. A picture's time is its sample's, and since the
 *           pictures come out in order, the picture that comes out is the
 *           one with the earliest time of all the samples still inside.
 *   AAC     The frames as they are, and the stream's AudioSpecificConfig
 *           in the input type, behind twelve bytes that say so.
 *
 * A build that compiles every file of this folder, as a console's script
 * does, meets this one too: there it is nothing, and movie_none.c answers.
 */

typedef int movie_mf_is_for_windows;

#ifdef _WIN32

#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>

#include "movie.h"
#include "mp4.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The two decoders, by the names the system registers them under
 * (wmcodecdsp.h: CLSID_CMSH264DecoderMFT, CLSID_CMSAACDecMFT). */
static const GUID H264_DECODER = { 0x62CE7E72, 0x4C71, 0x4D20, { 0xB1, 0x5D, 0x45, 0x28, 0x31, 0xA8, 0x7D, 0x9D } };
static const GUID AAC_DECODER  = { 0x32D186A7, 0x218F, 0x4C75, { 0x88, 0x76, 0xDD, 0x77, 0x27, 0x3A, 0x89, 0x99 } };

#define MOST_STREAMS 16
#define MOST_INSIDE  64                 /* samples a decoder holds at once: more than any stream makes it keep */
#define RELEASE(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)

struct movie {
    movie_reader from;
    mp4         *file;
    uint32_t     nstreams;
    movie_stream streams[MOST_STREAMS];
    uint8_t     *sample;                /* one sample's bytes as the file has them */
    uint32_t     sample_cap;
    int          started;               /* Media Foundation was started for this movie */

    /* pictures */
    int           video;                /* the track, or -1 */
    IMFTransform *vdec;
    IMFSample    *vout;                 /* where a picture comes out, when the decoder leaves that to its caller */
    uint32_t      vout_cap;
    uint32_t      vnext;                /* the next sample to go in */
    int           vdraining;
    struct { int64_t time; uint32_t shown; } inside[MOST_INSIDE];   /* the samples in the decoder: when each is shown, and for how long */
    uint32_t      ninside;
    uint32_t      unit_length;          /* bytes of length before each unit of a sample: 1 to 4 */
    uint8_t      *sets;                 /* the parameter sets, each behind a start code */
    uint32_t      sets_size;
    /* a decoded picture as the decoder lays it out: rows of `stride` bytes,
     * `rows` of them before the colour plane, the picture itself beginning
     * `left` across and `top` down */
    uint32_t      stride, rows, left, top, wide, high;

    /* sound */
    int           audio;
    IMFTransform *adec;
    IMFSample    *aout;
    uint32_t      aout_cap;
    uint32_t      anext;
    int           adraining;
    int16_t      *pcm;                  /* decoded and not yet asked for */
    uint32_t      pcm_cap, pcm_at, pcm_frames;      /* in frames */
    uint64_t      pcm_given;            /* frames handed out since the start */
    int64_t       pcm_skip;             /* frames still to drop: sound from before the track's playing begins */
};

/* COM is asked for once on each thread that gets here, and left: a guest
 * thread is a thread of the host's that lives as long as the program wants
 * it. A thread that already has COM in the other mode keeps it, which
 * serves as well. */
static void com_here(void) {
    static _Thread_local int asked;
    if (asked) return;
    asked = 1;
    (void)CoInitializeEx(NULL, COINIT_MULTITHREADED);
}

/* A sentence for the caller, with the system's own number for what went
 * wrong when it gave one. */
static void say(char *why, size_t cap, const char *what, HRESULT hr) {
    if (!why || !cap) return;
    if (hr) snprintf(why, cap, "%s (0x%08lX)", what, (unsigned long)hr);
    else snprintf(why, cap, "%s", what);
}

/* A sample of the decoder's kind holding one buffer of `size` bytes. */
static IMFSample *new_sample(uint32_t size, IMFMediaBuffer **buffer) {
    IMFSample *s = NULL;
    IMFMediaBuffer *b = NULL;
    if (FAILED(MFCreateSample(&s)) || FAILED(MFCreateMemoryBuffer(size ? size : 1, &b)) || FAILED(IMFSample_AddBuffer(s, b))) {
        RELEASE(b); RELEASE(s);
        return NULL;
    }
    if (buffer) *buffer = b; else RELEASE(b);
    return s;
}

/* Sample `index` of a track into m->sample. */
static int fetch(movie *m, int track, uint32_t index, mp4_sample *s) {
    if (!mp4_sample_at(m->file, (uint32_t)track, index, s)) return 0;
    if (s->size > m->sample_cap) {
        uint8_t *grown = (uint8_t *)realloc(m->sample, (size_t)s->size + 1024);
        if (!grown) return 0;
        m->sample = grown;
        m->sample_cap = s->size + 1024;
    }
    return m->from.read(m->from.user, s->offset, m->sample, s->size) == (int64_t)s->size;
}

/* The output type the decoder is to produce: the first it offers that
 * `wanted` says yes to. The type is left set and is returned for its
 * details to be read; NULL when none would do. */
static IMFMediaType *choose_output(IMFTransform *dec, int (*wanted)(IMFMediaType *)) {
    for (DWORD i = 0; ; i++) {
        IMFMediaType *type = NULL;
        if (FAILED(IMFTransform_GetOutputAvailableType(dec, 0, i, &type))) return NULL;
        if (wanted(type) && SUCCEEDED(IMFTransform_SetOutputType(dec, 0, type, 0))) return type;
        RELEASE(type);
    }
}

/* Whether the decoder leaves it to its caller to bring what a result goes
 * into, and how large that has to be. */
static int caller_allocates(IMFTransform *dec, uint32_t *size) {
    MFT_OUTPUT_STREAM_INFO info;
    memset(&info, 0, sizeof info);
    if (FAILED(IMFTransform_GetOutputStreamInfo(dec, 0, &info))) return 1;
    *size = info.cbSize;
    return !(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES));
}

/* One turn of a decoder's output. S_OK with the sample in `*got` (to be
 * released by the caller), or the HRESULT that says what the decoder wants
 * instead. `own` is the caller's sample for the result, remade when the
 * decoder asks for a larger one. */
static HRESULT take_output(IMFTransform *dec, IMFSample **own, uint32_t *own_cap, IMFSample **got) {
    MFT_OUTPUT_DATA_BUFFER out;
    DWORD status = 0;
    uint32_t size = 0;
    memset(&out, 0, sizeof out);
    *got = NULL;
    if (caller_allocates(dec, &size)) {
        if (!*own || size > *own_cap) {
            RELEASE(*own);
            *own = new_sample(size, NULL);
            *own_cap = size;
            if (!*own) return E_OUTOFMEMORY;
        }
        IMFMediaBuffer *b = NULL;
        if (SUCCEEDED(IMFSample_GetBufferByIndex(*own, 0, &b))) { IMFMediaBuffer_SetCurrentLength(b, 0); RELEASE(b); }
        out.pSample = *own;
    }
    HRESULT hr = IMFTransform_ProcessOutput(dec, 0, 1, &out, &status);
    RELEASE(out.pEvents);
    if (FAILED(hr)) {
        if (out.pSample && out.pSample != *own) RELEASE(out.pSample);
        return hr;
    }
    *got = out.pSample;
    if (*got == *own) IMFSample_AddRef(*got);
    return hr;
}

/* --- pictures --------------------------------------------------------------------------- */

static int is_nv12(IMFMediaType *type) {
    GUID sub;
    return SUCCEEDED(IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &sub)) && IsEqualGUID(&sub, &MFVideoFormat_NV12);
}

/* Settle what the decoder's pictures look like. Called once before the
 * first sample, when only the size the file declares is known, and again
 * whenever the decoder says its output has changed, which it does when the
 * stream itself has told it the size and how much of it is picture. */
static int video_output(movie *m) {
    IMFMediaType *type = choose_output(m->vdec, is_nv12);
    UINT64 size = 0;
    UINT32 stride = 0;
    MFVideoArea area;
    if (!type) return 0;
    (void)IMFMediaType_GetUINT64(type, &MF_MT_FRAME_SIZE, &size);
    m->stride = (uint32_t)(size >> 32);
    m->rows = (uint32_t)size;
    if (SUCCEEDED(IMFMediaType_GetUINT32(type, &MF_MT_DEFAULT_STRIDE, &stride)) && (INT32)stride > 0) m->stride = stride;
    m->left = m->top = 0;
    m->wide = (uint32_t)(size >> 32);
    m->high = (uint32_t)size;
    if (SUCCEEDED(IMFMediaType_GetBlob(type, &MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8 *)&area, sizeof area, NULL))) {
        m->left = (uint32_t)area.OffsetX.value;
        m->top = (uint32_t)area.OffsetY.value;
        m->wide = (uint32_t)area.Area.cx;
        m->high = (uint32_t)area.Area.cy;
    }
    RELEASE(type);
    return m->stride && m->rows;
}

/* The parameter sets out of the configuration record: a byte each of
 * version, profile, compatibility and level; the unit length less one in
 * two bits; then the sequence sets (a count in five bits, each set behind
 * sixteen bits of length) and the picture sets (a count in a byte). */
static int video_sets(movie *m, const uint8_t *record, uint32_t n) {
    if (n < 7) return 0;
    m->unit_length = (record[4] & 3u) + 1;
    m->sets = (uint8_t *)malloc((size_t)n * 3 + 16);
    if (!m->sets) return 0;
    uint32_t at = 5, out = 0;
    for (int kind = 0; kind < 2; kind++) {
        if (at >= n) return 0;
        uint32_t count = kind ? record[at] : record[at] & 31u;
        at++;
        for (; count; count--) {
            if (at + 2 > n) return 0;
            uint32_t length = ((uint32_t)record[at] << 8) | record[at + 1];
            at += 2;
            if (length > n - at) return 0;
            m->sets[out + 0] = 0; m->sets[out + 1] = 0; m->sets[out + 2] = 0; m->sets[out + 3] = 1;
            memcpy(m->sets + out + 4, record + at, length);
            out += 4 + length;
            at += length;
        }
    }
    m->sets_size = out;
    return out != 0;
}

static int video_start(movie *m, const mp4_track *t, char *why, size_t cap) {
    IMFMediaType *in = NULL;
    HRESULT hr = CoCreateInstance(&H264_DECODER, NULL, CLSCTX_INPROC_SERVER, &IID_IMFTransform, (void **)&m->vdec);
    if (FAILED(hr)) { say(why, cap, "this Windows has no H.264 decoder", hr); return 0; }
    if (!video_sets(m, t->config, t->config_size)) { say(why, cap, "the movie's H.264 set-up is not whole", 0); return 0; }
    hr = MFCreateMediaType(&in);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetGUID(in, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetGUID(in, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT64(in, &MF_MT_FRAME_SIZE, ((UINT64)t->width << 32) | t->height);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(in, &MF_MT_INTERLACE_MODE, MFVideoInterlace_MixedInterlaceOrProgressive);
    if (SUCCEEDED(hr)) hr = IMFTransform_SetInputType(m->vdec, 0, in, 0);
    RELEASE(in);
    if (FAILED(hr)) { say(why, cap, "the H.264 decoder would not take the movie's pictures", hr); return 0; }
    if (!video_output(m)) { say(why, cap, "the H.264 decoder offers no picture layout this can use", 0); return 0; }
    (void)IMFTransform_ProcessMessage(m->vdec, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    (void)IMFTransform_ProcessMessage(m->vdec, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return 1;
}

/* The next sample of pictures into the decoder: its units behind start
 * codes, and before a sample decoding can start at, the parameter sets. */
static HRESULT video_feed(movie *m) {
    mp4_sample s;
    if (!fetch(m, m->video, m->vnext, &s)) return E_FAIL;
    const uint32_t ul = m->unit_length;
    uint32_t total = s.sync ? m->sets_size : 0, at = 0;
    while (at + ul <= s.size) {
        uint32_t length = 0;
        for (uint32_t k = 0; k < ul; k++) length = (length << 8) | m->sample[at + k];
        at += ul;
        if (length > s.size - at) return E_FAIL;
        total += 4 + length;
        at += length;
    }

    IMFMediaBuffer *buffer = NULL;
    IMFSample *sample = new_sample(total, &buffer);
    BYTE *bytes = NULL;
    if (!sample) return E_OUTOFMEMORY;
    HRESULT hr = IMFMediaBuffer_Lock(buffer, &bytes, NULL, NULL);
    if (SUCCEEDED(hr)) {
        uint32_t out = 0;
        if (s.sync) { memcpy(bytes, m->sets, m->sets_size); out = m->sets_size; }
        for (at = 0; at + ul <= s.size; ) {
            uint32_t length = 0;
            for (uint32_t k = 0; k < ul; k++) length = (length << 8) | m->sample[at + k];
            at += ul;
            bytes[out] = 0; bytes[out + 1] = 0; bytes[out + 2] = 0; bytes[out + 3] = 1;
            memcpy(bytes + out + 4, m->sample + at, length);
            out += 4 + length;
            at += length;
        }
        IMFMediaBuffer_Unlock(buffer);
        hr = IMFMediaBuffer_SetCurrentLength(buffer, total);
    }
    /* times are in units of a hundred nanoseconds */
    if (SUCCEEDED(hr)) hr = IMFSample_SetSampleTime(sample, (s.time_us < 0 ? 0 : s.time_us) * 10);
    if (SUCCEEDED(hr)) hr = IMFSample_SetSampleDuration(sample, (LONGLONG)s.duration_us * 10);
    if (SUCCEEDED(hr)) hr = IMFTransform_ProcessInput(m->vdec, 0, sample, 0);
    RELEASE(buffer);
    RELEASE(sample);
    if (SUCCEEDED(hr)) {
        if (m->ninside == MOST_INSIDE) m->ninside--;        /* cannot be, for a stream the decoder gives every picture of */
        m->inside[m->ninside].time = s.time_us < 0 ? 0 : s.time_us;
        m->inside[m->ninside].shown = s.duration_us;
        m->ninside++;
        m->vnext++;
    }
    return hr;
}

/* A decoded picture into the caller's layout: the rows of the picture
 * itself out of the decoder's, which may be longer and more. */
static HRESULT video_copy(const movie *m, IMFSample *sample, uint8_t *nv12) {
    const movie_stream *st = &m->streams[m->video];
    IMFMediaBuffer *buffer = NULL;
    BYTE *bytes = NULL;
    DWORD have = 0;
    HRESULT hr = IMFSample_ConvertToContiguousBuffer(sample, &buffer);
    if (FAILED(hr)) return hr;
    hr = IMFMediaBuffer_Lock(buffer, &bytes, NULL, &have);
    if (SUCCEEDED(hr)) {
        const uint32_t w = st->width < m->wide ? st->width : m->wide, h = st->height < m->high ? st->height : m->high;
        const uint32_t pairs = (st->width + 1) / 2, cw = (w + 1) / 2 * 2, ch = (h + 1) / 2;
        const uint8_t *luma = bytes + (size_t)m->top * m->stride + m->left;
        const uint8_t *chroma = bytes + (size_t)m->rows * m->stride + (size_t)(m->top / 2) * m->stride + (m->left & ~1u);
        uint8_t *out = nv12 + (size_t)st->width * st->height;
        if ((size_t)m->rows * m->stride * 3 / 2 > have) hr = E_FAIL;
        for (uint32_t y = 0; SUCCEEDED(hr) && y < h; y++) memcpy(nv12 + (size_t)y * st->width, luma + (size_t)y * m->stride, w);
        for (uint32_t y = 0; SUCCEEDED(hr) && y < ch; y++) memcpy(out + (size_t)y * pairs * 2, chroma + (size_t)y * m->stride, cw);
        IMFMediaBuffer_Unlock(buffer);
    }
    RELEASE(buffer);
    return hr;
}

int movie_next_video(movie *m, uint8_t *nv12, uint64_t *time_us, uint64_t *shown_us) {
    if (!m || m->video < 0) return 0;
    com_here();
    for (;;) {
        IMFSample *got = NULL;
        HRESULT hr = take_output(m->vdec, &m->vout, &m->vout_cap, &got);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (!video_output(m)) return -1;
            continue;
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (m->vnext < mp4_track_at(m->file, (uint32_t)m->video)->samples) {
                if (FAILED(video_feed(m))) return -1;
                continue;
            }
            if (m->vdraining) return 0;
            /* no more to give it: it is told so, and hands over the pictures it was holding for their turn */
            m->vdraining = 1;
            (void)IMFTransform_ProcessMessage(m->vdec, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            (void)IMFTransform_ProcessMessage(m->vdec, MFT_MESSAGE_COMMAND_DRAIN, 0);
            continue;
        }
        if (FAILED(hr) || !got) return -1;
        /* its time: the earliest of the samples inside, which is then inside no longer */
        uint32_t first = 0;
        for (uint32_t k = 1; k < m->ninside; k++) if (m->inside[k].time < m->inside[first].time) first = k;
        if (time_us) *time_us = m->ninside ? (uint64_t)m->inside[first].time : 0;
        if (shown_us) *shown_us = m->ninside ? m->inside[first].shown : 0;
        if (m->ninside) m->inside[first] = m->inside[--m->ninside];
        hr = video_copy(m, got, nv12);
        RELEASE(got);
        return FAILED(hr) ? -1 : 1;
    }
}

/* --- sound ------------------------------------------------------------------------------ */

static int is_pcm16(IMFMediaType *type) {
    GUID sub;
    UINT32 bits = 0;
    return SUCCEEDED(IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &sub)) && IsEqualGUID(&sub, &MFAudioFormat_PCM) &&
           SUCCEEDED(IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_BITS_PER_SAMPLE, &bits)) && bits == 16;
}

/* The rate and the channels an AudioSpecificConfig declares: five bits of
 * object type, four that pick a rate from a list (or say that 24 bits of
 * rate follow), four of channel layout. 0 where it does not say. */
static void audio_declared(const uint8_t *config, uint32_t n, uint32_t *rate, uint32_t *channels) {
    static const uint32_t RATES[16] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350, 0, 0, 0 };
    *rate = *channels = 0;
    if (n < 2) return;
    uint32_t index = ((config[0] & 7u) << 1) | (config[1] >> 7);
    if (index == 15) {
        if (n < 5) return;
        *rate = ((config[1] & 0x7Fu) << 17) | ((uint32_t)config[2] << 9) | ((uint32_t)config[3] << 1) | (config[4] >> 7);
        *channels = (config[4] >> 3) & 15u;
    } else {
        *rate = RATES[index];
        *channels = (config[1] >> 3) & 15u;
    }
    if (*channels == 7) *channels = 8;      /* layout 7 is seven speakers and a woofer */
}

static int audio_start(movie *m, const mp4_track *t, movie_stream *st, char *why, size_t cap) {
    IMFMediaType *in = NULL, *out = NULL;
    uint8_t user[12 + 64];
    uint32_t rate, channels;
    UINT32 value = 0;
    if (t->config_size > 64) { say(why, cap, "the movie's AAC set-up is not one this knows", 0); return 0; }
    HRESULT hr = CoCreateInstance(&AAC_DECODER, NULL, CLSCTX_INPROC_SERVER, &IID_IMFTransform, (void **)&m->adec);
    if (FAILED(hr)) { say(why, cap, "this Windows has no AAC decoder", hr); return 0; }
    audio_declared(t->config, t->config_size, &rate, &channels);
    if (!rate) rate = t->rate;
    if (!channels) channels = t->channels;
    /* What follows a WAVEFORMATEX in the system's description of AAC, and
     * then the stream's own: payload 0 (bare frames), a profile left
     * unsaid (0xFE), a structure of kind 0, and eight bytes reserved. */
    memset(user, 0, sizeof user);
    user[2] = 0xFE;
    memcpy(user + 12, t->config, t->config_size);
    hr = MFCreateMediaType(&in);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetGUID(in, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetGUID(in, &MF_MT_SUBTYPE, &MFAudioFormat_AAC);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(in, &MF_MT_AAC_PAYLOAD_TYPE, 0);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(in, &MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0xFE);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(in, &MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(in, &MF_MT_AUDIO_NUM_CHANNELS, channels);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(in, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetBlob(in, &MF_MT_USER_DATA, user, 12 + t->config_size);
    if (SUCCEEDED(hr)) hr = IMFTransform_SetInputType(m->adec, 0, in, 0);
    RELEASE(in);
    if (FAILED(hr)) { say(why, cap, "the AAC decoder would not take the movie's sound", hr); return 0; }
    out = choose_output(m->adec, is_pcm16);
    if (!out) { say(why, cap, "the AAC decoder offers no sixteen-bit sound", 0); return 0; }
    /* what comes out is the decoder's to say: a stream may be coded at half the rate it plays at */
    if (SUCCEEDED(IMFMediaType_GetUINT32(out, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &value)) && value) rate = value;
    if (SUCCEEDED(IMFMediaType_GetUINT32(out, &MF_MT_AUDIO_NUM_CHANNELS, &value)) && value) channels = value;
    RELEASE(out);
    if (!rate || !channels || channels > 8) { say(why, cap, "the movie's sound has no rate or no channels", 0); return 0; }
    st->rate = rate;
    st->channels = channels;
    (void)IMFTransform_ProcessMessage(m->adec, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    (void)IMFTransform_ProcessMessage(m->adec, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return 1;
}

static HRESULT audio_feed(movie *m) {
    mp4_sample s;
    if (!fetch(m, m->audio, m->anext, &s)) return E_FAIL;
    IMFMediaBuffer *buffer = NULL;
    IMFSample *sample = new_sample(s.size, &buffer);
    BYTE *bytes = NULL;
    if (!sample) return E_OUTOFMEMORY;
    HRESULT hr = IMFMediaBuffer_Lock(buffer, &bytes, NULL, NULL);
    if (SUCCEEDED(hr)) {
        memcpy(bytes, m->sample, s.size);
        IMFMediaBuffer_Unlock(buffer);
        hr = IMFMediaBuffer_SetCurrentLength(buffer, s.size);
    }
    if (SUCCEEDED(hr)) hr = IMFSample_SetSampleTime(sample, (s.time_us < 0 ? 0 : s.time_us) * 10);
    if (SUCCEEDED(hr)) hr = IMFTransform_ProcessInput(m->adec, 0, sample, 0);
    RELEASE(buffer);
    RELEASE(sample);
    /* a track whose playing begins some way in has sound before its start, which is decoded and dropped */
    if (SUCCEEDED(hr) && !m->anext && s.time_us < 0)
        m->pcm_skip = (-s.time_us * (int64_t)m->streams[m->audio].rate + 500000) / 1000000;
    if (SUCCEEDED(hr)) m->anext++;
    return hr;
}

/* Decode until there is sound waiting. 1, 0 at the end, -1 on a failure. */
static int audio_more(movie *m) {
    const uint32_t channels = m->streams[m->audio].channels;
    for (;;) {
        IMFSample *got = NULL;
        HRESULT hr = take_output(m->adec, &m->aout, &m->aout_cap, &got);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            IMFMediaType *type = choose_output(m->adec, is_pcm16);
            if (!type) return -1;
            RELEASE(type);
            continue;
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (m->anext < mp4_track_at(m->file, (uint32_t)m->audio)->samples) {
                if (FAILED(audio_feed(m))) return -1;
                continue;
            }
            if (m->adraining) return 0;
            m->adraining = 1;
            (void)IMFTransform_ProcessMessage(m->adec, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            (void)IMFTransform_ProcessMessage(m->adec, MFT_MESSAGE_COMMAND_DRAIN, 0);
            continue;
        }
        if (FAILED(hr) || !got) return -1;

        IMFMediaBuffer *buffer = NULL;
        BYTE *bytes = NULL;
        DWORD have = 0;
        int kept = 0;
        hr = IMFSample_ConvertToContiguousBuffer(got, &buffer);
        if (SUCCEEDED(hr)) hr = IMFMediaBuffer_Lock(buffer, &bytes, NULL, &have);
        if (SUCCEEDED(hr)) {
            uint32_t frames = have / (2 * channels);
            const int16_t *from = (const int16_t *)bytes;
            if (m->pcm_skip > 0) {
                uint32_t drop = m->pcm_skip < (int64_t)frames ? (uint32_t)m->pcm_skip : frames;
                m->pcm_skip -= drop;
                from += (size_t)drop * channels;
                frames -= drop;
            }
            if (frames > m->pcm_cap) {
                int16_t *grown = (int16_t *)realloc(m->pcm, (size_t)frames * channels * sizeof(int16_t));
                if (grown) { m->pcm = grown; m->pcm_cap = frames; } else frames = 0;
            }
            memcpy(m->pcm, from, (size_t)frames * channels * sizeof(int16_t));
            m->pcm_at = 0;
            m->pcm_frames = frames;
            kept = frames != 0;
            IMFMediaBuffer_Unlock(buffer);
        }
        RELEASE(buffer);
        RELEASE(got);
        if (FAILED(hr)) return -1;
        if (kept) return 1;
    }
}

int movie_next_audio(movie *m, int16_t *pcm, uint32_t frames, uint64_t *time_us) {
    if (!m || m->audio < 0) return 0;
    com_here();
    const movie_stream *st = &m->streams[m->audio];
    uint32_t given = 0;
    if (time_us) *time_us = m->pcm_given * 1000000ull / st->rate;
    while (given < frames) {
        if (!m->pcm_frames) {
            int more = audio_more(m);
            if (more < 0) return given ? (int)given : -1;
            if (!more) break;
        }
        uint32_t take = m->pcm_frames < frames - given ? m->pcm_frames : frames - given;
        memcpy(pcm + (size_t)given * st->channels, m->pcm + (size_t)m->pcm_at * st->channels, (size_t)take * st->channels * sizeof(int16_t));
        m->pcm_at += take;
        m->pcm_frames -= take;
        given += take;
    }
    m->pcm_given += given;
    return (int)given;
}

/* --- the movie -------------------------------------------------------------------------- */

static int64_t file_read(void *user, uint64_t offset, void *dst, uint32_t length) {
    const movie *m = (const movie *)user;
    return m->from.read(m->from.user, offset, dst, length);
}

movie *movie_open(const movie_reader *from, char *why, size_t why_cap) {
    char none[1];
    if (!why || !why_cap) { why = none; why_cap = sizeof none; }
    why[0] = '\0';
    movie *m = (movie *)calloc(1, sizeof *m);
    if (!m) { snprintf(why, why_cap, "no memory for a movie"); return NULL; }
    m->from = *from;
    m->video = m->audio = -1;
    com_here();
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) { say(why, why_cap, "this Windows has no Media Foundation", hr); movie_close(m); return NULL; }
    m->started = 1;
    m->file = mp4_open(file_read, m, from->size, why, why_cap);
    if (!m->file) { movie_close(m); return NULL; }

    m->nstreams = mp4_tracks(m->file) < MOST_STREAMS ? mp4_tracks(m->file) : MOST_STREAMS;
    for (uint32_t k = 0; k < m->nstreams; k++) {
        const mp4_track *t = mp4_track_at(m->file, k);
        movie_stream *st = &m->streams[k];
        st->kind = t->kind == MP4_VIDEO ? MOVIE_VIDEO : t->kind == MP4_AUDIO ? MOVIE_AUDIO : MOVIE_OTHER;
        memcpy(st->language, t->language, 4);
        st->duration_us = t->duration_us;
        st->width = t->width;
        st->height = t->height;
        st->channels = t->channels;
        st->rate = t->rate;
        /* the first of each kind that a decoder takes is the one decoded; a failure's sentence is kept for when nothing is */
        if (t->codec == MP4_H264 && m->video < 0 && t->samples && t->width && t->height) {
            if (video_start(m, t, why, why_cap)) { m->video = (int)k; st->decoded = 1; }
            else { RELEASE(m->vdec); free(m->sets); m->sets = NULL; }
        } else if (t->codec == MP4_AAC && m->audio < 0 && t->samples) {
            if (audio_start(m, t, st, why, why_cap)) { m->audio = (int)k; st->decoded = 1; }
            else RELEASE(m->adec);
        }
    }
    if (m->video < 0 && m->audio < 0) {
        if (!why[0]) snprintf(why, why_cap, "a movie with no pictures in H.264 and no sound in AAC");
        movie_close(m);
        return NULL;
    }
    why[0] = '\0';
    return m;
}

void movie_close(movie *m) {
    if (!m) return;
    com_here();
    RELEASE(m->vout);
    RELEASE(m->aout);
    RELEASE(m->vdec);
    RELEASE(m->adec);
    if (m->started) MFShutdown();
    mp4_close(m->file);
    free(m->sets);
    free(m->sample);
    free(m->pcm);
    free(m);
}

uint32_t movie_streams(const movie *m) { return m ? m->nstreams : 0; }

const movie_stream *movie_stream_at(const movie *m, uint32_t stream) { return m && stream < m->nstreams ? &m->streams[stream] : NULL; }

int movie_rewind(movie *m) {
    if (!m) return 0;
    com_here();
    /* each decoder forgets what it holds, and is told a stream begins */
    if (m->vdec) {
        (void)IMFTransform_ProcessMessage(m->vdec, MFT_MESSAGE_COMMAND_FLUSH, 0);
        (void)IMFTransform_ProcessMessage(m->vdec, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }
    if (m->adec) {
        (void)IMFTransform_ProcessMessage(m->adec, MFT_MESSAGE_COMMAND_FLUSH, 0);
        (void)IMFTransform_ProcessMessage(m->adec, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }
    m->vnext = m->anext = 0;
    m->ninside = 0;
    m->vdraining = m->adraining = 0;
    m->pcm_at = m->pcm_frames = 0;
    m->pcm_given = 0;
    m->pcm_skip = 0;
    return 1;
}

#endif /* _WIN32 */
