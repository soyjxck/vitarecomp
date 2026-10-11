/* gxm_draw.c — one draw call, from index buffer to pixels. See gxm_draw.h. */

#include "gxm_draw.h"
#include "gxm_tex.h"
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
    gxm_tex  *tex[GXP_TEXTURE_UNITS];       /* by unit; NULL where nothing usable is bound */
    usse_env  env;
    usse_state st;
} stage;

/* The textures of both stages of the draw in progress. They are large, and
 * a draw is never entered twice. */
static gxm_tex g_textures[2][GXP_TEXTURE_UNITS];

static uint32_t env_read32(void *user, uint32_t address) {
    (void)user;
    return vita_mem_ptr(address, 4) ? vita_read32(address) : 0;
}

static gxm_tex *sampler(stage *g, uint32_t sa_register) {
    for (uint32_t i = 0; i < g->p->sampler_count; i++)
        if (g->p->samplers[i].reg == sa_register) {
            uint32_t unit = g->p->samplers[i].unit;
            return unit < GXP_TEXTURE_UNITS ? g->tex[unit] : NULL;
        }
    return NULL;
}

/* A sample the program asks for itself. There are no neighbouring pixels
 * to measure a footprint from, so it comes from the first level unless the
 * instruction names another. */
static int env_sample(void *user, uint32_t sa_register, const float *coord, int dimensions,
                      int lod_mode, float lod, float rgba[4]) {
    gxm_tex *t = sampler(user, sa_register);
    (void)dimensions;
    if (!t) { gxm_counters.no_texture++; rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.0f; return 4; }
    gxm_tex_sample(t, coord, lod_mode ? lod : 0.0f, rgba);
    return t->pf.components;
}

static void env_gather(void *user, uint32_t sa_register, const float *coord, float texels[4], float fraction[2]) {
    gxm_tex *t = sampler(user, sa_register);
    if (!t) { gxm_counters.no_texture++; memset(texels, 0, 4 * sizeof(float)); fraction[0] = fraction[1] = 0.0f; return; }
    gxm_tex_gather(t, coord, texels, fraction);
}

static void run(stage *g, const usse_insn *code, uint32_t count) {
    g->st.killed = 0;
    usse_reset(&g->st);
    uint32_t at = usse_run(code, count, &g->st);
    if (at) { gxm_counters.stopped++; gxm_counters.stopped_at = at; }
}

/* VITARECOMP_GXM_TEXTURES=<directory>: each texture a draw samples is written
 * there once as an image of its first level, named by where its texels are
 * and what they are, with a line on stderr saying how it is laid out. The
 * first thing to look at when a surface comes out wrong is whether the
 * texture on it was read right. */
static void dump_texture(gxm_tex *t, unsigned unit, int fragment) {
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
                gxm_tex_texel(t, face, 0, (int)x, (int)y, c);
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
                        uint32_t textures[GXP_TEXTURE_UNITS][4], gxm_tex *storage) {
    memset(g, 0, sizeof(*g));
    g->p = p;
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
        if (t->base) { g->tex[unit] = t; dump_texture(t, unit, storage == g_textures[1]); }
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
    stage  v, f;

    /* a vertex program's inputs, resolved */
    struct { uint32_t base, stride; uint16_t reg; uint8_t format, components, per_instance, type, wanted, raw; } in[16];
    uint32_t inputs;

    /* which of the vertex program's outputs travel to the fragment program,
     * four floats each, and where in a vertex's row of varyings */
    int slot_at[GXP_SLOTS];
    int w_at;                               /* the clip-space w, for programs that ask for the position */
    int varyings;

    const gxm_face *face;
    uint32_t pixels;
} draw;

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

