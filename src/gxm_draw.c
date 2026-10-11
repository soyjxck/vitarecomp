/* gxm_draw.c — one draw call, from index buffer to pixels. See gxm_draw.h. */

#include "gxm_draw.h"
#include "gxm_tex.h"
#include "half_rows.h"
#include "pool.h"
#include "raster.h"
#include "texdec.h"
#include "usse.h"
#include "vitarecomp/half.h"
#include "vitarecomp/mem.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

gxm_draw_counters gxm_counters;

static float bits_to_float(uint32_t u) { float f; memcpy(&f, &u, sizeof(f)); return f; }
static uint32_t float_to_bits(float f) { uint32_t u; memcpy(&u, &f, sizeof(u)); return u; }
static uint32_t halves(float lo, float hi) { return (uint32_t)vita_f32_to_f16(lo) | ((uint32_t)vita_f32_to_f16(hi) << 16); }

uint32_t gxm_pixel_offset(uint32_t layout, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                          uint32_t stride, uint32_t bytes) {
    if (layout == GXM_SURFACE_TILED)
        return (((y >> 5) * (stride / 32) + (x >> 5)) * 1024 + (y & 31) * 32 + (x & 31)) * bytes;
    if (layout == GXM_SURFACE_SWIZZLED)
        return tex_morton_index(x, y, width, height) * bytes;
    return (y * stride + x) * bytes;
}

/* --- one stage of a draw: a program, its uniforms, its textures ----------------------- */

typedef struct {
    const gxp_program *p;
    uint32_t  sa[USSE_WORDS];
    const gxm_tex *tex[GXP_TEXTURE_UNITS];  /* by unit; NULL where nothing usable is bound */
    gxm_tex_cache *cache;                   /* one a unit, belonging to whichever thread runs this */
    gxm_draw_counters *counters;            /* likewise */
    usse_env  env;
    usse_state st;
} stage;

/* The textures of both stages of the draw in progress. A draw is never
 * entered twice; its threads read these and write none of them. */
static gxm_tex g_textures[2][GXP_TEXTURE_UNITS];
static gxm_tex_cache g_vertex_cache[GXP_TEXTURE_UNITS];

static uint32_t env_read32(void *user, uint32_t address) {
    (void)user;
    return vita_mem_ptr(address, 4) ? vita_read32(address) : 0;
}

/* The unit a sample instruction means, by the register it names; 16 for none. */
static unsigned sampler(const stage *g, uint32_t sa_register) {
    for (uint32_t i = 0; i < g->p->sampler_count; i++)
        if (g->p->samplers[i].reg == sa_register && g->p->samplers[i].unit < GXP_TEXTURE_UNITS)
            return g->tex[g->p->samplers[i].unit] ? g->p->samplers[i].unit : GXP_TEXTURE_UNITS;
    return GXP_TEXTURE_UNITS;
}

/* A sample the program asks for itself. There are no neighbouring pixels
 * to measure a footprint from, so it comes from the first level unless the
 * instruction names another. */
static int env_sample(void *user, uint32_t sa_register, const float *coord, int dimensions,
                      int lod_mode, float lod, float rgba[4]) {
    stage *g = user;
    unsigned unit = sampler(g, sa_register);
    (void)dimensions;
    if (unit == GXP_TEXTURE_UNITS) { g->counters->no_texture++; rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.0f; return 4; }
    gxm_tex_sample(g->tex[unit], g->cache ? &g->cache[unit] : NULL, coord, lod_mode ? lod : 0.0f, rgba);
    return g->tex[unit]->pf.components;
}

static void env_gather(void *user, uint32_t sa_register, const float *coord, float texels[4], float fraction[2]) {
    stage *g = user;
    unsigned unit = sampler(g, sa_register);
    if (unit == GXP_TEXTURE_UNITS) { g->counters->no_texture++; memset(texels, 0, 4 * sizeof(float)); fraction[0] = fraction[1] = 0.0f; return; }
    gxm_tex_gather(g->tex[unit], g->cache ? &g->cache[unit] : NULL, coord, texels, fraction);
}

static void run(stage *g, const usse_insn *code, uint32_t count) {
    g->st.killed = 0;
    usse_reset(&g->st);
    uint32_t at = usse_run(code, count, &g->st);
    if (at) { g->counters->stopped++; g->counters->stopped_at = at; }
}

/* --- compressed textures, decoded once ---------------------------------------------------
 * A texel of a compressed texture costs a block's decoding, and a PVRTC
 * block is decoded from nine words. Sampled as a scene samples them, eight
 * texels to a pixel from blocks all over the texture, that was most of a
 * frame. So a compressed texture is decoded whole the first time a draw
 * binds it, every level, and kept. Its bytes are the title's and can
 * change under it: once a frame, a hash of some of them is compared with
 * what it was, and the texture decoded again if it differs. */

#define UNPACKED_LIMIT ((size_t)768 << 20)      /* bytes kept before the least recently used go */
#define UNPACK_ROWS    8                        /* rows of blocks a thread takes at a time */

typedef struct {
    uint32_t  data, format, sides, shape;       /* what it is: where, of what, how large, laid out how */
    uint64_t  stamp;                            /* the hash of its bytes it was decoded from */
    uint32_t  checked, used;                    /* the frames it was last compared and last drawn with */
    uint32_t *level[GXM_TEX_LEVELS];
    size_t    bytes;
} unpacked;

static unpacked *g_unpacked;
static uint32_t g_unpacked_count, g_unpacked_cap, g_frame = 1;
static size_t g_unpacked_bytes;

void gxm_draw_frame(void) { g_frame++; }

static uint64_t stamp(const gxm_tex *t) {
    uint32_t n = t->bytes < t->avail ? t->bytes : t->avail;
    uint64_t h = 1469598103934665603ull ^ n, word;
    if (n < 16) return h;
    /* all of a small one; of a large one, 256 places spread over it */
    uint32_t places = n <= 4096 ? n / 8 : 256, step = n <= 4096 ? 8 : (n - 8) / 255;
    for (uint32_t i = 0; i < places; i++) {
        memcpy(&word, t->base + (size_t)i * step, 8);
        h = (h ^ word) * 1099511628211ull;
    }
    return h;
}

typedef struct { const gxm_tex *t; unsigned face, level; uint32_t *out; } unpack_job;

static void unpack_rows(void *ctx, unsigned item, unsigned worker) {
    const unpack_job *job = ctx;
    (void)worker;
    gxm_tex_unpack(job->t, job->face, job->level, item * UNPACK_ROWS, (item + 1) * UNPACK_ROWS, job->out);
}

static void unpack_all(const gxm_tex *t, unpacked *e) {
    unsigned faces = t->cube ? 6 : 1;
    for (unsigned level = 0; level < t->levels; level++)
        for (unsigned face = 0; face < faces; face++) {
            unpack_job job = { t, face, level, e->level[level] + (size_t)face * t->level_w[level] * t->level_h[level] };
            uint32_t rows = t->store_h[level];
            if (rows <= 2 * UNPACK_ROWS) gxm_tex_unpack(t, face, level, 0, rows, job.out);
            else vita_pool_run((rows + UNPACK_ROWS - 1) / UNPACK_ROWS, unpack_rows, &job);
        }
    e->stamp = stamp(t);
}

/* Give a compressed texture its decoded levels, decoding them if this is
 * the first draw to bind it or its bytes have changed. */
