/* gxm_gpu.c — a scene's list of draws, drawn by the host's graphics card. See gxm_gpu.h.
 *
 * What this file decides is whether a list is one the card's path can draw
 * as the processor's does, and if it is, what to hand over: for each draw
 * its two programs as shaders for what is bound, its vertices as the words
 * the vertex program would find in its registers, its secondary attributes
 * as they stand after the secondary program, its textures, and the
 * console's state in the card's terms. Anything it is not sure of sends the
 * whole list to the processor, with the reason said once.
 *
 * And it keeps account of the title's surfaces: which of them the card
 * holds newer than the title's memory does, what a texture that reads one
 * of those is made from, and when the bytes have to go back.
 */

#include "gxm_gpu.h"
#include "gpu.h"
#define GPU_TWEEN_CALLER                    /* what is asked of the card from here on is written down first (gpu_tween.h) */
#include "gpu_tween.h"
#include "gxp_hlsl.h"
#include "vitarecomp/mem.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- what was asked ------------------------------------------------------------------------- */

static int g_mode = -1;

int gxm_gpu_mode(void) {
    if (g_mode >= 0) return g_mode;
    const char *e = getenv("VITARECOMP_GXM_GPU");
    g_mode = GXM_GPU_OFF;
    if (!e || !*e || !strcmp(e, "0")) return g_mode;
    if (gpu_open()) {
        fprintf(stderr, "gxm gpu: there is no card to draw with; every list is drawn on the processor\n");
        return g_mode;
    }
    g_mode = !strcmp(e, "check") ? GXM_GPU_CHECK : GXM_GPU_DRAW;
    fprintf(stderr, "gxm gpu: %s with %s\n", g_mode == GXM_GPU_CHECK ? "lists are checked against" : "lists are drawn", gpu_name());
    return g_mode;
}

/* Why a list was left to the processor: each reason once. Returns 0. */
static uint32_t g_refused;                  /* lists left to the processor */
static uint32_t g_list = 1;                 /* counts lists */
static uint32_t g_now;                      /* the moment the list in hand began at (mem.h): what was written before it is known */

/* What the path costs, for the stage profile (VITARECOMP_GXM_PROFILE):
 * nanoseconds since the last report, by what was being done. */
enum { P_HASH = 0, P_TEXTURE, P_IN, P_VERTICES, P_DRAW, P_BACK, P_OUT, P_SHOW, P_PROGRAM, P_WRITTEN, P_STAGES };
extern uint64_t vita_mem_counts, vita_mem_counted, vita_mem_asking, vita_mem_ticks;         /* mem.c */
extern int vita_mem_timed;
static uint64_t g_ns[P_STAGES], g_hashed, g_made, g_lists, g_draws, g_backs, g_puts, g_views, g_programs;
static uint64_t g_mesh_hits, g_mesh_made, g_mesh_passed;
static int g_timed = -1;

static uint64_t now_ns(void) {
    struct timespec t;
    if (g_timed < 0) { const char *e = getenv("VITARECOMP_GXM_PROFILE"); g_timed = e && strtoul(e, NULL, 0) != 0; vita_mem_timed = g_timed; }
    if (!g_timed) return 0;
    timespec_get(&t, TIME_UTC);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

void gxm_gpu_profile(uint32_t frames) {
    static const char *const NAME[P_STAGES] = { "textures' bytes looked over", "textures made from memory", "targets and textures made ready",
                                                "vertices made ready", "draws handed over", "the card waited for and surfaces read back",
                                                "surfaces put back in memory", "the picture shown", "programs made ready",
                                                "and of all those, asking which pages were written" };
    static unsigned compiled;
    static uint64_t counts, counted, asking;
    /* the asking is done wherever bytes are about to be looked at, inside the other stages: its time is of theirs */
    g_ns[P_WRITTEN] = vita_mem_ticks ? (vita_mem_asking - asking) * 1000000000ull / vita_mem_ticks : 0;
    asking = vita_mem_asking;
    unsigned compiled_now = gpu_shaders_compiled();
    double per = frames ? 1e-6 / (double)frames : 0.0;
    if (!g_lists) return;
    fprintf(stderr, "    on the card, ms a frame:");
    for (int k = 0; k < P_STAGES; k++) fprintf(stderr, "%s %s %.1f", k ? "," : "", NAME[k], (double)g_ns[k] * per);
    fprintf(stderr, "; %.1f lists and %.0f draws a frame, %.1f MB of textures looked over, %.1f textures made from memory and %.1f from "
                    "surfaces on the card, %.1f surfaces filled from memory and %.1f sent back to it; of the draws' geometry %.0f was "
                    "kept from before, %.0f made and kept, %.0f made for the once; %u programs made ready in all, %u of them compiled "
                    "and the rest found compiled; which pages were written asked %.1f times a frame, %.0f pages a frame found\n",
            (double)g_lists / (double)frames, (double)g_draws / (double)frames, (double)g_hashed / (double)frames / 1048576.0,
            (double)g_made / (double)frames, (double)g_views / (double)frames, (double)g_puts / (double)frames, (double)g_backs / (double)frames,
            (double)g_mesh_hits / (double)frames, (double)g_mesh_made / (double)frames, (double)g_mesh_passed / (double)frames,
            (unsigned)g_programs, compiled_now - compiled, (double)(vita_mem_counts - counts) / (double)frames,
            (double)(vita_mem_counted - counted) / (double)frames);
    compiled = compiled_now;
    counts = vita_mem_counts;
    counted = vita_mem_counted;
    memset(g_ns, 0, sizeof(g_ns));
    g_hashed = g_made = g_lists = g_draws = g_backs = g_puts = g_views = g_programs = 0;
    g_mesh_hits = g_mesh_made = g_mesh_passed = 0;
}

/* VITARECOMP_GXM_GPU_LOG=1 says, besides each texture made from memory,
 * each surface filled from the title's memory and each sent back to it. */
static int logging(void) {
    static int asked, wanted;
    if (!asked) { const char *e = getenv("VITARECOMP_GXM_GPU_LOG"); asked = 1; wanted = e && *e && *e != '0'; }
    return wanted;
}

static int said(const char *what, const char *fmt, va_list ap) {
    static char seen[96][200];
    static unsigned count;
    char why[200];
    vsnprintf(why, sizeof(why), fmt, ap);
    for (unsigned i = 0; i < count; i++)
        if (!strcmp(seen[i], why)) return 0;
    if (count < 96) {
        memcpy(seen[count++], why, sizeof(why));
        fprintf(stderr, "gxm gpu: %s: %s\n", what, why);
    }
    return 0;
}

static int refuse(const char *fmt, ...) {
    va_list ap;
    g_refused++;
    va_start(ap, fmt);
    said("a list is left to the processor", fmt, ap);
    va_end(ap);
    return 0;
}

/* Why a surface the card held was sent back to memory to be read as a
 * texture from there: each reason once. Returns NULL. */
static gpu_texture *by_memory(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    said("a surface goes by way of memory to be read as a texture", fmt, ap);
    va_end(ap);
    return NULL;
}

/* --- shaders, one for each program and what was bound with it ------------------------------- */

typedef struct { int used; uint64_t name; gxp_hlsl_bind bind; gpu_shader *shader; } shader_slot;

#define SHADERS 4096
static shader_slot g_shaders[SHADERS];

static gpu_shader *shader_for(const gxm_program *program, const gxp_hlsl_bind *bind) {
    uint64_t h = program->name * 1099511628211ull;
    const uint8_t *b = (const uint8_t *)bind;
    for (size_t i = 0; i < sizeof(*bind); i++) h = (h ^ b[i]) * 1099511628211ull;
    for (unsigned tries = 0, at = (unsigned)(h % SHADERS); tries < SHADERS; tries++, at = (at + 1) % SHADERS) {
        shader_slot *s = &g_shaders[at];
        if (s->used && (s->name != program->name || memcmp(&s->bind, bind, sizeof(*bind)))) continue;
        if (s->used) return s->shader;

        char why[200], log[1024];
        int fragment = (program->gxp.flags & GXP_FLAG_FRAGMENT) != 0;
        uint64_t began = now_ns();
        /* in the language the card reads */
        int glsl = gpu_language() == GPU_GLSL;
        char *text = gxp_text(&program->gxp, bind, glsl ? GXP_GLSL : GXP_HLSL, why, sizeof(why));
        s->used = 1;
        s->name = program->name;
        s->bind = *bind;
        s->shader = NULL;
        if (!text) {
            refuse("program %016llx is not translated: it has %s", (unsigned long long)program->name, why);
            return NULL;
        }
        unsigned compiled = gpu_shaders_compiled();
        s->shader = gpu_shader_make(fragment, text, bind->pa_words, log, sizeof(log));
        if (logging() && gpu_shaders_compiled() != compiled)
            fprintf(stderr, "gxm gpu: list %u: %s program %016llx compiled, %u bytes of text, %.0f ms\n", g_list, fragment ? "fragment" : "vertex",
                    (unsigned long long)program->name, (unsigned)strlen(text), (double)(now_ns() - began) * 1e-6);
        g_ns[P_PROGRAM] += now_ns() - began;
        g_programs++;
        if (!s->shader) refuse("program %016llx did not compile: %.120s", (unsigned long long)program->name, log);
        const char *dump = getenv("VITARECOMP_GXM_GPU_SHADERS");
        if (dump && *dump) {                        /* the text, for reading: a file a program and binding */
            char path[1024];
            snprintf(path, sizeof(path), "%s/%c_%016llx_%08x.%s", dump, fragment ? 'f' : 'v', (unsigned long long)program->name, (unsigned)h,
                     glsl ? "glsl" : "hlsl");
            FILE *f = fopen(path, "wb");
            if (f) { fputs(text, f); fclose(f); }
        }
        free(text);
        return s->shader;
    }
    refuse("more programs than there is room to keep");
    return NULL;
}

/* --- what a texture's bytes are known by ------------------------------------------------------ */

/* A hash of a texture's bytes, all of them, and of its palette's. Nothing
 * less will do: a title writes into its textures where it pleases, a glyph
 * into a sheet of them, a scene's depth into the surface a later scene
 * samples, and a hash of some of the bytes is one the change can miss.
 * Four sums side by side, each word folded in by a multiply and a turn. */
static uint64_t turn(uint64_t v, int by) { return (v << by) | (v >> (64 - by)); }

static uint64_t fold(uint64_t h, const uint8_t *p, size_t n) {
    static const uint64_t A = 0x9E3779B185EBCA87ull, B = 0xC2B2AE3D27D4EB4Full;
    uint64_t lane[4] = { h + A + B, h + B, h, h - A }, w[4];
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        memcpy(w, p + i, 32);
        for (int k = 0; k < 4; k++) lane[k] = turn(lane[k] + w[k] * B, 31) * A;
    }
    h = turn(lane[0], 1) + turn(lane[1], 7) + turn(lane[2], 12) + turn(lane[3], 18) + n;
    for (; i < n; i++) h = turn(h ^ (p[i] * 0x27D4EB2F165667C5ull), 11) * A;
    h ^= h >> 33; h *= B; h ^= h >> 29;
    return h;
}

static uint64_t content_stamp(const gxm_tex *t) {
    uint64_t h = fold(0, t->base, t->bytes < t->avail ? t->bytes : t->avail);
    if (t->pal) h = fold(h, t->pal, (t->kind == GXM_TEX_PALETTE4 ? 16u : 256u) * 4u);
    return h;
}

/* Whether a texture goes to the card as a byte a channel where that loses
 * nothing. A card keeps a texture as it is given it, and four floats a
 * texel are sixteen bytes where most textures were made of four: a
 * level's worth of them was a gigabyte and a half of the card's memory.
 * So what was decoded is looked at, and when every number in it is one a
 * byte stands for on the card (k / 255, to the last bit) the texture is
 * handed over as bytes. The card then makes the same numbers of them, and
 * a texture of anything else (floats, signed channels, a gamma curve
 * taken out) goes as floats as before.
 *
 * Not while the card is checked against the processor: the two then agree
 * on every texel but a card may weigh four bytes with fewer bits than it
 * weighs four floats, and the check is of everything else.
 * VITARECOMP_GXM_GPU_TEXELS=float keeps every texture in floats. */
static int bytes_wanted(void) {
    static int wanted = -1;
    if (wanted < 0) {
        const char *e = getenv("VITARECOMP_GXM_GPU_TEXELS");
        wanted = !(e && !strcmp(e, "float")) && gxm_gpu_mode() != GXM_GPU_CHECK;
    }
    return wanted;
}

/* `count` numbers as bytes, if every one of them is a byte's. */
static int as_bytes(const float *in, size_t count, uint8_t *out) {
    static float unit[256];
    if (unit[255] != 1.0f)
        for (unsigned k = 0; k < 256; k++) unit[k] = (float)k / 255.0f;
    for (size_t i = 0; i < count; i++) {
        const float v = in[i];
        if (!(v >= 0.0f && v <= 1.0f)) return 0;
        const unsigned k = (unsigned)(v * 255.0f + 0.5f);
        if (unit[k] != v) return 0;
        out[i] = (uint8_t)k;
    }
    return 1;
}

static gpu_texture *upload(const gxm_tex *t) {
    const void *images[6 * GXM_TEX_LEVELS];
    unsigned faces = t->cube ? 6 : 1, levels = t->levels < GXM_TEX_LEVELS ? t->levels : GXM_TEX_LEVELS, made = 0;
    gpu_texture *out = NULL;
    int bytes = bytes_wanted();
    if (levels > 16) levels = 16;
    /* the card wants each level half the last: stop where the texture's own levels are not */
    for (unsigned level = 1; level < levels; level++) {
        uint32_t w = t->level_w[0] >> level ? t->level_w[0] >> level : 1, h = t->level_h[0] >> level ? t->level_h[0] >> level : 1;
        if (t->level_w[level] != w || t->level_h[level] != h) { levels = level; break; }
    }
    for (unsigned level = 0; level < levels; level++)
        for (unsigned face = 0; face < faces; face++) {
            float *texels = malloc((size_t)t->level_w[level] * t->level_h[level] * 4 * sizeof(float));
            if (!texels) goto done;
            gxm_tex_level_floats(t, NULL, face, level, texels);
            images[made++] = texels;
        }
    /* as bytes, if every level of every face is bytes: each image's floats give way to a quarter as much */
    for (unsigned i = 0, level = 0; bytes && level < levels; level++)
        for (unsigned face = 0; bytes && face < faces; face++, i++) {
            const size_t count = (size_t)t->level_w[level] * t->level_h[level] * 4;
            uint8_t *small = malloc(count);
            if (small && as_bytes((const float *)images[i], count, small)) {
                free((void *)images[i]);
                images[i] = small;
            } else {
                /* one that is not: the ones before it are made floats again, the long way round */
                free(small);
                bytes = 0;
                for (unsigned j = 0, l = 0; j < i; l++)
                    for (unsigned f = 0; f < faces && j < i; f++, j++) {
                        float *texels = malloc((size_t)t->level_w[l] * t->level_h[l] * 4 * sizeof(float));
                        if (!texels) goto done;
                        gxm_tex_level_floats(t, NULL, f, l, texels);
                        free((void *)images[j]);
                        images[j] = texels;
                    }
            }
        }
    out = gpu_texture_make(bytes ? GPU_RGBA8 : GPU_RGBA32F, t->level_w[0], t->level_h[0], levels, t->cube, images);
    {
        if (logging())
            fprintf(stderr, "gxm gpu: texture at 0x%08X: %u by %u, %u of %u levels (second level %u by %u)%s, format 0x%08X kind %u, "
                            "filters %u %u %u, lod bias %g min %u, modes %u %u, as %s: %s\n", t->data, t->level_w[0], t->level_h[0], levels,
                    t->levels, t->levels > 1 ? t->level_w[1] : 0, t->levels > 1 ? t->level_h[1] : 0, t->cube ? ", a cube" : "", t->format,
                    t->kind, t->min_filter, t->mag_filter, t->mip_filter, (double)t->lod_bias, t->lod_min, t->u_mode, t->v_mode,
                    bytes ? "bytes" : "floats", out ? "made" : "REFUSED");
    }
done:
    for (unsigned i = 0; i < made; i++) free((void *)images[i]);
    return out;
}

/* Whether a texture is read as the one texel a point falls in: flat, of
 * one level, and not filtered. gxm_tex.c reads a texture of one level by
 * the filter for enlarging, whatever its footprint. */
static uint8_t point_read(const gxm_tex *t) {
    return !t->cube && t->levels <= 1 && !((t->mag_filter == 1 || t->mag_filter == 2) && !t->pf.raw);
}

static void sampler_for(const gxm_tex *t, gpu_sampler *out) {
    memset(out, 0, sizeof(*out));
    /* a texture whose bits are its value is never filtered */
    out->min_linear = (t->min_filter == 1 || t->min_filter == 2) && !t->pf.raw;
    out->mag_linear = (t->mag_filter == 1 || t->mag_filter == 2) && !t->pf.raw;
    if (t->levels <= 1) out->min_linear = out->mag_linear;      /* as gxm_tex.c has it: see point_read */
    out->mip = t->levels > 1 ? ((t->mip_filter && !t->pf.raw) ? 2 : 1) : 0;
    uint8_t modes[2] = { t->cube ? 2 : t->u_mode, t->cube ? 2 : t->v_mode };
    for (int k = 0; k < 2; k++) {
        uint8_t m = modes[k], a = (m == 0 || m == 4) ? 0 : m == 1 ? 1 : m == 3 ? 3 : 2;
        if (k == 0) out->address_u = a; else out->address_v = a;
    }
    out->lod_bias = t->lod_bias;
    out->lod_min = (float)t->lod_min;
    out->lod_max = (float)(t->levels ? t->levels - 1 : 0);
}

/* --- the target ------------------------------------------------------------------------------ */

/* The card's form of a colour surface, or GPU_NONE. `raw` is how many
 * words a pixel is when the surface is kept as words and not as colours. */
