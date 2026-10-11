/* gxp.c — a compiled shader program, read into what a renderer needs. See gxp.h. */

#include "gxp.h"

#include <stdlib.h>
#include <string.h>

/* Header fields. An offset in a GXP counts from the field that holds it. */
enum {
    H_MAGIC = 0x00, H_FLAGS = 0x14, H_PARAM_COUNT = 0x24, H_PARAMS = 0x28, H_VARYINGS = 0x2C,
    H_PA_COUNT = 0x30, H_SA_COUNT = 0x32, H_PRIMARY_COUNT = 0x3C, H_PRIMARY = 0x40,
    H_SECONDARY = 0x48, H_SECONDARY_END = 0x4C, H_DEFAULT_WORDS = 0x64,
    H_LITERAL_COUNT = 0x70, H_LITERALS = 0x74, H_BASE_COUNT = 0x78, H_BASES = 0x7C,
    H_SAMPLER_COUNT = 0x80, H_SAMPLERS = 0x84, H_CONTAINER_COUNT = 0x90, H_CONTAINERS = 0x94,
    H_SIZE = 0x98
};

/* Container indices beyond the uniform buffers. */
enum { C_LITERALS = 16, C_DATA = 19 };

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (rd16(p + 2) << 16); }

/* The table a header field points at, checked to lie inside the file. */
static const uint8_t *table(const gxp_program *p, uint32_t field, uint32_t count, uint32_t each) {
    uint64_t at = (uint64_t)field + rd32(p->data + field);
    if (at + (uint64_t)count * each > p->size) return NULL;
    return p->data + at;
}

static int decode_code(const uint8_t *code, uint32_t count, int secondary, usse_insn **out, uint32_t *undecoded) {
    *out = count ? calloc(count, sizeof(usse_insn)) : NULL;
    if (count && !*out) return -1;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t word = (uint64_t)rd32(code + 8 * i) | ((uint64_t)rd32(code + 8 * i + 4) << 32);
        if (!usse_decode(word, secondary, &(*out)[i])) (*undecoded)++;
    }
    return 0;
}

/* A vertex program writes one packed record to its output registers:
 * position, the colours, fog, the texture coordinates it has in the widths
 * it has them, the point size. */
static void vertex_outputs(gxp_program *p, const uint8_t *v) {
    uint32_t out1 = rd32(v + 16), out2 = rd32(v + 20), o = 4;
    p->attribute_regs = (uint64_t)rd32(v) | ((uint64_t)rd32(v + 4) << 32);
    p->untyped_regs = (uint64_t)rd32(v + 8) | ((uint64_t)rd32(v + 12) << 32);
    p->out_reg[GXP_SLOT_POSITION] = 0;
    p->out_count[GXP_SLOT_POSITION] = 4;
    if (out1 & 0x800) { p->out_reg[GXP_SLOT_COLOR0] = (uint8_t)o; p->out_count[GXP_SLOT_COLOR0] = 4; o += 4; }
    if (out1 & 0x400) { p->out_reg[GXP_SLOT_COLOR1] = (uint8_t)o; p->out_count[GXP_SLOT_COLOR1] = 4; o += 4; }
    if (out1 & 0x200) { p->out_reg[GXP_SLOT_FOG] = (uint8_t)o; p->out_count[GXP_SLOT_FOG] = 2; o += 2; }
    for (uint32_t n = 0; n < 10; n++) {
        uint32_t t = (out2 >> (3 * n)) & 7;
        uint32_t count = (t & 1 ? 2u : 0u) + (t & 2 ? 1u : 0u) + (t & 4 ? 1u : 0u);
        if (!count) continue;
        p->out_reg[n] = (uint8_t)o;
        p->out_count[n] = (uint8_t)count;
        o += count;
    }
    if (out1 & 0x100) { p->psize_reg = (uint8_t)o; p->has_psize = 1; o += 1; }
    p->out_words = (uint8_t)o;
}

/* A fragment program's input list. Each entry is two things side by side:
 * an interpolated vertex output to load, and a texture to sample at an
 * interpolated coordinate before the program starts. The registers are
 * handed out in order, each taking the number its size field says. */
