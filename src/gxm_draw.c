/* gxm_draw.c — one draw call, from index buffer to pixels. See gxm_draw.h. */

#include "gxm_draw.h"
#include "gxm_draw_list.h"
#include "gxm_gpu.h"
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
#include <threads.h>
#include <time.h>

gxm_draw_counters gxm_counters;

/* --- where the time went -------------------------------------------------------------
 * VITARECOMP_GXM_PROFILE=<n>: every n frames, a line on stderr saying what
 * a frame of them cost, by stage. The first four are the drawing thread's
 * own time, one after another: making the draws, and the three steps of
 * drawing a scene's list of them. The rest are summed over every thread
 * that took part, so on sixteen threads they add up to many times the step
 * they are part of. Clocks are read once a batch of pixels, not once a
 * pixel, and not at all when this is off. */
enum { T_BEGIN, T_VERTICES, T_SETUP, T_FILL, T_MAIN };                  /* the drawing thread */
enum { T_INPUTS, T_SAMPLES, T_PROGRAM, T_STORE, T_BAND, T_PREPARE, T_WORKER };     /* each thread */
static uint32_t g_profile;
static uint64_t g_main_ns[T_MAIN];
/* and the same by the size of the surface drawn to: a frame is a dozen scenes, of a few sizes */
static struct { uint32_t width, height, lists, draws; uint64_t ns; } g_by_target[16];

/* What a fragment program cost in the fill: the time its draws took in
 * their tiles, walk and all, and the pixels that came of it. A table by the
 * program's name, small and open: a frame has a few dozen programs. */
#define BY_PROGRAM 128
typedef struct { uint64_t name, ns, pixels, covered; uint32_t draws, used; } program_cost;

static void cost_add(program_cost *table, uint64_t name, uint64_t ns, uint64_t pixels, uint64_t covered, uint32_t draws) {
    unsigned at = (unsigned)((name ^ (name >> 29)) % BY_PROGRAM);
    for (unsigned tries = 0; tries < BY_PROGRAM; tries++, at = (at + 1) % BY_PROGRAM) {
        program_cost *c = &table[at];
        if (c->used && c->name != name) continue;
        c->used = 1; c->name = name;
        c->ns += ns; c->pixels += pixels; c->covered += covered; c->draws += draws;
        return;
    }
    table[0].ns += ns; table[0].pixels += pixels; table[0].covered += covered;       /* full: counted somewhere */
}