static unsigned colour_format(const gxm_draw_state *s, unsigned *raw) {
    const pixfmt *f = &s->colour;
    *raw = 0;
    if (f->bytes == 4 && f->fields >= 3 && !f->raw) {
        int r = f->from[0], g = f->from[1], b = f->from[2], a = f->from[3];
        if (r < 0 || g < 0 || b < 0) return GPU_NONE;
        for (int k = 0; k < f->fields; k++)
            if (f->f[k].bits != 8 || f->f[k].kind != PIX_UNORM) return GPU_NONE;
        if (f->f[g].shift != 8 || (a >= 0 && f->f[a].shift != 24)) return GPU_NONE;
        if (f->f[r].shift == 0 && f->f[b].shift == 16) return s->colour_gamma ? GPU_RGBA8_SRGB : GPU_RGBA8;
        if (f->f[r].shift == 16 && f->f[b].shift == 0) return s->colour_gamma ? GPU_BGRA8_SRGB : GPU_BGRA8;
        return GPU_NONE;
    }
    if (f->bytes == 8 && f->fields == 4 && !s->colour_gamma) {
        for (int k = 0; k < 4; k++)
            if (f->f[k].kind != PIX_F16 || f->from[k] < 0 || f->f[f->from[k]].shift != 16 * k) return GPU_NONE;
        return GPU_RGBA16F;
    }
    if (f->bytes == 8 && f->fields == 2 && f->f[0].kind == PIX_F32) { *raw = 2; return GPU_RG32UI; }
    return GPU_NONE;
}

/* What a fragment program leaves when its result goes across as it is:
 * how many words, 0 when it leaves a colour. As write_colour decides. */
static unsigned raw_result_words(const gxm_draw_state *s, const gxp_program *p) {
    if (p->result_type == GXP_U32 || p->result_type == GXP_S32) return p->result_components > 4 ? 4 : p->result_components;
    if (p->result_type == GXP_F16 && p->result_components > 2 && s->colour.bytes == 8 && s->colour.fields == 2 &&
        s->colour.f[0].kind == PIX_F32) return 2;
    return 0;
}

/* --- a draw's geometry ----------------------------------------------------------------------- */

static uint32_t *g_vertex_words, *g_index_words, *g_slots;
static size_t g_vertex_cap, g_index_cap, g_slot_cap;

static int grow(uint32_t **p, size_t *cap, size_t want) {
    if (want <= *cap) return 1;
    size_t n = *cap ? *cap : 4096;
    while (n < want) n *= 2;
    uint32_t *grown = realloc(*p, n * sizeof(uint32_t));
    if (!grown) return 0;
    *p = grown;
    *cap = n;
    return 1;
}

static uint32_t index_at(const uint8_t *data, unsigned size, uint32_t i) {
    return size == 2 ? (uint32_t)(data[2 * i] | data[2 * i + 1] << 8)
                     : (uint32_t)data[4 * i] | (uint32_t)data[4 * i + 1] << 8 |
                       (uint32_t)data[4 * i + 2] << 16 | (uint32_t)data[4 * i + 3] << 24;
}

/* Each distinct vertex once, as the words its program's registers would
 * hold, and the indices renumbered to match: a fan turned into single
 * triangles, which is all the card has. */
static int geometry(const queued *q, uint32_t pa_words, uint32_t *vertices, uint32_t *indices, int *strip) {
    const uint8_t *data = vita_mem_ptr(q->indices, q->count * q->index_size);
    uint32_t low = 0xFFFFFFFFu, high = 0, count = q->count, used = 0, made = 0;
    if (!data) return 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t v = index_at(data, q->index_size, i);
        if (v < low) low = v;
        if (v > high) high = v;
    }
    uint32_t range = high - low + 1;
    if (range > 0x1000000u || !grow(&g_slots, &g_slot_cap, range) || !grow(&g_index_words, &g_index_cap, (size_t)count * 3) ||
        !grow(&g_vertex_words, &g_vertex_cap, (size_t)(range < count ? range : count) * (pa_words ? pa_words : 1))) return 0;
    memset(g_slots, 0xFF, (size_t)range * sizeof(*g_slots));
    for (uint32_t i = 0; i < count; i++) {
        uint32_t v = index_at(data, q->index_size, i);
        if (g_slots[v - low] != 0xFFFFFFFFu) continue;
        uint32_t pa[USSE_WORDS];
        memset(pa, 0, sizeof(pa));
        gxm_draw_vertex_words(&q->d, v, 0, pa);
        if (pa_words) memcpy(g_vertex_words + (size_t)used * pa_words, pa, (size_t)pa_words * sizeof(uint32_t));
        g_slots[v - low] = used++;
    }
    *strip = q->primitive == GXM_TRIANGLE_STRIP;
    if (q->primitive == GXM_TRIANGLE_FAN) {
        uint32_t first = g_slots[index_at(data, q->index_size, 0) - low];
        for (uint32_t i = 2; i < count; i++) {
            g_index_words[made++] = first;
            g_index_words[made++] = g_slots[index_at(data, q->index_size, i - 1) - low];
            g_index_words[made++] = g_slots[index_at(data, q->index_size, i) - low];
        }
    } else {
        uint32_t whole = *strip ? count : count - count % 3;
        for (uint32_t i = 0; i < whole; i++) g_index_words[made++] = g_slots[index_at(data, q->index_size, i) - low];
    }
    *vertices = used;
    *indices = made;
    return made >= 3;
}

/* --- geometry kept ----------------------------------------------------------------------------
 * Most of what a title draws it drew the frame before from the same
 * bytes: a wall is a wall, and a character's bones move in the uniforms,
 * not in the vertices. So what `geometry` makes of a draw is kept on the
 * card, found again by where the draw's indices and streams lie and how
 * they are read, and trusted for as long as a hash of the indices and of
 * the vertices they reach is what it was. A draw whose bytes keep changing,
 * a sheet of particles written anew each frame, is noticed and no longer
 * kept: making a mesh costs more than using one once. */

typedef struct {
    uint8_t  key[16 * 16 + 24];             /* how the draw reads its streams, where its indices are, how many */
    uint64_t indices_stamp, vertices_stamp;
    uint32_t low, high;                     /* the vertices the indices reach */
    uint32_t moment;                        /* when its bytes were last read (mem.h) */
    gpu_mesh *mesh;
    uint32_t bytes, used;
    uint8_t  strip, held, changes;          /* changes: times running its bytes were found different */
} mesh_slot;

#define MESHES 8192
#define MESH_BYTES (256u << 20)             /* what all of them together may take of the card */
#define RESTLESS 3                          /* changes after which a draw's geometry is made each time and not kept */
static mesh_slot g_meshes[MESHES];
static uint64_t g_mesh_bytes;

/* A hash of the vertices the indices reach, over the streams' records: each run of bytes once, however many
 * attributes are read from it. With `since` not NULL nothing is hashed: the answer is whether any of those bytes
 * may have been written since that moment. */
static uint64_t vertices_stamp(const draw *d, uint32_t low, uint32_t high, const uint32_t *since) {
    uint32_t lo[16], hi[16], n = 0;
    uint64_t h = 0;
    for (uint32_t i = 0; i < d->inputs; i++) {
        uint32_t stride = d->in[i].stride, a = d->in[i].base + (d->in[i].per_instance ? 0 : low * stride);
        uint32_t b = d->in[i].base + (d->in[i].per_instance ? 0 : high * stride) + (stride ? stride : 64);
        uint32_t merged = 0;
        for (uint32_t k = 0; k < n && !merged; k++)
            if (a <= hi[k] && b >= lo[k]) { if (a < lo[k]) lo[k] = a; if (b > hi[k]) hi[k] = b; merged = 1; }
        if (!merged) { lo[n] = a; hi[n] = b; n++; }
    }
    for (uint32_t k = 0; k < n; k++) {
        uint32_t avail = vita_mem_avail(lo[k]), bytes = hi[k] - lo[k];
        const uint8_t *p = avail ? vita_mem_ptr(lo[k], 1) : NULL;
        if (bytes > avail) bytes = avail;
        if (p && since) { if (vita_mem_written(p, bytes, *since)) return 1; }
        else if (p) h = fold(h ^ lo[k], p, bytes);
    }
    return since ? 0 : h;
}

/* The draw's geometry on the card: kept from before, or made now and kept;
 * or NULL, and then `vertices`, `indices` and `strip` are as `geometry`
 * left them, to be drawn from once. 0 when there is nothing to draw. */
static int mesh_for(const queued *q, uint32_t pa_words, gpu_mesh **mesh, uint32_t *vertices, uint32_t *indices, int *strip) {
    const draw *d = &q->d;
    uint8_t key[sizeof(g_meshes[0].key)];
    uint32_t index_bytes = q->count * q->index_size, tail[6] = { q->indices, q->count, q->index_size, q->primitive, pa_words, d->inputs };
    const uint8_t *index_data = vita_mem_ptr(q->indices, index_bytes);
    *mesh = NULL;
    if (!index_data) return 0;
    memset(key, 0, sizeof(key));
    memcpy(key, d->in, d->inputs * sizeof(d->in[0]));
    memcpy(key + 16 * 16, tail, sizeof(tail));
    uint64_t h = fold(0, key, sizeof(key));
    mesh_slot *slot = NULL;
    for (unsigned tries = 0, at = (unsigned)(h % MESHES); tries < 16; tries++, at = (at + 1) % MESHES) {
        mesh_slot *m = &g_meshes[at];
        if (m->held && memcmp(m->key, key, sizeof(key))) continue;
        slot = m;
        break;
    }
    if (!slot) return geometry(q, pa_words, vertices, indices, strip);         /* a crowded corner of the table: made, not kept */
    if (slot->held && slot->changes >= RESTLESS) {
        g_mesh_passed++;
        slot->used = g_list;
        return geometry(q, pa_words, vertices, indices, strip);
    }
    /* bytes not written since they were last read are what they were, and are not read again */
    if (slot->held && slot->mesh && !vita_mem_written(index_data, index_bytes, slot->moment) &&
        !vertices_stamp(d, slot->low, slot->high, &slot->moment)) {
        slot->used = g_list;
        *mesh = slot->mesh;
        *strip = slot->strip;
        g_mesh_hits++;
        return 1;
    }
    uint64_t indices_stamp = fold(0, index_data, index_bytes);
    if (slot->held && slot->mesh && slot->indices_stamp == indices_stamp && slot->vertices_stamp == vertices_stamp(d, slot->low, slot->high, NULL)) {
        slot->used = g_list;
        slot->moment = g_now;
        slot->changes = 0;
        *mesh = slot->mesh;
        *strip = slot->strip;
        g_mesh_hits++;
        return 1;
    }
    /* made anew */
    if (!geometry(q, pa_words, vertices, indices, strip)) return 0;
    uint32_t low = 0xFFFFFFFFu, high = 0;
    for (uint32_t i = 0; i < q->count; i++) {
        uint32_t v = index_at(index_data, q->index_size, i);
        if (v < low) low = v;
        if (v > high) high = v;
    }
    if (slot->held) {
        if (slot->changes < 255) slot->changes++;
        gpu_mesh_free(slot->mesh);
        g_mesh_bytes -= slot->bytes;
        slot->mesh = NULL;
        slot->bytes = 0;
        if (slot->changes >= RESTLESS) { slot->used = g_list; return 1; }
    } else {
        memcpy(slot->key, key, sizeof(key));
        slot->held = 1;
        slot->changes = 0;
    }
    slot->used = g_list;
    slot->indices_stamp = indices_stamp;
    slot->low = low;
    slot->high = high;
    slot->vertices_stamp = vertices_stamp(d, low, high, NULL);
    slot->moment = g_now;
    slot->strip = (uint8_t)*strip;
    slot->bytes = *vertices * pa_words * 4 + *indices * 4;
    if (g_mesh_bytes + slot->bytes > MESH_BYTES) {
        /* room: what has not been drawn for longest goes, a tenth of the table at a time */
        for (unsigned round = 0; round < 8 && g_mesh_bytes + slot->bytes > MESH_BYTES; round++) {
            uint32_t oldest = 0xFFFFFFFFu;
            for (unsigned i = 0; i < MESHES; i++) if (g_meshes[i].mesh && g_meshes[i].used < oldest) oldest = g_meshes[i].used;
            uint32_t until = oldest + (g_list - oldest) / 4;
            for (unsigned i = 0; i < MESHES; i++) {
                mesh_slot *m = &g_meshes[i];
                if (!m->mesh || m == slot || m->used > until) continue;
                gpu_mesh_free(m->mesh);
                g_mesh_bytes -= m->bytes;
                m->mesh = NULL;
                m->bytes = 0;
                m->held = 0;
            }
        }
    }
    slot->mesh = gpu_mesh_make(g_vertex_words, *vertices, pa_words * 4, g_index_words, *indices);
    if (!slot->mesh) { slot->bytes = 0; return 1; }           /* drawn from what geometry left, this once */
    g_mesh_bytes += slot->bytes;
    g_mesh_made++;
    *mesh = slot->mesh;
    return 1;
}

/* --- the constants, and the memory a load instruction reads ---------------------------------- */

typedef struct {
    uint32_t sa[USSE_WORDS];
    float    viewport[12];                  /* scale, offset, two over the target's width and height */
    uint32_t memory[8][4];                  /* first address, bytes, first word in the buffer */
} constants;

#define REACH 16384u                        /* bytes of a buffer a load instruction is given */

static uint32_t g_memory[2][8 * REACH / 4];

static uint32_t memory_for(const gxp_program *p, const uint32_t buffers[GXP_BUFFERS], constants *c, uint32_t *words) {
    uint32_t made = 0, ranges = 0;
    for (uint32_t i = 0; i < p->base_count && ranges < 8; i++) {
        const gxp_base *b = &p->bases[i];
        uint32_t address = b->buffer < GXP_BUFFERS ? buffers[b->buffer] : 0, avail = address ? vita_mem_avail(address) : 0;
        int seen = 0;
        if (!avail) continue;
        for (uint32_t k = 0; k < ranges; k++) if (c->memory[k][0] == address) seen = 1;
        if (seen) continue;
        uint32_t bytes = (avail < REACH ? avail : REACH) & ~3u;
        const void *src = vita_mem_ptr(address, bytes);
        if (!src || !bytes) continue;
        memcpy(words + made, src, bytes);
        c->memory[ranges][0] = address;
        c->memory[ranges][1] = bytes;
        c->memory[ranges][2] = made;
        made += bytes / 4;
        ranges++;
    }
    return made;
}

/* --- a list ---------------------------------------------------------------------------------- */

typedef struct {
    gpu_shader *vertex, *fragment;
    int reads_pixel, blended;
    unsigned raw;                           /* words of a raw result, for the mask */
    gpu_texture *textures[2][GXP_TEXTURE_UNITS];
    uint32_t kept_larger[2];                /* a bit a unit: its texture is a surface the card keeps, at the size the card keeps it */
} programs;

static unsigned larger(void);
static int packed_in_surface(const gxm_tex *t);

/* The two shaders of a draw, for what is bound with it. 0 when either is
 * one the card's path has nothing for. */
static int shaders_for(const queued *q, unsigned target_raw, unsigned target_format, programs *out) {
    const gxm_draw_state *s = &q->s;
    const gxm_program *vp = s->vertex, *fp = s->fragment;
    gxp_hlsl_bind bind;
    memset(out, 0, sizeof(*out));

    uint32_t slots = 0;
    for (unsigned slot = 0; slot < GXP_SLOT_POSITION; slot++)
        if (vp->gxp.out_count[slot]) slots |= 1u << slot;

    memset(&bind, 0, sizeof(bind));
    bind.slots = slots;
    bind.pa_words = (vp->words[1] + 3u) & ~3u;
    if (bind.pa_words > 64) bind.pa_words = 64;
    for (unsigned unit = 0; unit < GXP_TEXTURE_UNITS; unit++) {
        const gxm_tex *t = q->d.v.tex[unit];
        if (!t) continue;
        bind.tex_components[unit] = t->pf.components;
        bind.tex_type[unit] = t->pf.type;
        bind.tex_cube[unit] = t->cube;
        bind.tex_wrap[unit] = (uint8_t)((t->u_mode & 15) | (t->v_mode << 4));
        bind.tex_point[unit] = point_read(t);
    }
    out->vertex = shader_for(vp, &bind);
    if (!out->vertex) return 0;

    /* a face whose program is off draws depth and stencil only */
    if (!fp || s->face[0].program_off) return 1;
    if (!s->has_colour && !(fp->gxp.flags & GXP_FLAG_DISCARD)) return 1;

    memset(&bind, 0, sizeof(bind));
    bind.slots = slots;
    for (unsigned unit = 0; unit < GXP_TEXTURE_UNITS; unit++) {
        const gxm_tex *t = q->d.f.tex[unit];
        if (!t) continue;
        bind.tex_components[unit] = t->pf.components;
        bind.tex_type[unit] = t->pf.type;
        bind.tex_cube[unit] = t->cube;
        bind.tex_wrap[unit] = (uint8_t)((t->u_mode & 15) | (t->v_mode << 4));
        bind.tex_point[unit] = point_read(t);
        if (larger() > 1) bind.tex_locked[unit] = (uint8_t)packed_in_surface(t);
    }
    if (larger() > 1) bind.larger = (uint8_t)larger();
    unsigned raw = s->has_colour ? raw_result_words(s, &fp->gxp) : 0;
    if (s->has_colour && (raw != 0) != (target_raw != 0))
        return refuse(raw ? "a result of words drawn into a surface kept as colours (program %016llx)"
                          : "a colour drawn into a surface kept as words (program %016llx)", (unsigned long long)fp->name);
    bind.result_words = (uint8_t)(raw > target_raw ? target_raw : raw);
    bind.clamp = fp->output_format == 1 && !raw;
    if ((fp->gxp.flags & GXP_FLAG_FRAGCOLOR) && s->has_colour) {
        bind.now = target_raw ? 2 : 1;
        bind.now_words = (uint8_t)target_raw;
        out->reads_pixel = 1;
        /* The target itself, in the order the triangles come, where the card
         * can: a draw whose own triangles lie over each other adds each to
         * what the last left, as the console does. Not a program that can
         * discard, whose depth has to wait for it; nor one the fixed blend
         * follows, which needs the target at the output. */
        int blended = !(fp->gxp.flags & GXP_FLAG_NATIVECOLOR) && ((fp->blend[1] & 15) || (fp->blend[1] >> 4));
        if (!target_raw && !blended && !(fp->gxp.flags & GXP_FLAG_DISCARD) && gpu_ordered(target_format)) {
            bind.ordered = 1;
            out->reads_pixel = 2;
        }
    }
    out->raw = bind.result_words;
    out->fragment = shader_for(fp, &bind);
    return out->fragment != NULL;
}