static void attach_unpacked(gxm_tex *t) {
    uint32_t shape = t->type | (t->levels << 8) | ((uint32_t)t->no_chain << 16), sides = t->width | (t->height << 16);
    unsigned faces = t->cube ? 6 : 1;
    unpacked *e = NULL;
    for (uint32_t i = 0; i < g_unpacked_count; i++) {
        unpacked *u = &g_unpacked[i];
        if (u->data == t->data && u->format == t->format && u->sides == sides && u->shape == shape) { e = u; break; }
    }
    if (!e) {
        size_t texels = 0;
        for (unsigned level = 0; level < t->levels; level++) texels += (size_t)faces * t->level_w[level] * t->level_h[level];
        /* make room: what no draw of this frame or the last has used goes first */
        while (g_unpacked_bytes + texels * 4 > UNPACKED_LIMIT && g_unpacked_count) {
            uint32_t oldest = 0;
            for (uint32_t i = 1; i < g_unpacked_count; i++) if (g_unpacked[i].used < g_unpacked[oldest].used) oldest = i;
            if (g_unpacked[oldest].used + 1 >= g_frame) break;
            g_unpacked_bytes -= g_unpacked[oldest].bytes;
            free(g_unpacked[oldest].level[0]);
            g_unpacked[oldest] = g_unpacked[--g_unpacked_count];
        }
        if (g_unpacked_count == g_unpacked_cap) {
            uint32_t cap = g_unpacked_cap ? g_unpacked_cap * 2 : 256;
            unpacked *grown = realloc(g_unpacked, (size_t)cap * sizeof(*grown));
            if (!grown) return;
            g_unpacked = grown; g_unpacked_cap = cap;
        }
        uint32_t *texel = malloc(texels * 4 + 4);
        if (!texel) return;
        e = &g_unpacked[g_unpacked_count++];
        memset(e, 0, sizeof(*e));
        e->data = t->data; e->format = t->format; e->sides = sides; e->shape = shape;
        e->bytes = texels * 4;
        g_unpacked_bytes += e->bytes;
        for (unsigned level = 0; level < t->levels; level++) {
            e->level[level] = texel;
            texel += (size_t)faces * t->level_w[level] * t->level_h[level];
        }
        unpack_all(t, e);
        e->checked = g_frame;
    } else if (e->checked != g_frame) {
        e->checked = g_frame;
        if (stamp(t) != e->stamp) unpack_all(t, e);
    }
    e->used = g_frame;
    for (unsigned level = 0; level < t->levels; level++) t->plain[level] = e->level[level];
}

/* VITARECOMP_GXM_TEXTURES=<directory>: each texture a draw samples is written
 * there once as an image of its first level, named by where its texels are
 * and what they are, with a line on stderr saying how it is laid out. The
 * first thing to look at when a surface comes out wrong is whether the
 * texture on it was read right. */
static void dump_texture(const gxm_tex *t, unsigned unit, int fragment) {
    static const char *dir;
    static int asked;
    static uint32_t seen[4096][3], count;
    if (!asked) { asked = 1; dir = getenv("VITARECOMP_GXM_TEXTURES"); }
    if (!dir || !*dir) return;
    uint32_t key[3] = { t->data, t->format ^ (t->type << 8), t->width | t->height << 16 };
    for (uint32_t i = 0; i < count; i++) if (!memcmp(seen[i], key, sizeof(key))) return;
    if (count == 4096) return;
    memcpy(seen[count++], key, sizeof(key));

    char path[1024];
    snprintf(path, sizeof(path), "%s/%08X_%08X_%ux%u.ppm", dir, t->data, t->format, t->width, t->height);
    fprintf(stderr, "gxm texture %s unit %u: 0x%08X type %u format 0x%08X %ux%u, %u levels, kind %u, filters %u/%u mip %u, "
                    "address %u/%u, gamma %u, bias %.2f, %u bytes of %u readable\n",
            fragment ? "fragment" : "vertex", unit, t->data, t->type, t->format, t->width, t->height, t->levels, t->kind,
            t->min_filter, t->mag_filter, t->mip_filter, t->u_mode, t->v_mode, t->gamma, (double)t->lod_bias, t->bytes, t->avail);
    /* a format this file cannot read yet is kept as its bytes, to work on */
    if (t->kind == GXM_TEX_UNKNOWN) {
        char raw[1040];
        uint32_t n = t->width * t->height;                  /* at most a byte a texel */
        snprintf(raw, sizeof(raw), "%s.bin", path);
        FILE *b = fopen(raw, "wb");
        if (b) { fwrite(t->base, 1, n < t->avail ? n : t->avail, b); fclose(b); }
    }
    FILE *f = fopen(path, "wb");
    if (!f) return;
    /* a cube is its six faces side by side */
    unsigned faces = t->cube ? 6 : 1;
    fprintf(f, "P6\n%u %u\n255\n", t->width * faces, t->height);
    for (uint32_t y = 0; y < t->height; y++)
        for (unsigned face = 0; face < faces; face++)
            for (uint32_t x = 0; x < t->width; x++) {
                float c[4];
                gxm_tex_texel(t, NULL, face, 0, (int)x, (int)y, c);
                for (int k = 0; k < 3; k++) {
                    float v = c[k];
                    uint32_t bits;
                    memcpy(&bits, &c[k], 4);
                    if (t->pf.raw) v = (float)(bits & 0xFF) / 255.0f;      /* whole words: show the low byte */
                    fputc((int)(( !(v > 0.0f) ? 0.0f : v > 1.0f ? 1.0f : v) * 255.0f + 0.5f), f);
                }
            }
    fclose(f);
}

/* Fill the secondary attributes: the uniform buffers the program keeps in
 * registers, the addresses of those it reads from memory, its literals;
 * then run its secondary program, once for the whole draw. */
static void stage_begin(stage *g, const gxp_program *p, const uint32_t buffers[GXP_BUFFERS],
                        uint32_t textures[GXP_TEXTURE_UNITS][4], gxm_tex *storage, gxm_tex_cache *cache) {
    memset(g, 0, sizeof(*g));
    g->p = p;
    g->cache = cache;
    g->counters = &gxm_counters;
    for (uint32_t i = 0; i < p->block_count; i++) {
        const gxp_block *b = &p->blocks[i];
        uint32_t from = buffers[b->buffer], words = b->words;
        if (b->reg >= USSE_WORDS) continue;
        if (b->reg + words > USSE_WORDS) words = USSE_WORDS - b->reg;
        const void *src = from ? vita_mem_ptr(from, words * 4) : NULL;
        if (src) memcpy(&g->sa[b->reg], src, words * 4);
    }
    for (uint32_t i = 0; i < p->base_count; i++) {
        const gxp_base *b = &p->bases[i];
        if (b->reg < USSE_WORDS && b->buffer < GXP_BUFFERS) g->sa[b->reg] = buffers[b->buffer] + (uint32_t)b->bias;
    }
    for (uint32_t i = 0; i < p->literal_count; i++)
        if (p->literals[i].reg < USSE_WORDS) g->sa[p->literals[i].reg] = p->literals[i].value;

    for (uint32_t unit = 0; unit < GXP_TEXTURE_UNITS; unit++) {
        if (!((p->textures_used >> unit) & 1)) continue;
        gxm_tex *t = &storage[unit];
        if (!gxm_tex_decode(t, textures[unit])) continue;
        t->avail = vita_mem_avail(t->data);
        t->base = t->avail ? vita_mem_ptr(t->data, 1) : NULL;
        t->pal = t->palette ? vita_mem_ptr(t->palette, t->kind == GXM_TEX_PALETTE4 ? 64 : 1024) : NULL;
        if (!t->base) continue;
        if (gxm_tex_is_packed(t)) attach_unpacked(t);
        g->tex[unit] = t;
        gxm_tex_cache_reset(&cache[unit]);          /* what it held was another draw's texture */
        dump_texture(t, unit, storage == g_textures[1]);
    }

    g->env.user = g;
    g->env.read32 = env_read32;
    g->env.sample = env_sample;
    g->env.gather = env_gather;
    g->st.sa = g->sa;
    g->st.env = &g->env;
    if (p->secondary_count) run(g, p->secondary, p->secondary_count);
}