static uint64_t now_ns(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

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


/* What a vertex stage's secondary program decodes blocks into, while a draw
 * is being made. */
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

static void profile_report(void);

void gxm_draw_frame(void) {
    g_frame++;
    if (g_profile && (g_frame - 1) % g_profile == 0) profile_report();
}

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
                        uint32_t textures[GXP_TEXTURE_UNITS][4], gxm_tex *storage, gxm_tex_cache *cache, int fragment) {
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
        dump_texture(t, unit, fragment);
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


struct batch;

/* What a thread drawing part of a scene has to itself: a copy of the stage
 * it is running, the blocks it has decoded, its counts, and the vertices of
 * the draw it is setting up. */
typedef struct {
    const draw *d;
    uint32_t serial;                        /* the draw `f` was last copied for */
    stage    f, v;
    gxm_tex_cache cache[GXP_TEXTURE_UNITS];
    gxm_draw_counters counters;
    uint32_t pixels;
    uint32_t *draw_pixels, draw_pixels_cap; /* what it has put on the screen, by draw of the list */
    raster_vertex *vertices;                /* of the draw in hand: each distinct index, shaded */
    uint32_t *slot, *vertex_index, vertices_cap, slot_cap;
    struct batch *b;                        /* the pixels waiting for the program, when they go in rows */
    uint64_t ns[T_WORKER];                  /* for the profile */
    uint64_t in_rows, alone;                /* program runs: pixels that went in rows, and one at a time */
    uint64_t covered;                       /* pixels some triangle covered, whatever became of them */
    program_cost by_program[BY_PROGRAM];    /* and the fill's time by the fragment program it went to */
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

/* A vertex's attributes into the primary attribute registers `pa`, which
 * hold nothing yet: each read from its stream in the stream's form and
 * packed as the program's parameter says. */
void gxm_draw_vertex_words(const draw *d, uint32_t index, uint32_t instance, uint32_t *pa) {
    for (uint32_t i = 0; i < d->inputs; i++) {
        uint32_t size = (uint32_t)ATTRIBUTE_BYTES[d->in[i].format] * d->in[i].components;
        uint32_t at = d->in[i].base + (d->in[i].per_instance ? instance : index) * d->in[i].stride;
        const uint8_t *src = vita_mem_ptr(at, size);
        uint32_t *reg = &pa[d->in[i].reg];
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
}

static void shade_vertex(const draw *d, stage *g, uint32_t index, uint32_t instance, raster_vertex *out) {
    const gxp_program *p = g->p;
    usse_state *st = &g->st;
    const uint16_t *words = d->s->vertex->words;
    memset(st->temp, 0, words[0] * sizeof(uint32_t));
    memset(st->pa, 0, words[1] * sizeof(uint32_t));
    memset(st->internal, 0, sizeof(st->internal));
    memset(st->pred, 0, sizeof(st->pred));
    gxm_draw_vertex_words(d, index, instance, st->pa);
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
    /* a texture of one level is sampled the same whatever its footprint, and a logarithm is not free */
    gxm_tex_sample(t, &wk->cache[in->unit], v, t->cube || t->levels == 1 ? 0.0f : gxm_tex_footprint(t, dx[0], dx[1], dy[0], dy[1]), rgba);
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

    /* a program starts on registers that hold nothing (flush says why) */
    const uint16_t *words = d->s->fragment->words;
    memset(st->temp, 0, words[0] * sizeof(uint32_t));
    memset(st->pa, 0, words[1] * sizeof(uint32_t));
    memset(st->out, 0, words[2] * sizeof(uint32_t));
    memset(st->internal, 0, sizeof(st->internal));
    memset(st->pred, 0, sizeof(st->pred));

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
    wk->alone++;
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

/* Whether a draw's results can be stored by lookup: four halves left by the
 * program, every channel written, no blending, and a surface whose fields
 * pixfmt has tables for. What write_colour does to such a result is what
 * the tables were made by. Called while a draw is being made. */
static const pixfmt_halves *stored_for(const gxm_draw_state *s) {
    const gxm_program *fp = s->fragment;
    const gxp_program *p = fp ? &fp->gxp : NULL;
    if (!p || !s->has_colour || p->result_type != GXP_F16) return NULL;
    if (p->result_components > 2 && s->colour.bytes == 8 && s->colour.fields == 2 && s->colour.f[0].kind == PIX_F32) return NULL;
    if (((fp->blend[1] & 15) || (fp->blend[1] >> 4)) && !(p->flags & GXP_FLAG_NATIVECOLOR)) return NULL;
    if ((fp->blend[0] & 15) != 15) return NULL;
    return pixfmt_halves_for(&s->colour, s->colour_gamma != 0, fp->output_format == 1);
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
static float whole_depth(const gxm_draw_state *s, float depth) { return raster_whole_depth(s->depth_bits, depth); }

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

/* shade_vertex for a row of vertices: `count` of the draw's distinct ones
 * from `first`, their attributes into rows of registers, the program once
 * for all of them, what it leaves taken out vertex by vertex. The registers
 * are the ones a row of pixels uses later; nothing is drawn while vertices
 * are shaded. Each vertex comes out as shade_vertex leaves it. */
static void shade_vertex_row(const draw *d, worker *wk, uint32_t first, unsigned count, uint32_t instance) {
    stage *g = &wk->v;
    const gxp_program *p = g->p;
    usse_wide *w = &wk->b->w;
    const uint16_t *words = d->s->vertex->words;
    unsigned out_words = p->out_words < 8 ? 8 : p->out_words;
    for (unsigned r = 0; r < words[0]; r++) memset(w->temp[r], 0, count * sizeof(uint32_t));
    for (unsigned r = 0; r < words[1]; r++) memset(w->pa[r], 0, count * sizeof(uint32_t));
    for (unsigned r = 0; r < out_words; r++) memset(w->out[r], 0, count * sizeof(uint32_t));
    for (unsigned r = 0; r < 16; r++) memset(w->internal[r], 0, count * sizeof(uint32_t));
    memset(w->pred, 0, sizeof(w->pred));

    for (uint32_t i = 0; i < d->inputs; i++) {
        uint32_t size = (uint32_t)ATTRIBUTE_BYTES[d->in[i].format] * d->in[i].components;
        uint32_t (*row)[USSE_LANES] = &w->pa[d->in[i].reg];
        int as_bytes = d->in[i].raw || d->in[i].format == 10;
        /* the words an attribute can reach, as shade_vertex's packing reaches them */
        unsigned reach = as_bytes ? ((size > 64 ? 64 : size) + 3) / 4 : 4;
        for (unsigned l = 0; l < count; l++) {
            uint32_t at = d->in[i].base + (d->in[i].per_instance ? instance : wk->vertex_index[first + l]) * d->in[i].stride;
            const uint8_t *src = vita_mem_ptr(at, size);
            uint32_t reg[16];
            if (!src) continue;
            for (unsigned k = 0; k < reach; k++) reg[k] = row[k][l];
            if (as_bytes) {
                memcpy(reg, src, size > 64 ? 64 : size);
            } else {
                float v[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                for (unsigned k = 0; k < d->in[i].components && k < 4; k++)
                    v[k] = attribute_value(src + k * ATTRIBUTE_BYTES[d->in[i].format], d->in[i].format);
                for (unsigned k = 0; k < d->in[i].wanted; k++) pack(reg, d->in[i].type, k, v[k]);
            }
            for (unsigned k = 0; k < reach; k++) row[k][l] = reg[k];
        }
    }

    w->sa = g->sa;
    w->env = &g->env;
    w->lanes = count;
    usse_run_wide(p->primary, p->primary_count, w);

    for (unsigned l = 0; l < count; l++) {
        raster_vertex *out = &wk->vertices[first + l];
        for (int k = 0; k < 4; k++) out->pos[k] = bits_to_float(w->out[k][l]);
        for (unsigned slot = 0; slot < GXP_SLOTS; slot++) {
            if (d->slot_at[slot] < 0) continue;
            float *var = &out->var[d->slot_at[slot]];
            for (unsigned k = 0; k < 4; k++)
                var[k] = k < p->out_count[slot] ? bits_to_float(w->out[p->out_reg[slot] + k][l]) : (k == 3 ? 1.0f : 0.0f);
        }
        if (d->w_at >= 0) out->var[d->w_at] = out->pos[3];
    }
}

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

/* sample_input for every pixel waiting: the sample of texture `t` that
 * input `in` hands each of them, component k of pixel l left in out[k][l].
 * The coordinates and their rates of change are those slot_value gives a
 * pixel by itself, projected the same way, and the numbers are worked out
 * by the same arithmetic; what differs is that a row is asked of the
 * texture at once. Returns how many components the texture has. */
static int sample_rows(worker *wk, const gxp_input *in, const gxm_tex *t, const struct batch *b, unsigned n,
                       float (*out)[USSE_LANES]) {
    const draw *d = wk->d;
    float held[4][USSE_LANES], cu[USSE_LANES], cv[USSE_LANES], lod[USSE_LANES];
    if (!t) {
        for (int k = 0; k < 4; k++) memset(out[k], 0, n * sizeof(float));
        wk->counters.no_texture += n;
        return 4;
    }
    unsigned slot = in->coord;
    int position = slot == GXP_SLOT_POSITION, at = !position && slot < GXP_SLOTS ? d->slot_at[slot] : -1;
    const float *v0 = slot_row(d, b, n, slot, 0, held[0]), *v1 = slot_row(d, b, n, slot, 1, held[1]);
    const float *v2 = slot_row(d, b, n, slot, 2, held[2]);
    int wanted = !(t->cube || t->levels == 1);              /* a level to choose: the rates of change are looked at */

    if (in->projection && !t->cube) {
        /* the coordinate over its w, or over its z, and the rate of change of that */
        int by = in->projection == 1 ? 3 : 2;
        const float *over = by == 3 ? slot_row(d, b, n, slot, 3, held[3]) : v2;
        for (unsigned l = 0; l < n; l++) {
            float q = 1.0f / over[l], u = v0[l], v = v1[l];
            if (wanted) {
                float dx[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, dy[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                if (position) { dx[0] = 1.0f; dy[1] = 1.0f; }
                else if (at >= 0) { memcpy(dx, b->ddx[l] + at, sizeof(dx)); memcpy(dy, b->ddy[l] + at, sizeof(dy)); }
                dx[0] = (dx[0] - u * q * dx[by]) * q; dy[0] = (dy[0] - u * q * dy[by]) * q;
                dx[1] = (dx[1] - v * q * dx[by]) * q; dy[1] = (dy[1] - v * q * dy[by]) * q;
                lod[l] = gxm_tex_footprint(t, dx[0], dx[1], dy[0], dy[1]);
            }
            cu[l] = u * q;
            cv[l] = v * q;
        }
        v0 = cu; v1 = cv;
    } else if (wanted) {
        if (position) {
            float one = gxm_tex_footprint(t, 1.0f, 0.0f, 0.0f, 1.0f);
            for (unsigned l = 0; l < n; l++) lod[l] = one;
        } else if (at >= 0) {
            for (unsigned l = 0; l < n; l++)
                lod[l] = gxm_tex_footprint(t, b->ddx[l][at], b->ddx[l][at + 1], b->ddy[l][at], b->ddy[l][at + 1]);
        } else {
            float none = gxm_tex_footprint(t, 0.0f, 0.0f, 0.0f, 0.0f);
            for (unsigned l = 0; l < n; l++) lod[l] = none;
        }
    }
    const float *coord[3] = { v0, v1, v2 };
    float *rows[4] = { out[0], out[1], out[2], out[3] };
    gxm_tex_sample_row(t, &wk->cache[in->unit], n, coord, wanted ? lod : NULL, rows);
    return t->pf.components;
}

/* store_sample for every pixel waiting: the samples into the rows of the
 * registers the program looks for them in, in the form it was compiled to
 * find there. A word store_sample leaves alone is left alone. */
static void store_rows(const gxp_input *in, const gxm_tex *t, float (*rgba)[USSE_LANES], int got,
                       uint32_t (*row)[USSE_LANES], unsigned n) {
    if (in->result == 3) {
        int kept = in->single ? 1 : got;
        for (int k = 0; k < 4; k++) {
            if (k < kept) memcpy(row[k], rgba[k], n * sizeof(float));
            else if (!in->single) memset(row[k], 0, n * sizeof(uint32_t));
        }
    } else if (in->result == 0 && t && t->pf.type == USSE_F32) {
        /* a texture of whole words, kept as it is */
        for (int k = 0; k < got; k++) memcpy(row[k], rgba[k], n * sizeof(float));
    } else if (in->result == 0 && (!t || t->pf.type == USSE_U8 || t->pf.type == USSE_S8)) {
        int is_signed = t && t->pf.type == USSE_S8;
        for (unsigned l = 0; l < n; l++) {
            uint32_t word = 0;
            for (int k = 0; k < got; k++) {
                float c = rgba[k][l];
                if (is_signed) pack(&word, GXP_S8, (unsigned)k, roundf((c < -1.0f ? -1.0f : c > 1.0f ? 1.0f : c) * 127.0f));
                else pack(&word, GXP_U8, (unsigned)k, roundf(unit_clamp(c) * 255.0f));
            }
            row[0][l] = word;
        }
    } else {
        memset(row[0], 0, n * sizeof(uint32_t));
        memset(row[1], 0, n * sizeof(uint32_t));
        for (int k = 0; k < got; k++) vita_floats_to_halves(rgba[k], row[k >> 1], k & 1, n);
    }
}

static mtx_t g_unwritten_lock;
static void unwritten_lock_init(void) { mtx_init(&g_unwritten_lock, mtx_plain); }

/* VITARECOMP_GXM_UNWRITTEN=1: a row of pixels goes through its program
 * twice, with every register filled beforehand with one mark and then with
 * another. A program that reads a register still marked is named, once,
 * with the first instruction of it that does; and whether anything of the
 * difference between the two fills reached its result or its verdict on a
 * pixel, which is whether what it draws depends on what ran before it. */
static void report_unwritten(const gxm_draw_state *s, const gxp_program *p, const usse_unwritten *u,
                             uint32_t (*one)[USSE_LANES], const uint8_t *one_killed,
                             uint32_t (*two)[USSE_LANES], const uint8_t *two_killed, unsigned n) {
    static const char *const BANK[] = { "r", "pa", "o", "sa", "i" };
    static struct { uint64_t name; uint32_t reached; } told[256];
    static uint32_t tellers;
    static once_flag once = ONCE_FLAG_INIT;
    const gxm_program *fp = s->fragment;
    /* which bits of the result the two fills left different, and whether a pixel was discarded on one only */
    uint32_t differs[4] = { 0, 0, 0, 0 }, reached = 0;
    unsigned words = p->result_type == GXP_F32 ? 4 : p->result_type == GXP_U8 || p->result_type == GXP_S8 ? 1 : 2;
    for (unsigned l = 0; l < n; l++) {
        if (one_killed[l] != two_killed[l]) reached |= 0x100;
        if (one_killed[l] || two_killed[l]) continue;
        for (unsigned k = 0; k < words; k++) differs[k] |= one[k][l] ^ two[k][l];
    }
    for (unsigned k = 0; k < words; k++) if (differs[k]) reached |= 1u << k;

    call_once(&once, unwritten_lock_init);
    mtx_lock(&g_unwritten_lock);
    uint32_t i = 0;
    while (i < tellers && told[i].name != fp->name) i++;
    if (i == tellers && tellers < 256) { told[tellers].name = fp->name; told[tellers++].reached = 0xFFFFFFFFu; }
    if (i < tellers && (told[i].reached == 0xFFFFFFFFu || (reached & ~told[i].reached))) {
        told[i].reached = told[i].reached == 0xFFFFFFFFu ? reached : told[i].reached | reached;
        if (u->bank == USSE_PREDICATE)
            fprintf(stderr, "gxm: f_%016llx, instruction %u, is conditional on p%u, which nothing of it has set",
                    (unsigned long long)fp->name, u->instruction + 1, u->num);
        else
            fprintf(stderr, "gxm: f_%016llx, instruction %u, reads %s%u.%c (type %u), which nothing has written",
                    (unsigned long long)fp->name, u->instruction + 1, u->bank < 5 ? BANK[u->bank] : "?", u->num,
                    "xyzw"[u->channel & 3], u->type);
        if (!reached) fprintf(stderr, "; nothing of it reaches the result\n");
        else fprintf(stderr, "; it reaches the result: bits %08X %08X %08X %08X of its words%s (result type %u, %u components; "
                             "blend %02X%02X%02X%02X, output %u, surface of %u bytes)\n",
                     differs[0], differs[1], differs[2], differs[3], (reached & 0x100) ? ", and whether a pixel is discarded" : "",
                     p->result_type, p->result_components, fp->blend[0], fp->blend[1], fp->blend[2], fp->blend[3],
                     fp->output_format, s->colour.bytes);
    }
    mtx_unlock(&g_unwritten_lock);
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
    float scratch[USSE_LANES], sampled[4][USSE_LANES];
    uint32_t reg[4];
    uint64_t lap = g_profile ? now_ns() : 0, at;
#define LAP(stage) do { if (g_profile) { at = now_ns(); wk->ns[stage] += at - lap; lap = at; } } while (0)

    uint32_t (*result)[USSE_LANES] = (p->flags & GXP_FLAG_NATIVECOLOR) ? w->out : w->pa;
    uint32_t first_result[4][USSE_LANES];
    uint8_t first_killed[USSE_LANES];
    usse_unwritten first_read = { 0 };
    for (int pass = usse_wide_watch ? 0 : 1; pass < 2; pass++) {
        if (usse_wide_watch) {
            /* twice, on two fills; the picture drawn in this mode is no picture */
            uint32_t fill = pass ? USSE_UNWRITTEN : ~USSE_UNWRITTEN;
            for (unsigned r = 0; r < USSE_WORDS; r++)
                for (unsigned l = 0; l < n; l++) w->temp[r][l] = w->pa[r][l] = w->out[r][l] = fill;
            for (unsigned r = 0; r < 16; r++) for (unsigned l = 0; l < n; l++) w->internal[r][l] = fill;
            memset(w->pred, pass ? USSE_UNWRITTEN_PREDICATE : 0, sizeof(w->pred));
        } else {
            /* A program starts on registers that hold nothing. On the console
             * they hold what the last program to run on that pipeline left,
             * which no title can mean anything by; here that would be what the
             * last row this thread shaded left, and a picture that changed with
             * which thread drew which tile. */
            const uint16_t *words = s->fragment->words;
            for (unsigned r = 0; r < words[0]; r++) memset(w->temp[r], 0, n * sizeof(uint32_t));
            for (unsigned r = 0; r < words[1]; r++) memset(w->pa[r], 0, n * sizeof(uint32_t));
            for (unsigned r = 0; r < words[2]; r++) memset(w->out[r], 0, n * sizeof(uint32_t));
            for (unsigned r = 0; r < 16; r++) memset(w->internal[r], 0, n * sizeof(uint32_t));
            memset(w->pred, 0, sizeof(w->pred));
        }

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
                LAP(T_INPUTS);
            }
            if (in->has_texture && in->texture_reg < USSE_WORDS - 4) {
                const gxm_tex *t = in->unit < GXP_TEXTURE_UNITS ? wk->f.tex[in->unit] : NULL;
                int got = sample_rows(wk, in, t, b, n, sampled);
                store_rows(in, t, sampled, got, &w->pa[in->texture_reg], n);
                LAP(T_SAMPLES);
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
        LAP(T_INPUTS);
        usse_run_wide(p->primary, p->primary_count, w);
        LAP(T_PROGRAM);
        if (usse_wide_watch && !pass) {
            memcpy(first_result, result, sizeof(first_result));
            memcpy(first_killed, w->killed, sizeof(first_killed));
        } else if (usse_wide_watch) {
            first_read = *usse_wide_unwritten();
            if (first_read.hits) report_unwritten(s, p, &first_read, first_result, first_killed, result, w->killed, n);
        }
    }
    wk->in_rows += n;

    for (unsigned l = 0; l < n; l++) {
        int x = b->x[l], y = b->y[l];
        if (d->late) {
            if (w->killed[l]) continue;
            if (!tested(s, &s->face[b->back[l]], (size_t)y * s->width + (size_t)x, b->depth[l])) continue;
            wk->pixels++;
            if (!s->has_colour) continue;
        }
        for (int k = 0; k < 4; k++) reg[k] = result[k][l];
        if (d->stored) {
            uint8_t *px = pixel(s, x, y);
            if (px) pixfmt_halves_store(d->stored, px, reg);
        } else {
            write_colour(wk, x, y, reg);
        }
    }
    LAP(T_STORE);
#undef LAP
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

/* Whether a draw's stencil and depth tests can be left to the walk, which
 * makes them before a pixel's varyings are worked out: nearly half the
 * pixels a scene's triangles cover fail one or the other. They can when a
 * pixel that fails changes nothing and nothing is asked first that would:
 * one state for both faces, a program that has no say in the tests, and
 * nothing done to the stencil where either test fails. (The mask is asked
 * first, but a masked pixel changes nothing either way: it is dropped
 * after the tests here, before anything is kept.) */
static int tests_in_the_walk(const draw *d) {
    const gxm_draw_state *s = d->s;
    const gxm_face *f = &s->face[0];
    return d->wide && !d->late && !s->two_sided && !f->program_off &&
           (!s->stencil || !f->write_mask || (!f->stencil_fail && (!s->depth || !f->depth_fail)));
}

/* collect_pixel for such a draw: the pixel has passed both tests, with the
 * depth it will be kept at, and the walk keeps it. */
static int collect_passed(void *user, int x, int y, float depth, int clockwise,
                          const float *var, const float *ddx, const float *ddy) {
    worker *wk = user;
    const draw *d = wk->d;
    gxm_draw_state *s = d->s;
    size_t at = (size_t)y * s->width + (size_t)x;
    (void)clockwise;
    if (s->mask && !s->mask[at]) return 0;
    if (!s->depth) depth = whole_depth(s, depth);       /* the walk has done it where there is a depth to test */
    if (s->stencil) stencil_update(&s->stencil[at], s->face[0].depth_pass, &s->face[0]);
    wk->pixels++;
    if (!s->has_colour) return 1;

    struct batch *b = wk->b;
    unsigned n = b->n;
    b->x[n] = x; b->y[n] = y; b->depth[n] = depth; b->back[n] = 0;
    for (int i = 0; i < d->varyings; i++) b->var[i][n] = var[i];
    memcpy(b->ddx[n], ddx, (size_t)d->varyings * sizeof(float));
    memcpy(b->ddy[n], ddy, (size_t)d->varyings * sizeof(float));
    if (++b->n == USSE_LANES) flush(wk);
    return 1;
}

/* --- a scene's draws ---------------------------------------------------------------------
 * A draw is not drawn when it is made. Its state is copied, its uniforms
 * and its textures are read, and it is put on a list; the list is drawn
 * when the scene ends, which is when the console draws it too, and why a
 * title leaves a draw's vertices and textures alone until then. Drawing
 * the list has three steps, and the two long ones are shared out whole:
 *
 *   every draw's vertices through its vertex program and its triangles
 *   clipped and set up, a draw to a thread;
 *   the triangles sorted into the tiles of the target they touch, in the
 *   order they were drawn;
 *   every tile filled, a tile to a thread: its triangles in that order,
 *   the pixels of each that fall in the tile tested and shaded.
 *
 * Tiles share no pixel, so the order of the triangles is kept wherever it
 * matters, and nothing is locked. Drawing a draw at a time, its rows
 * shared out, left most threads waiting through most draws: a scene is
 * hundreds of draws and most of them are small. */

/* Pixels a side. Measured on twenty-four threads: at 64 a target of 720 by
 * 408 is 84 tiles, the threads on the slower cores are still on their last
 * when the rest have none, and the fill of a frame took 78 ms; at 32 it
 * took 59; at 16 the taking of tiles costs more than the evenness saves, 72. */
#define TILE 32u


typedef struct { uint32_t draw, piece; } tile_ref;

static queued **g_queue;                    /* kept from scene to scene, with what each has grown to */
static uint32_t g_queued, g_queue_cap;
static tile_ref *g_refs;                    /* every tile's triangles, tile after tile */
static uint32_t *g_tile_first, *g_tile_at;  /* where each tile's begin in that, and a place in each while filling it */
static uint32_t *g_tile_order;              /* the tiles, those with most triangles first */
static uint32_t g_refs_cap, g_tiles_cap, g_tiles_x, g_tiles_y;
static worker *g_workers;
static float g_first[4][4];
static unsigned g_first_count;

static void profile_report(void) {
    uint64_t ns[T_WORKER] = { 0 }, in_rows = 0, alone = 0, covered = 0;
    for (unsigned w = 0; g_workers && w < vita_pool_size(); w++) {
        worker *wk = &g_workers[w];
        for (int k = 0; k < T_WORKER; k++) ns[k] += wk->ns[k];
        in_rows += wk->in_rows;
        alone += wk->alone;
        covered += wk->covered;
        memset(wk->ns, 0, sizeof(wk->ns));
        wk->in_rows = wk->alone = wk->covered = 0;
    }
    double per = 1e-6 / (double)g_profile;              /* nanoseconds over the frames, to milliseconds a frame */
    uint64_t batched = ns[T_INPUTS] + ns[T_SAMPLES] + ns[T_PROGRAM] + ns[T_STORE];
    /* and what a frame took from one to the next, all told: the title's own work is in that and in nothing below */
    static uint64_t last;
    uint64_t now = now_ns();
    fprintf(stderr, "gxm time, ms a frame over %u frames to %u: %.1f from one frame to the next; draws begun %.1f, vertices and triangles %.1f, "
                    "sorting %.1f, fill %.1f; summed over %u threads: vertices and triangles %.1f, and in the fill "
                    "inputs %.1f, samples %.1f, programs %.1f, tests and store %.1f, "
                    "the walk and pixels shaded alone %.1f; %.0f pixels a frame covered by a triangle, %.0f shaded in rows, %.0f alone\n",
            g_profile, g_frame - 1, last ? (double)(now - last) * per : 0.0,
            (double)g_main_ns[T_BEGIN] * per, (double)g_main_ns[T_VERTICES] * per,
            (double)g_main_ns[T_SETUP] * per, (double)g_main_ns[T_FILL] * per, vita_pool_size(),
            (double)ns[T_PREPARE] * per,
            (double)ns[T_INPUTS] * per, (double)ns[T_SAMPLES] * per, (double)ns[T_PROGRAM] * per, (double)ns[T_STORE] * per,
            (double)(ns[T_BAND] > batched ? ns[T_BAND] - batched : 0) * per,
            (double)covered / (double)g_profile, (double)in_rows / (double)g_profile, (double)alone / (double)g_profile);
    fprintf(stderr, "    by the surface drawn to, ms a frame (lists of draws, draws):");
    for (unsigned i = 0; i < 16 && g_by_target[i].width; i++)
        fprintf(stderr, " %ux%u %.1f (%.1f, %.1f)", g_by_target[i].width, g_by_target[i].height, (double)g_by_target[i].ns * per,
                (double)g_by_target[i].lists / (double)g_profile, (double)g_by_target[i].draws / (double)g_profile);
    fprintf(stderr, "\n");

    /* the fragment programs the fill's time went to, the dearest first */
    static program_cost all[BY_PROGRAM];
    memset(all, 0, sizeof(all));
    for (unsigned w = 0; g_workers && w < vita_pool_size(); w++) {
        worker *wk = &g_workers[w];
        for (unsigned i = 0; i < BY_PROGRAM; i++)
            if (wk->by_program[i].used)
                cost_add(all, wk->by_program[i].name, wk->by_program[i].ns, wk->by_program[i].pixels, wk->by_program[i].covered,
                         wk->by_program[i].draws);
        memset(wk->by_program, 0, sizeof(wk->by_program));
    }
    uint64_t every = 0;
    for (unsigned i = 0; i < BY_PROGRAM; i++) every += all[i].ns;
    fprintf(stderr, "    by the fragment program, of %.1f ms a frame summed over the threads (ms, pixels shaded, of pixels covered, "
                    "ns a pixel shaded, draws in tiles):", (double)every * per);
    for (int shown = 0; shown < 14; shown++) {
        program_cost *most = NULL;
        for (unsigned i = 0; i < BY_PROGRAM; i++)
            if (all[i].used && (!most || all[i].ns > most->ns)) most = &all[i];
        if (!most || !most->ns) break;
        fprintf(stderr, "\n      %016llx %6.1f %8.0f %8.0f %5.0f %6.0f", (unsigned long long)most->name, (double)most->ns * per,
                (double)most->pixels / (double)g_profile, (double)most->covered / (double)g_profile,
                most->pixels ? (double)most->ns / (double)most->pixels : 0.0, (double)most->draws / (double)g_profile);
        most->used = 0;
    }
    fprintf(stderr, "\n");

    memset(g_by_target, 0, sizeof(g_by_target));
    memset(g_main_ns, 0, sizeof(g_main_ns));
    last = now;
}

unsigned gxm_draw_positions(float (*out)[4], unsigned max) {
    unsigned n = g_first_count < max ? g_first_count : max;
    for (unsigned i = 0; i < n; i++) memcpy(out[i], g_first[i], 4 * sizeof(float));
    return n;
}

/* --- a draw is made ---------------------------------------------------------------------- */

static uint32_t up_to_two(uint32_t n) { uint32_t p = 1; while (p < n) p <<= 1; return p; }

/* Whether a stage of a draw has a texture whose bytes lie among those of
 * the colour surface the draw is for. */
static int samples_target(const gxm_draw_state *s, const stage *g) {
    if (!s->has_colour || !s->colour_data) return 0;
    size_t pixels = s->colour_layout == GXM_SURFACE_TILED ? (size_t)((s->height + 31) / 32) * (s->colour_stride / 32) * 1024
                  : s->colour_layout == GXM_SURFACE_SWIZZLED ? (size_t)up_to_two(s->width) * up_to_two(s->height)
                  : (size_t)s->colour_stride * s->height;
    const uint8_t *lo = s->colour_data, *hi = lo + pixels * s->colour.bytes;
    for (unsigned unit = 0; unit < GXP_TEXTURE_UNITS; unit++) {
        const gxm_tex *t = g->tex[unit];
        if (t && t->base && t->base < hi && t->base + (t->bytes < t->avail ? t->bytes : t->avail) > lo) return 1;
    }
    return 0;
}

static queued *next_queued(void) {
    if (g_queued == g_queue_cap) {
        uint32_t cap = g_queue_cap ? g_queue_cap * 2 : 256;
        queued **grown = realloc(g_queue, (size_t)cap * sizeof(*grown));
        if (!grown) return NULL;
        memset(grown + g_queue_cap, 0, (size_t)(cap - g_queue_cap) * sizeof(*grown));
        g_queue = grown;
        g_queue_cap = cap;
    }
    if (!g_queue[g_queued]) g_queue[g_queued] = calloc(1, sizeof(queued));
    return g_queue[g_queued];
}

void gxm_draw_add(const gxm_draw_state *s, unsigned primitive, unsigned index_size, uint32_t indices,
                  uint32_t count, uint32_t instances, uint32_t tally, int tally_set) {
    static uint32_t serial;
    static int asked, scalar, at_once;
    const gxm_program *vp = s->vertex, *fp = s->fragment;
    gxm_counters.draws++;
    if (primitive == GXM_LINES || primitive == GXM_POINTS) { gxm_counters.not_triangles++; return; }
    if (!vp || !vp->ok || (fp && !fp->ok) || !s->width || !s->height) { gxm_counters.no_program++; return; }
    if (count < 3 || !instances || s->region_mode == GXM_REGION_ALL) return;
    if (!g_workers) g_workers = calloc(vita_pool_size(), sizeof(*g_workers));
    if (!g_workers) return;
    if (!asked) {
        const char *e = getenv("VITARECOMP_GXM_PROFILE");
        g_profile = e ? (uint32_t)strtoul(e, NULL, 0) : 0;
        /* VITARECOMP_GXM_SCALAR=1 shades every pixel by itself, as the definition
         * of what the rows must come out as. */
        e = getenv("VITARECOMP_GXM_SCALAR");
        scalar = e && *e && *e != '0';
        /* VITARECOMP_GXM_AT_ONCE=1 draws each draw as it is made: what keeping
         * them for the end of the scene must not change, and the way to tell
         * a title that changes a draw's data before its scene has ended. */
        e = getenv("VITARECOMP_GXM_AT_ONCE");
        at_once = e && *e && *e != '0';
        e = getenv("VITARECOMP_GXM_UNWRITTEN");
        usse_wide_watch = e && *e && *e != '0';
        /* VITARECOMP_GXM_ONE_LEVEL=1 samples every texture from its first
         * level: for holding two renderers against each other (gxm_tex.h). */
        e = getenv("VITARECOMP_GXM_ONE_LEVEL");
        gxm_tex_one_level = e && *e && *e != '0';
        asked = 1;
    }
    /* a list is of one target: what waits for another is drawn first */
    if (g_queued) {
        const gxm_draw_state *was = &g_queue[0]->s;
        if (was->width != s->width || was->height != s->height || was->colour_data != s->colour_data ||
            was->depth != s->depth || was->stencil != s->stencil || was->mask != s->mask) gxm_draw_flush();
    }
    uint64_t began = g_profile ? now_ns() : 0;
    queued *q = next_queued();
    if (!q) return;
    q->s = *s;
    q->primitive = primitive; q->index_size = index_size;
    q->indices = indices; q->count = count; q->instances = instances;
    q->tally = tally; q->tally_set = tally_set;
    q->piece_count = 0; q->pixels = 0; q->first_count = 0;

    /* the uniforms and the textures are read now, as the title has just left them */
    draw *d = &q->d;
    memset(&d->in, 0, sizeof(d->in));
    d->s = &q->s;
    d->serial = ++serial;
    stage_begin(&d->v, &vp->gxp, q->s.vertex_buffers, q->s.vertex_textures, q->tex[0], g_vertex_cache, 0);
    if (fp) stage_begin(&d->f, &fp->gxp, q->s.fragment_buffers, q->s.fragment_textures, q->tex[1], g_workers[0].cache, 1);
    else memset(&d->f, 0, sizeof(d->f));
    vertex_inputs(d);

    /* A draw that samples the surface its scene draws to reads what the
     * draws before it left there: a chain of smaller and smaller copies kept
     * side by side in one surface is made that way, each read from the last.
     * The console gets the same by finishing its tiles in order. So
     * everything before such a draw is drawn first, and it is drawn before
     * anything after it. */
    int own = fp && samples_target(&q->s, &d->f);
    if (own && g_queued) {
        uint32_t k = g_queued;
        gxm_draw_flush();
        queued *first = g_queue[0];             /* the record in hand becomes the list's first */
        g_queue[0] = g_queue[k];
        g_queue[k] = first;
    }

    d->wide = fp && fp->wide && !scalar;
    d->wide_vertices = vp->wide && !scalar;
    d->stored = d->wide ? stored_for(&q->s) : NULL;
    d->late = fp && (fp->gxp.flags & (GXP_FLAG_DISCARD | GXP_FLAG_DEPTH)) != 0;
    d->each = fp && (fp->gxp.flags & GXP_FLAG_FRAGCOLOR) && s->has_colour;

    /* What the fragment program reads of the vertex program's outputs is
     * what gets interpolated; nothing else is carried. */
    d->varyings = 0;
    d->w_at = -1;
    for (unsigned slot = 0; slot < GXP_SLOTS; slot++) {
        d->slot_at[slot] = -1;
        if (!fp || slot >= GXP_SLOT_POSITION || !((fp->gxp.slots_used >> slot) & 1) || !vp->gxp.out_count[slot]) continue;
        if (d->varyings + 4 > RASTER_MAX_VARYINGS) continue;
        d->slot_at[slot] = d->varyings;
        d->varyings += 4;
    }
    if (fp && ((fp->gxp.slots_used >> GXP_SLOT_POSITION) & 1) && d->varyings < RASTER_MAX_VARYINGS) d->w_at = d->varyings++;

    raster *r = &q->r;
    memset(r, 0, sizeof(*r));
    r->width = (int)s->width;
    r->height = (int)s->height;
    r->clip_x0 = 0; r->clip_y0 = 0; r->clip_x1 = (int)s->width; r->clip_y1 = (int)s->height;
    if (s->region_mode == GXM_REGION_OUTSIDE) {
        /* whole tiles: the region grows to the 32-pixel grid */
        int x0 = (int)(s->region[0] & ~31u), y0 = (int)(s->region[1] & ~31u);
        int x1 = (int)((s->region[2] + 32) & ~31u), y1 = (int)((s->region[3] + 32) & ~31u);
        if (x0 > r->clip_x0) r->clip_x0 = x0;
        if (y0 > r->clip_y0) r->clip_y0 = y0;
        if (x1 < r->clip_x1) r->clip_x1 = x1;
        if (y1 < r->clip_y1) r->clip_y1 = y1;
    }
    if (s->viewport_on) {
        r->x_offset = s->offset[0]; r->x_scale = s->scale[0];
        r->y_offset = s->offset[1]; r->y_scale = s->scale[1];
        r->z_offset = s->offset[2]; r->z_scale = s->scale[2];
    } else {
        r->x_scale = r->y_scale = r->z_scale = 1.0f;
    }
    r->cull = s->cull == 1 ? RASTER_CULL_CW : s->cull == 2 ? RASTER_CULL_CCW : RASTER_CULL_NONE;
    r->clip_z = RASTER_CLIP_FAR;            /* nearer than the near plane is drawn, at depth 0 */
    r->clamp_depth = 1;
    r->depth_func = RASTER_ALWAYS;          /* the tests are made here, with the stencil */
    r->varyings = d->varyings;
    /* The rate of change of a varying is wanted where it places a sample of
     * a texture that has smaller copies of itself to choose among. */
    r->derivatives = 0;
    for (uint32_t i = 0; fp && i < fp->gxp.input_count; i++) {
        const gxp_input *in = &fp->gxp.inputs[i];
        const gxm_tex *t = in->has_texture && in->unit < GXP_TEXTURE_UNITS ? d->f.tex[in->unit] : NULL;
        if (!t || t->cube || t->levels == 1 || in->coord >= GXP_SLOTS || d->slot_at[in->coord] < 0) continue;
        r->derivatives |= 15ull << d->slot_at[in->coord];
    }
    g_queued++;
    if (g_profile) g_main_ns[T_BEGIN] += now_ns() - began;
    if (at_once || own) gxm_draw_flush();
}

/* --- the first step: vertices and triangles ------------------------------------------------ */

static int reserve(worker *wk, uint32_t vertices, uint32_t range) {
    if (vertices > wk->vertices_cap) {
        raster_vertex *v = realloc(wk->vertices, (size_t)vertices * sizeof(*v));
        if (v) wk->vertices = v;
        uint32_t *n = realloc(wk->vertex_index, (size_t)vertices * sizeof(*n));
        if (n) wk->vertex_index = n;
        if (!v || !n) return 0;
        wk->vertices_cap = vertices;
    }
    if (range > wk->slot_cap) {
        uint32_t *m = realloc(wk->slot, (size_t)range * sizeof(*m));
        if (!m) return 0;
        wk->slot = m; wk->slot_cap = range;
    }
    return 1;
}

/* Set a triangle up after the others of its draw. */
static void triangle(queued *q, const raster_vertex *a, const raster_vertex *b, const raster_vertex *c) {
    size_t per = raster_plane_floats(&q->r), need = ((size_t)q->piece_count + RASTER_MAX_PIECES) * per;
    if (q->piece_count + RASTER_MAX_PIECES > q->pieces_cap) {
        uint32_t cap = q->pieces_cap ? q->pieces_cap * 2 : 64;
        raster_piece *p = realloc(q->pieces, (size_t)cap * sizeof(*p));
        if (!p) return;
        q->pieces = p; q->pieces_cap = cap;
    }
    if (need > q->planes_cap) {
        size_t cap = q->planes_cap ? q->planes_cap : 4096;
        while (cap < need) cap *= 2;
        float *f = realloc(q->planes, cap * sizeof(*f));
        if (!f) return;
        q->planes = f; q->planes_cap = cap;
    }
    q->piece_count += (uint32_t)raster_setup(&q->r, a, b, c, &q->pieces[q->piece_count], q->planes + (size_t)q->piece_count * per);
}

static uint32_t index_at(const uint8_t *data, unsigned size, uint32_t i) {
    return size == 2 ? (uint32_t)(data[2 * i] | data[2 * i + 1] << 8)
                     : (uint32_t)data[4 * i] | (uint32_t)data[4 * i + 1] << 8 |
                       (uint32_t)data[4 * i + 2] << 16 | (uint32_t)data[4 * i + 3] << 24;
}

/* One draw, by one thread: each index shaded once an instance, with the
 * thread's own copy of the vertex stage, and the triangles put together. */
static void prepare(void *ctx, unsigned item, unsigned index);
static void prepare(void *ctx, unsigned item, unsigned index) {
    queued *q = g_queue[item];
    worker *wk = &g_workers[index];
    uint64_t began = g_profile ? now_ns() : 0;
    (void)ctx;
    const uint8_t *data = vita_mem_ptr(q->indices, q->count * q->index_size);
    if (!data) return;
    uint32_t low = 0xFFFFFFFFu, high = 0, count = q->count;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t v = index_at(data, q->index_size, i);
        if (v < low) low = v;
        if (v > high) high = v;
    }
    uint32_t range = high - low + 1;
    if (range > 0x1000000u || !reserve(wk, range < count ? range : count, range)) return;

    wk->v = q->d.v;
    wk->v.cache = NULL;
    wk->v.counters = &wk->counters;
    wk->v.env.user = &wk->v;
    wk->v.st.sa = wk->v.sa;
    wk->v.st.env = &wk->v.env;

    for (uint32_t instance = 0; instance < q->instances; instance++) {
        uint32_t used = 0, tri[3];
        memset(wk->slot, 0xFF, (size_t)range * sizeof(*wk->slot));
        /* number the distinct vertices, shade them, then put the triangles together */
        for (uint32_t i = 0; i < count; i++) {
            uint32_t v = index_at(data, q->index_size, i);
            if (wk->slot[v - low] != 0xFFFFFFFFu) continue;
            wk->slot[v - low] = used;
            wk->vertex_index[used++] = v;
        }
        if (q->d.wide_vertices && !wk->b) wk->b = calloc(1, sizeof(*wk->b));
        if (q->d.wide_vertices && wk->b) {
            for (uint32_t i = 0; i < used; i += USSE_LANES)
                shade_vertex_row(&q->d, wk, i, used - i < USSE_LANES ? used - i : USSE_LANES, instance);
        } else {
            for (uint32_t i = 0; i < used; i++) shade_vertex(&q->d, &wk->v, wk->vertex_index[i], instance, &wk->vertices[i]);
        }
        if (!instance) {
            q->first_count = used < 4 ? used : 4;
            for (unsigned i = 0; i < q->first_count; i++) memcpy(q->first[i], wk->vertices[i].pos, 4 * sizeof(float));
        }
        for (uint32_t i = 0; i < count; i++) {
            uint32_t at = wk->slot[index_at(data, q->index_size, i) - low];
            if (q->primitive == GXM_TRIANGLE_STRIP) {
                if (i < 2) { tri[i] = at; continue; }
                tri[2] = at;
                /* every other triangle of a strip is wound the other way: swap its first two */
                if (i & 1) triangle(q, &wk->vertices[tri[1]], &wk->vertices[tri[0]], &wk->vertices[tri[2]]);
                else       triangle(q, &wk->vertices[tri[0]], &wk->vertices[tri[1]], &wk->vertices[tri[2]]);
                tri[0] = tri[1]; tri[1] = tri[2];
            } else if (q->primitive == GXM_TRIANGLE_FAN) {
                if (i == 0) { tri[0] = at; continue; }
                if (i == 1) { tri[1] = at; continue; }
                triangle(q, &wk->vertices[tri[0]], &wk->vertices[tri[1]], &wk->vertices[at]);
                tri[1] = at;
            } else {
                tri[i % 3] = at;
                if (i % 3 == 2) triangle(q, &wk->vertices[tri[0]], &wk->vertices[tri[1]], &wk->vertices[tri[2]]);
            }
        }
    }
    if (g_profile) wk->ns[T_PREPARE] += now_ns() - began;
}

/* --- the second step: which triangles a tile has -------------------------------------------- */

static int fuller_tile(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    uint32_t in_x = g_tile_first[x + 1] - g_tile_first[x], in_y = g_tile_first[y + 1] - g_tile_first[y];
    return in_x != in_y ? (in_x < in_y ? 1 : -1) : (x > y) - (x < y);
}

static int sort_into_tiles(uint32_t width, uint32_t height) {
    g_tiles_x = (width + TILE - 1) / TILE;
    g_tiles_y = (height + TILE - 1) / TILE;
    uint32_t tiles = g_tiles_x * g_tiles_y;
    if (tiles + 1 > g_tiles_cap) {
        uint32_t *first = realloc(g_tile_first, (size_t)(tiles + 1) * sizeof(*first));
        if (first) g_tile_first = first;
        uint32_t *at = realloc(g_tile_at, (size_t)(tiles + 1) * sizeof(*at));
        if (at) g_tile_at = at;
        uint32_t *order = realloc(g_tile_order, (size_t)(tiles + 1) * sizeof(*order));
        if (order) g_tile_order = order;
        if (!first || !at || !order) return 0;
        g_tiles_cap = tiles + 1;
    }
    /* count, then place: a tile's triangles end up in the order of the draws and, in a draw, of its triangles */
    memset(g_tile_first, 0, (size_t)(tiles + 1) * sizeof(*g_tile_first));
    for (uint32_t i = 0; i < g_queued; i++) {
        const queued *q = g_queue[i];
        for (uint32_t k = 0; k < q->piece_count; k++) {
            const raster_piece *t = &q->pieces[k];
            for (uint32_t ty = (uint32_t)t->y0 / TILE; ty <= (uint32_t)t->y1 / TILE; ty++)
                for (uint32_t tx = (uint32_t)t->x0 / TILE; tx <= (uint32_t)t->x1 / TILE; tx++)
                    g_tile_first[ty * g_tiles_x + tx + 1]++;
        }
    }
    for (uint32_t i = 0; i < tiles; i++) g_tile_first[i + 1] += g_tile_first[i];
    uint32_t total = g_tile_first[tiles];
    if (total > g_refs_cap) {
        uint32_t cap = g_refs_cap ? g_refs_cap : 65536;
        while (cap < total) cap *= 2;
        tile_ref *refs = realloc(g_refs, (size_t)cap * sizeof(*refs));
        if (!refs) return 0;
        g_refs = refs; g_refs_cap = cap;
    }
    memcpy(g_tile_at, g_tile_first, (size_t)tiles * sizeof(*g_tile_at));
    for (uint32_t i = 0; i < g_queued; i++) {
        const queued *q = g_queue[i];
        for (uint32_t k = 0; k < q->piece_count; k++) {
            const raster_piece *t = &q->pieces[k];
            for (uint32_t ty = (uint32_t)t->y0 / TILE; ty <= (uint32_t)t->y1 / TILE; ty++)
                for (uint32_t tx = (uint32_t)t->x0 / TILE; tx <= (uint32_t)t->x1 / TILE; tx++) {
                    tile_ref *ref = &g_refs[g_tile_at[ty * g_tiles_x + tx]++];
                    ref->draw = i;
                    ref->piece = k;
                }
        }
    }
    /* Threads take tiles in turn. The ones with most in them go first, so
     * that the last tile anyone is left finishing is a small one. */
    for (uint32_t i = 0; i < tiles; i++) g_tile_order[i] = i;
    qsort(g_tile_order, tiles, sizeof(*g_tile_order), fuller_tile);
    return total != 0;
}

/* --- the third step: a tile's pixels --------------------------------------------------------- */

/* A thread's own copy of a draw's fragment stage, made each time it comes
 * to the draw from another. */
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

static void fill_tile(void *ctx, unsigned item, unsigned index) {
    worker *wk = &g_workers[index];
    uint32_t tile = g_tile_order[item];
    uint32_t at = g_tile_first[tile], end = g_tile_first[tile + 1];
    (void)ctx;
    if (at == end) return;
    int col0 = (int)((tile % g_tiles_x) * TILE), row0 = (int)((tile / g_tiles_x) * TILE), side = (int)TILE;
    uint64_t began = g_profile ? now_ns() : 0;
    while (at < end) {
        uint32_t which = g_refs[at].draw;
        const queued *q = g_queue[which];
        size_t per = raster_plane_floats(&q->r);
        raster_shade shade = shade_pixel;
        raster r = q->r;                    /* this thread's, to say how the pixels are to be taken */
        worker_for(&q->d, index);
        if (q->d.wide) {
            if (!wk->b) wk->b = calloc(1, sizeof(*wk->b));
            if (wk->b) shade = collect_pixel;
            if (wk->b && tests_in_the_walk(&q->d)) {
                const gxm_draw_state *s = &q->s;
                const gxm_face *f = &s->face[0];
                shade = collect_passed;
                if (s->stencil && (f->stencil_func & 7) != 7) {
                    r.stencil = s->stencil;
                    r.stencil_func = f->stencil_func & 7;
                    r.stencil_ref = f->stencil_ref;
                    r.stencil_mask = f->compare_mask;
                }
                if (s->depth) {
                    r.depth = s->depth;
                    r.depth_bits = s->depth_bits;
                    r.depth_func = f->depth_func & 7;
                    r.depth_write = f->depth_write;
                }
            }
        }
        uint64_t draw_began = g_profile ? now_ns() : 0, covered_before = wk->covered;
        for (; at < end && g_refs[at].draw == which; at++) {
            uint32_t k = g_refs[at].piece;
            wk->covered += raster_walk_rect(&r, &q->pieces[k], q->planes + (size_t)k * per, row0, row0 + side, col0, col0 + side, shade, wk);
            if (q->d.each) flush(wk);
        }
        flush(wk);                          /* nothing waits beyond its draw, or its tile */
        if (g_profile)
            cost_add(wk->by_program, q->s.fragment ? q->s.fragment->name : 0, now_ns() - draw_began, wk->pixels,
                     wk->covered - covered_before, 1);
        wk->draw_pixels[which] += wk->pixels;
        wk->pixels = 0;
    }
    if (g_profile) wk->ns[T_BAND] += now_ns() - began;
}

/* --- the list is drawn ------------------------------------------------------------------------ */

/* The list drawn here, on the processor: what each draw put on the screen
 * is left in its `pixels`. Returns 0 when it could not be. */
static int draw_list(uint32_t queued_now) {
    uint64_t lap = g_profile ? now_ns() : 0, at, began = lap;
#define LAP(stage) do { if (g_profile) { at = now_ns(); g_main_ns[stage] += at - lap; lap = at; } } while (0)

    for (unsigned w = 0; w < vita_pool_size(); w++) {
        worker *wk = &g_workers[w];
        if (wk->draw_pixels_cap < queued_now) {
            uint32_t *grown = realloc(wk->draw_pixels, (size_t)queued_now * sizeof(*grown));
            if (!grown) return 0;
            wk->draw_pixels = grown;
            wk->draw_pixels_cap = queued_now;
        }
        memset(wk->draw_pixels, 0, (size_t)queued_now * sizeof(*wk->draw_pixels));
        wk->pixels = 0;
    }

    vita_pool_run(queued_now, prepare, NULL);
    LAP(T_VERTICES);
    int any = sort_into_tiles(g_queue[0]->s.width, g_queue[0]->s.height);
    LAP(T_SETUP);
    if (any) vita_pool_run(g_tiles_x * g_tiles_y, fill_tile, NULL);
    LAP(T_FILL);
#undef LAP
    if (g_profile) {
        uint32_t width = g_queue[0]->s.width, height = g_queue[0]->s.height;
        unsigned i = 0;
        while (i < 15 && g_by_target[i].width && (g_by_target[i].width != width || g_by_target[i].height != height)) i++;
        g_by_target[i].width = width; g_by_target[i].height = height;
        g_by_target[i].lists++;
        g_by_target[i].draws += queued_now;
        g_by_target[i].ns += now_ns() - began;
    }
    for (uint32_t i = 0; i < queued_now; i++)
        for (unsigned w = 0; w < vita_pool_size(); w++) g_queue[i]->pixels += g_workers[w].draw_pixels[i];
    return 1;
}

uint32_t gxm_draw_flush(void) {
    uint32_t pixels = 0, queued_now = g_queued;
    if (!queued_now) return 0;

    /* The host's card draws the list when it has been asked to and can
     * (gxm_gpu.h); what it cannot, and everything when it has not been
     * asked, is drawn here. Asked to check, it draws first and aside, and
     * what it drew is held against what is then drawn here. */
    int card = gxm_gpu_mode(), drawn = 0;
    if (card == GXM_GPU_DRAW) drawn = gxm_gpu_list(g_queue, queued_now, 0);
    else if (card == GXM_GPU_CHECK) gxm_gpu_list(g_queue, queued_now, 1);
    if (!drawn && !draw_list(queued_now)) { g_queued = 0; return 0; }
    if (card == GXM_GPU_CHECK) gxm_gpu_compare(g_queue, queued_now);

    for (uint32_t i = 0; i < queued_now; i++) {
        queued *q = g_queue[i];
        gxm_counters.triangles += q->r.triangles_drawn;
        gxm_counters.pixels += q->pixels;
        pixels += q->pixels;
        /* the visibility test counts what the draw put on the screen */
        if (q->tally && vita_mem_ptr(q->tally, 4)) {
            if (q->tally_set) { if (q->pixels) vita_write32(q->tally, 1); }
            else vita_write32(q->tally, vita_read32(q->tally) + q->pixels);
        }
    }
    for (unsigned w = 0; w < vita_pool_size(); w++) {
        worker *wk = &g_workers[w];
        gxm_counters.stopped += wk->counters.stopped;
        gxm_counters.no_texture += wk->counters.no_texture;
        if (wk->counters.stopped_at) gxm_counters.stopped_at = wk->counters.stopped_at;
        memset(&wk->counters, 0, sizeof(wk->counters));
        wk->d = NULL;                       /* the list's places are used again by other draws */
    }
    const queued *last = g_queue[queued_now - 1];
    g_first_count = last->first_count;
    memcpy(g_first, last->first, sizeof(g_first));
    g_queued = 0;
    return pixels;
}

uint32_t gxm_draw(gxm_draw_state *s, unsigned primitive, unsigned index_size, uint32_t indices,
                  uint32_t count, uint32_t instances) {
    gxm_draw_flush();
    g_first_count = 0;
    gxm_draw_add(s, primitive, index_size, indices, count, instances, 0, 0);
    return gxm_draw_flush();
}