/* --- the target, to the card and back ---------------------------------------------------------
 * A colour surface reaches the card one of three ways. Where the card has
 * the same layout of a pixel, the pixel's bytes go as they are: four bytes
 * of colour, four halves. A surface the programs write whole words into
 * goes as words. Anything else, ten-bit floats, a single float, goes as
 * four floats a pixel, decoded on the way and encoded on the way back, and
 * on the display's curve or off it as the surface says: nothing is lost by
 * it that the surface could have kept. In every case the pixels are put in
 * rows first, whatever order the surface keeps them in. */

enum { AS_BYTES = 1, AS_FLOATS };

typedef struct {
    int      mode;                          /* 0: no colour */
    unsigned format;                        /* the card's */
    unsigned raw;                           /* words a pixel, when the surface is kept as words */
    uint32_t bytes;                         /* of a pixel on the card */
} colour_way;

static void colour_way_of(const gxm_draw_state *s, colour_way *way) {
    memset(way, 0, sizeof(*way));
    if (!s->has_colour) return;
    way->format = colour_format(s, &way->raw);
    if (way->format != GPU_NONE) {
        way->mode = AS_BYTES;
        way->bytes = s->colour.bytes;
    } else if (!s->colour.raw) {
        way->mode = AS_FLOATS;
        way->format = GPU_RGBA32F;
        way->bytes = 16;
    }
}

static const uint8_t *pixel_at(const gxm_draw_state *s, uint32_t x, uint32_t y) {
    uint32_t at = gxm_pixel_offset(s->colour_layout, x, y, s->width, s->height, s->colour_stride, s->colour.bytes);
    return at + s->colour.bytes <= s->colour_avail ? s->colour_data + at : NULL;
}

/* The surface as rows for the card. */
static void colour_in(const gxm_draw_state *s, const colour_way *way, uint8_t *out) {
    for (uint32_t y = 0; y < s->height; y++)
        for (uint32_t x = 0; x < s->width; x++, out += way->bytes) {
            const uint8_t *px = pixel_at(s, x, y);
            if (way->mode == AS_BYTES) {
                if (px) memcpy(out, px, way->bytes); else memset(out, 0, way->bytes);
                continue;
            }
            float c[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            if (px) pixfmt_decode(&s->colour, px, c);
            if (s->colour_gamma) for (int k = 0; k < 3; k++) c[k] = pix_srgb_to_linear(c[k]);
            memcpy(out, c, sizeof(c));
        }
}

/* And the card's rows back into it. */
static void colour_out(const gxm_draw_state *s, const colour_way *way, const uint8_t *in) {
    for (uint32_t y = 0; y < s->height; y++)
        for (uint32_t x = 0; x < s->width; x++, in += way->bytes) {
            uint8_t *px = (uint8_t *)pixel_at(s, x, y);
            if (!px) continue;
            if (way->mode == AS_BYTES) { memcpy(px, in, way->bytes); continue; }
            float c[4];
            memcpy(c, in, sizeof(c));
            if (s->colour_gamma) for (int k = 0; k < 3; k++) c[k] = pix_linear_to_srgb(c[k]);
            pixfmt_encode(&s->colour, px, c, 15);
        }
}

/* What gxm_gpu_list drew aside, for gxm_gpu_compare. */
static struct {
    int      held;
    uint32_t draws;
    uint32_t width, height, depth_bytes;
    unsigned depth_format;
    colour_way way;
    uint8_t *colour, *depth;
    size_t   colour_cap, depth_cap;
} g_aside;

/* While lists are checked, each begins on the card from what the processor
 * has, and so a draw that passes only where the depth equals what an
 * earlier draw left would be held to the processor's arithmetic, which the
 * card's never equals to the last bit. So the card's own depth and stencil
 * are kept beside the processor's as a checked list left both, and a list
 * that finds the processor's standing as one of those begins from the
 * card's that went with it: as it would were nothing being checked. They
 * are found by what they hold and not by where they are, because every
 * scene's depth is worked on in the same planes and lives in the title's
 * own surface between scenes. */
typedef struct {
    uint32_t width, height, used;           /* used: the list that left them, 0 for none yet */
    unsigned format;
    float   *depth_then;
    uint8_t *stencil_then, *card;
    size_t   cap, card_cap;
} kept_depth;

static kept_depth g_kept[8];
static kept_depth *g_kept_from;             /* the one the list in hand began from, or NULL */

static kept_depth *kept_holding(const gxm_draw_state *s, unsigned format) {
    size_t pixels = (size_t)s->width * s->height;
    for (int i = 0; i < 8; i++) {
        kept_depth *k = &g_kept[i];
        if (k->used && k->width == s->width && k->height == s->height && k->format == format &&
            !memcmp(k->depth_then, s->depth, pixels * sizeof(float)) && !memcmp(k->stencil_then, s->stencil, pixels)) return k;
    }
    return NULL;
}

/* Both as they stand after a list: in the place of what the list began
 * from, which nothing will find again, or of whatever was left longest ago. */
static void keep_depth(const gxm_draw_state *s, uint32_t list) {
    size_t pixels = (size_t)s->width * s->height, card_bytes = pixels * g_aside.depth_bytes;
    kept_depth *k = g_kept_from;
    if (!k) {
        k = &g_kept[0];
        for (int i = 1; i < 8; i++) if (g_kept[i].used < k->used) k = &g_kept[i];
    }
    g_kept_from = NULL;
    k->used = 0;
    if (pixels > k->cap) {
        float *d = realloc(k->depth_then, pixels * sizeof(float));
        if (d) k->depth_then = d;
        uint8_t *t = realloc(k->stencil_then, pixels);
        if (t) k->stencil_then = t;
        if (!d || !t) return;
        k->cap = pixels;
    }
    if (card_bytes > k->card_cap) {
        uint8_t *c = realloc(k->card, card_bytes);
        if (!c) return;
        k->card = c;
        k->card_cap = card_bytes;
    }
    memcpy(k->depth_then, s->depth, pixels * sizeof(float));
    memcpy(k->stencil_then, s->stencil, pixels);
    memcpy(k->card, g_aside.depth, card_bytes);
    k->width = s->width; k->height = s->height;
    k->format = g_aside.depth_format;
    k->used = list;
}

static uint8_t *g_colour_rows, *g_depth_rows;
static size_t g_colour_rows_cap, g_depth_rows_cap;

static int bytes_for(uint8_t **p, size_t *cap, size_t want) {
    if (want <= *cap) return 1;
    uint8_t *grown = realloc(*p, want);
    if (!grown) return 0;
    *p = grown;
    *cap = want;
    return 1;
}

/* --- larger than the title drew ----------------------------------------------------------------
 *
 * VITARECOMP_GXM_GPU_SCALE=n, n from 2 to 8: every surface the card keeps
 * is n times as wide and as high as the title made it, and a list is
 * drawn into it at that size. Nothing the title says changes: its
 * viewports, its textures' coordinates and its programs are in fractions
 * of a surface, and a program that reads the pixel it writes reads the
 * one it is at. What is in the title's own units is put right where it
 * meets the card: a clip rectangle, the count of pixels a draw passed, and
 * a surface's rows on their way from memory (each pixel n by n of the
 * card's) or to it (a pixel from the n by n it has become: their mean
 * where the form is one of numbers that can be averaged, the middle one
 * where it is not). A surface read as a texture stays the larger size
 * (view_text).
 *
 * Not while the card is checked against the processor: that renderer draws
 * at the title's size, and the title's size is what is compared. */
static unsigned larger(void) {
    static unsigned n;
    if (!n) {
        const char *e = getenv("VITARECOMP_GXM_GPU_SCALE");
        unsigned long asked = e ? strtoul(e, NULL, 10) : 1;
        n = asked < 1 ? 1 : asked > 8 ? 8 : (unsigned)asked;
        if (g_mode == GXM_GPU_CHECK) n = 1;
        if (n > 1) fprintf(stderr, "gxm gpu: surfaces are %u times the size the title makes them\n", n);
    }
    return n;
}

static uint8_t *g_large_rows;
static size_t g_large_rows_cap;

static gpu_surface *surface_make(unsigned format, uint32_t width, uint32_t height) {
    return gpu_surface_make(format, width * larger(), height * larger());
}

/* Rows as the title has them, `bytes` a pixel and no gaps, into a surface. */
static int surface_put(gpu_surface *to, const uint8_t *rows, uint32_t width, uint32_t height, uint32_t bytes) {
    unsigned n = larger();
    size_t row = (size_t)width * bytes, large_row = row * n;
    if (n == 1) return gpu_surface_put(to, rows, (uint32_t)row);
    if (!bytes_for(&g_large_rows, &g_large_rows_cap, large_row * height * n)) return -1;
    for (uint32_t y = 0; y < height; y++) {
        uint8_t *first = g_large_rows + (size_t)y * n * large_row, *out = first;
        const uint8_t *in = rows + (size_t)y * row;
        for (uint32_t x = 0; x < width; x++, in += bytes)
            for (unsigned k = 0; k < n; k++, out += bytes) memcpy(out, in, bytes);
        for (unsigned k = 1; k < n; k++) memcpy(first + k * large_row, first, large_row);
    }
    return gpu_surface_put(to, g_large_rows, (uint32_t)large_row);
}

/* And a surface into such rows. */
static int surface_get(gpu_surface *from, uint8_t *rows, uint32_t width, uint32_t height, uint32_t bytes, unsigned format) {
    unsigned n = larger(), middle = n / 2;
    size_t row = (size_t)width * bytes, large_row = row * n;
    int as_bytes = format == GPU_RGBA8 || format == GPU_RGBA8_SRGB || format == GPU_BGRA8 || format == GPU_BGRA8_SRGB;
    if (n == 1) return gpu_surface_get(from, rows, (uint32_t)row);
    if (!bytes_for(&g_large_rows, &g_large_rows_cap, large_row * height * n) || gpu_surface_get(from, g_large_rows, (uint32_t)large_row)) return -1;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *in = g_large_rows + (size_t)y * n * large_row;
        uint8_t *out = rows + (size_t)y * row;
        for (uint32_t x = 0; x < width; x++, in += (size_t)bytes * n, out += bytes) {
            if (as_bytes) {
                uint32_t sum[4] = { 0, 0, 0, 0 };
                for (unsigned j = 0; j < n; j++)
                    for (unsigned i = 0; i < n; i++)
                        for (unsigned c = 0; c < 4; c++) sum[c] += in[j * large_row + (size_t)i * 4 + c];
                for (unsigned c = 0; c < 4; c++) out[c] = (uint8_t)((sum[c] + n * n / 2) / (n * n));
            } else if (format == GPU_RGBA32F) {
                float sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, one[4];
                for (unsigned j = 0; j < n; j++)
                    for (unsigned i = 0; i < n; i++) {
                        memcpy(one, in + j * large_row + (size_t)i * 16, 16);
                        for (unsigned c = 0; c < 4; c++) sum[c] += one[c];
                    }
                for (unsigned c = 0; c < 4; c++) sum[c] /= (float)(n * n);
                memcpy(out, sum, 16);
            } else {
                memcpy(out, in + middle * large_row + (size_t)middle * bytes, bytes);
            }
        }
    }
    return 0;
}

/* The scene's depth and stencil planes as the card's depth texture lies. */
static void pack_depth(const gxm_draw_state *s, unsigned format, uint8_t *out) {
    size_t pixels = (size_t)s->width * s->height;
    if (format == GPU_D24S8) {
        for (size_t i = 0; i < pixels; i++) {
            double scaled = (double)s->depth[i] * 16777215.0 + 0.5;
            uint32_t w = (scaled >= 16777215.0 ? 0xFFFFFFu : scaled > 0.0 ? (uint32_t)scaled : 0u) | ((uint32_t)s->stencil[i] << 24);
            memcpy(out + i * 4, &w, 4);
        }
    } else if (format == GPU_D16) {
        for (size_t i = 0; i < pixels; i++) {
            float f = s->depth[i];
            uint16_t w = (uint16_t)(f >= 1.0f ? 0xFFFFu : f > 0.0f ? (uint32_t)(f * 65535.0f + 0.5f) : 0u);
            memcpy(out + i * 2, &w, 2);
        }
    } else {
        for (size_t i = 0; i < pixels; i++) {
            uint32_t w = s->stencil[i];
            memcpy(out + i * 8, &s->depth[i], 4);
            memcpy(out + i * 8 + 4, &w, 4);
        }
    }
}

static void unpack_depth(const uint8_t *in, unsigned format, uint32_t width, uint32_t height, float *depth, uint8_t *stencil) {
    size_t pixels = (size_t)width * height;
    if (format == GPU_D24S8) {
        for (size_t i = 0; i < pixels; i++) {
            uint32_t w;
            memcpy(&w, in + i * 4, 4);
            depth[i] = (float)((double)(w & 0xFFFFFFu) / 16777215.0);
            stencil[i] = (uint8_t)(w >> 24);
        }
    } else if (format == GPU_D16) {
        /* no stencil beside it: the stencil plane is left as it is */
        for (size_t i = 0; i < pixels; i++) {
            uint16_t w;
            memcpy(&w, in + i * 2, 2);
            depth[i] = (float)((double)w / 65535.0);
        }
    } else {
        for (size_t i = 0; i < pixels; i++) {
            memcpy(&depth[i], in + i * 8, 4);
            stencil[i] = in[i * 8 + 4];
        }
    }
}

/* Surfaces on the card to draw a list in, one of each form and size met. */
typedef struct { unsigned format; uint32_t width, height; gpu_surface *surface; } spare;

static gpu_surface *spare_for(unsigned format, uint32_t width, uint32_t height) {
    static spare spares[32];
    static unsigned count;
    for (unsigned i = 0; i < count; i++)
        if (spares[i].format == format && spares[i].width == width && spares[i].height == height) return spares[i].surface;
    gpu_surface *made = surface_make(format, width, height);
    if (!made) return NULL;
    if (count == 32) {                              /* full: the oldest goes */
        gpu_surface_free(spares[0].surface);
        memmove(spares, spares + 1, 31 * sizeof(spares[0]));
        count--;
    }
    spares[count].format = format;
    spares[count].width = width;
    spares[count].height = height;
    spares[count++].surface = made;
    return made;
}

/* --- the title's surfaces, kept on the card ---------------------------------------------------
 * A list drawn by the card leaves its target on the card, and the title's
 * memory is not told: the next list that draws to the surface, or samples
 * it, finds it where it is. The bytes go back into memory when something
 * off the card wants them, a list the processor draws, the display, and
 * not before. So for each surface it is known which of the two holds what
 * the surface is: the card (`newer`), or memory, and if memory, whether
 * the card still agrees with it (by a hash of the bytes, which the title
 * may write whenever it likes).
 *
 * VITARECOMP_GXM_GPU_KEEP=0 sends every surface back at the end of every
 * list, as if none were kept: the same arithmetic by the same card, by the
 * long road, for telling a fault in the keeping from a fault in the
 * drawing. */

static int keeping(void) {
    static int keep = -1;
    if (keep < 0) { const char *e = getenv("VITARECOMP_GXM_GPU_KEEP"); keep = !(e && *e == '0'); }
    return keep;
}

typedef struct {
    gxm_draw_state s;                       /* the surface as a scene described it: where, how large, of what */
    colour_way way;
    const uint8_t *lo, *hi;                 /* the memory it lies in */
    gpu_surface *card;
    int      newer;                         /* the card holds what the title's memory does not yet */
    int      agreed;                        /* otherwise: the two held the same when `stamp` was taken of memory, */
    uint64_t stamp;
    uint32_t moment;                        /* at this moment (mem.h) */
    uint32_t version;                       /* counts the lists drawn to it: a texture made from it is made from one of them */
    uint32_t drawn;                         /* the list that drew to it last: of two laid over each other in memory, which is on top */
    uint32_t since;                         /* the moment (mem.h) after which a write to its memory is the title's, and not this path's */
    uint32_t used;
} colour_record;

/* A depth surface of the title's likewise. A scene works on its depth in
 * the renderer's planes or on the card, and the surface is the scene's
 * depth only once the scene has ended and said it keeps it. */
typedef struct {
    uint32_t control[5];                    /* the surface, as the scene that last used it described it */
    uint32_t width, height, row_bytes;
    const uint8_t *lo, *hi;                 /* its depth, in memory */
    unsigned format;                        /* the card's */
    gpu_surface *card;
    int      newer;
    uint32_t version, used;
} depth_record;

#define SMALL_BYTES 8192u                   /* a surface of no more is never left behind in memory */
#define COLOURS 48
#define DEPTHS 16
static colour_record g_colours[COLOURS];
static depth_record g_depths[DEPTHS];
static unsigned g_colour_count, g_depth_count;

static uint32_t next_power(uint32_t v) { uint32_t p = 1; while (p < v) p <<= 1; return p; }

static void colour_extent(const gxm_draw_state *s, const uint8_t **lo, const uint8_t **hi) {
    uint64_t bytes;
    if (s->colour_layout == GXM_SURFACE_LINEAR) bytes = (uint64_t)s->colour_stride * s->height * s->colour.bytes;
    else if (s->colour_layout == GXM_SURFACE_TILED) bytes = (uint64_t)((s->width + 31) & ~31u) * ((s->height + 31) & ~31u) * s->colour.bytes;
    else bytes = (uint64_t)next_power(s->width) * next_power(s->height) * s->colour.bytes;
    if (bytes > s->colour_avail) bytes = s->colour_avail;
    *lo = s->colour_data;
    *hi = s->colour_data + bytes;
}

