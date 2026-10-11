/* shader.c — `armrecomp gxp`: what is in a compiled shader program.
 *
 * A title's shaders are GXP containers of USSE bytecode. This prints one:
 * what kind of program it is, its named parameters, where its inputs
 * arrive and its outputs leave, and its code as text, the secondary
 * program first and then the primary. With --listing only the code is
 * printed, in the form the Vita3K project's tools list it, so the two
 * listings can be compared line for line.
 */

#include "gxp.h"
#include "gxp_hlsl.h"
#include "usse.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* `reference` leaves out what the Vita3K listing leaves out: branches. */
static void listing(const usse_insn *code, uint32_t count, int reference) {
    usse_state st;
    char line[2048];
    memset(&st, 0, sizeof(st));
    usse_reset(&st);
    for (uint32_t i = 0; i < count; i++) {
        if (!code[i].op) {
            printf("%016llx: <not decoded, format %u>\n", (unsigned long long)code[i].raw, code[i].format);
            continue;
        }
        if (reference && code[i].op == USSE_OP_BR) continue;
        usse_disassemble(&code[i], &st, line, sizeof(line));
        fputs(line, stdout);
    }
}

static const char *slot_name(unsigned slot, char *buf, size_t cap) {
    static const char *const NAMES[] = { "COLOR0", "COLOR1", "FOG", "POSITION", "SPRITECOORD" };
    if (slot < 10) snprintf(buf, cap, "TEXCOORD%u", slot);
    else snprintf(buf, cap, "%s", slot < GXP_SLOTS ? NAMES[slot - 10] : "?");
    return buf;
}

static uint32_t cannot_run(const gxp_program *p) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < p->secondary_count; i++) n += !usse_can_run(&p->secondary[i]);
    for (uint32_t i = 0; i < p->primary_count; i++) n += !usse_can_run(&p->primary[i]);
    return n;
}