/* --- the vertex stage ----------------------------------------------------------------- */

typedef struct {
    gxm_draw_state *s;
    stage  v, f;                            /* f is the pattern each thread's fragment stage is copied from */
    uint32_t serial;                        /* which draw this is, counting from the first */

    /* a vertex program's inputs, resolved */
    struct { uint32_t base, stride; uint16_t reg; uint8_t format, components, per_instance, type, wanted, raw; } in[16];
    uint32_t inputs;

    /* which of the vertex program's outputs travel to the fragment program,
     * four floats each, and where in a vertex's row of varyings */
    int slot_at[GXP_SLOTS];
    int w_at;                               /* the clip-space w, for programs that ask for the position */
    int varyings;

    /* how the pixels go through the fragment program */
    int wide;                               /* a row of them at a time */
    int late;                               /* the program can discard: the tests come after it */
    int each;                               /* the program reads the pixel: no triangle waits on another */
} draw;

struct batch;

/* What a thread shading part of a draw has to itself: the fragment stage's
 * registers, the blocks it has decoded, its counts. */
typedef struct {
    const draw *d;
    uint32_t serial, vertex_serial;         /* the draws `f` and `v` were last copied for */
    stage    f, v;
    gxm_tex_cache cache[GXP_TEXTURE_UNITS];
    gxm_draw_counters counters;
    uint32_t pixels;
    struct batch *b;                        /* the pixels waiting for the program, when they go in rows */
} worker;

static const uint8_t ATTRIBUTE_BYTES[11] = { 1, 1, 2, 2, 1, 1, 2, 2, 2, 4, 4 };

static float attribute_value(const uint8_t *p, unsigned format) {
    uint16_t h = (uint16_t)(p[0] | (ATTRIBUTE_BYTES[format] > 1 ? p[1] << 8 : 0));
    float v;
    switch (format) {
    case 0:  return (float)p[0];
    case 1:  return (float)(int8_t)p[0];
    case 2:  return (float)h;
    case 3:  return (float)(int16_t)h;
    case 4:  return (float)p[0] / 255.0f;
    case 5:  v = (float)(int8_t)p[0] / 127.0f; return v < -1.0f ? -1.0f : v;
    case 6:  return (float)h / 65535.0f;
    case 7:  v = (float)(int16_t)h / 32767.0f; return v < -1.0f ? -1.0f : v;
    case 8:  return vita_f16_to_f32(h);
    default: memcpy(&v, p, 4); return v;
    }
}

/* A component into a run of registers, packed as its type packs. */
static void pack(uint32_t *reg, unsigned type, unsigned k, float value) {
    switch (type) {
    case GXP_F16: {
        uint32_t h = vita_f32_to_f16(value);
        reg[k >> 1] = (reg[k >> 1] & ~(0xFFFFu << (16 * (k & 1)))) | (h << (16 * (k & 1)));
        break;
    }
    case GXP_U16: case GXP_S16: {
        uint32_t h = (uint32_t)(int32_t)value & 0xFFFFu;
        reg[k >> 1] = (reg[k >> 1] & ~(0xFFFFu << (16 * (k & 1)))) | (h << (16 * (k & 1)));
        break;
    }
    case GXP_U8: case GXP_S8: {
        uint32_t b = (uint32_t)(int32_t)value & 0xFFu;
        reg[k >> 2] = (reg[k >> 2] & ~(0xFFu << (8 * (k & 3)))) | (b << (8 * (k & 3)));
        break;
    }
    case GXP_U32: case GXP_S32: reg[k] = (uint32_t)(int32_t)value; break;
    default: reg[k] = float_to_bits(value); break;
    }
}

static void vertex_inputs(draw *d) {
    const gxm_program *vp = d->s->vertex;
    d->inputs = 0;
    for (uint32_t i = 0; i < vp->attribute_count && i < 16; i++) {
        const gxm_attribute *a = &vp->attributes[i];
        if (a->stream >= 16 || a->format > 10 || a->reg >= USSE_WORDS - 16) continue;
        const uint8_t *param = gxp_find_param(&vp->gxp, GXP_ATTRIBUTE, a->reg);
        unsigned n = d->inputs++;
        d->in[n].base = d->s->streams[a->stream] + a->offset;
        d->in[n].stride = a->stream < vp->stream_count ? vp->streams[a->stream].stride : 0;
        d->in[n].per_instance = a->stream < vp->stream_count && vp->streams[a->stream].index_source >= 2;
        d->in[n].reg = a->reg;
        d->in[n].format = a->format;
        d->in[n].components = a->components;
        d->in[n].type = param ? (uint8_t)gxp_param_type(param) : GXP_F32;
        d->in[n].wanted = param ? (uint8_t)gxp_param_components(param) : 4;
        if (!d->in[n].wanted || d->in[n].wanted > 4) d->in[n].wanted = 4;
        d->in[n].raw = a->reg < 64 && ((vp->gxp.untyped_regs >> a->reg) & 1);
    }
}