/* The card's surface into the title's memory, if the card's is the newer. */
static void forget_views_of(const void *record);

/* A surface the card holds newer than memory is the surface for as long as
 * the title leaves that memory alone. A title lets go of a target and puts
 * its next textures where it lay, or clears one with a loop of its own;
 * from then on what is in memory is newer than what the card holds, and
 * the card's must neither be written back over it nor read in its place.
 * The letters of a place newly come to were once the last place's frame,
 * written back over them when they were first drawn with.
 *
 * So each surface keeps the moment it was last drawn, and one whose memory
 * has been written since is let go of: of all the card holds newer, before
 * a list is made ready. Where writes are not known (mem.h) the question
 * always answers yes, and is not asked. */
static void stale_surfaces(void) {
    for (unsigned i = 0; i < g_colour_count; i++) {
        colour_record *r = &g_colours[i];
        if (!r->newer || vita_mem_written(r->lo, 0, 0)) continue;           /* nothing known of writes: nothing to go by */
        if (!vita_mem_written(r->lo, (size_t)(r->hi - r->lo), r->since)) continue;
        if (logging())
            fprintf(stderr, "gxm gpu: list %u: a colour surface of %ux%u is let go: the title has written where it lay\n", g_list, r->s.width,
                    r->s.height);
        r->newer = 0;
        r->agreed = 0;
        forget_views_of(r);
    }
}

/* This path's own writing into memory is not the title's: the surfaces the
 * card still holds newer over the same bytes are from this moment on. */
static void written_here(const colour_record *by) {
    uint32_t now = vita_mem_moment();
    for (unsigned i = 0; i < g_colour_count; i++) {
        colour_record *o = &g_colours[i];
        if (o != by && o->newer && o->lo < by->hi && o->hi > by->lo) o->since = now;
    }
}

static int colour_back(colour_record *r) {
    if (!r->newer) return 1;
    size_t pixels = (size_t)r->s.width * r->s.height;
    uint64_t began = now_ns();
    if (!bytes_for(&g_colour_rows, &g_colour_rows_cap, pixels * r->way.bytes + 16) ||
        surface_get(r->card, g_colour_rows, r->s.width, r->s.height, r->way.bytes, r->way.format)) return 0;
    uint64_t at = now_ns();
    g_ns[P_BACK] += at - began;
    colour_out(&r->s, &r->way, g_colour_rows);
    if (logging())
        fprintf(stderr, "gxm gpu: list %u: a colour surface of %ux%u, %u bytes a pixel, goes back to memory\n", g_list, r->s.width, r->s.height,
                r->s.colour.bytes);
    r->newer = 0;
    r->agreed = 1;
    r->stamp = fold(0, r->lo, (size_t)(r->hi - r->lo));
    r->moment = g_now;                              /* its own writing is after that moment: the bytes are hashed once more, then no more */
    written_here(r);
    g_ns[P_OUT] += now_ns() - at;
    g_backs++;
    return 1;
}

static void depth_back(depth_record *d);

/* Every surface the card holds newer than the memory between `lo` and `hi`
 * goes back into it; all of them when `lo` is NULL. Two that lie over each
 * other in memory go in the order they were drawn, so that what was drawn
 * last is what memory ends with. */
static void surfaces_back(const uint8_t *lo, const uint8_t *hi, const colour_record *but) {
    stale_surfaces();
    for (;;) {
        colour_record *first = NULL;
        for (unsigned i = 0; i < g_colour_count; i++) {
            colour_record *r = &g_colours[i];
            if (r != but && r->newer && (!lo || (r->lo < hi && r->hi > lo)) && (!first || r->drawn < first->drawn)) first = r;
        }
        if (!first || !colour_back(first)) break;
    }
    for (unsigned i = 0; i < g_depth_count; i++) {
        depth_record *d = &g_depths[i];
        if (d->newer && (!lo || (d->lo < hi && d->hi > lo))) depth_back(d);
    }
}

void gxm_gpu_memory_wanted(const void *bytes, size_t count) {
    if (g_mode != GXM_GPU_DRAW || !bytes) return;
    surfaces_back(bytes, (const uint8_t *)bytes + count, NULL);
}

int gxm_gpu_show(void *window, const void *bytes, uint32_t width, uint32_t height, uint32_t pitch) {
    static struct { gpu_surface *card; const uint8_t *lo; uint32_t width, height, pitch, moment; int filled; } own;
    static struct { const colour_record *record; uint32_t version; } last;
    static int no_way;
    const uint8_t *lo = (const uint8_t *)bytes, *hi = lo + (size_t)pitch * height * 4;
    /* with no window there is nothing to show, unless frames between are made: those are looked at as files */
    if (gxm_gpu_mode() != GXM_GPU_DRAW || no_way || (!window && !gpu_tween_on())) return 0;
    uint64_t began = now_ns();
    gpu_surface *shown = NULL;
    int fresh = 0;
    if (bytes && width && height) {
        /* the surface the card holds newer than those bytes, when there is one and it is all of them as the display reads them */
        colour_record *top = NULL;
        unsigned over = 0;
        for (unsigned i = 0; i < g_colour_count; i++) {
            colour_record *r = &g_colours[i];
            if (!r->newer || r->lo >= hi || r->hi <= lo) continue;
            over++;
            if (!top || r->drawn > top->drawn) top = r;
        }
        if (over == 1 && top->lo == lo && top->s.width == width && top->s.height == height && top->s.colour_stride == pitch &&
            top->s.colour_layout == GXM_SURFACE_LINEAR && top->way.mode == AS_BYTES &&
            (top->way.format == GPU_RGBA8 || top->way.format == GPU_RGBA8_SRGB)) {
            shown = top->card;
            fresh = last.record != top || last.version != top->version;
            last.record = top;
            last.version = top->version;
        } else {
            /* otherwise memory is what the display reads, with whatever the card holds of it put there first */
            if (over) surfaces_back(lo, hi, NULL);
            if (!own.card || own.width != width || own.height != height) {
                gpu_surface_free(own.card);
                own.card = gpu_surface_make(GPU_RGBA8, width, height);
                own.width = width;
                own.height = height;
                own.filled = 0;
            }
            uint32_t moment = vita_mem_moment();
            if (own.card && (!own.filled || own.lo != lo || own.pitch != pitch || vita_mem_written(lo, (size_t)(hi - lo), own.moment))) {
                own.filled = !gpu_surface_put(own.card, lo, pitch * 4);
                fresh = 1;
            }
            own.lo = lo;
            own.pitch = pitch;
            own.moment = moment;
            shown = own.card && own.filled ? own.card : NULL;
            last.record = NULL;
        }
    }
    if (gpu_show(window, shown, fresh)) {
        no_way = 1;
        fprintf(stderr, "gxm gpu: the card shows nothing in the host's window; the picture goes by way of memory\n");
        return 0;
    }
    g_ns[P_SHOW] += now_ns() - began;
    return 1;
}

static void forget_views_of(const void *record);
static uint32_t rows_of(const gxm_tex *t);

/* Whether a texture's texels are parts of the pixels of a surface the card
 * has met: rows of the same length, and a pixel several texels long. */
static int packed_in_surface(const gxm_tex *t) {
    uint32_t row_bytes = rows_of(t);
    for (unsigned i = 0; i < g_colour_count; i++) {
        const colour_record *r = &g_colours[i];
        if (r->lo <= t->base && t->base < r->hi && r->s.colour_layout == GXM_SURFACE_LINEAR && row_bytes &&
            row_bytes == r->s.colour_stride * r->s.colour.bytes && t->pf.bytes < r->s.colour.bytes) return 1;
    }
    return 0;
}

static colour_record *colour_for(const gxm_draw_state *s, const colour_way *way) {
    colour_record *r = NULL;
    for (unsigned i = 0; i < g_colour_count && !r; i++) {
        colour_record *c = &g_colours[i];
        if (c->s.colour_data == s->colour_data && c->s.width == s->width && c->s.height == s->height &&
            c->s.colour_stride == s->colour_stride && c->s.colour_layout == s->colour_layout && c->s.colour_gamma == s->colour_gamma &&
            !memcmp(&c->s.colour, &s->colour, sizeof(s->colour))) r = c;
    }
    if (!r) {
        if (g_colour_count == COLOURS) {            /* room: the one longest unused goes, into memory first if it must */
            colour_record *oldest = &g_colours[0];
            for (unsigned i = 1; i < COLOURS; i++) if (g_colours[i].used < oldest->used) oldest = &g_colours[i];
            colour_back(oldest);
            forget_views_of(oldest);
            gpu_surface_free(oldest->card);
            *oldest = g_colours[--g_colour_count];
            forget_views_of(&g_colours[g_colour_count]);        /* it has moved: what was made from it is made again */
        }
        r = &g_colours[g_colour_count];
        memset(r, 0, sizeof(*r));
        r->card = surface_make(way->format, s->width, s->height);
        if (!r->card) return NULL;
        g_colour_count++;
        r->s = *s;
        r->way = *way;
    }
    r->s.colour_avail = s->colour_avail;
    colour_extent(&r->s, &r->lo, &r->hi);
    r->used = g_list;
    return r;
}

/* A title draws a half-size picture into the corner of a full-size
 * surface's memory, as a surface of its own with the larger one's rows, and
 * then samples the larger one. The two are one piece of memory, and the
 * card holds them as two surfaces. When it holds the larger one newer than
 * memory, the smaller is drawn through it: fetched from its corner before
 * the list and put back there after, on the card, so that the larger one
 * stays what the memory would be. Returns the larger, or NULL. */
static colour_record *whole_of(const colour_record *r) {
    for (unsigned i = 0; i < g_colour_count; i++) {
        colour_record *w = &g_colours[i];
        if (w != r && w->newer && w->s.colour_data == r->s.colour_data && w->s.colour_stride == r->s.colour_stride &&
            w->s.colour_layout == GXM_SURFACE_LINEAR && r->s.colour_layout == GXM_SURFACE_LINEAR && w->s.colour_gamma == r->s.colour_gamma &&
            w->way.format == r->way.format && !memcmp(&w->s.colour, &r->s.colour, sizeof(r->s.colour)) &&
            w->s.width >= r->s.width && w->s.height >= r->s.height) return w;
    }
    return NULL;
}

/* --- a scene's depth ---------------------------------------------------------------------------
 * The renderer keeps a scene's depth and stencil in planes of its own,
 * made at the scene's start from one value or from the title's depth
 * surface, and written to that surface at its end if the surface says so.
 * The card's lists never look at the planes. So for the scene in hand it
 * is known where its depth is, the planes or the card, and each kind of
 * list fetches it from the other when it finds it there; and a scene that
 * ends on the card with a surface to keep leaves it on the card, as that
 * surface's, with nothing written to memory. */

/* And a third place: the title's memory, not yet read. A scene that loads
 * its depth from a surface the card does not hold would have it fetched
 * from memory before its first draw, and whatever the card holds newer of
 * that memory sent there first, which waits for the card and goes through
 * every pixel twice. A title that keeps one depth buffer for its frame and
 * shrinks its picture beside it, in scenes of other sizes that neither test
 * depth nor write it, pays that three times a frame for depth nothing looks
 * at. So the fetching waits for a list that looks (depth_fetched), and a
 * scene that ends without one leaves its depth where it was, as it was. */
enum { IN_PLANES = 0, ON_CARD, IN_MEMORY };

static struct {
    int      active;
    depth_record *from;                     /* IN_MEMORY: the surface it is loaded from, and that surface's five words */
    uint32_t control[5];
    depth_record *of;                       /* the surface the scene's depth becomes at its end; NULL when it keeps none */
    gpu_surface *card;                      /* where its depth is drawn on the card: the surface's own, or a spare */
    int      where;
    int      cleared;                       /* the planes are one value throughout: the card is cleared, not filled */
    float    clear_depth;
    uint8_t  clear_stencil;
    uint32_t width, height;
    unsigned format;
} g_scene;

static unsigned depth_format_of(uint32_t bits) { return bits == 24 ? GPU_D24S8 : bits == 16 ? GPU_D16 : bits == 0 ? GPU_D32FS8 : GPU_NONE; }
static size_t depth_bytes_of(unsigned format) { return format == GPU_D24S8 ? 4 : format == GPU_D16 ? 2 : 8; }

static depth_record *depth_for(const uint32_t control[5], uint32_t width, uint32_t height, unsigned format) {
    uint32_t stride = (((control[0] >> 3) & 0xFF) + 1) * 32, bytes = format == GPU_D16 ? 2 : 4;
    uint32_t avail = control[1] ? vita_mem_avail(control[1]) : 0;
    const uint8_t *lo = avail ? vita_mem_ptr(control[1], 1) : NULL;
    depth_record *d = NULL;
    if (!lo) return NULL;
    for (unsigned i = 0; i < g_depth_count && !d; i++)
        if (g_depths[i].lo == lo && g_depths[i].width == width && g_depths[i].height == height && g_depths[i].format == format &&
            g_depths[i].row_bytes == stride * bytes) d = &g_depths[i];
    if (!d) {
        if (g_depth_count == DEPTHS) {
            depth_record *oldest = &g_depths[0];
            for (unsigned i = 1; i < DEPTHS; i++) if (g_depths[i].used < oldest->used) oldest = &g_depths[i];
            if (oldest == g_scene.of) return NULL;
            depth_back(oldest);
            forget_views_of(oldest);
            gpu_surface_free(oldest->card);
            if (g_scene.of == &g_depths[g_depth_count - 1]) g_scene.of = oldest;
            *oldest = g_depths[--g_depth_count];
            forget_views_of(&g_depths[g_depth_count]);
        }
        d = &g_depths[g_depth_count];
        memset(d, 0, sizeof(*d));
        d->card = surface_make(format, width, height);
        if (!d->card) return NULL;
        g_depth_count++;
        d->width = width;
        d->height = height;
        d->format = format;
        d->row_bytes = stride * bytes;
        d->lo = lo;
    }
    uint64_t extent = (uint64_t)d->row_bytes * height;
    memcpy(d->control, control, sizeof(d->control));
    d->hi = d->lo + (extent < avail ? extent : avail);
    d->used = g_list;
    return d;
}

/* A depth surface the card holds, into the title's memory: through planes
 * of the moment and the graphics library's own way of storing them. */
static void depth_back(depth_record *d) {
    static float *depth;
    static uint8_t *stencil;
    static size_t cap;
    size_t pixels = (size_t)d->width * d->height, bytes = depth_bytes_of(d->format);
    if (!d->newer) return;
    d->newer = 0;
    if (pixels > cap) {
        float *a = realloc(depth, pixels * sizeof(float));
        if (a) depth = a;
        uint8_t *b = realloc(stencil, pixels);
        if (b) stencil = b;
        if (!a || !b) return;
        cap = pixels;
    }
    if (!bytes_for(&g_depth_rows, &g_depth_rows_cap, pixels * bytes) ||
        surface_get(d->card, g_depth_rows, d->width, d->height, (uint32_t)bytes, d->format)) return;
    memset(stencil, 0, pixels);
    unpack_depth(g_depth_rows, d->format, d->width, d->height, depth, stencil);
    gxm_depth_surface_store(d->control, d->width, d->height, depth, stencil);
    if (logging()) fprintf(stderr, "gxm gpu: list %u: a depth surface of %ux%u goes back to memory\n", g_list, d->width, d->height);
    g_backs++;
}

int gxm_gpu_scene_begin(const uint32_t control[5], uint32_t width, uint32_t height, uint32_t depth_bits, int load, int store,
                        float depth, uint8_t stencil) {
    memset(&g_scene, 0, sizeof(g_scene));
    if (g_mode != GXM_GPU_DRAW) return 0;
    g_scene.active = 1;
    g_scene.width = width;
    g_scene.height = height;
    g_scene.format = depth_format_of(depth_bits);
    g_scene.where = IN_PLANES;
    g_scene.cleared = !load;
    g_scene.clear_depth = depth;
    g_scene.clear_stencil = stencil;
    if (g_scene.format == GPU_NONE || !keeping()) return 0;
    depth_record *d = control ? depth_for(control, width, height, g_scene.format) : NULL;
    if (!d) return 0;
    if (store) {
        /* the scene's depth is the surface's when it ends: drawn where the surface is kept */
        g_scene.of = d;
        g_scene.card = d->card;
        if (load && d->newer) { g_scene.where = ON_CARD; return 1; }
    } else if (load && d->newer) {
        /* the surface is where the scene starts and is not to be changed by it: the scene has a copy */
        gpu_surface *copy = spare_for(g_scene.format, width, height);
        if (copy && !gpu_surface_copy(copy, d->card)) {
            g_scene.card = copy;
            g_scene.where = ON_CARD;
            return 1;
        }
    } else if (load) {
        /* A title reads the corner of a larger depth surface as a smaller one of the same rows. If the card holds the
         * larger, the scene's copy is made from its corner there; the stencil does not come with it, and the scene is
         * given what its planes were made of. */
        for (unsigned i = 0; i < g_depth_count; i++) {
            depth_record *w = &g_depths[i];
            gpu_surface *copy;
            if (w == d || !w->newer || w->lo != d->lo || w->row_bytes != d->row_bytes || w->format != d->format ||
                w->width < width || w->height < height) continue;
            copy = spare_for(g_scene.format, width, height);
            if (!copy || gpu_surface_depth_corner(copy, w->card, stencil)) break;
            g_scene.card = copy;
            g_scene.where = ON_CARD;
            return 1;
        }
    }
    /* The scene's depth is in memory, then, under whatever the card holds newer of that memory. Depth that is whole
     * numbers is left there until a list looks at it, and the caller told the planes need nothing; a float's top bit
     * is a mask every list is turned away by or not, and that is fetched now, by the caller, after what the card
     * holds has gone to memory. */
    if (load && g_scene.format != GPU_D32FS8) {
        g_scene.where = IN_MEMORY;
        g_scene.from = d;
        memcpy(g_scene.control, control, sizeof(g_scene.control));
        return 1;
    }
    if (load) surfaces_back(d->lo, d->hi, NULL);
    return 0;
}

