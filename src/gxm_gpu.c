/* gxm_gpu.c — a scene's list of draws, drawn by the host's graphics card. See gxm_gpu.h.
 *
 * What this file decides is whether a list is one the card's path can draw
 * as the processor's does, and if it is, what to hand over: for each draw
 * its two programs as shaders for what is bound, its vertices as the words
 * the vertex program would find in its registers, its secondary attributes
 * as they stand after the secondary program, its textures, and the
 * console's state in the card's terms. Anything it is not sure of sends the
 * whole list to the processor, with the reason said once.
 */

#include "gxm_gpu.h"
#include "gpu.h"
#include "gxp_hlsl.h"
#include "vitarecomp/mem.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int refuse(const char *fmt, ...) {
    static char seen[48][200];
    static unsigned count;
    g_refused++;
    char why[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof(why), fmt, ap);
    va_end(ap);
    for (unsigned i = 0; i < count; i++)
        if (!strcmp(seen[i], why)) return 0;
    if (count < 48) {
        memcpy(seen[count++], why, sizeof(why));
        fprintf(stderr, "gxm gpu: a list is left to the processor: %s\n", why);
    }
    return 0;
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
        char *text = gxp_hlsl(&program->gxp, bind, why, sizeof(why));
        s->used = 1;
        s->name = program->name;
        s->bind = *bind;
        s->shader = NULL;
        if (!text) {
            refuse("program %016llx is not translated: it has %s", (unsigned long long)program->name, why);
            return NULL;
        }
        s->shader = gpu_shader_make(fragment, text, bind->pa_words, log, sizeof(log));
        if (!s->shader) refuse("program %016llx did not compile: %.120s", (unsigned long long)program->name, log);
        const char *dump = getenv("VITARECOMP_GXM_GPU_SHADERS");
        if (dump && *dump) {                        /* the text, for reading: a file a program and binding */
            char path[1024];
            snprintf(path, sizeof(path), "%s/%c_%016llx_%08x.hlsl", dump, fragment ? 'f' : 'v', (unsigned long long)program->name, (unsigned)h);
            FILE *f = fopen(path, "wb");
            if (f) { fputs(text, f); fclose(f); }
        }
        free(text);
        return s->shader;
    }
    refuse("more programs than there is room to keep");
    return NULL;
}

/* --- textures, kept on the card until their bytes change ------------------------------------- */

typedef struct {
    uint32_t words[4], data, palette;
    uint64_t stamp;
    uint32_t checked, used;                 /* the list it was last compared in, and drawn with */
    gpu_texture *texture;
} texture_slot;

#define TEXTURES 2048
static texture_slot g_textures[TEXTURES];
static unsigned g_texture_count;
static uint32_t g_list = 1;                 /* counts lists */

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

static gpu_texture *upload(const gxm_tex *t) {
    const void *images[6 * GXM_TEX_LEVELS];
    unsigned faces = t->cube ? 6 : 1, levels = t->levels < GXM_TEX_LEVELS ? t->levels : GXM_TEX_LEVELS, made = 0;
    gpu_texture *out = NULL;
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
    out = gpu_texture_make(GPU_RGBA32F, t->level_w[0], t->level_h[0], levels, t->cube, images);
    {
        static int asked, wanted;
        if (!asked) { const char *e = getenv("VITARECOMP_GXM_GPU_LOG"); asked = 1; wanted = e && *e && *e != '0'; }
        if (wanted)
            fprintf(stderr, "gxm gpu: texture at 0x%08X: %u by %u, %u of %u levels (second level %u by %u)%s, format 0x%08X kind %u, "
                            "filters %u %u %u, lod bias %g min %u, modes %u %u: %s\n", t->data, t->level_w[0], t->level_h[0], levels, t->levels,
                    t->levels > 1 ? t->level_w[1] : 0, t->levels > 1 ? t->level_h[1] : 0, t->cube ? ", a cube" : "", t->format, t->kind,
                    t->min_filter, t->mag_filter, t->mip_filter, (double)t->lod_bias, t->lod_min, t->u_mode, t->v_mode, out ? "made" : "REFUSED");
    }
done:
    for (unsigned i = 0; i < made; i++) free((void *)images[i]);
    return out;
}