static void shade_vertex(const draw *d, stage *g, uint32_t index, uint32_t instance, raster_vertex *out) {
    const gxp_program *p = g->p;
    usse_state *st = &g->st;
    for (uint32_t i = 0; i < d->inputs; i++) {
        uint32_t size = (uint32_t)ATTRIBUTE_BYTES[d->in[i].format] * d->in[i].components;
        uint32_t at = d->in[i].base + (d->in[i].per_instance ? instance : index) * d->in[i].stride;
        const uint8_t *src = vita_mem_ptr(at, size);
        uint32_t *reg = &st->pa[d->in[i].reg];
        if (!src) continue;
        if (d->in[i].raw || d->in[i].format == 10) {            /* the bytes, as they are */
            memcpy(reg, src, size > 64 ? 64 : size);
            continue;
        }
        float v[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        for (unsigned k = 0; k < d->in[i].components && k < 4; k++)
            v[k] = attribute_value(src + k * ATTRIBUTE_BYTES[d->in[i].format], d->in[i].format);
        for (unsigned k = 0; k < d->in[i].wanted; k++) pack(reg, d->in[i].type, k, v[k]);
    }
    memset(st->out, 0, sizeof(uint32_t) * (p->out_words < 8 ? 8 : p->out_words));
    run(g, p->primary, p->primary_count);

    for (int k = 0; k < 4; k++) out->pos[k] = bits_to_float(st->out[k]);
    for (unsigned slot = 0; slot < GXP_SLOTS; slot++) {
        if (d->slot_at[slot] < 0) continue;
        float *var = &out->var[d->slot_at[slot]];
        for (unsigned k = 0; k < 4; k++)
            var[k] = k < p->out_count[slot] ? bits_to_float(st->out[p->out_reg[slot] + k]) : (k == 3 ? 1.0f : 0.0f);
    }
    if (d->w_at >= 0) out->var[d->w_at] = out->pos[3];
}

/* --- the fragment stage ---------------------------------------------------------------- */

/* The four floats of an interpolated quantity at this pixel, and how they
 * change to the next pixel across and down. */
static void slot_value(const draw *d, unsigned slot, int x, int y, float depth,
                       const float *var, const float *ddx, const float *ddy,
                       float v[4], float dx[4], float dy[4]) {
    memset(dx, 0, 4 * sizeof(float));
    memset(dy, 0, 4 * sizeof(float));
    if (slot == GXP_SLOT_POSITION) {
        v[0] = (float)x + 0.5f; v[1] = (float)y + 0.5f; v[2] = depth;
        v[3] = d->w_at >= 0 ? var[d->w_at] : 1.0f;
        dx[0] = 1.0f; dy[1] = 1.0f;
        return;
    }
    if (slot >= GXP_SLOTS || d->slot_at[slot] < 0) { v[0] = v[1] = v[2] = 0.0f; v[3] = 1.0f; return; }
    memcpy(v, var + d->slot_at[slot], 4 * sizeof(float));
    memcpy(dx, ddx + d->slot_at[slot], 4 * sizeof(float));
    memcpy(dy, ddy + d->slot_at[slot], 4 * sizeof(float));
}

static float unit_clamp(float v) { return !(v > 0.0f) ? 0.0f : v > 1.0f ? 1.0f : v; }

static uint8_t *pixel(const gxm_draw_state *s, int x, int y) {
    uint32_t at = gxm_pixel_offset(s->colour_layout, (uint32_t)x, (uint32_t)y, s->width, s->height,
                                   s->colour_stride, s->colour.bytes);
    return at + s->colour.bytes <= s->colour_avail ? s->colour_data + at : NULL;
}

static int is_raw_result(const gxp_program *p) { return p->result_type == GXP_U32 || p->result_type == GXP_S32; }

/* The colour a pixel has now, as light. */
static void read_pixel(const gxm_draw_state *s, const uint8_t *px, float rgba[4]) {
    pixfmt_decode(&s->colour, px, rgba);
    if (s->colour_gamma) for (int i = 0; i < 3; i++) rgba[i] = pix_srgb_to_linear(rgba[i]);
}

/* The sample of a texture that a program is handed as an input: the
 * coordinate `v` and its rates of change, projected if the input says so,
 * to the texels. Returns how many components the texture has. */
static int sample_input(worker *wk, const gxp_input *in, const gxm_tex *t, float v[4], float dx[4], float dy[4],
                        float rgba[4]) {
    rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.0f;
    if (!t) { wk->counters.no_texture++; return 4; }
    if (in->projection && !t->cube) {
        /* the coordinate over its w, or over its z, and the rate of change of that */
        int by = in->projection == 1 ? 3 : 2;
        float q = 1.0f / v[by];
        for (int k = 0; k < 2; k++) {
            dx[k] = (dx[k] - v[k] * q * dx[by]) * q;
            dy[k] = (dy[k] - v[k] * q * dy[by]) * q;
            v[k] *= q;
        }
    }
    gxm_tex_sample(t, &wk->cache[in->unit], v, t->cube ? 0.0f : gxm_tex_footprint(t, dx[0], dx[1], dy[0], dy[1]), rgba);
    return t->pf.components;
}

/* Such a sample into the registers the program looks for it in, in the
 * form the program was compiled to find there. */
static void store_sample(const gxp_input *in, const gxm_tex *t, const float rgba[4], int n, uint32_t *reg) {
    if (in->result == 3) {
        if (in->single) n = 1;
        for (int k = 0; k < 4; k++) if (k < n || !in->single) reg[k] = k < n ? float_to_bits(rgba[k]) : 0;
    } else if (in->result == 0 && t && t->pf.type == USSE_F32) {
        /* a texture of whole words, kept as it is */
        for (int k = 0; k < n; k++) reg[k] = float_to_bits(rgba[k]);
    } else if (in->result == 0 && (!t || t->pf.type == USSE_U8 || t->pf.type == USSE_S8)) {
        reg[0] = 0;
        for (int k = 0; k < n; k++) {
            if (t && t->pf.type == USSE_S8) pack(reg, GXP_S8, (unsigned)k, roundf((rgba[k] < -1.0f ? -1.0f : rgba[k] > 1.0f ? 1.0f : rgba[k]) * 127.0f));
            else pack(reg, GXP_U8, (unsigned)k, roundf(unit_clamp(rgba[k]) * 255.0f));
        }
    } else {
        reg[0] = reg[1] = 0;
        for (int k = 0; k < n; k++) pack(reg, GXP_F16, (unsigned)k, rgba[k]);
    }
}

/* A program that blends for itself is given the pixel as it stands, in the
 * registers its own result will be left in. */
static void preload(const gxm_draw_state *s, const gxp_program *p, int x, int y, uint32_t *out) {
    const uint8_t *px = pixel(s, x, y);
    if (!px) return;
    if (is_raw_result(p)) {
        memset(out, 0, 16);
        memcpy(out, px, s->colour.bytes > 16 ? 16 : s->colour.bytes);
        return;
    }
    float now[4];
    read_pixel(s, px, now);
    if (p->result_type == GXP_F16) { out[0] = halves(now[0], now[1]); out[1] = halves(now[2], now[3]); }
    else if (p->result_type == GXP_F32) for (int k = 0; k < 4; k++) out[k] = float_to_bits(now[k]);
    else { out[0] = 0; for (unsigned k = 0; k < 4; k++) pack(out, GXP_U8, k, roundf(unit_clamp(now[k]) * 255.0f)); }
}

/* Fill the primary attributes from the program's input list and run it. */
static void run_fragment(worker *wk, int x, int y, float depth, const float *var, const float *ddx, const float *ddy) {
    const draw *d = wk->d;
    const gxp_program *p = wk->f.p;
    usse_state *st = &wk->f.st;
    float v[4], dx[4], dy[4], rgba[4];

    for (uint32_t i = 0; i < p->input_count; i++) {
        const gxp_input *in = &p->inputs[i];
        if (in->has_iterator && in->iterator_reg < USSE_WORDS - 4) {
            uint32_t *reg = &st->pa[in->iterator_reg];
            slot_value(d, in->source, x, y, depth, var, ddx, ddy, v, dx, dy);
            if (in->type == USSE_F32) {
                for (unsigned k = 0; k < in->components; k++) reg[k] = float_to_bits(v[k]);
            } else if (in->type == USSE_F16) {
                for (unsigned k = 0; k < in->components; k++) pack(reg, GXP_F16, k, v[k]);
            } else {
                for (unsigned k = 0; k < in->components; k++) pack(reg, GXP_U8, k, roundf(unit_clamp(v[k]) * 255.0f));
            }
        }
        if (in->has_texture && in->texture_reg < USSE_WORDS - 4) {
            const gxm_tex *t = in->unit < GXP_TEXTURE_UNITS ? wk->f.tex[in->unit] : NULL;
            if (t) slot_value(d, in->coord, x, y, depth, var, ddx, ddy, v, dx, dy);
            int n = sample_input(wk, in, t, v, dx, dy, rgba);
            store_sample(in, t, rgba, n, &st->pa[in->texture_reg]);
        }
    }
    if ((p->flags & GXP_FLAG_FRAGCOLOR) && d->s->has_colour) preload(d->s, p, x, y, st->out);
    run(&wk->f, p->primary, p->primary_count);
}

static float blend_factor(unsigned f, int alpha, int c, const float s[4], const float dst[4]) {
    switch (f) {
    case 0:  return 0.0f;
    case 1:  return 1.0f;
    case 2:  return s[alpha ? 3 : c];
    case 3:  return 1.0f - s[alpha ? 3 : c];
    case 4:  return s[3];
    case 5:  return 1.0f - s[3];
    case 6:  return dst[alpha ? 3 : c];
    case 7:  return 1.0f - dst[alpha ? 3 : c];
    case 8:  return dst[3];
    case 9:  return 1.0f - dst[3];
    case 10: return alpha ? 1.0f : (s[3] < 1.0f - dst[3] ? s[3] : 1.0f - dst[3]);
    default: return alpha ? 1.0f : (dst[3] < 1.0f - s[3] ? dst[3] : 1.0f - s[3]);
    }
}

static float blend_channel(unsigned func, float s, float fs, float dst, float fd) {
    switch (func) {
    case 1:  return s * fs + dst * fd;
    case 2:  return s * fs - dst * fd;
    case 3:  return dst * fd - s * fs;
    case 4:  return s < dst ? s : dst;
    case 5:  return s > dst ? s : dst;
    default: return s;
    }
}

/* The program's result into the colour surface: decoded from the four
 * registers it was left in, blended with what is there, stored in the
 * surface's form. */
static void write_colour(worker *wk, int x, int y, const uint32_t *reg) {
    const gxm_draw_state *s = wk->d->s;
    const gxm_program *fp = s->fragment;
    const gxp_program *p = wk->f.p;
    uint8_t *px = pixel(s, x, y);
    if (!px) return;

    /* a result that is the pixel's own bits, and a surface of two floats
     * written by a program that thinks in halves, go across untouched */
    if (is_raw_result(p) || (p->result_type == GXP_F16 && p->result_components > 2 && s->colour.bytes == 8 &&
                             s->colour.fields == 2 && s->colour.f[0].kind == PIX_F32)) {
        uint32_t n = is_raw_result(p) ? p->result_components * 4u : 8u;
        memcpy(px, reg, n < s->colour.bytes ? n : s->colour.bytes);
        return;
    }

    float c[4];
    switch (p->result_type) {
    case GXP_F32: for (int k = 0; k < 4; k++) c[k] = bits_to_float(reg[k]); break;
    case GXP_U8:  for (int k = 0; k < 4; k++) c[k] = (float)((reg[0] >> (8 * k)) & 0xFF) / 255.0f; break;
    case GXP_S8:  for (int k = 0; k < 4; k++) { c[k] = (float)(int8_t)(reg[0] >> (8 * k)) / 127.0f; if (c[k] < -1.0f) c[k] = -1.0f; } break;
    case GXP_U16: for (int k = 0; k < 4; k++) c[k] = (float)((reg[k >> 1] >> (16 * (k & 1))) & 0xFFFF) / 65535.0f; break;
    case GXP_S16: for (int k = 0; k < 4; k++) { c[k] = (float)(int16_t)(reg[k >> 1] >> (16 * (k & 1))) / 32767.0f; if (c[k] < -1.0f) c[k] = -1.0f; } break;
    default:      for (int k = 0; k < 4; k++) c[k] = vita_f16_to_f32((uint16_t)(reg[k >> 1] >> (16 * (k & 1)))); break;
    }
    if (fp->output_format == 1) for (int k = 0; k < 4; k++) c[k] = unit_clamp(c[k]);       /* four unsigned bytes */

    unsigned colour_func = fp->blend[1] & 15, alpha_func = fp->blend[1] >> 4, mask = fp->blend[0];
    if ((colour_func || alpha_func) && !(p->flags & GXP_FLAG_NATIVECOLOR)) {
        float now[4], out[4];
        read_pixel(s, px, now);
        for (int k = 0; k < 3; k++)
            out[k] = blend_channel(colour_func, c[k], blend_factor(fp->blend[2] & 15, 0, k, c, now),
                                   now[k], blend_factor(fp->blend[2] >> 4, 0, k, c, now));
        out[3] = blend_channel(alpha_func, c[3], blend_factor(fp->blend[3] & 15, 1, 3, c, now),
                               now[3], blend_factor(fp->blend[3] >> 4, 1, 3, c, now));
        memcpy(c, out, sizeof(c));
        if (fp->output_format == 1) for (int k = 0; k < 4; k++) c[k] = unit_clamp(c[k]);
    }
    if (s->colour_gamma) for (int k = 0; k < 3; k++) c[k] = pix_linear_to_srgb(c[k]);
    /* the mask's bits are alpha, red, green, blue */
    pixfmt_encode(&s->colour, px, c, ((mask >> 1) & 7) | ((mask & 1) << 3));
}

static int compare(unsigned func, float a, float b) {
    switch (func & 7) {
    case 0:  return 0;
    case 1:  return a < b;
    case 2:  return a == b;
    case 3:  return a <= b;
    case 4:  return a > b;
    case 5:  return a != b;
    case 6:  return a >= b;
    default: return 1;
    }
}

static void stencil_update(uint8_t *stored, unsigned op, const gxm_face *f) {
    uint8_t now, next;
    if (!stored || !op) return;
    now = *stored;
    switch (op) {
    case 1:  next = 0; break;
    case 2:  next = f->stencil_ref; break;
    case 3:  next = now == 255 ? 255 : (uint8_t)(now + 1); break;
    case 4:  next = now == 0 ? 0 : (uint8_t)(now - 1); break;
    case 5:  next = (uint8_t)~now; break;
    case 6:  next = (uint8_t)(now + 1); break;
    default: next = (uint8_t)(now - 1); break;
    }
    *stored = (uint8_t)((now & ~f->write_mask) | (next & f->write_mask));
}

/* A depth surface of integers compares and keeps integers: without this
 * a surface stored and loaded again would never equal the depth of the
 * same geometry drawn a second time. */
static float whole_depth(const gxm_draw_state *s, float depth) {
    if (!s->depth_bits) return depth;
    double top = (double)((1u << s->depth_bits) - 1), steps = floor((double)depth * top + 0.5);
    return (float)((steps > top ? top : steps > 0.0 ? steps : 0.0) / top);
}

/* The stencil test and then the depth test, and what the outcome does to
 * the stencil and to the depth kept for the pixel. Whether it is drawn. */
static int tested(gxm_draw_state *s, const gxm_face *f, size_t at, float depth) {
    uint8_t *stencil = s->stencil ? &s->stencil[at] : NULL, stored = stencil ? *stencil : 0;
    if (!compare(f->stencil_func, (float)(f->stencil_ref & f->compare_mask), (float)(stored & f->compare_mask))) {
        stencil_update(stencil, f->stencil_fail, f);
        return 0;
    }
    if (s->depth && !compare(f->depth_func, depth, s->depth[at])) {
        stencil_update(stencil, f->depth_fail, f);
        return 0;
    }
    stencil_update(stencil, f->depth_pass, f);
    if (s->depth && f->depth_write) s->depth[at] = depth;
    return 1;
}

/* One covered pixel: the mask, the stencil and depth tests, the program,
 * the colour. A program that may discard runs before the tests, since its
 * verdict decides whether depth and stencil change at all. */
static int shade_pixel(void *user, int x, int y, float depth, int clockwise,
                       const float *var, const float *ddx, const float *ddy) {
    worker *wk = user;
    const draw *d = wk->d;
    gxm_draw_state *s = d->s;
    size_t at = (size_t)y * s->width + (size_t)x;
    if (s->mask && !s->mask[at]) return 0;
    depth = whole_depth(s, depth);

    /* counter-clockwise on the screen is the front */
    const gxm_face *f = &s->face[(s->two_sided && clockwise) ? 1 : 0];
    int shaded = wk->f.p && !f->program_off;
    int ran = 0;
    if (shaded && (wk->f.p->flags & (GXP_FLAG_DISCARD | GXP_FLAG_DEPTH))) {
        run_fragment(wk, x, y, depth, var, ddx, ddy);
        if (wk->f.st.killed) return 0;
        ran = 1;
    }
    if (!tested(s, f, at, depth)) return 0;
    wk->pixels++;

    if (shaded && s->has_colour) {
        if (!ran) run_fragment(wk, x, y, depth, var, ddx, ddy);
        write_colour(wk, x, y, (wk->f.p->flags & GXP_FLAG_NATIVECOLOR) ? wk->f.st.out : wk->f.st.pa);
    }
    return 1;
}

/* --- the same, for a row of pixels at once ------------------------------------------------
 * Most of what a pixel costs is the fragment program, and most of what an
 * interpreted program costs is finding out what each instruction says. So
 * the pixels a thread covers are not shaded as they are met: they wait, up
 * to USSE_LANES of them, and the program is then run once for all of them,
 * each instruction taken apart once and done for every pixel waiting
 * (usse.h). Nothing a pixel ends as is different. What has to hold for
 * that: a pixel's depth and stencil tests are made in the order the
 * triangles came in, and so are its writes to the colour surface; both are
 * kept by letting pixels wait and leave in the order they arrived. A
 * program that reads the pixel it is about to write must not find a pixel
 * of an earlier triangle still waiting, so for those nothing waits across
 * triangles. */

struct batch {
    unsigned n;                             /* pixels waiting */
    int      x[USSE_LANES], y[USSE_LANES];
    float    depth[USSE_LANES];
    uint8_t  back[USSE_LANES];              /* drawn by a back face */
    float    var[RASTER_MAX_VARYINGS][USSE_LANES];                  /* each varying, for every pixel */
    float    ddx[USSE_LANES][RASTER_MAX_VARYINGS], ddy[USSE_LANES][RASTER_MAX_VARYINGS];
    usse_wide w;
};

/* Component k of an interpolated quantity, for each of the n pixels waiting. */
static const float *slot_row(const draw *d, const struct batch *b, unsigned n, unsigned slot, unsigned k, float *scratch) {
    if (slot == GXP_SLOT_POSITION) {
        if (k == 0) { for (unsigned l = 0; l < n; l++) scratch[l] = (float)b->x[l] + 0.5f; return scratch; }
        if (k == 1) { for (unsigned l = 0; l < n; l++) scratch[l] = (float)b->y[l] + 0.5f; return scratch; }
        if (k == 2) return b->depth;
        if (d->w_at >= 0) return b->var[d->w_at];
        for (unsigned l = 0; l < n; l++) scratch[l] = 1.0f;
        return scratch;
    }
    if (slot >= GXP_SLOTS || d->slot_at[slot] < 0) {
        for (unsigned l = 0; l < n; l++) scratch[l] = k == 3 ? 1.0f : 0.0f;
        return scratch;
    }
    return b->var[d->slot_at[slot] + (int)k];
}

/* slot_value, for one of the pixels waiting. */
static void lane_value(const draw *d, const struct batch *b, unsigned slot, unsigned l, float v[4], float dx[4], float dy[4]) {
    memset(dx, 0, 4 * sizeof(float));
    memset(dy, 0, 4 * sizeof(float));
    if (slot == GXP_SLOT_POSITION) {
        v[0] = (float)b->x[l] + 0.5f; v[1] = (float)b->y[l] + 0.5f; v[2] = b->depth[l];
        v[3] = d->w_at >= 0 ? b->var[d->w_at][l] : 1.0f;
        dx[0] = 1.0f; dy[1] = 1.0f;
        return;
    }
    if (slot >= GXP_SLOTS || d->slot_at[slot] < 0) { v[0] = v[1] = v[2] = 0.0f; v[3] = 1.0f; return; }
    int at = d->slot_at[slot];
    for (int k = 0; k < 4; k++) v[k] = b->var[at + k][l];
    memcpy(dx, b->ddx[l] + at, 4 * sizeof(float));
    memcpy(dy, b->ddy[l] + at, 4 * sizeof(float));
}

/* Shade the pixels waiting: their inputs into rows of registers, the
 * program once, then each pixel's tests if they come after it, and its
 * colour. */
static void flush(worker *wk) {
    struct batch *b = wk->b;
    unsigned n = b ? b->n : 0;
    if (!n) return;
    const draw *d = wk->d;
    gxm_draw_state *s = d->s;
    const gxp_program *p = wk->f.p;
    usse_wide *w = &b->w;
    float scratch[USSE_LANES], v[4], dx[4], dy[4], rgba[4];
    uint32_t reg[4];

    for (uint32_t i = 0; i < p->input_count; i++) {
        const gxp_input *in = &p->inputs[i];
        if (in->has_iterator && in->iterator_reg < USSE_WORDS - 4) {
            uint32_t (*row)[USSE_LANES] = &w->pa[in->iterator_reg];
            for (unsigned k = 0; k < in->components; k++) {
                const float *from = slot_row(d, b, n, in->source, k, scratch);
                if (in->type == USSE_F32) memcpy(row[k], from, n * sizeof(float));
                else if (in->type == USSE_F16) vita_floats_to_halves(from, row[k >> 1], (int)(k & 1), n);
                else for (unsigned l = 0; l < n; l++) pack(&row[k >> 2][l], GXP_U8, k & 3, roundf(unit_clamp(from[l]) * 255.0f));
            }
        }
        if (in->has_texture && in->texture_reg < USSE_WORDS - 4) {
            uint32_t (*row)[USSE_LANES] = &w->pa[in->texture_reg];
            const gxm_tex *t = in->unit < GXP_TEXTURE_UNITS ? wk->f.tex[in->unit] : NULL;
            for (unsigned l = 0; l < n; l++) {
                if (t) lane_value(d, b, in->coord, l, v, dx, dy);
                int got = sample_input(wk, in, t, v, dx, dy, rgba);
                for (int k = 0; k < 4; k++) reg[k] = row[k][l];
                store_sample(in, t, rgba, got, reg);
                for (int k = 0; k < 4; k++) row[k][l] = reg[k];
            }
        }
    }
    if ((p->flags & GXP_FLAG_FRAGCOLOR) && s->has_colour) {
        for (unsigned l = 0; l < n; l++) {
            for (int k = 0; k < 4; k++) reg[k] = w->out[k][l];
            preload(s, p, b->x[l], b->y[l], reg);
            for (int k = 0; k < 4; k++) w->out[k][l] = reg[k];
        }
    }

    w->sa = wk->f.sa;
    w->env = &wk->f.env;
    w->lanes = n;
    usse_run_wide(p->primary, p->primary_count, w);

    uint32_t (*result)[USSE_LANES] = (p->flags & GXP_FLAG_NATIVECOLOR) ? w->out : w->pa;
    for (unsigned l = 0; l < n; l++) {
        int x = b->x[l], y = b->y[l];
        if (d->late) {
            if (w->killed[l]) continue;
            if (!tested(s, &s->face[b->back[l]], (size_t)y * s->width + (size_t)x, b->depth[l])) continue;
            wk->pixels++;
            if (!s->has_colour) continue;
        }
        for (int k = 0; k < 4; k++) reg[k] = result[k][l];
        write_colour(wk, x, y, reg);
    }
    b->n = 0;
}

/* One covered pixel, as shade_pixel takes it: tested now if the program has
 * no say in the tests, and then left to wait for the program. */
static int collect_pixel(void *user, int x, int y, float depth, int clockwise,
                         const float *var, const float *ddx, const float *ddy) {
    worker *wk = user;
    const draw *d = wk->d;
    gxm_draw_state *s = d->s;
    size_t at = (size_t)y * s->width + (size_t)x;
    if (s->mask && !s->mask[at]) return 0;
    depth = whole_depth(s, depth);

    unsigned back = (s->two_sided && clockwise) ? 1 : 0;
    const gxm_face *f = &s->face[back];
    if (f->program_off) {
        /* depth and stencil only: but after whatever waits to be tested, which may be at this pixel */
        if (d->late) flush(wk);
        if (!tested(s, f, at, depth)) return 0;
        wk->pixels++;
        return 1;
    }
    if (!d->late) {
        if (!tested(s, f, at, depth)) return 0;
        wk->pixels++;
        if (!s->has_colour) return 1;
    }

    struct batch *b = wk->b;
    unsigned n = b->n;
    b->x[n] = x; b->y[n] = y; b->depth[n] = depth; b->back[n] = (uint8_t)back;
    for (int i = 0; i < d->varyings; i++) b->var[i][n] = var[i];
    memcpy(b->ddx[n], ddx, (size_t)d->varyings * sizeof(float));
    memcpy(b->ddy[n], ddy, (size_t)d->varyings * sizeof(float));
    if (++b->n == USSE_LANES) flush(wk);
    return 1;
}

/* --- the draw --------------------------------------------------------------------------
 * In two parts. First, on the calling thread: every vertex through the
 * vertex program, every triangle clipped and set up. Then the pixels: the
 * rows the draw covers are cut into bands, and each band is walked by one
 * thread, which takes the draw's triangles in their order and shades what
 * falls in its rows. Bands share no pixel, so the order of the triangles is
 * kept wherever it matters, and nothing is locked. */

#define BAND_ROWS      8
#define WORTH_SHARING  16384            /* pixels, by the boxes of the triangles: below this, one thread */

static raster_vertex *g_vertices;
static uint32_t *g_slot, *g_vertex_index, g_vertices_cap, g_slot_cap;

/* The draw's triangles, set up: each piece, and its planes at the same place
 * in a table of floats. */
static raster_piece *g_pieces;
static float *g_planes;
static uint32_t g_pieces_cap, g_piece_count;
static size_t g_planes_cap;

static worker *g_workers;

static int reserve(uint32_t vertices, uint32_t range) {
    if (vertices > g_vertices_cap) {
        raster_vertex *v = realloc(g_vertices, (size_t)vertices * sizeof(*v));
        if (v) g_vertices = v;
        uint32_t *n = realloc(g_vertex_index, (size_t)vertices * sizeof(*n));
        if (n) g_vertex_index = n;
        if (!v || !n) return 0;
        g_vertices_cap = vertices;
    }
    if (range > g_slot_cap) {
        uint32_t *m = realloc(g_slot, (size_t)range * sizeof(*m));
        if (!m) return 0;
        g_slot = m; g_slot_cap = range;
    }
    return 1;
}

/* Set a triangle up after the others. */
static void triangle(raster *r, const raster_vertex *a, const raster_vertex *b, const raster_vertex *c) {
    size_t per = raster_plane_floats(r), need = ((size_t)g_piece_count + RASTER_MAX_PIECES) * per;
    if (g_piece_count + RASTER_MAX_PIECES > g_pieces_cap) {
        uint32_t cap = g_pieces_cap ? g_pieces_cap * 2 : 1024;
        raster_piece *p = realloc(g_pieces, (size_t)cap * sizeof(*p));
        if (!p) return;
        g_pieces = p; g_pieces_cap = cap;
    }
    if (need > g_planes_cap) {
        size_t cap = g_planes_cap ? g_planes_cap : 65536;
        while (cap < need) cap *= 2;
        float *f = realloc(g_planes, cap * sizeof(*f));
        if (!f) return;
        g_planes = f; g_planes_cap = cap;
    }
    g_piece_count += (uint32_t)raster_setup(r, a, b, c, &g_pieces[g_piece_count], g_planes + (size_t)g_piece_count * per);
}

/* A thread's own copy of the fragment stage, made the first time it takes
 * part in a draw. */
static worker *worker_for(const draw *d, unsigned index) {
    worker *wk = &g_workers[index];
    if (wk->d == d && wk->serial == d->serial) return wk;
    wk->d = d;
    wk->serial = d->serial;
    wk->f = d->f;
    wk->f.cache = wk->cache;
    wk->f.counters = &wk->counters;
    wk->f.env.user = &wk->f;
    wk->f.st.sa = wk->f.sa;
    wk->f.st.env = &wk->f.env;
    for (unsigned unit = 0; unit < GXP_TEXTURE_UNITS; unit++)
        if (wk->f.tex[unit]) gxm_tex_cache_reset(&wk->cache[unit]);
    return wk;
}

#define VERTEX_RUN 64                   /* vertices a thread takes at a time */

typedef struct { const draw *d; uint32_t count, instance; } vertex_job;

/* Shade one run of the draw's distinct vertices, with this thread's own
 * copy of the vertex stage. */
static void shade_run(void *ctx, unsigned item, unsigned index) {
    const vertex_job *job = ctx;
    worker *wk = &g_workers[index];
    if (wk->vertex_serial != job->d->serial) {
        wk->vertex_serial = job->d->serial;
        wk->v = job->d->v;
        wk->v.cache = NULL;
        wk->v.counters = &wk->counters;
        wk->v.env.user = &wk->v;
        wk->v.st.sa = wk->v.sa;
        wk->v.st.env = &wk->v.env;
    }
    uint32_t first = item * VERTEX_RUN, last = first + VERTEX_RUN;
    if (last > job->count) last = job->count;
    for (uint32_t i = first; i < last; i++) shade_vertex(job->d, &wk->v, g_vertex_index[i], job->instance, &g_vertices[i]);
}

typedef struct { const draw *d; const raster *r; int first_row, last_row, rows; } fill_job;

static void fill_band(void *ctx, unsigned band, unsigned index) {
    const fill_job *job = ctx;
    worker *wk = worker_for(job->d, index);
    int row0 = job->first_row + (int)band * job->rows, row1 = row0 + job->rows;
    size_t per = raster_plane_floats(job->r);
    if (row1 > job->last_row + 1) row1 = job->last_row + 1;
    raster_shade shade = shade_pixel;
    if (job->d->wide) {
        if (!wk->b) wk->b = calloc(1, sizeof(*wk->b));
        if (wk->b) shade = collect_pixel;
    }
    for (uint32_t i = 0; i < g_piece_count; i++) {
        const raster_piece *t = &g_pieces[i];
        if (t->y1 < row0 || t->y0 >= row1) continue;
        raster_walk(job->r, t, g_planes + (size_t)i * per, row0, row1, shade, wk);
        if (job->d->each) flush(wk);
    }
    flush(wk);                              /* nothing waits beyond its band */
}

uint32_t gxm_draw(gxm_draw_state *s, unsigned primitive, unsigned index_size, uint32_t indices,
                  uint32_t count, uint32_t instances) {
    static draw d;                          /* large, and never entered twice */
    static uint32_t serial;
    const gxm_program *vp = s->vertex, *fp = s->fragment;
    gxm_counters.draws++;
    if (primitive == GXM_LINES || primitive == GXM_POINTS) { gxm_counters.not_triangles++; return 0; }
    if (!vp || !vp->ok || (fp && !fp->ok) || !s->width || !s->height) { gxm_counters.no_program++; return 0; }
    if (count < 3 || !instances) return 0;
    const uint8_t *index_data = vita_mem_ptr(indices, count * index_size);
    if (!index_data) return 0;
    if (!g_workers) g_workers = calloc(vita_pool_size(), sizeof(*g_workers));
    if (!g_workers) return 0;

    memset(&d.in, 0, sizeof(d.in));
    d.s = s;
    d.serial = ++serial;
    stage_begin(&d.v, &vp->gxp, s->vertex_buffers, s->vertex_textures, g_textures[0], g_vertex_cache);
    if (fp) stage_begin(&d.f, &fp->gxp, s->fragment_buffers, s->fragment_textures, g_textures[1], g_workers[0].cache);
    else memset(&d.f, 0, sizeof(d.f));
    vertex_inputs(&d);

    /* VITARECOMP_GXM_SCALAR=1 shades every pixel by itself, as the definition
     * of what the rows must come out as. */
    static int scalar = -1;
    if (scalar < 0) { const char *e = getenv("VITARECOMP_GXM_SCALAR"); scalar = e && *e && *e != '0'; }
    d.wide = fp && fp->wide && !scalar;
    d.late = fp && (fp->gxp.flags & (GXP_FLAG_DISCARD | GXP_FLAG_DEPTH)) != 0;
    d.each = fp && (fp->gxp.flags & GXP_FLAG_FRAGCOLOR) && s->has_colour;

    /* What the fragment program reads of the vertex program's outputs is
     * what gets interpolated; nothing else is carried. */
    d.varyings = 0;
    d.w_at = -1;
    for (unsigned slot = 0; slot < GXP_SLOTS; slot++) {
        d.slot_at[slot] = -1;
        if (!fp || slot >= GXP_SLOT_POSITION || !((fp->gxp.slots_used >> slot) & 1) || !vp->gxp.out_count[slot]) continue;
        if (d.varyings + 4 > RASTER_MAX_VARYINGS) continue;
        d.slot_at[slot] = d.varyings;
        d.varyings += 4;
    }
    if (fp && ((fp->gxp.slots_used >> GXP_SLOT_POSITION) & 1) && d.varyings < RASTER_MAX_VARYINGS) d.w_at = d.varyings++;

    raster r;
    memset(&r, 0, sizeof(r));
    r.width = (int)s->width;
    r.height = (int)s->height;
    r.clip_x0 = 0; r.clip_y0 = 0; r.clip_x1 = (int)s->width; r.clip_y1 = (int)s->height;
    if (s->region_mode == GXM_REGION_ALL) return 0;
    if (s->region_mode == GXM_REGION_OUTSIDE) {
        /* whole tiles: the region grows to the 32-pixel grid */
        int x0 = (int)(s->region[0] & ~31u), y0 = (int)(s->region[1] & ~31u);
        int x1 = (int)((s->region[2] + 32) & ~31u), y1 = (int)((s->region[3] + 32) & ~31u);
        if (x0 > r.clip_x0) r.clip_x0 = x0;
        if (y0 > r.clip_y0) r.clip_y0 = y0;
        if (x1 < r.clip_x1) r.clip_x1 = x1;
        if (y1 < r.clip_y1) r.clip_y1 = y1;
    }
    if (s->viewport_on) {
        r.x_offset = s->offset[0]; r.x_scale = s->scale[0];
        r.y_offset = s->offset[1]; r.y_scale = s->scale[1];
        r.z_offset = s->offset[2]; r.z_scale = s->scale[2];
    } else {
        r.x_scale = r.y_scale = r.z_scale = 1.0f;
    }
    r.cull = s->cull == 1 ? RASTER_CULL_CW : s->cull == 2 ? RASTER_CULL_CCW : RASTER_CULL_NONE;
    r.clip_z = RASTER_CLIP_FAR;             /* nearer than the near plane is drawn, at depth 0 */
    r.clamp_depth = 1;
    r.depth_func = RASTER_ALWAYS;           /* the tests are made here, with the stencil */
    r.want_derivatives = 1;
    r.varyings = d.varyings;

    /* each index is shaded once per instance */
    uint32_t low = 0xFFFFFFFFu, high = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t v = index_size == 2 ? (uint32_t)(index_data[2 * i] | index_data[2 * i + 1] << 8)
                                     : (uint32_t)index_data[4 * i] | (uint32_t)index_data[4 * i + 1] << 8 |
                                       (uint32_t)index_data[4 * i + 2] << 16 | (uint32_t)index_data[4 * i + 3] << 24;
        if (v < low) low = v;
        if (v > high) high = v;
    }
    uint32_t range = high - low + 1, pixels = 0;
    if (range > 0x1000000u || !reserve(range < count ? range : count, range)) return 0;

    for (uint32_t instance = 0; instance < instances; instance++) {
        uint32_t used = 0, tri[3];
        memset(g_slot, 0xFF, (size_t)range * sizeof(*g_slot));
        g_piece_count = 0;
        /* number the distinct vertices, shade them, then put the triangles together */
        for (uint32_t i = 0; i < count; i++) {
            uint32_t v = index_size == 2 ? (uint32_t)(index_data[2 * i] | index_data[2 * i + 1] << 8)
                                         : (uint32_t)index_data[4 * i] | (uint32_t)index_data[4 * i + 1] << 8 |
                                           (uint32_t)index_data[4 * i + 2] << 16 | (uint32_t)index_data[4 * i + 3] << 24;
            if (g_slot[v - low] != 0xFFFFFFFFu) continue;
            g_slot[v - low] = used;
            g_vertex_index[used++] = v;
        }
        vertex_job shading = { &d, used, instance };
        if (used < 4 * VERTEX_RUN) for (unsigned n = 0; n * VERTEX_RUN < used; n++) shade_run(&shading, n, 0);
        else vita_pool_run((used + VERTEX_RUN - 1) / VERTEX_RUN, shade_run, &shading);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t v = index_size == 2 ? (uint32_t)(index_data[2 * i] | index_data[2 * i + 1] << 8)
                                         : (uint32_t)index_data[4 * i] | (uint32_t)index_data[4 * i + 1] << 8 |
                                           (uint32_t)index_data[4 * i + 2] << 16 | (uint32_t)index_data[4 * i + 3] << 24;
            uint32_t at = g_slot[v - low];
            if (primitive == GXM_TRIANGLE_STRIP) {
                if (i < 2) { tri[i] = at; continue; }
                tri[2] = at;
                /* every other triangle of a strip is wound the other way: swap its first two */
                if (i & 1) triangle(&r, &g_vertices[tri[1]], &g_vertices[tri[0]], &g_vertices[tri[2]]);
                else       triangle(&r, &g_vertices[tri[0]], &g_vertices[tri[1]], &g_vertices[tri[2]]);
                tri[0] = tri[1]; tri[1] = tri[2];
            } else if (primitive == GXM_TRIANGLE_FAN) {
                if (i == 0) { tri[0] = at; continue; }
                if (i == 1) { tri[1] = at; continue; }
                triangle(&r, &g_vertices[tri[0]], &g_vertices[tri[1]], &g_vertices[at]);
                tri[1] = at;
            } else {
                tri[i % 3] = at;
                if (i % 3 == 2) triangle(&r, &g_vertices[tri[0]], &g_vertices[tri[1]], &g_vertices[tri[2]]);
            }
        }
        if (!g_piece_count) continue;

        /* the rows the instance covers, and whether there is enough in them to share out */
        fill_job job = { &d, &r, g_pieces[0].y0, g_pieces[0].y1, BAND_ROWS };
        uint64_t area = 0;
        for (uint32_t i = 0; i < g_piece_count; i++) {
            const raster_piece *t = &g_pieces[i];
            if (t->y0 < job.first_row) job.first_row = t->y0;
            if (t->y1 > job.last_row) job.last_row = t->y1;
            area += (uint64_t)(t->x1 - t->x0 + 1) * (uint64_t)(t->y1 - t->y0 + 1);
        }
        int rows = job.last_row - job.first_row + 1;
        if (area < WORTH_SHARING || vita_pool_size() == 1) {
            job.rows = rows;
            fill_band(&job, 0, 0);
        } else {
            vita_pool_run((unsigned)((rows + BAND_ROWS - 1) / BAND_ROWS), fill_band, &job);
        }
        for (unsigned w = 0; w < vita_pool_size(); w++) {
            worker *wk = &g_workers[w];
            pixels += wk->pixels;
            gxm_counters.stopped += wk->counters.stopped;
            gxm_counters.no_texture += wk->counters.no_texture;
            if (wk->counters.stopped_at) gxm_counters.stopped_at = wk->counters.stopped_at;
            wk->pixels = 0;
            memset(&wk->counters, 0, sizeof(wk->counters));
        }
    }
    gxm_counters.triangles += r.triangles_drawn;
    gxm_counters.pixels += pixels;
    return pixels;
}