/* Whether a draw looks at its scene's depth or stencil, or leaves either
 * other than it found it. */
static int looks_at_depth(const queued *q) {
    const gxm_draw_state *s = &q->s;
    for (int k = 0; k < (s->two_sided ? 2 : 1); k++) {
        const gxm_face *f = &s->face[k];
        if (f->depth_func != 7 || f->depth_write || f->stencil_func != 7) return 1;
        if ((f->stencil_fail | f->depth_fail | f->depth_pass) && f->write_mask) return 1;
    }
    return 0;
}

/* The depth of a scene that left it in memory, into the planes, now that
 * something looks: what the card holds newer of that memory goes there
 * first. */
static void depth_fetched(const gxm_draw_state *s) {
    if (!g_scene.active || g_scene.where != IN_MEMORY) return;
    if (logging()) fprintf(stderr, "gxm gpu: list %u: a scene's depth of %ux%u is fetched from memory, a draw looks at it\n", g_list, g_scene.width, g_scene.height);
    surfaces_back(g_scene.from->lo, g_scene.from->hi, NULL);
    gxm_depth_surface_load(g_scene.control, g_scene.width, g_scene.height, s->depth, s->stencil, s->mask);
    g_scene.where = IN_PLANES;
    g_scene.cleared = 0;
}

int gxm_gpu_scene_end(int store, float *depth, uint8_t *stencil) {
    int kept = 0;
    if (g_mode != GXM_GPU_DRAW || !g_scene.active) return 0;
    if (g_scene.where == IN_MEMORY) {
        /* nothing looked at its depth: it is what it was, where it was, and there is nothing to write */
        g_scene.active = 0;
        return 1;
    }
    if (g_scene.of) {
        depth_record *d = g_scene.of;
        if (store && g_scene.where == IN_PLANES && g_scene.cleared && d->card) {
            /* A scene that loads nothing, draws nothing and keeps its depth is how a title clears a depth surface. The
             * card's is cleared, and is the surface from here on: nothing goes to memory, and the scene that loads it
             * next finds it on the card. */
            gpu_surface_clear(d->card, g_scene.clear_depth, g_scene.clear_stencil);
            g_scene.where = ON_CARD;
        }
        if (store && g_scene.where == ON_CARD) {
            /* another surface over the same memory that the card held goes to memory before this one is laid over it */
            for (unsigned i = 0; i < g_depth_count; i++)
                if (&g_depths[i] != d && g_depths[i].newer && g_depths[i].lo < d->hi && g_depths[i].hi > d->lo) depth_back(&g_depths[i]);
            d->newer = 1;
            d->version++;
            kept = 1;
        } else {
            /* the caller writes the planes into memory, or the scene drew nothing to keep: the card's is not the surface */
            d->newer = 0;
            forget_views_of(d);
        }
    }
    if (store && !kept && g_scene.where == ON_CARD && g_scene.card) {
        /* a depth to keep, on the card, and no surface of the card's to keep it in: the planes have it, for the caller */
        size_t pixels = (size_t)g_scene.width * g_scene.height, bytes = depth_bytes_of(g_scene.format);
        if (bytes_for(&g_depth_rows, &g_depth_rows_cap, pixels * bytes) &&
            !surface_get(g_scene.card, g_depth_rows, g_scene.width, g_scene.height, (uint32_t)bytes, g_scene.format))
            unpack_depth(g_depth_rows, g_scene.format, g_scene.width, g_scene.height, depth, stencil);
    }
    g_scene.active = 0;
    return kept;
}

/* The scene's depth into the renderer's planes, for a list the processor
 * is about to draw; and from then on the planes are where it is. */
static void planes_wanted(const gxm_draw_state *s) {
    if (g_scene.active && g_scene.where == IN_MEMORY) { depth_fetched(s); return; }
    if (!g_scene.active || g_scene.where != ON_CARD || !g_scene.card) { g_scene.where = IN_PLANES; g_scene.cleared = 0; return; }
    size_t pixels = (size_t)s->width * s->height, bytes = depth_bytes_of(g_scene.format);
    if (bytes_for(&g_depth_rows, &g_depth_rows_cap, pixels * bytes) &&
        !surface_get(g_scene.card, g_depth_rows, s->width, s->height, (uint32_t)bytes, g_scene.format))
        unpack_depth(g_depth_rows, g_scene.format, s->width, s->height, s->depth, s->stencil);
    g_scene.where = IN_PLANES;
    g_scene.cleared = 0;
}

void gxm_gpu_for_processor(const gxm_draw_state *s) {
    if (g_mode != GXM_GPU_DRAW) return;
    surfaces_back(NULL, NULL, NULL);
    planes_wanted(s);
}

/* --- a surface read as a texture, on the card ----------------------------------------------------
 * A title samples what it drew, and seldom in the form it drew it in: a
 * geometry buffer of two words a pixel is read as a texture twice as wide
 * of four bytes a texel, and again from four bytes further on; depth is
 * read as a number. From memory that is no matter, bytes being bytes. On
 * the card the surface is whatever the card keeps, so a program is written
 * for each such reading: for a texel of the texture, which bytes of the
 * surface's memory it would be, which pixel of the card's surface holds
 * them, the words those were, and the texel's fields out of the words as
 * pixfmt.c reads them. gpu_convert runs it over a texture of floats, and
 * that is what the draw samples. */

typedef struct { uint64_t hash; gpu_shader *program; } view_program;
static view_program g_view_programs[64];
static unsigned g_view_program_count;

typedef struct { char *s; size_t n, cap; int bad; } words;

static void say(words *w, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { w->bad = 1; return; }
    if (w->n + (size_t)n + 1 > w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 4096;
        while (cap < w->n + (size_t)n + 1) cap *= 2;
        char *grown = realloc(w->s, cap);
        if (!grown) { w->bad = 1; return; }
        w->s = grown;
        w->cap = cap;
    }
    va_start(ap, fmt);
    vsnprintf(w->s + w->n, (size_t)n + 1, fmt, ap);
    va_end(ap);
    w->n += (size_t)n;
}

/* One field of a texel, from the texel's low and high words, as pixfmt.c's
 * field_value has it. 0 for a kind there is no text for. */
static int say_field(words *w, const pixfmt *f, int i) {
    unsigned shift = f->f[i].shift, bits = f->f[i].bits, kind = f->f[i].kind;
    uint32_t mask = bits >= 32 ? 0xFFFFFFFFu : (1u << bits) - 1u;
    if (!bits || bits > 32) return 0;
    if (shift + bits <= 32) say(w, "    uint x%d = (lo >> %uu) & 0x%08Xu;\n", i, shift, mask);
    else if (shift >= 32)   say(w, "    uint x%d = (hi >> %uu) & 0x%08Xu;\n", i, shift - 32, mask);
    else                    say(w, "    uint x%d = ((lo >> %uu) | (hi << %uu)) & 0x%08Xu;\n", i, shift, 32 - shift, mask);
    switch (kind) {
    case PIX_UNORM: say(w, "    float v%d = float(x%d) / %.1f;\n", i, i, (double)mask); break;
    case PIX_SNORM:
        if (bits < 2) return 0;
        say(w, "    float v%d = max(float(int(x%d << %uu) >> %u) / %.1f, -1.0);\n", i, i, 32 - bits, 32 - bits, (double)((1u << (bits - 1)) - 1u));
        break;
    case PIX_RAW: case PIX_F32: say(w, "    float v%d = asfloat(x%d);\n", i, i); break;
    case PIX_F32M: say(w, "    float v%d = asfloat(x%d & 0x7FFFFFFFu);\n", i, i); break;
    case PIX_F16:  say(w, "    float v%d = f16tof32(x%d);\n", i, i); break;
    case PIX_UF11: say(w, "    float v%d = small(x%d, 6u);\n", i, i); break;
    case PIX_UF10: say(w, "    float v%d = small(x%d, 5u);\n", i, i); break;
    case PIX_SE9:  say(w, "    float v%d = float(x%d) * asfloat((103u + ((%s >> 27) & 31u)) << 23);\n", i, i, f->bytes > 4 ? "hi" : "lo"); break;
    default: return 0;
    }
    return 1;
}

/* What a field of a surface keeps of a number put into it: for a surface
 * the card holds as plain floats, read in the surface's own form. */
static int say_kept(words *w, const pixfmt *f, int channel) {
    int field = f->from[channel];
    char c = "xyzw"[channel];
    if (field == PIX_ZERO) { say(w, "    c.%c = 0.0;\n", c); return 1; }
    if (field == PIX_ONE)  { say(w, "    c.%c = 1.0;\n", c); return 1; }
    unsigned bits = f->f[field].bits;
    switch (f->f[field].kind) {
    case PIX_UNORM:
        if (bits > 16) return 1;
        say(w, "    c.%c = floor(saturate(c.%c) * %.1f + 0.5) / %.1f;\n", c, c, (double)((1u << bits) - 1u), (double)((1u << bits) - 1u));
        return 1;
    case PIX_UF11: say(w, "    c.%c = small_kept(c.%c, 6u);\n", c, c); return 1;
    case PIX_UF10: say(w, "    c.%c = small_kept(c.%c, 5u);\n", c, c); return 1;
    case PIX_F16:  say(w, "    c.%c = f16tof32(f32tof16(c.%c));\n", c, c); return 1;
    case PIX_F32:  return 1;
    default:       return 0;
    }
}

/* These programs are written once, in HLSL's words, for either language
 * the card may read (gpu.h's gpu_language): a GLSL text begins by saying
 * what those words are in its own (gxp_hlsl.h's gxp_glsl_names). Where the
 * two have different things and not different names, the text is left open
 * (a %s) and view_text fills it: how the constants and the surface are
 * declared, how a pixel of the surface is fetched by its place, and what
 * the program is called and handed. */
static const char VIEW_SAID[] =
    "// Written by the toolkit: a surface on the card, read as the texture a title makes of its memory.\n";

/* Three places: what the constants are declared as; what a variable of the program's own that it keeps is; and what
 * that variable begins as, where the language does not make it nothing by itself. */
static const char VIEW_GIVEN[] =
    "%s { int4 A; int4 B; };   // A: the texture's first byte in the surface, bytes a row of it, bytes a texel,\n"
    "                                                    // bytes a row of the surface; B: bytes a pixel of it, its width, its height,\n"
    "                                                    // and how many of the card's pixels across and down are one of those\n"
    "%sint2 SUB%s;                                    // which of them, of the texel being made\n";

static const char VIEW_PRELUDE[] =
    "float small(uint x, uint mb) {\n"
    "    uint e = x >> mb, m = x & ((1u << mb) - 1u);\n"
    "    if (e == 0u) return float(m) * asfloat((113u - mb) << 23);\n"
    "    if (e == 31u) return asfloat(m != 0u ? 0x7FC00000u : 0x7F800000u);\n"
    "    return asfloat(((e + 112u) << 23) | (m << (23u - mb)));\n"
    "}\n"
    "float small_kept(float v, uint mb) {\n"
    "    float top = asfloat((142u << 23) | (((1u << mb) - 1u) << (23u - mb)));\n"
    "    if (v != v) return v;\n"
    "    if (v <= 0.0) return 0.0;\n"
    "    if (v > top) return top;\n"
    "    uint bits = asuint(v);\n"
    "    int e = int(bits >> 23) - 112;\n"
    "    if (e <= 0) return min(floor(v * asfloat((141u + mb) << 23) + 0.5), float(1u << mb)) * asfloat((113u - mb) << 23);\n"
    "    uint m = ((bits & 0x7FFFFFu) + (1u << (22u - mb))) >> (23u - mb);\n"
    "    if (m >= (1u << mb)) { m = 0u; e++; }\n"
    "    if (e >= 31) return top;\n"
    "    return asfloat((uint(e + 112) << 23) | (m << (23u - mb)));\n"
    "}\n"
    "float off_curve(float v) { if (!(v > 0.0)) return 0.0; if (v >= 1.0) return 1.0; return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }\n"
    "float on_curve(float v) { if (!(v > 0.0)) return 0.0; if (v >= 1.0) return 1.0; return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055; }\n";

/* The program that reads a surface of the card's form `format` as texels
 * of `tf`, one of three ways. BY_BYTES: the texture's rows of bytes are
 * found among the surface's, wherever they begin and however long a row of
 * either is. BY_PIXEL: the texture is laid out as the surface is, texel
 * for pixel, and a texel is its pixel's words. AS_KEPT: the same, of a
 * surface the card holds as plain floats and the texture reads in the
 * surface's own form, `kept`: the texel is the pixel as the surface would
 * have kept it. NULL when there is no such program. */
enum { BY_BYTES = 0, BY_PIXEL, AS_KEPT };

/* What a program finds the surface as, declared in the card's language:
 * `of` is what a texel of it is there, in HLSL's words. */
static void say_source(words *w, int glsl, const char *of, const char *name, int unit) {
    if (glsl) say(w, "layout(binding = %d) uniform %ssampler2D %s;\n", unit, of[0] == 'u' ? "u" : "", name);
    else say(w, "Texture2D<%s> %s : register(t%d);\n", of, name, unit);
}

static char *view_text(unsigned format, const pixfmt *tf, unsigned texture_gamma, int how, const pixfmt *kept, unsigned surface_gamma) {
    words w = { 0 };
    int glsl = gpu_language() == GPU_GLSL;
    /* the program, and what it is handed: in GLSL a function, with a `main` after it that hands it the texel's place */
    const char *head = glsl ? "float4 program(float4 pos) {\n" : "float4 main(float4 pos : SV_Position) : SV_Target {\n";
    /* a pixel of the surface by its column and row; its stencil's; and its depth, which is one number: there the
     * whole of what is fetched, here the first of four */
    const char *px = glsl ? "texelFetch(SRC, int2(col * B.w + SUB.x, row * B.w + SUB.y), 0)" : "SRC.Load(int3(col * B.w + SUB.x, row * B.w + SUB.y, 0))";
    const char *px2 = glsl ? "texelFetch(SRC2, int2(col * B.w + SUB.x, row * B.w + SUB.y), 0)" : "SRC2.Load(int3(col * B.w + SUB.x, row * B.w + SUB.y, 0))";
    const char *deep = glsl ? "texelFetch(SRC, int2(col * B.w + SUB.x, row * B.w + SUB.y), 0).x" : px;
    /* whole numbers divided, none of them below nothing: there the language's own `/` and `%`, here a function
     * that asks no divide of the card (gxp_hlsl.h's gxp_glsl_names says why). In turn: which part of a pixel the
     * texel is and which pixel; the row and the column the texture begins at; the row and the column of a byte. */
    const char *sub = glsl ? "whole - over(whole, B.w) * B.w" : "whole % B.w", *each = glsl ? "over(whole, B.w)" : "whole / B.w";
    const char *row0 = glsl ? "over(A.x, A.w)" : "A.x / A.w", *col0 = glsl ? "over(A.x - row * A.w, B.x)" : "(A.x - row * A.w) / B.x";
    const char *row_of = glsl ? "over(byte, A.w)" : "byte / A.w", *col_of = glsl ? "over(rest, B.x)" : "rest / B.x";
    say(&w, "%s", VIEW_SAID);
    if (glsl) say(&w, "%s", gxp_glsl_names);
    say(&w, VIEW_GIVEN, glsl ? "layout(std140, binding = 1) uniform view" : "cbuffer view : register(b0)", glsl ? "" : "static ",
        glsl ? " = int2(0, 0)" : "");
    say(&w, "%s", VIEW_PRELUDE);
    if (how == AS_KEPT) {
        say_source(&w, glsl, "float4", "SRC", 0);
        say(&w, "%s"
                "    int2 whole = int2(pos.xy);\n"
                "    SUB = %s;\n"
                "    int row = %s, col = %s;\n"
                "    int2 at = %s + int2(col, row);\n"
                "    if (at.x >= B.y || at.y >= B.z) return float4(0.0, 0.0, 0.0, 0.0);\n"
                "    float4 c = %s;\n", head, sub, row0, col0, each, glsl ? "texelFetch(SRC, at * B.w + SUB, 0)" : "SRC.Load(int3(at * B.w + SUB, 0))");
        /* the card holds light; the surface's bytes would hold it on the display's curve if the surface is on it */
        if (surface_gamma) say(&w, "    c.xyz = float3(on_curve(c.x), on_curve(c.y), on_curve(c.z));\n");
        for (int k = 0; k < 4; k++) if (!say_kept(&w, kept, k)) { free(w.s); return NULL; }
        /* the fields are the surface's; the texture gives them to red, green, blue and alpha its own way */
        say(&w, "    float4 s = c;\n");
        for (int k = 0; k < 4; k++) {
            int field = tf->from[k], held = -1;
            if (field == PIX_ZERO) { say(&w, "    c.%c = 0.0;\n", "xyzw"[k]); continue; }
            if (field == PIX_ONE)  { say(&w, "    c.%c = 1.0;\n", "xyzw"[k]); continue; }
            for (int j = 0; j < 4; j++) if (kept->from[j] == field) held = j;
            if (held < 0) { free(w.s); return NULL; }           /* a field the surface gives to no channel: the card does not have it */
            say(&w, "    c.%c = s.%c;\n", "xyzw"[k], "xyzw"[held]);
        }
    } else {
        switch (format) {
        case GPU_RG32UI:
            say_source(&w, glsl, "uint4", "SRC", 0);
            say(&w, "uint unit_at(int col, int row, int k) { uint4 p = %s; return k == 0 ? p.x : p.y; }\n", px);
            break;
        case GPU_RGBA8: case GPU_RGBA8_SRGB:
            say_source(&w, glsl, "float4", "SRC", 0);
            say(&w, "uint unit_at(int col, int row, int k) { uint4 b = uint4(%s * 255.0 + 0.5); return b.x | (b.y << 8) | (b.z << 16) | (b.w << 24); }\n", px);
            break;
        case GPU_BGRA8: case GPU_BGRA8_SRGB:
            say_source(&w, glsl, "float4", "SRC", 0);
            say(&w, "uint unit_at(int col, int row, int k) { uint4 b = uint4(%s * 255.0 + 0.5); return b.z | (b.y << 8) | (b.x << 16) | (b.w << 24); }\n", px);
            break;
        case GPU_RGBA16F:
            say_source(&w, glsl, "float4", "SRC", 0);
            say(&w, "uint unit_at(int col, int row, int k) {\n"
                    "    float4 p = %s;\n"
                    "    return k == 0 ? (f32tof16(p.x) | (f32tof16(p.y) << 16)) : (f32tof16(p.z) | (f32tof16(p.w) << 16));\n"
                    "}\n", px);
            break;
        case GPU_D24S8:
            say_source(&w, glsl, "float", "SRC", 0);
            say_source(&w, glsl, "uint2", "SRC2", 1);
            say(&w, "uint unit_at(int col, int row, int k) { return uint(%s * 16777215.0 + 0.5) | (%s.y << 24); }\n", deep, px2);
            break;
        case GPU_D32FS8:
            say_source(&w, glsl, "float", "SRC", 0);
            say(&w, "uint unit_at(int col, int row, int k) { return asuint(%s); }\n", deep);
            break;
        case GPU_D16:
            /* two bytes a pixel: a word is two pixels side by side */
            say_source(&w, glsl, "float", "SRC", 0);
            say(&w, "uint half_at(int col, int row) { return col < B.y ? uint(%s * 65535.0 + 0.5) : 0u; }\n"
                    "uint unit_at(int col, int row, int k) { return half_at(col, row) | (half_at(col + 1, row) << 16); }\n", deep);
            break;
        default:
            free(w.s);
            return NULL;
        }
        if (how == BY_PIXEL)
            say(&w, "%s"
                    "    int2 whole = int2(pos.xy), at = %s;\n"
                    "    SUB = %s;\n"
                    "    if (at.x >= B.y || at.y >= B.z) return float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    uint lo = unit_at(at.x, at.y, 0), hi = unit_at(at.x, at.y, 1);\n", head, each, sub);
        else
            say(&w, "uint word_at(int byte) {\n"
                    "    if (byte < 0) return 0u;\n"
                    "    int row = %s, rest = byte - row * A.w, col = %s, k = (rest - col * B.x) >> 2;\n"
                    "    if (col >= B.y || row >= B.z) return 0u;\n"
                    "    return unit_at(col, row, k);\n"
                    "}\n"
                    "%s"
                    "    int2 whole = int2(pos.xy), at = %s;\n"
                    "    SUB = %s;\n"
                    "    int byte = A.x + at.y * A.y + at.x * A.z, first = byte & ~3;\n"
                    "    uint turn = uint(byte & 3) * 8u, w0 = word_at(first), w1 = word_at(first + 4);\n"
                    "    uint lo = turn != 0u ? (w0 >> turn) | (w1 << (32u - turn)) : w0, hi = turn != 0u ? w1 >> turn : w1;\n",
                    row_of, col_of, head, each, sub);
        for (int i = 0; i < tf->fields; i++) if (!say_field(&w, tf, i)) { free(w.s); return NULL; }
        say(&w, "    float4 c;\n");
        for (int k = 0; k < 4; k++) {
            if (tf->from[k] == PIX_ZERO) say(&w, "    c.%c = 0.0;\n", "xyzw"[k]);
            else if (tf->from[k] == PIX_ONE) say(&w, "    c.%c = 1.0;\n", "xyzw"[k]);
            else say(&w, "    c.%c = v%d;\n", "xyzw"[k], tf->from[k]);
        }
    }
    /* a texture kept on the display's curve is handed over as light: gxm_tex.c's gxm_tex_texel */
    if (texture_gamma) say(&w, "    c.xy = float2(off_curve(c.x), off_curve(c.y));\n%s", texture_gamma != 3 ? "    c.z = off_curve(c.z);\n" : "");
    say(&w, "    return c;\n}\n");
    if (glsl) say(&w, "layout(location = 0) out vec4 result;\nvoid main() { result = program(gl_FragCoord); }\n");
    if (w.bad) { free(w.s); return NULL; }
    return w.s;
}