static void shade_vertex(draw *d, uint32_t index, uint32_t instance, raster_vertex *out) {
    const gxp_program *p = d->v.p;
    usse_state *st = &d->v.st;
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
    run(&d->v, p->primary, p->primary_count);

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

/* Fill the primary attributes from the program's input list and run it. */
static void run_fragment(draw *d, int x, int y, float depth, const float *var, const float *ddx, const float *ddy) {
    const gxp_program *p = d->f.p;
    usse_state *st = &d->f.st;
    float v[4], dx[4], dy[4];

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
            uint32_t *reg = &st->pa[in->texture_reg];
            gxm_tex *t = in->unit < GXP_TEXTURE_UNITS ? d->f.tex[in->unit] : NULL;
            float rgba[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            int n = 4;
            if (t) {
                slot_value(d, in->coord, x, y, depth, var, ddx, ddy, v, dx, dy);
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
                gxm_tex_sample(t, v, t->cube ? 0.0f : gxm_tex_footprint(t, dx[0], dx[1], dy[0], dy[1]), rgba);
                n = t->pf.components;
            } else {
                gxm_counters.no_texture++;
            }
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
    }

    /* a program that blends for itself is given the pixel as it stands */
    if ((p->flags & GXP_FLAG_FRAGCOLOR) && d->s->has_colour) {
        const uint8_t *px = pixel(d->s, x, y);
        if (px && is_raw_result(p)) {
            memset(st->out, 0, 16);
            memcpy(st->out, px, d->s->colour.bytes > 16 ? 16 : d->s->colour.bytes);
        } else if (px) {
            float now[4];
            read_pixel(d->s, px, now);
            if (p->result_type == GXP_F16) { st->out[0] = halves(now[0], now[1]); st->out[1] = halves(now[2], now[3]); }
            else if (p->result_type == GXP_F32) for (int k = 0; k < 4; k++) st->out[k] = float_to_bits(now[k]);
            else { st->out[0] = 0; for (unsigned k = 0; k < 4; k++) pack(st->out, GXP_U8, k, roundf(unit_clamp(now[k]) * 255.0f)); }
        }
    }
    run(&d->f, p->primary, p->primary_count);
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

/* The program's result into the colour surface: decoded from the registers
 * it was left in, blended with what is there, stored in the surface's form. */
static void write_colour(draw *d, int x, int y) {
    const gxm_draw_state *s = d->s;
    const gxm_program *fp = s->fragment;
    const gxp_program *p = d->f.p;
    const uint32_t *reg = (p->flags & GXP_FLAG_NATIVECOLOR) ? d->f.st.out : d->f.st.pa;
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

/* One covered pixel: the mask, the stencil and depth tests, the program,
 * the colour. A program that may discard runs before the tests, since its
 * verdict decides whether depth and stencil change at all. */
static int shade_pixel(void *user, int x, int y, float depth, int clockwise,
                       const float *var, const float *ddx, const float *ddy) {
    draw *d = user;
    gxm_draw_state *s = d->s;
    size_t at = (size_t)y * s->width + (size_t)x;
    if (s->mask && !s->mask[at]) return 0;

    /* A depth surface of integers compares and keeps integers: without this
     * a surface stored and loaded again would never equal the depth of the
     * same geometry drawn a second time. */
    if (s->depth_bits) {
        double top = (double)((1u << s->depth_bits) - 1), steps = floor((double)depth * top + 0.5);
        depth = (float)((steps > top ? top : steps > 0.0 ? steps : 0.0) / top);
    }

    /* counter-clockwise on the screen is the front */
    const gxm_face *f = &s->face[(s->two_sided && clockwise) ? 1 : 0];
    int shaded = d->f.p && !f->program_off;
    int ran = 0;
    if (shaded && (d->f.p->flags & (GXP_FLAG_DISCARD | GXP_FLAG_DEPTH))) {
        run_fragment(d, x, y, depth, var, ddx, ddy);
        if (d->f.st.killed) return 0;
        ran = 1;
    }

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
    d->pixels++;

    if (shaded && s->has_colour) {
        if (!ran) run_fragment(d, x, y, depth, var, ddx, ddy);
        write_colour(d, x, y);
    }
    return 1;
}

/* --- the draw -------------------------------------------------------------------------- */

static raster_vertex *g_vertices;
static uint32_t *g_slot, g_vertices_cap, g_slot_cap;

static int reserve(uint32_t vertices, uint32_t range) {
    if (vertices > g_vertices_cap) {
        raster_vertex *v = realloc(g_vertices, (size_t)vertices * sizeof(*v));
        if (!v) return 0;
        g_vertices = v; g_vertices_cap = vertices;
    }
    if (range > g_slot_cap) {
        uint32_t *m = realloc(g_slot, (size_t)range * sizeof(*m));
        if (!m) return 0;
        g_slot = m; g_slot_cap = range;
    }
    return 1;
}

void gxm_draw(gxm_draw_state *s, unsigned primitive, unsigned index_size, uint32_t indices,
              uint32_t count, uint32_t instances) {
    static draw d;                          /* large, and never entered twice */
    const gxm_program *vp = s->vertex, *fp = s->fragment;
    gxm_counters.draws++;
    if (primitive == GXM_LINES || primitive == GXM_POINTS) { gxm_counters.not_triangles++; return; }
    if (!vp || !vp->ok || (fp && !fp->ok) || !s->width || !s->height) { gxm_counters.no_program++; return; }
    if (count < 3 || !instances) return;
    const uint8_t *index_data = vita_mem_ptr(indices, count * index_size);
    if (!index_data) return;

    memset(&d.in, 0, sizeof(d.in));
    d.s = s;
    d.pixels = 0;
    stage_begin(&d.v, &vp->gxp, s->vertex_buffers, s->vertex_textures, g_textures[0]);
    if (fp) stage_begin(&d.f, &fp->gxp, s->fragment_buffers, s->fragment_textures, g_textures[1]);
    else memset(&d.f, 0, sizeof(d.f));
    vertex_inputs(&d);

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
    if (s->region_mode == GXM_REGION_ALL) return;
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
    r.shade = shade_pixel;
    r.user = &d;

    /* each index is shaded once per instance */
    uint32_t low = 0xFFFFFFFFu, high = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t v = index_size == 2 ? (uint32_t)(index_data[2 * i] | index_data[2 * i + 1] << 8)
                                     : (uint32_t)index_data[4 * i] | (uint32_t)index_data[4 * i + 1] << 8 |
                                       (uint32_t)index_data[4 * i + 2] << 16 | (uint32_t)index_data[4 * i + 3] << 24;
        if (v < low) low = v;
        if (v > high) high = v;
    }
    uint32_t range = high - low + 1;
    if (range > 0x1000000u || !reserve(range < count ? range : count, range)) return;

    for (uint32_t instance = 0; instance < instances; instance++) {
        uint32_t used = 0, tri[3];
        memset(g_slot, 0xFF, (size_t)range * sizeof(*g_slot));
        for (uint32_t i = 0; i < count; i++) {
            uint32_t v = index_size == 2 ? (uint32_t)(index_data[2 * i] | index_data[2 * i + 1] << 8)
                                         : (uint32_t)index_data[4 * i] | (uint32_t)index_data[4 * i + 1] << 8 |
                                           (uint32_t)index_data[4 * i + 2] << 16 | (uint32_t)index_data[4 * i + 3] << 24;
            if (g_slot[v - low] == 0xFFFFFFFFu) {
                g_slot[v - low] = used;
                shade_vertex(&d, v, instance, &g_vertices[used++]);
            }
            uint32_t at = g_slot[v - low];
            if (primitive == GXM_TRIANGLE_STRIP) {
                if (i < 2) { tri[i] = at; continue; }
                tri[2] = at;
                /* every other triangle of a strip is wound the other way: swap its first two */
                if (i & 1) raster_triangle(&r, &g_vertices[tri[1]], &g_vertices[tri[0]], &g_vertices[tri[2]]);
                else       raster_triangle(&r, &g_vertices[tri[0]], &g_vertices[tri[1]], &g_vertices[tri[2]]);
                tri[0] = tri[1]; tri[1] = tri[2];
            } else if (primitive == GXM_TRIANGLE_FAN) {
                if (i == 0) { tri[0] = at; continue; }
                if (i == 1) { tri[1] = at; continue; }
                raster_triangle(&r, &g_vertices[tri[0]], &g_vertices[tri[1]], &g_vertices[at]);
                tri[1] = at;
            } else {
                tri[i % 3] = at;
                if (i % 3 == 2) raster_triangle(&r, &g_vertices[tri[0]], &g_vertices[tri[1]], &g_vertices[tri[2]]);
            }
        }
    }
    gxm_counters.triangles += r.triangles_drawn;
    gxm_counters.pixels += d.pixels;
}