static void tables(const gxp_program *p) {
    static const char *const CATEGORY[] = { "attribute", "uniform", "sampler", "surface", "buffer" };
    static const char *const TYPE[] = { "float", "half", "fixed", "uint", "int", "ushort", "short", "uchar", "char", "block" };
    static const char *const USSE_TYPE[] = { "char", "short", "int", "fixed", "half", "float", "uchar", "ushort", "uint", "?" };
    char name[32];

    printf("%s program, %u bytes, flags 0x%X\n", (p->flags & GXP_FLAG_FRAGMENT) ? "fragment" : "vertex", p->size, p->flags);
    printf("  registers     %u primary, %u secondary\n", p->pa_count, p->sa_count);
    printf("  instructions  %u secondary, %u primary", p->secondary_count, p->primary_count);
    if (p->undecoded) printf(", %u not decoded", p->undecoded);
    if (cannot_run(p)) printf(", %u the interpreter cannot run", cannot_run(p));
    printf("\n");

    if (p->param_count) printf("\nparameters:\n");
    for (uint32_t i = 0; i < p->param_count; i++) {
        const uint8_t *e = p->params + 16 * i;
        const char *text = (const char *)e + (int32_t)rd32(e);
        int category = gxp_param_category(e), type = gxp_param_type(e);
        if ((const uint8_t *)text < p->data || (const uint8_t *)text >= p->data + p->size) text = "?";
        printf("  %-10s %-28s", category < 5 ? CATEGORY[category] : "?", *text ? text : "(unnamed)");
        if (category == GXP_ATTRIBUTE)
            printf(" %s%d, register pa%u\n", TYPE[type > 9 ? 9 : type], gxp_param_components(e), rd32(e + 12));
        else if (category == GXP_UNIFORM)
            printf(" %s%d x %u, buffer %d, word %u\n", TYPE[type > 9 ? 9 : type], gxp_param_components(e), rd32(e + 8),
                   gxp_param_container(e), rd32(e + 12));
        else if (category == GXP_SAMPLER)
            printf(" texture unit %u%s\n", rd32(e + 12), (e[7] & 0x10) ? ", cube" : "");
        else
            printf(" buffer %u, %u bytes\n", rd32(e + 12), rd32(e + 8));
    }

    printf("\nsecondary attributes:\n");
    for (uint32_t i = 0; i < p->block_count; i++) {
        if (p->blocks[i].buffer == GXP_DEFAULT_BUFFER)
            printf("  sa%-4u %3u words  the default uniform buffer\n", p->blocks[i].reg, p->blocks[i].words);
        else
            printf("  sa%-4u %3u words  uniform buffer %u\n", p->blocks[i].reg, p->blocks[i].words, p->blocks[i].buffer);
    }
    for (uint32_t i = 0; i < p->base_count; i++)
        printf("  sa%-4u            address of buffer %u, %+d\n", p->bases[i].reg, p->bases[i].buffer, (int)p->bases[i].bias);
    for (uint32_t i = 0; i < p->sampler_count; i++)
        printf("  sa%-4u            texture unit %u, sampled by the code\n", p->samplers[i].reg, p->samplers[i].unit);
    for (uint32_t i = 0; i < p->literal_count; i++) {
        float f;
        memcpy(&f, &p->literals[i].value, sizeof(f));
        printf("  sa%-4u            literal 0x%08X  (%g)\n", p->literals[i].reg, p->literals[i].value, f);
    }

    if (!(p->flags & GXP_FLAG_FRAGMENT)) {
        printf("\noutputs, %u registers:\n", p->out_words);
        static const uint8_t ORDER[] = { GXP_SLOT_POSITION, GXP_SLOT_COLOR0, GXP_SLOT_COLOR1, GXP_SLOT_FOG,
                                         0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
        for (size_t k = 0; k < sizeof(ORDER); k++) {
            unsigned slot = ORDER[k];
            if (!p->out_count[slot]) continue;
            printf("  o%-3u %u  %s\n", p->out_reg[slot], p->out_count[slot], slot_name(slot, name, sizeof(name)));
        }
        if (p->has_psize) printf("  o%-3u 1  PSIZE\n", p->psize_reg);
    } else {
        if (p->input_count) printf("\nprimary attributes:\n");
        for (uint32_t i = 0; i < p->input_count; i++) {
            const gxp_input *in = &p->inputs[i];
            if (in->has_iterator)
                printf("  pa%-4u %s%u  %s\n", in->iterator_reg, USSE_TYPE[in->type > 9 ? 9 : in->type], in->components,
                       slot_name(in->source, name, sizeof(name)));
            if (in->has_texture)
                printf("  pa%-4u %s  texture unit %u at %s%s\n", in->texture_reg,
                       in->result == 2 ? "half4" : in->result == 3 ? (in->single ? "float1" : "float4") : "texel",
                       in->unit, slot_name(in->coord, name, sizeof(name)),
                       in->projection == 1 ? ".xy / w" : in->projection == 2 ? ".xy / z" : ".xy");
        }
        printf("\nresult: %s%u in %s0%s\n", TYPE[p->result_type > 9 ? 9 : p->result_type], p->result_components,
               (p->flags & GXP_FLAG_NATIVECOLOR) ? "o" : "pa", (p->flags & GXP_FLAG_FRAGCOLOR) ? ", after reading the pixel from o0" : "");
    }
}

/* The program as the HLSL the toolkit writes for it (src/gxp_hlsl.h), for
 * what would most often be bound: every texture it names a texture of four
 * bytes, every interpolated quantity carried, the colour as four floats.
 * `raw` words of the result leave as they are when it is not 0. */
int cmd_hlsl(const char *path, int raw) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "armrecomp: cannot read %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = size > 0 ? malloc((size_t)size) : NULL;
    if (!d || fread(d, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(d); return 1; }
    fclose(f);

    gxp_program p;
    int err = gxp_parse(&p, d, (uint32_t)size);
    if (err) {
        fprintf(stderr, "armrecomp: %s is not a GXP program this tool can read (%d)\n", path, err);
        gxp_free(&p);
        free(d);
        return 1;
    }
    gxp_hlsl_bind bind;
    char why[200];
    memset(&bind, 0, sizeof(bind));
    for (unsigned unit = 0; unit < GXP_TEXTURE_UNITS; unit++)
        if ((p.textures_used >> unit) & 1) {
            const uint8_t *param = gxp_find_param(&p, GXP_SAMPLER, unit);
            bind.tex_components[unit] = 4;
            bind.tex_type[unit] = USSE_U8;
            bind.tex_cube[unit] = param && (param[7] >> 4) != 0;
        }
    if (p.flags & GXP_FLAG_FRAGMENT) {
        bind.slots = p.slots_used & ((1u << GXP_SLOT_POSITION) - 1);
        bind.result_words = (uint8_t)(raw < 0 ? 0 : raw > 4 ? 4 : raw);
        bind.now = bind.result_words ? 2 : 1;
        bind.now_words = bind.result_words;
    } else {
        for (unsigned slot = 0; slot < GXP_SLOT_POSITION; slot++)
            if (p.out_count[slot]) bind.slots |= 1u << slot;
        bind.pa_words = (p.pa_count + 3u) & ~3u;
        if (bind.pa_words > 64) bind.pa_words = 64;
    }
    char *text = gxp_hlsl(&p, &bind, why, sizeof(why));
    if (!text) fprintf(stderr, "armrecomp: %s is not translated: it has %s\n", path, why);
    else fputs(text, stdout);
    int ok = text != NULL;
    free(text);
    gxp_free(&p);
    free(d);
    return ok ? 0 : 1;
}

int cmd_gxp(const char *path, int listing_only) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "armrecomp: cannot read %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = size > 0 ? malloc((size_t)size) : NULL;
    if (!d || fread(d, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(d); return 1; }
    fclose(f);

    gxp_program p;
    int err = gxp_parse(&p, d, (uint32_t)size);
    if (err) {
        fprintf(stderr, "armrecomp: %s is not a GXP program this tool can read (%d)\n", path, err);
        gxp_free(&p);
        free(d);
        return 1;
    }
    if (!listing_only) {
        tables(&p);
        printf("\nsecondary program:\n");
    }
    listing(p.secondary, p.secondary_count, listing_only);
    if (!listing_only) printf("\nprimary program:\n");
    listing(p.primary, p.primary_count, listing_only);
    gxp_free(&p);
    free(d);
    return 0;
}