static gpu_shader *view_program_for(char *text) {
    uint64_t hash;
    if (!text) return NULL;
    hash = fold(0, (const uint8_t *)text, strlen(text));
    for (unsigned i = 0; i < g_view_program_count; i++)
        if (g_view_programs[i].hash == hash) { free(text); return g_view_programs[i].program; }
    char log[1024];
    uint64_t began = now_ns();
    gpu_shader *made = gpu_shader_make(1, text, 0, log, sizeof(log));
    g_ns[P_PROGRAM] += now_ns() - began;
    g_programs++;
    if (!made) refuse("a program to read a surface as a texture did not compile: %.120s", log);
    const char *dump = getenv("VITARECOMP_GXM_GPU_SHADERS");
    if (dump && *dump) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/view_%016llx.%s", dump, (unsigned long long)hash, gpu_language() == GPU_GLSL ? "glsl" : "hlsl");
        FILE *f = fopen(path, "wb");
        if (f) { fputs(text, f); fclose(f); }
    }
    free(text);
    if (g_view_program_count < 64) {
        g_view_programs[g_view_program_count].hash = hash;
        g_view_programs[g_view_program_count++].program = made;
    }
    return made;
}

/* --- textures, kept on the card until their bytes change ------------------------------------- */

typedef struct {
    uint32_t words[4], data, palette;
    uint64_t stamp;
    uint32_t checked, used;                 /* the list it was last compared in, and drawn with */
    uint32_t moment;                        /* when its bytes were last read (mem.h): not written since, not read again */
    gpu_texture *texture;
    const void *from;                       /* made on the card from this surface's record, not from memory; NULL otherwise */
    uint32_t from_version;                  /* as the surface stood after so many lists */
    uint32_t width, height;                 /* of such a one */
} texture_slot;

#define TEXTURES 2048
static texture_slot g_textures[TEXTURES];
static unsigned g_texture_count;

/* A texture that is replaced while a list is being made ready may already
 * have been handed to one of the list's draws: it is put aside, and freed
 * when the next list begins. */
static gpu_texture **g_retired;
static size_t g_retired_count, g_retired_cap;

static void retire(gpu_texture *t) {
    if (!t) return;
    if (g_retired_count == g_retired_cap) {
        size_t cap = g_retired_cap ? g_retired_cap * 2 : 64;
        gpu_texture **grown = realloc(g_retired, cap * sizeof(*grown));
        if (!grown) return;                         /* kept for good, which is better than freed too soon */
        g_retired = grown;
        g_retired_cap = cap;
    }
    g_retired[g_retired_count++] = t;
}

static void retired_go(void) {
    for (size_t i = 0; i < g_retired_count; i++) gpu_texture_free(g_retired[i]);
    g_retired_count = 0;
}

static void forget_views_of(const void *record) {
    for (unsigned i = 0; i < g_texture_count; i++)
        if (g_textures[i].from == record) { g_textures[i].from = NULL; g_textures[i].stamp = 0; g_textures[i].checked = 0; }
}

/* A texture that is a plain run of texels in rows, one level: its bytes
 * a row, or 0 for any other. */
static uint32_t rows_of(const gxm_tex *t) {
    if (t->kind != GXM_TEX_PLAIN || t->cube || t->levels != 1 || t->pal) return 0;
    if (t->type == GXM_TEX_LINEAR) return t->store_w[0] * t->unit_bytes;
    if (t->type == GXM_TEX_LINEAR_STRIDED) return t->stride;
    return 0;
}

/* The bytes a texture's texels lie in: of rows, no more than the rows. */
static size_t texture_extent(const gxm_tex *t) {
    uint32_t rows = rows_of(t);
    uint64_t bytes = rows ? (uint64_t)rows * t->level_h[0] : t->bytes;
    return (size_t)(bytes < t->avail ? bytes : t->avail);
}

/* The texture `t`, whose bytes lie in a surface the card holds, made from
 * that surface on the card. NULL when it cannot be: the surface is then
 * the caller's to send back, and the texture comes from memory. */
static gpu_texture *view_for(texture_slot *slot, const gxm_tex *t, const colour_record *c, const depth_record *d) {
    const void *record = c ? (const void *)c : (const void *)d;
    uint32_t version = c ? c->version : d->version, row_bytes = rows_of(t);
    gpu_surface *surface = c ? c->card : d->card;
    const uint8_t *lo = c ? c->lo : d->lo;
    int32_t constants[8] = { 0 };
    char *text = NULL;
    if (slot->from == record && slot->from_version == version && slot->texture) return slot->texture;
    if (t->kind != GXM_TEX_PLAIN || t->cube || t->levels != 1 || t->pal)
        return by_memory("the texture is not plain texels of one level (kind %u, %u levels%s)", t->kind, t->levels, t->cube ? ", a cube" : "");
    if (t->base < lo)
        return by_memory("the texture begins %ld bytes before the surface (texture 0x%08X, %ux%u format 0x%08X type %u kind %u, %u levels, %u bytes)",
                         (long)(lo - t->base), t->data, t->level_w[0], t->level_h[0], t->format, t->type, t->kind, t->levels, t->bytes);
    if (t->pf.bytes != 1 && t->pf.bytes != 2 && t->pf.bytes != 3 && t->pf.bytes != 4 && t->pf.bytes != 8)
        return by_memory("a texel of %u bytes", t->pf.bytes);
    constants[0] = (int32_t)(t->base - lo);
    constants[1] = (int32_t)row_bytes;
    constants[2] = t->pf.bytes;
    constants[7] = (int32_t)larger();
    if (c) {
        uint32_t layout = c->s.colour_layout;
        /* laid out as the surface is, texel for pixel: in rows of one length, or in the same tiles */
        int alike = !constants[0] && t->pf.bytes == c->s.colour.bytes && t->level_w[0] == c->s.width && t->level_h[0] == c->s.height &&
                    ((layout == GXM_SURFACE_LINEAR && row_bytes && row_bytes == c->s.colour_stride * c->s.colour.bytes) ||
                     (layout == GXM_SURFACE_TILED && t->type == GXM_TEX_TILED));
        constants[3] = (int32_t)(c->s.colour_stride * c->s.colour.bytes);
        constants[4] = c->s.colour.bytes;
        constants[5] = (int32_t)c->s.width;
        constants[6] = (int32_t)c->s.height;
        if (c->way.mode == AS_FLOATS) {
            /* the card has the surface as light and not as bytes: only a texture of the surface's own fields, texel for
             * pixel, can be had from it, the whole of it or rows of it from some pixel on */
            const pixfmt *a = &t->pf, *b = &c->s.colour;
            int fields = a->bytes == b->bytes && a->fields == b->fields && !memcmp(a->f, b->f, sizeof(a->f));
            int within = layout == GXM_SURFACE_LINEAR && row_bytes && row_bytes == c->s.colour_stride * b->bytes && !(constants[0] % b->bytes);
            if (within) {
                /* and every texel of it one of the surface's pixels, or in what is left of a row past the surface's
                 * last pixel: a title that draws 960 pixels into rows of 1024 reads them back as a texture 1024 wide.
                 * No draw puts anything there, and the program gives nothing for it */
                uint32_t row = (uint32_t)constants[0] / row_bytes, col = ((uint32_t)constants[0] - row * row_bytes) / b->bytes;
                within = col + t->level_w[0] <= c->s.colour_stride && row + t->level_h[0] <= c->s.height;
            }
            if (!fields || !(alike || within))
                return by_memory("a surface the card keeps as floats (%ux%u, %u bytes a pixel, %u a row, layout %u) read another way "
                                 "(texture %ux%u format 0x%08X type %u, %u bytes a texel, %u a row, from byte %d)", c->s.width, c->s.height,
                                 c->s.colour.bytes, constants[3], layout, t->level_w[0], t->level_h[0], t->format, t->type, t->pf.bytes,
                                 row_bytes, constants[0]);
            text = view_text(c->way.format, &t->pf, t->gamma, AS_KEPT, &c->s.colour, c->s.colour_gamma);
        } else if (alike) {
            text = view_text(c->way.format, &t->pf, t->gamma, BY_PIXEL, NULL, 0);
        } else {
            /* a texel across two words is put together from both: only when it is no wider than a word; and the texture's
             * rows lie among the surface's, give or take the end of its last */
            if (layout != GXM_SURFACE_LINEAR || !row_bytes || ((constants[0] & 3) && t->pf.bytes > 4) ||
                t->base + (size_t)row_bytes * t->level_h[0] > c->hi + row_bytes)
                return by_memory("a surface of layout %u (%ux%u, %u bytes a pixel) and a texture of type %u (%ux%u format 0x%08X, from byte %d) "
                                 "that are not both rows", layout, c->s.width, c->s.height, c->s.colour.bytes, t->type, t->level_w[0],
                                 t->level_h[0], t->format, constants[0]);
            text = view_text(c->way.format, &t->pf, t->gamma, BY_BYTES, NULL, 0);
        }
    } else {
        int tiled = (d->control[0] & 0x00011000u) == 0x00011000u, unit = d->format == GPU_D16 ? 2 : 4;
        constants[3] = (int32_t)d->row_bytes;
        constants[4] = unit;
        constants[5] = (int32_t)d->width;
        constants[6] = (int32_t)d->height;
        if (tiled && t->type == GXM_TEX_TILED && !constants[0] && t->pf.bytes == unit && t->level_w[0] == d->width && t->level_h[0] == d->height) {
            /* in the same tiles, texel for pixel */
            text = view_text(d->format, &t->pf, t->gamma, BY_PIXEL, NULL, 0);
        } else {
            if (tiled || !row_bytes)
                return by_memory("a depth surface or its texture not in rows (surface 0x%08X, texture type %u, %ux%u of %u bytes from byte %d)",
                                 d->control[0], t->type, t->level_w[0], t->level_h[0], t->pf.bytes, constants[0]);
            if (t->base + (size_t)row_bytes * t->level_h[0] > d->hi + row_bytes)
                return by_memory("a texture of %u rows that reaches past the depth surface it begins in", t->level_h[0]);
            text = view_text(d->format, &t->pf, t->gamma, BY_BYTES, NULL, 0);
        }
    }
    if (!text) return by_memory("no program for a texture of format 0x%08X read from a surface the card keeps in form %u", t->format,
                                c ? c->way.format : d->format);
    gpu_shader *program = view_program_for(text);
    if (!program) return NULL;
    if (logging()) {
        static char seen[64][160];
        static unsigned count;
        char what[160];
        unsigned i;
        snprintf(what, sizeof(what), "a surface of %dx%d, %d bytes a pixel and %d a row, is read as a texture of %ux%u, %u bytes a texel and %u a row, from byte %d",
                 constants[5], constants[6], constants[4], constants[3], t->level_w[0], t->level_h[0], t->pf.bytes, row_bytes, constants[0]);
        for (i = 0; i < count && strcmp(seen[i], what); i++) {}
        if (i == count && count < 64) {
            memcpy(seen[count++], what, sizeof(what));
            fprintf(stderr, "gxm gpu: list %u: %s (%s, format 0x%08X)\n", g_list, what, c ? "colour" : "depth", t->format);
        }
    }
    if (!slot->from || !slot->texture || slot->width != t->level_w[0] || slot->height != t->level_h[0]) {
        retire(slot->texture);
        slot->texture = gpu_texture_blank(t->level_w[0] * larger(), t->level_h[0] * larger());
        slot->width = t->level_w[0];
        slot->height = t->level_h[0];
    }
    slot->from = NULL;
    if (!slot->texture || gpu_convert(slot->texture, surface, program, constants, sizeof(constants))) return NULL;
    slot->from = record;
    slot->from_version = version;
    g_views++;
    return slot->texture;
}

static int g_from_surface;                  /* what texture_for last gave was made from a surface on the card, and is the size that is */