static int fragment_inputs(gxp_program *p, const uint8_t *v) {
    uint32_t count = rd16(v + 12);
    uint64_t at = (uint64_t)(v - p->data) + 16 + rd32(v + 16);
    if (at + (uint64_t)count * 16 > p->size) return -1;
    const uint8_t *d = p->data + at;
    p->result_type = v[10];
    p->result_components = v[11];
    p->inputs = count ? calloc(count, sizeof(gxp_input)) : NULL;
    if (count && !p->inputs) return -1;
    p->input_count = count;

    uint32_t pa = 0;
    for (uint32_t i = 0; i < count; i++, d += 16) {
        uint32_t info = rd32(d), unit = rd32(d + 4), size = rd32(d + 8), comp = rd32(d + 12);
        gxp_input *in = &p->inputs[i];
        if ((info & 0x4000F000u) != 0x0000F000u) {
            uint32_t source = (info >> 12) & 15, precision = (info >> 28) & 3;
            int colour = source == 10 || source == 11;
            in->has_iterator = 1;
            in->source = (info & 0x40000000u) ? GXP_SLOT_SPRITE : (uint8_t)source;
            in->components = (uint8_t)(((info >> 22) & 3) + 1);
            /* colours arrive as bytes and everything else as floats, unless
             * the entry says the other, or names a precision outright */
            if (precision == 2)      in->type = USSE_F16;
            else if (precision == 1) in->type = USSE_C10;
            else if (info & 0x00100000u) in->type = colour ? USSE_F32 : USSE_U8;
            else                         in->type = colour ? USSE_U8 : USSE_F32;
            in->iterator_reg = (uint16_t)pa;
            pa += ((size >> 4) & 3) + 1;
            if (in->source < GXP_SLOTS) p->slots_used |= 1u << in->source;
        }
        if ((info & 0x0000040Fu) != 0x0000000Fu) {
            uint32_t use = (info >> 8) & 3;
            in->has_texture = 1;
            in->coord = (info & 0x400) ? GXP_SLOT_SPRITE : (uint8_t)(info & 15);
            in->projection = use == 2 ? 1 : use == 3 ? 2 : 0;
            in->unit = (uint8_t)unit;
            in->result = (uint8_t)((comp >> 4) & 3);
            in->single = (uint8_t)((comp >> 6) & 1);
            in->texture_reg = (uint16_t)pa;
            pa += ((size >> 6) & 3) + 1;
            if (in->coord < GXP_SLOTS) p->slots_used |= 1u << in->coord;
            if (unit < GXP_TEXTURE_UNITS) p->textures_used |= 1u << unit;
        }
    }
    return 0;
}