static gpu_texture *texture_for(const gxm_tex *t, const uint32_t words[4]) {
    texture_slot *slot = NULL;
    for (unsigned i = 0; i < g_texture_count; i++) {
        texture_slot *s = &g_textures[i];
        if (s->data == t->data && s->palette == t->palette && !memcmp(s->words, words, sizeof(s->words))) { slot = s; break; }
    }
    /* nothing is written while a list is being made ready: once a list is enough */
    if (slot && slot->checked == g_list) { slot->used = g_list; return slot->texture; }
    uint64_t stamp = content_stamp(t);
    if (slot && slot->stamp == stamp) { slot->checked = slot->used = g_list; return slot->texture; }
    if (!slot) {
        if (g_texture_count == TEXTURES) {          /* room: the one longest unused goes */
            unsigned oldest = 0;
            for (unsigned i = 1; i < TEXTURES; i++) if (g_textures[i].used < g_textures[oldest].used) oldest = i;
            gpu_texture_free(g_textures[oldest].texture);
            g_textures[oldest] = g_textures[--g_texture_count];
        }
        slot = &g_textures[g_texture_count++];
        memset(slot, 0, sizeof(*slot));
        memcpy(slot->words, words, sizeof(slot->words));
        slot->data = t->data;
        slot->palette = t->palette;
    }
    gpu_texture_free(slot->texture);
    slot->texture = upload(t);
    slot->stamp = stamp;
    slot->checked = slot->used = g_list;
    return slot->texture;
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
} programs;

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
    }
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