static gpu_texture *texture_for(const gxm_tex *t, const uint32_t words[4]) {
    texture_slot *slot = NULL;
    for (unsigned i = 0; i < g_texture_count; i++) {
        texture_slot *s = &g_textures[i];
        if (s->data == t->data && s->palette == t->palette && !memcmp(s->words, words, sizeof(s->words))) { slot = s; break; }
    }
    if (!slot) {
        if (g_texture_count == TEXTURES) {          /* room: the one longest unused goes */
            unsigned oldest = 0;
            for (unsigned i = 1; i < TEXTURES; i++) if (g_textures[i].used < g_textures[oldest].used) oldest = i;
            retire(g_textures[oldest].texture);
            g_textures[oldest] = g_textures[--g_texture_count];
        }
        slot = &g_textures[g_texture_count++];
        memset(slot, 0, sizeof(*slot));
        memcpy(slot->words, words, sizeof(slot->words));
        slot->data = t->data;
        slot->palette = t->palette;
    }
    slot->used = g_list;

    /* its bytes may lie in a surface the card holds newer than memory does */
    const uint8_t *lo = t->base, *hi = t->base + texture_extent(t), *within = NULL;
    /* A texture that begins inside a surface is a reading of that surface, and what its layout rounds up to past the
     * surface's end is nothing: it is not a reading of whatever lies next in memory. */
    for (unsigned i = 0; i < g_colour_count; i++)
        if (g_colours[i].lo <= lo && lo < g_colours[i].hi && (!within || g_colours[i].hi > within)) within = g_colours[i].hi;
    for (unsigned i = 0; i < g_depth_count; i++)
        if (g_depths[i].lo <= lo && lo < g_depths[i].hi && (!within || g_depths[i].hi > within)) within = g_depths[i].hi;
    if (within && within < hi) hi = within;
    /* Two surfaces the card holds, laid over each other where the texture reads: what it reads is what memory would
     * hold of both. If the one drawn last holds all the texture's bytes, that is that one, and the texture is a
     * reading of it; if not, memory is asked. */
    colour_record *top = NULL;
    unsigned over = 0;
    for (unsigned i = 0; i < g_colour_count; i++) {
        colour_record *r = &g_colours[i];
        if (!r->newer || r->lo >= hi || r->hi <= lo) continue;
        over++;
        if (!top || r->drawn > top->drawn) top = r;
    }
    if (over > 1 && !(top->lo <= lo && top->hi >= hi)) { surfaces_back(lo, hi, NULL); top = NULL; }
    g_from_surface = 1;
    for (unsigned i = 0; i < g_colour_count; i++) {
        colour_record *r = &g_colours[i];
        if (!r->newer || r->lo >= hi || r->hi <= lo || (over > 1 && r != top)) continue;
        gpu_texture *made = view_for(slot, t, r, NULL);
        if (made) return made;
        colour_back(r);
    }
    for (unsigned i = 0; i < g_depth_count; i++) {
        depth_record *d = &g_depths[i];
        if (!d->newer || d->lo >= hi || d->hi <= lo) continue;
        gpu_texture *made = view_for(slot, t, NULL, d);
        if (made) return made;
        depth_back(d);
    }
    g_from_surface = 0;

    /* from memory. Nothing is written while a list is being made ready: once a list is enough; and bytes that have
     * not been written since they were last read are not read again */
    if (!slot->from && slot->texture && slot->checked == g_list) return slot->texture;
    if (!slot->from && slot->texture && !vita_mem_written(t->base, t->bytes < t->avail ? t->bytes : t->avail, slot->moment) &&
        !(t->pal && vita_mem_written(t->pal, 1024, slot->moment))) {
        slot->checked = g_list;
        return slot->texture;
    }
    uint64_t began = now_ns();
    uint64_t stamp = content_stamp(t);
    g_ns[P_HASH] += now_ns() - began;
    g_hashed += t->bytes < t->avail ? t->bytes : t->avail;
    if (!slot->from && slot->texture && slot->stamp == stamp) { slot->checked = g_list; slot->moment = g_now; return slot->texture; }
    retire(slot->texture);
    slot->from = NULL;
    began = now_ns();
    slot->texture = upload(t);
    g_ns[P_TEXTURE] += now_ns() - began;
    g_made++;
    slot->stamp = stamp;
    slot->checked = g_list;
    slot->moment = g_now;
    return slot->texture;
}

/* --- a list ---------------------------------------------------------------------------------- */

/* What the card says each draw of the last list put on the screen: the
 * place of its count among those the list asked for, or -1. */
static uint64_t *counts;
static int *counted;
static uint32_t counts_cap;

int gxm_gpu_list(queued **list, uint32_t count, int aside) {
    const gxm_draw_state *s0 = &list[0]->s;
    uint32_t width = s0->width, height = s0->height;
    unsigned depth = depth_format_of(s0->depth_bits);
    colour_way way;
    static programs *shaders;
    static uint32_t shaders_cap;
    g_aside.held = 0;
    g_list++;
    g_now = vita_mem_moment();
    retired_go();
    stale_surfaces();

    /* --- is it a list the card's path draws as the processor's would? --- */
    if (!s0->depth || !s0->stencil) return refuse("a scene with no depth or stencil planes");
    if (depth == GPU_NONE) return refuse("a depth surface of %u bits", s0->depth_bits);
    colour_way_of(s0, &way);
    if (s0->has_colour && !way.mode) {
        const pixfmt *f = &s0->colour;
        return refuse("a colour surface of words the card has no equal for: %u bytes a pixel, %u fields (%u bits of kind %u, %u of %u, "
                      "%u of %u, %u of %u)", f->bytes, f->fields, f->f[0].bits, f->f[0].kind, f->f[1].bits, f->f[1].kind,
                      f->f[2].bits, f->f[2].kind, f->f[3].bits, f->f[3].kind);
    }
    if (s0->mask)
        for (size_t i = 0, n = (size_t)width * height; i < n; i++)
            if (!s0->mask[i]) return refuse("a scene whose mask turns pixels away");
    if (count > shaders_cap) {
        programs *grown = realloc(shaders, (size_t)count * sizeof(*grown));
        if (!grown) return 0;
        shaders = grown;
        shaders_cap = count;
    }
    for (uint32_t i = 0; i < count; i++) {
        const queued *q = list[i];
        const gxm_draw_state *s = &q->s;
        if (q->instances != 1) return refuse("a draw of %u instances", q->instances);
        if (s->two_sided) {
            const gxm_face *a = &s->face[0], *b = &s->face[1];
            if (a->depth_func != b->depth_func || a->depth_write != b->depth_write || a->stencil_ref != b->stencil_ref ||
                a->compare_mask != b->compare_mask || a->write_mask != b->write_mask || a->program_off != b->program_off)
                return refuse("a draw whose two faces are tested differently");
        }
        /* the card keeps sixteen bits of depth with no stencil beside them */
        if (depth == GPU_D16 && (s->face[0].stencil_func != 7 || s->face[s->two_sided ? 1 : 0].stencil_func != 7))
            return refuse("a stencil test beside sixteen bits of depth");
        if (s->region_mode == GXM_REGION_INSIDE) return refuse("a draw clipped to the outside of a region");
        if (!shaders_for(q, way.raw, way.format, &shaders[i])) return 0;
        const gxm_program *fp = s->fragment;
        shaders[i].blended = 0;
        if (shaders[i].fragment && fp && !(fp->gxp.flags & GXP_FLAG_NATIVECOLOR) && ((fp->blend[1] & 15) || (fp->blend[1] >> 4))) {
            if (way.raw) return refuse("blending into a surface kept as words");
            if ((fp->blend[2] & 15) > 10 || (fp->blend[2] >> 4) > 10 || (fp->blend[3] & 15) > 10 || (fp->blend[3] >> 4) > 10)
                return refuse("a blend factor the card has no equal for");
            shaders[i].blended = 1;
        }
    }

    /* --- the target: where the card keeps it, or as the title's memory has it --- */
    size_t depth_bytes = depth_bytes_of(depth), pixels = (size_t)width * height;
    int kept = !aside && keeping();
    if (!bytes_for(&g_colour_rows, &g_colour_rows_cap, pixels * way.bytes + 16) || !bytes_for(&g_depth_rows, &g_depth_rows_cap, pixels * depth_bytes)) return 0;
    uint64_t lap = now_ns(), at;
    colour_record *record = NULL, *whole = NULL;
    gpu_surface *colour = NULL, *deep = NULL;
    if (way.mode && !aside) {
        record = colour_for(s0, &way);
        if (!record) return refuse("the card would not take a target of %u by %u", width, height);
        colour = record->card;
        /* A list whose first draw covers the target leaves nothing of what the target held: nothing is fetched for it. */
        int covers = keeping() && shaders[0].fragment && gxm_draw_covers(list[0]);
        whole = keeping() && !record->newer ? whole_of(record) : NULL;
        if (whole && !covers && gpu_surface_copy_corner(colour, whole->card, width * larger(), height * larger())) whole = NULL;
        if (!whole && covers) {
            /* Nor is anything sent to memory to make way. Another surface the card holds that lies wholly within this
             * one's memory is gone, unseen; one that lies partly over it stays where it is, this one drawn over it, and
             * the two go to memory in that order if memory is ever asked for. */
            for (unsigned i = 0; i < g_colour_count; i++) {
                colour_record *o = &g_colours[i];
                if (o == record || !o->newer || o->lo < record->lo || o->hi > record->hi) continue;
                o->newer = 0;
                o->agreed = 0;
                forget_views_of(o);
            }
            record->agreed = 0;
        } else if (!whole) {
            /* The same memory in another form, held newer by the card, goes into memory before this form is read from
             * it; and this one with it, in its turn, if one drawn later lies over it. */
            int under = 0;
            for (unsigned i = 0; i < g_colour_count; i++) {
                const colour_record *o = &g_colours[i];
                if (o != record && o->newer && o->lo < record->hi && o->hi > record->lo && o->drawn > record->drawn) under = 1;
            }
            surfaces_back(record->lo, record->hi, record->newer && under ? NULL : record);
        }
        if (!whole && !covers && !record->newer &&
            !(record->agreed && !vita_mem_written(record->lo, (size_t)(record->hi - record->lo), record->moment))) {
            /* memory holds the surface, and may have been written: the card's is filled from it unless the two still agree */
            uint64_t stamp = fold(0, record->lo, (size_t)(record->hi - record->lo));
            if (!(record->agreed && stamp == record->stamp)) {
                colour_in(s0, &way, g_colour_rows);
                if (surface_put(colour, g_colour_rows, width, height, way.bytes)) return refuse("the card would not take a target of %u by %u", width, height);
                g_puts++;
                if (logging())
                    fprintf(stderr, "gxm gpu: list %u: a colour surface of %ux%u, %u bytes a pixel, is filled from memory (%s)\n", g_list, width,
                            height, s0->colour.bytes, record->agreed ? "its bytes had changed" : "the card had nothing of it");
            }
            record->agreed = 1;
            record->stamp = stamp;
            record->moment = g_now;
        }
    } else if (way.mode) {
        colour = spare_for(way.format, width, height);
        if (!colour) return refuse("the card would not take a target of %u by %u", width, height);
        colour_in(s0, &way, g_colour_rows);
        if (surface_put(colour, g_colour_rows, width, height, way.bytes)) return refuse("the card would not take a target of %u by %u", width, height);
    }
    /* its textures, on the card before anything is begun: made from surfaces the card holds, or from memory */
    for (uint32_t i = 0; i < count; i++) {
        const queued *q = list[i];
        const gxm_draw_state *s = &q->s;
        for (int stage_at = 0; stage_at < 2; stage_at++) {
            const stage *g = stage_at ? &q->d.f : &q->d.v;
            if (stage_at && !shaders[i].fragment) continue;
            for (unsigned unit = 0; unit < GXP_TEXTURE_UNITS; unit++) {
                const gxm_tex *t = g->tex[unit];
                if (!t) continue;
                shaders[i].textures[stage_at][unit] = texture_for(t, stage_at ? s->fragment_textures[unit] : s->vertex_textures[unit]);
                if (g_from_surface) shaders[i].kept_larger[stage_at] |= 1u << unit;
                if (!shaders[i].textures[stage_at][unit])
                    return refuse("a texture the card would not take: %u by %u, %u levels%s, format 0x%08X, kind %u", t->level_w[0], t->level_h[0],
                                  t->levels, t->cube ? ", a cube" : "", t->format, t->kind);
            }
        }
    }

    /* A scene whose depth is still in memory: fetched if this list looks at it, and if not, the list is drawn beside
     * a depth surface that holds nothing and is not kept. */
    int unlooked = 0;
    if (!aside && g_scene.active && g_scene.where == IN_MEMORY) {
        int looks = 0;
        for (uint32_t i = 0; i < count && !looks; i++) looks = looks_at_depth(list[i]);
        if (looks) depth_fetched(s0);
        else unlooked = 1;
    }
    if (unlooked) {
        deep = spare_for(depth, width, height);
        if (!deep) return refuse("the card would not take a target of %u by %u", width, height);
        kept = 0;
    } else if (kept && g_scene.active && g_scene.format == depth && g_scene.width == width && g_scene.height == height) {
        if (!g_scene.card) g_scene.card = spare_for(depth, width, height);
        deep = g_scene.card;
        if (!deep) return refuse("the card would not take a target of %u by %u", width, height);
        if (g_scene.where == IN_PLANES) {
            if (g_scene.cleared) gpu_surface_clear(deep, g_scene.clear_depth, g_scene.clear_stencil);
            else {
                pack_depth(s0, depth, g_depth_rows);
                if (surface_put(deep, g_depth_rows, width, height, (uint32_t)depth_bytes)) return refuse("the card would not take a target of %u by %u", width, height);
                g_puts++;
                if (logging()) fprintf(stderr, "gxm gpu: list %u: a scene's depth of %ux%u is filled from its planes\n", g_list, width, height);
            }
        }
    } else {
        if (!aside && g_scene.active && g_scene.where == ON_CARD) planes_wanted(s0);
        deep = spare_for(depth, width, height);
        if (!deep) return refuse("the card would not take a target of %u by %u", width, height);
        g_kept_from = aside ? kept_holding(s0, depth) : NULL;
        if (g_kept_from) memcpy(g_depth_rows, g_kept_from->card, pixels * depth_bytes);
        else pack_depth(s0, depth, g_depth_rows);
        if (surface_put(deep, g_depth_rows, width, height, (uint32_t)depth_bytes)) return refuse("the card would not take a target of %u by %u", width, height);
        kept = 0;
    }
    if (gpu_begin(colour, deep)) return refuse("the card would not take a target of %u by %u", width, height);
    at = now_ns(); g_ns[P_IN] += at - lap; lap = at;
    g_lists++;
    g_draws += count;

    /* --- the draws --- */
    if (count > counts_cap) {
        uint64_t *c = realloc(counts, (size_t)count * sizeof(*c));
        if (c) counts = c;
        int *k = realloc(counted, (size_t)count * sizeof(*k));
        if (k) counted = k;
        if (!c || !k) { gpu_end(NULL, 0); return 0; }
        counts_cap = count;
    }
    for (uint32_t i = 0; i < count; i++) {
        const queued *q = list[i];
        const gxm_draw_state *s = &q->s;
        const gxm_program *vp = s->vertex, *fp = s->fragment;
        static constants c[2];
        gpu_draw_call call;
        uint32_t vertices = 0, indices = 0, pa_words = (vp->words[1] + 3u) & ~3u;
        int strip = 0;
        counted[i] = -1;
        if (pa_words > 64) pa_words = 64;
        lap = now_ns();
        gpu_mesh *mesh = NULL;
        if (!mesh_for(q, pa_words, &mesh, &vertices, &indices, &strip)) continue;
        at = now_ns(); g_ns[P_VERTICES] += at - lap; lap = at;

        memset(&call, 0, sizeof(call));
        memset(c, 0, sizeof(c));
        call.vertex = shaders[i].vertex;
        call.fragment = shaders[i].fragment;
        call.mesh = mesh;
        call.vertices = g_vertex_words;
        call.vertex_count = vertices;
        call.vertex_stride = pa_words * 4;
        call.indices = g_index_words;
        call.index_count = indices;
        call.index_size = 4;
        call.strip = (uint8_t)strip;

        for (int stage_at = 0; stage_at < 2; stage_at++) {
            const stage *g = stage_at ? &q->d.f : &q->d.v;
            const gxm_program *program = stage_at ? fp : vp;
            if (!program || (stage_at && !call.fragment)) continue;
            memcpy(c[stage_at].sa, g->sa, sizeof(c[stage_at].sa));
            for (int k = 0; k < 3; k++) {
                c[stage_at].viewport[k] = s->viewport_on ? s->scale[k] : 1.0f;
                c[stage_at].viewport[4 + k] = s->viewport_on ? s->offset[k] : 0.0f;
            }
            c[stage_at].viewport[8] = 2.0f / (float)width;
            c[stage_at].viewport[9] = 2.0f / (float)height;
            memcpy(&c[stage_at].viewport[11], &shaders[i].kept_larger[stage_at], 4);
            call.memory_words[stage_at] = memory_for(&program->gxp, stage_at ? s->fragment_buffers : s->vertex_buffers, &c[stage_at], g_memory[stage_at]);
            call.memory[stage_at] = g_memory[stage_at];
            call.constants[stage_at] = &c[stage_at];
            call.constants_bytes[stage_at] = sizeof(constants);
            for (unsigned unit = 0; unit < GXP_TEXTURE_UNITS; unit++) {
                const gxm_tex *t = g->tex[unit];
                if (!t) continue;
                call.textures[stage_at][unit] = shaders[i].textures[stage_at][unit];
                sampler_for(t, &call.samplers[stage_at][unit]);
            }
        }
        call.reads_pixel = (uint8_t)shaders[i].reads_pixel;

        call.cull = (uint8_t)s->cull;
        call.scissor[0] = q->r.clip_x0 * (int32_t)larger(); call.scissor[1] = q->r.clip_y0 * (int32_t)larger();
        call.scissor[2] = q->r.clip_x1 * (int32_t)larger(); call.scissor[3] = q->r.clip_y1 * (int32_t)larger();
        const gxm_face *front = &s->face[0], *back = &s->face[s->two_sided ? 1 : 0];
        call.depth_func = front->depth_func;
        call.depth_write = front->depth_write;
        call.stencil_read = front->compare_mask;
        call.stencil_write = front->write_mask;
        call.stencil_ref = front->stencil_ref;
        const gxm_face *faces[2] = { front, back };
        for (int k = 0; k < 2; k++) {
            call.stencil[k].func = faces[k]->stencil_func;
            call.stencil[k].fail = faces[k]->stencil_fail;
            call.stencil[k].depth_fail = faces[k]->depth_fail;
            call.stencil[k].pass = faces[k]->depth_pass;
        }
        if (fp && call.fragment) {
            /* the mask's bits are alpha, red, green, blue */
            unsigned mask = fp->blend[0];
            call.write_mask = (uint8_t)(((mask >> 1) & 7) | ((mask & 1) << 3));
            if (way.raw) call.write_mask = (uint8_t)((1u << shaders[i].raw) - 1);
            if (shaders[i].blended) {
                call.blend = 1;
                call.colour_op = fp->blend[1] & 15;
                call.alpha_op = fp->blend[1] >> 4;
                call.colour_src = fp->blend[2] & 15;
                call.colour_dst = fp->blend[2] >> 4;
                call.alpha_src = fp->blend[3] & 15;
                call.alpha_dst = fp->blend[3] >> 4;
            }
        }
        /* a program that writes the target itself is told which channels (gxp_hlsl.h) */
        if (call.reads_pixel == 2) { uint32_t keep = call.write_mask; memcpy(&c[1].viewport[10], &keep, 4); }
        call.count = aside || q->tally != 0;
        gpu_tween_about(&vp->gxp, s->vertex_buffers, c[0].memory, (q->count * 16u + q->primitive) | 0x80000000u);
        if (fp && call.fragment) gpu_tween_about_fragment(&fp->gxp, s->fragment_buffers, c[1].memory);
        counted[i] = gpu_draw(&call);
        g_ns[P_DRAW] += now_ns() - lap;
    }

    /* --- and what becomes of it --- */
    if (aside) {
        if (!bytes_for(&g_aside.colour, &g_aside.colour_cap, pixels * way.bytes + 16) ||
            !bytes_for(&g_aside.depth, &g_aside.depth_cap, pixels * depth_bytes)) { gpu_end(NULL, 0); return 0; }
        if (gpu_end(counts, count) || (way.mode && surface_get(colour, g_aside.colour, width, height, way.bytes, way.format)) ||
            surface_get(deep, g_aside.depth, width, height, (uint32_t)depth_bytes, depth)) return 0;
        g_aside.held = 1;
        g_aside.draws = count;
        g_aside.width = width; g_aside.height = height;
        g_aside.way = way;
        g_aside.depth_bytes = (uint32_t)depth_bytes;
        g_aside.depth_format = depth;
        return 0;
    }
    lap = now_ns();
    if (gpu_end(counts, count)) return refuse("the card did not finish a list of %u draws", count);
    g_ns[P_BACK] += now_ns() - lap;
    /* pixels of the title's size: as many as the card's would make up, and not none when the card drew any */
    for (uint32_t i = 0; i < count; i++)
        if (counted[i] >= 0) list[i]->pixels = (uint32_t)((counts[counted[i]] + larger() * larger() - 1) / (larger() * larger()));
    /* the card's is the surface now, and the title's memory is behind */
    if (record && whole && !gpu_surface_copy_corner(whole->card, colour, width * larger(), height * larger())) {
        /* drawn through the larger surface it is a corner of: that one has it, and this one holds nothing of its own */
        whole->version++;
        whole->drawn = g_list;
        whole->since = vita_mem_moment();
        record->agreed = 0;
    } else if (record) {
        record->newer = 1;
        record->agreed = 0;
        record->version++;
        record->drawn = g_list;
        record->since = vita_mem_moment();
        /* A title's own code reads what it has drawn, too, and nothing says when: the brightness a frame came out at,
         * off a surface of a few pixels, to set the next frame's exposure by. What the card holds of a surface that
         * small is put in memory at once; it costs little, and there is no telling otherwise. */
        if (!keeping() || (size_t)(record->hi - record->lo) <= SMALL_BYTES) colour_back(record);
    }
    if (unlooked) {
        /* its depth is where it was, in memory: the list neither read it nor changed it */
    } else if (kept) {
        g_scene.where = ON_CARD;
        g_scene.cleared = 0;
    } else {
        lap = now_ns();
        if (surface_get(deep, g_depth_rows, width, height, (uint32_t)depth_bytes, depth)) return refuse("the card did not give back a list's depth");
        at = now_ns(); g_ns[P_BACK] += at - lap;
        unpack_depth(g_depth_rows, depth, width, height, s0->depth, s0->stencil);
        g_ns[P_OUT] += now_ns() - at;
        if (g_scene.active) { g_scene.where = IN_PLANES; g_scene.cleared = 0; }
    }
    return 1;
}