int gxp_parse(gxp_program *p, const uint8_t *data, uint32_t size) {
    memset(p, 0, sizeof(*p));
    if (size < H_SIZE || rd32(data) != 0x00505847u) return -1;
    p->data = data;
    p->size = size;
    p->flags = rd32(data + H_FLAGS);
    p->pa_count = rd16(data + H_PA_COUNT);
    p->sa_count = rd16(data + H_SA_COUNT);
    p->default_buffer_words = rd32(data + H_DEFAULT_WORDS);

    p->param_count = rd32(data + H_PARAM_COUNT);
    p->params = table(p, H_PARAMS, p->param_count, 16);
    if (!p->params) return -2;

    /* The secondary attributes: which registers each source owns. */
    uint32_t containers = rd32(data + H_CONTAINER_COUNT);
    const uint8_t *c = table(p, H_CONTAINERS, containers, 8);
    if (!c) return -3;
    uint32_t data_base = 0, literal_base = 0;
    int has_data = 0, has_literals = 0;
    for (uint32_t i = 0; i < containers; i++, c += 8) {
        uint32_t index = rd16(c), base = rd16(c + 4), words = rd16(c + 6);
        if (index <= GXP_DEFAULT_BUFFER) {
            if (p->block_count < GXP_BUFFERS) {
                gxp_block *b = &p->blocks[p->block_count++];
                b->buffer = (uint16_t)index; b->reg = (uint16_t)base; b->words = (uint16_t)words;
            }
            p->buffers_used |= 1u << index;
        } else if (index == C_DATA) {
            data_base = base; has_data = 1;
        } else if (index == C_LITERALS) {
            literal_base = base; has_literals = 1;
        }
    }
    if (!has_literals) literal_base = data_base;

    uint32_t n = rd32(data + H_BASE_COUNT);
    const uint8_t *t = table(p, H_BASES, n, 8);
    if (!t) return -4;
    for (uint32_t i = 0; i < n && has_data; i++, t += 8) {
        if (p->base_count == sizeof(p->bases) / sizeof(p->bases[0])) break;
        gxp_base *b = &p->bases[p->base_count++];
        b->buffer = (uint16_t)rd16(t);
        b->reg = (uint16_t)(data_base + rd16(t + 2));
        b->bias = (int32_t)rd32(t + 4);
        if (b->buffer <= GXP_DEFAULT_BUFFER) p->buffers_used |= 1u << b->buffer;
    }

    n = rd32(data + H_LITERAL_COUNT);
    t = table(p, H_LITERALS, n, 8);
    if (!t) return -5;
    p->literals = n ? calloc(n, sizeof(gxp_literal)) : NULL;
    if (n && !p->literals) return -6;
    p->literal_count = n;
    for (uint32_t i = 0; i < n; i++, t += 8) {
        p->literals[i].reg = (uint16_t)(literal_base + rd32(t));
        p->literals[i].value = rd32(t + 4);
    }

    /* A texture the bytecode samples owns four registers, and a sample
     * instruction names the first. */
    n = rd32(data + H_SAMPLER_COUNT);
    t = table(p, H_SAMPLERS, n, 4);
    if (!t) return -7;
    for (uint32_t i = 0; i < n; i++, t += 4) {
        uint32_t where = rd16(t), unit = where / 4;
        if (where % 4 || p->sampler_count == GXP_TEXTURE_UNITS) continue;
        p->samplers[p->sampler_count].reg = (uint16_t)(data_base + rd16(t + 2));
        p->samplers[p->sampler_count].unit = (uint16_t)unit;
        p->sampler_count++;
        if (unit < GXP_TEXTURE_UNITS) p->textures_used |= 1u << unit;
    }

    const uint8_t *v = table(p, H_VARYINGS, 1, (p->flags & GXP_FLAG_FRAGMENT) ? 20 : 28);
    if (!v) return -8;
    if (p->flags & GXP_FLAG_FRAGMENT) { if (fragment_inputs(p, v)) return -9; }
    else vertex_outputs(p, v);

    uint32_t secondary = H_SECONDARY + rd32(data + H_SECONDARY), end = H_SECONDARY_END + rd32(data + H_SECONDARY_END);
    uint32_t primary = H_PRIMARY + rd32(data + H_PRIMARY);
    p->secondary_count = end > secondary ? (end - secondary) / 8 : 0;
    p->primary_count = rd32(data + H_PRIMARY_COUNT);
    if ((uint64_t)secondary + 8ull * p->secondary_count > size || (uint64_t)primary + 8ull * p->primary_count > size) return -10;
    if (decode_code(data + secondary, p->secondary_count, 1, &p->secondary, &p->undecoded)) return -11;
    if (decode_code(data + primary, p->primary_count, 0, &p->primary, &p->undecoded)) return -11;
    return 0;
}

void gxp_free(gxp_program *p) {
    free(p->literals);
    free(p->inputs);
    free(p->secondary);
    free(p->primary);
    memset(p, 0, sizeof(*p));
}

const uint8_t *gxp_find_param(const gxp_program *p, int category, uint32_t resource_index) {
    for (uint32_t i = 0; i < p->param_count; i++) {
        const uint8_t *e = p->params + 16 * i;
        if (gxp_param_category(e) == category && rd32(e + 12) == resource_index) return e;
    }
    return NULL;
}