/* The scene's depth and stencil planes as the card's depth texture lies. */
static void pack_depth(const gxm_draw_state *s, unsigned format, uint8_t *out) {
    size_t pixels = (size_t)s->width * s->height;
    if (format == GPU_D24S8) {
        for (size_t i = 0; i < pixels; i++) {
            double scaled = (double)s->depth[i] * 16777215.0 + 0.5;
            uint32_t w = (scaled >= 16777215.0 ? 0xFFFFFFu : scaled > 0.0 ? (uint32_t)scaled : 0u) | ((uint32_t)s->stencil[i] << 24);
            memcpy(out + i * 4, &w, 4);
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
    } else {
        for (size_t i = 0; i < pixels; i++) {
            memcpy(&depth[i], in + i * 8, 4);
            stencil[i] = in[i * 8 + 4];
        }
    }
}

/* What the card says each draw of the last list put on the screen: the
 * place of its count among those the list asked for, or -1. */
static uint64_t *counts;
static int *counted;
static uint32_t counts_cap;

int gxm_gpu_list(queued **list, uint32_t count, int aside) {
    const gxm_draw_state *s0 = &list[0]->s;
    uint32_t width = s0->width, height = s0->height;
    unsigned depth = s0->depth_bits == 24 ? GPU_D24S8 : s0->depth_bits == 0 ? GPU_D32FS8 : GPU_NONE;
    colour_way way;
    static programs *shaders;
    static uint32_t shaders_cap;
    g_aside.held = 0;
    g_list++;

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
        if (s->region_mode == GXM_REGION_INSIDE) return refuse("a draw clipped to the outside of a region");
        if (!shaders_for(q, way.raw, way.format, &shaders[i])) return 0;
        const gxm_program *fp = s->fragment;
        shaders[i].blended = 0;
        /* its textures, on the card before anything is begun */
        for (int stage_at = 0; stage_at < 2; stage_at++) {
            const stage *g = stage_at ? &q->d.f : &q->d.v;
            if (stage_at && !shaders[i].fragment) continue;
            for (unsigned unit = 0; unit < GXP_TEXTURE_UNITS; unit++) {
                const gxm_tex *t = g->tex[unit];
                if (!t) continue;
                shaders[i].textures[stage_at][unit] = texture_for(t, stage_at ? s->fragment_textures[unit] : s->vertex_textures[unit]);
                if (!shaders[i].textures[stage_at][unit])
                    return refuse("a texture the card would not take: %u by %u, %u levels%s, format 0x%08X, kind %u", t->level_w[0], t->level_h[0],
                                  t->levels, t->cube ? ", a cube" : "", t->format, t->kind);
            }
        }
        if (shaders[i].fragment && fp && !(fp->gxp.flags & GXP_FLAG_NATIVECOLOR) && ((fp->blend[1] & 15) || (fp->blend[1] >> 4))) {
            if (way.raw) return refuse("blending into a surface kept as words");
            if ((fp->blend[2] & 15) > 10 || (fp->blend[2] >> 4) > 10 || (fp->blend[3] & 15) > 10 || (fp->blend[3] >> 4) > 10)
                return refuse("a blend factor the card has no equal for");
            shaders[i].blended = 1;
        }
    }

    /* --- the target, as the title's memory has it --- */
    gpu_target target;
    size_t depth_bytes = depth == GPU_D24S8 ? 4 : 8, pixels = (size_t)width * height;
    memset(&target, 0, sizeof(target));
    target.width = width;
    target.height = height;
    if (!bytes_for(&g_colour_rows, &g_colour_rows_cap, pixels * way.bytes + 16) || !bytes_for(&g_depth_rows, &g_depth_rows_cap, pixels * depth_bytes)) return 0;
    if (way.mode) {
        colour_in(s0, &way, g_colour_rows);
        target.colour_format = way.format;
        target.colour = g_colour_rows;
        target.colour_pitch = width * way.bytes;
    }
    g_kept_from = aside ? kept_holding(s0, depth) : NULL;
    if (g_kept_from) memcpy(g_depth_rows, g_kept_from->card, pixels * depth_bytes);
    else pack_depth(s0, depth, g_depth_rows);
    target.depth_format = depth;
    target.depth = g_depth_rows;
    target.depth_pitch = (uint32_t)(width * depth_bytes);
    if (gpu_begin(&target)) return refuse("the card would not take a target of %u by %u", width, height);

    /* --- the draws --- */
    if (count > counts_cap) {
        uint64_t *c = realloc(counts, (size_t)count * sizeof(*c));
        if (c) counts = c;
        int *k = realloc(counted, (size_t)count * sizeof(*k));
        if (k) counted = k;
        if (!c || !k) { gpu_end(NULL, 0, NULL, 0, NULL, 0); return 0; }
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
        if (!geometry(q, pa_words, &vertices, &indices, &strip)) continue;

        memset(&call, 0, sizeof(call));
        memset(c, 0, sizeof(c));
        call.vertex = shaders[i].vertex;
        call.fragment = shaders[i].fragment;
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
        call.scissor[0] = q->r.clip_x0; call.scissor[1] = q->r.clip_y0;
        call.scissor[2] = q->r.clip_x1; call.scissor[3] = q->r.clip_y1;
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
        counted[i] = gpu_draw(&call);
    }

    /* --- and back --- */
    uint32_t colour_pitch = width * way.bytes, depth_pitch = (uint32_t)(width * depth_bytes);
    if (aside) {
        if (!bytes_for(&g_aside.colour, &g_aside.colour_cap, pixels * way.bytes + 16) ||
            !bytes_for(&g_aside.depth, &g_aside.depth_cap, pixels * depth_bytes)) { gpu_end(NULL, 0, NULL, 0, NULL, 0); return 0; }
        if (gpu_end(way.mode ? g_aside.colour : NULL, colour_pitch, g_aside.depth, depth_pitch, counts, count)) return 0;
        g_aside.held = 1;
        g_aside.draws = count;
        g_aside.width = width; g_aside.height = height;
        g_aside.way = way;
        g_aside.depth_bytes = (uint32_t)depth_bytes;
        g_aside.depth_format = depth;
        return 0;
    }
    if (gpu_end(way.mode ? g_colour_rows : NULL, colour_pitch, g_depth_rows, depth_pitch, counts, count))
        return refuse("the card did not finish a list of %u draws", count);
    /* everything arrived: only now is anything of the title's changed */
    if (way.mode) colour_out(s0, &way, g_colour_rows);
    unpack_depth(g_depth_rows, depth, width, height, s0->depth, s0->stencil);
    for (uint32_t i = 0; i < count; i++)
        if (counted[i] >= 0) list[i]->pixels = (uint32_t)counts[counted[i]];
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