/* --- the two ways, held against each other ---------------------------------------------------- */

/* How far apart two numbers of a channel may be and still be one result
 * rounded two ways: a step and a half of what the channel's field can tell
 * apart at their size, and never less than a step and a half of a byte. */
static float allowed(const pixfmt *f, int channel, float x, float y) {
    float ax = x < 0.0f ? -x : x, ay = y < 0.0f ? -y : y, size = ax > ay ? ax : ay;
    float least = 1.5f / 255.0f * (size > 1.0f ? size : 1.0f), step = 0.0f;
    int field = f ? f->from[channel] : -1, mantissa = 0;
    if (field < 0) return least;
    switch (f->f[field].kind) {
    case PIX_UNORM: step = 1.0f / (float)((1u << f->f[field].bits) - 1); break;
    case PIX_SNORM: step = 1.0f / (float)((1u << (f->f[field].bits - 1)) - 1); break;
    case PIX_F16:   mantissa = 10; break;
    case PIX_UF11:  mantissa = 6; break;
    case PIX_UF10:  mantissa = 5; break;
    case PIX_SE9:   mantissa = 9; break;
    default:        break;
    }
    if (mantissa && size > 0.0f && size < 1e30f) {
        int exponent;
        frexpf(size, &exponent);                        /* size = m * 2^exponent, m in [0.5, 1) */
        step = ldexpf(1.0f, exponent - 1 - mantissa);
    }
    return 1.5f * step > least ? 1.5f * step : least;
}

static float against(const pixfmt *f, int channel, float x, float y) {
    if (x != x || y != y) return x != x && y != y ? 0.0f : 1e30f;
    float d = x - y;
    if (d < 0.0f) d = -d;
    return d / allowed(f, channel, x, y);
}

/* How far apart two pixels are, the card's and the processor's, both as the
 * card has them: 0 when they are the same, 1 when they are as far apart as
 * rounding alone can leave them. */
static float apart(const gxm_draw_state *s, const colour_way *way, const uint8_t *a, const uint8_t *b) {
    float most = 0.0f, x[4], y[4];
    if (!memcmp(a, b, way->bytes)) return 0.0f;
    if (way->raw) {
        /* words: their bytes, each a number that may be signed, so 255 is a step from 0 */
        for (uint32_t k = 0; k < way->bytes; k++) {
            unsigned d = a[k] > b[k] ? (unsigned)(a[k] - b[k]) : (unsigned)(b[k] - a[k]);
            if (d > 128) d = 256 - d;
            if ((float)d / 1.5f > most) most = (float)d / 1.5f;
        }
        return most;
    }
    if (way->mode == AS_FLOATS) {
        /* the card's floats are not yet what the surface would hold: put through its fields */
        uint8_t px[16] = { 0 };
        memcpy(x, a, sizeof(x));
        memcpy(y, b, sizeof(y));
        if (s->colour_gamma) for (int k = 0; k < 3; k++) x[k] = pix_linear_to_srgb(x[k]);
        pixfmt_encode(&s->colour, px, x, 15);
        pixfmt_decode(&s->colour, px, x);
        if (s->colour_gamma) for (int k = 0; k < 3; k++) x[k] = pix_srgb_to_linear(x[k]);
    } else {
        pixfmt_decode(&s->colour, a, x);
        pixfmt_decode(&s->colour, b, y);
    }
    for (int k = 0; k < 4; k++) {
        if (s->colour.from[k] < 0) continue;            /* a channel the surface does not keep */
        float d = against(&s->colour, k, x[k], y[k]);
        if (d > most) most = d;
    }
    return most;
}

/* A pixel as the card has it, for a picture: three bytes. */
static void shown(const gxm_draw_state *s, const colour_way *way, const uint8_t *px, uint8_t rgb[3]) {
    float c[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (way->raw) { rgb[0] = px[0]; rgb[1] = px[1]; rgb[2] = px[2]; return; }
    if (way->mode == AS_FLOATS) {
        memcpy(c, px, sizeof(c));
        if (s->colour_gamma) for (int k = 0; k < 3; k++) c[k] = pix_linear_to_srgb(c[k]);
    } else {
        pixfmt_decode(&s->colour, px, c);
    }
    for (int k = 0; k < 3; k++) rgb[k] = (uint8_t)((!(c[k] > 0.0f) ? 0.0f : c[k] > 1.0f ? 1.0f : c[k]) * 255.0f + 0.5f);
}

/* What the checking came to, said once when the program ends. */
static uint32_t lists, differing;

static void checked(void) {
    fprintf(stderr, "gxm gpu: %u lists checked, %u differed; %u were not the card's to draw\n", lists, differing, g_refused);
}

void gxm_gpu_compare(queued **list, uint32_t count) {
    static int told;
    if (!told) { told = 1; atexit(checked); }
    const gxm_draw_state *s = &list[0]->s;
    const colour_way *way = &g_aside.way;
    if (!g_aside.held || g_aside.width != s->width || g_aside.height != s->height) return;
    g_aside.held = 0;
    lists++;

    size_t pixels = (size_t)s->width * s->height, colour_off = 0, colour_far = 0, depth_off = 0, stencil_off = 0;
    float worst = 0.0f, worst_depth = 0.0f;
    uint32_t worst_x = 0, worst_y = 0;
    if (way->mode) {
        /* what the processor has just drawn, put the way the card's result is */
        if (!bytes_for(&g_colour_rows, &g_colour_rows_cap, pixels * way->bytes + 16)) return;
        colour_in(s, way, g_colour_rows);
        for (size_t i = 0; i < pixels; i++) {
            float d = apart(s, way, g_aside.colour + i * way->bytes, g_colour_rows + i * way->bytes);
            if (d > 1.0f) colour_off++;
            if (d > 4.0f) colour_far++;
            if (d > worst) { worst = d; worst_x = (uint32_t)(i % s->width); worst_y = (uint32_t)(i / s->width); }
        }
    }
    static float *depth;
    static uint8_t *stencil;
    static size_t cap;
    if (pixels > cap) {
        float *d = realloc(depth, pixels * sizeof(float));
        if (d) depth = d;
        uint8_t *t = realloc(stencil, pixels);
        if (t) stencil = t;
        if (!d || !t) return;
        cap = pixels;
    }
    memcpy(stencil, s->stencil, pixels);                /* where the card keeps none, there is none to differ */
    unpack_depth(g_aside.depth, g_aside.depth_format, s->width, s->height, depth, stencil);
    keep_depth(s, lists);
    for (size_t i = 0; i < pixels; i++) {
        float d = depth[i] - s->depth[i];
        if (d < 0.0f) d = -d;
        if (d > 1.0f / 65536.0f) depth_off++;
        if (d > worst_depth) worst_depth = d;
        if (stencil[i] != s->stencil[i]) stencil_off++;
    }

    /* Two rasterisers part along edges, and two samplers where a texture
     * is between two of its levels: a few pixels far apart, and more that
     * are a few steps apart, are their business. More than that is this
     * file's. The colours are measured against what the list drew; the
     * depth and the stencil against the surface, since the card's are its
     * own from list to list and what earlier lists left apart stays so. */
    size_t drawn = 0;
    for (uint32_t i = 0; i < count; i++) drawn += list[i]->pixels;
    if (drawn > pixels) drawn = pixels;
    size_t allowed = drawn / 200 + 16, kept_apart = pixels / 500 + 16;
    if (colour_far <= allowed && colour_off <= drawn / 3 + 16 && depth_off <= kept_apart && stencil_off <= kept_apart) return;
    differing++;
    if (differing > 200) return;

    /* VITARECOMP_GXM_GPU_DUMP=<directory>: the first lists that differ as
     * pictures, the card's and the processor's side by side in name: a
     * colour's red, green and blue held to 0..1, or a surface of words by
     * the bytes of its first word; the depth, white far; and where the
     * colours are apart: grey within what rounding leaves, yellow up to
     * four times that, red beyond. */
    const char *dir = getenv("VITARECOMP_GXM_GPU_DUMP");
    if (dir && *dir && differing <= 24) {
        for (int which = 0; which < 5; which++) {
            char path[1024];
            static const char *const NAME[5] = { "card-colour", "here-colour", "card-depth", "here-depth", "apart" };
            if ((which < 2 || which == 4) && !way->mode) continue;
            snprintf(path, sizeof(path), "%s/list-%04u-%s.ppm", dir, lists, NAME[which]);
            FILE *f = fopen(path, "wb");
            if (!f) continue;
            fprintf(f, "P6\n%u %u\n255\n", s->width, s->height);
            for (size_t i = 0; i < pixels; i++) {
                uint8_t rgb[3];
                if (which == 4) {
                    float d = apart(s, way, g_aside.colour + i * way->bytes, g_colour_rows + i * way->bytes);
                    rgb[0] = d > 1.0f ? 255 : d > 0.0f ? 96 : 0;
                    rgb[1] = d > 4.0f ? 0 : d > 1.0f ? 224 : d > 0.0f ? 96 : 0;
                    rgb[2] = d > 1.0f ? 0 : d > 0.0f ? 96 : 0;
                } else if (which >= 2) {
                    float v = which == 2 ? depth[i] : s->depth[i];
                    rgb[0] = rgb[1] = rgb[2] = (uint8_t)((!(v > 0.0f) ? 0.0f : v > 1.0f ? 1.0f : v) * 255.0f + 0.5f);
                } else {
                    shown(s, way, (which == 0 ? g_aside.colour : g_colour_rows) + i * way->bytes, rgb);
                }
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
        }
        /* VITARECOMP_GXM_GPU_DUMP_RAW=1 as well: the numbers themselves, for
         * working a pixel out by hand. Both results as the card holds them
         * (.raw: width, height and bytes a pixel as three words, then the
         * pixels); the first draw's textures, first level, four floats a
         * texel (.f32: width and height, then the texels); and its two
         * stages' secondary attributes as words (.txt). */
        const char *raw = getenv("VITARECOMP_GXM_GPU_DUMP_RAW");
        if (raw && *raw && *raw != '0') {
            char path[1024];
            const queued *q = list[0];
            for (int which = 0; which < 2 && way->mode; which++) {
                uint32_t head[3] = { s->width, s->height, way->bytes };
                snprintf(path, sizeof(path), "%s/list-%04u-%s.raw", dir, lists, which ? "here" : "card");
                FILE *f = fopen(path, "wb");
                if (!f) continue;
                fwrite(head, 4, 3, f);
                fwrite(which ? g_colour_rows : g_aside.colour, way->bytes, pixels, f);
                fclose(f);
            }
            for (unsigned unit = 0; unit < GXP_TEXTURE_UNITS; unit++) {
                const gxm_tex *t = q->d.f.tex[unit];
                if (!t || !t->level_w[0] || !t->level_h[0]) continue;
                float *texels = malloc((size_t)t->level_w[0] * t->level_h[0] * 4 * sizeof(float));
                if (!texels) continue;
                gxm_tex_level_floats(t, NULL, 0, 0, texels);
                snprintf(path, sizeof(path), "%s/list-%04u-unit-%02u.f32", dir, lists, unit);
                FILE *f = fopen(path, "wb");
                if (f) {
                    uint32_t head[2] = { t->level_w[0], t->level_h[0] };
                    fwrite(head, 4, 2, f);
                    fwrite(texels, 16, (size_t)head[0] * head[1], f);
                    fclose(f);
                }
                free(texels);
            }
            snprintf(path, sizeof(path), "%s/list-%04u.txt", dir, lists);
            FILE *f = fopen(path, "w");
            if (f) {
                for (int stage_at = 0; stage_at < 2; stage_at++) {
                    const stage *g = stage_at ? &q->d.f : &q->d.v;
                    fprintf(f, "%s", stage_at ? "fragment" : "vertex");
                    for (int w = 0; w < 64; w++) fprintf(f, " %08x", g->sa[w]);
                    fprintf(f, "\n");
                }
                for (unsigned v = 0; v < q->first_count; v++)
                    fprintf(f, "corner %g %g %g %g\n", (double)q->first[v][0], (double)q->first[v][1], (double)q->first[v][2], (double)q->first[v][3]);
                fclose(f);
            }
        }
    }
    fprintf(stderr, "gxm gpu: list %u (%u draws, %zu pixels of %ux%u) differs: colour at %zu pixels, %zu of them by over four times what rounding leaves "
                    "(worst %.1f times, at %u,%u), depth at %zu (worst %.6f), stencil at %zu\n",
            lists, count, drawn, s->width, s->height, colour_off, colour_far, (double)worst, worst_x, worst_y, depth_off, (double)worst_depth, stencil_off);
    if (way->mode == AS_FLOATS) {
        float a[4], b[4];
        size_t at = ((size_t)worst_y * s->width + worst_x) * way->bytes;
        memcpy(a, g_aside.colour + at, sizeof(a));
        memcpy(b, g_colour_rows + at, sizeof(b));
        fprintf(stderr, "    there the card has %g %g %g %g and the processor %g %g %g %g\n", (double)a[0], (double)a[1], (double)a[2],
                (double)a[3], (double)b[0], (double)b[1], (double)b[2], (double)b[3]);
    } else if (way->mode) {
        const uint8_t *a = g_aside.colour + ((size_t)worst_y * s->width + worst_x) * way->bytes;
        const uint8_t *b = g_colour_rows + ((size_t)worst_y * s->width + worst_x) * way->bytes;
        fprintf(stderr, "    there the card has");
        for (uint32_t k = 0; k < way->bytes; k++) fprintf(stderr, " %02x", a[k]);
        fprintf(stderr, " and the processor");
        for (uint32_t k = 0; k < way->bytes; k++) fprintf(stderr, " %02x", b[k]);
        fprintf(stderr, "\n");
    }
    for (uint32_t i = 0; i < count && i < 12; i++) {
        const gxm_draw_state *d = &list[i]->s;
        const gxm_face *f = &d->face[0];
        fprintf(stderr, "    draw %u, programs %016llx/%016llx: ", i, (unsigned long long)d->vertex->name,
                (unsigned long long)(d->fragment ? d->fragment->name : 0));
        if (g_aside.draws == count && counted[i] >= 0) fprintf(stderr, "%llu pixels on the card", (unsigned long long)counts[counted[i]]);
        else fprintf(stderr, "not drawn on the card");
        fprintf(stderr, ", %u on the processor; depth test %u%s, stencil test %u against %u under 0x%02X, writing under 0x%02X, cull %u%s\n",
                list[i]->pixels, f->depth_func, f->depth_write ? " and written" : "", f->stencil_func, f->stencil_ref, f->compare_mask,
                f->write_mask, d->cull, d->two_sided ? ", two faces" : "");
    }
    if (count > 12) fprintf(stderr, "    and %u more draws\n", count - 12);
}
