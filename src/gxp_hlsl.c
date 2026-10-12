/* gxp_hlsl.c — a shader program in a host GPU's language. See gxp_hlsl.h.
 *
 * Every emitter below is the interpreter's code for the same thing with the
 * arithmetic turned into text: `load` is usse.c's load, `store` its store,
 * and `emit` its execute, case for case. Where the two differ it is this
 * file that is wrong.
 */

#include "gxp_hlsl.h"
#include "usse_private.h"
#include "vitarecomp/half.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { K_FLOAT = 0, K_SINT, K_UINT };

/* --- text that grows ------------------------------------------------------------------ */

typedef struct { char *s; size_t len, cap; int bad; } text;

static void put(text *t, const char *fmt, ...) {
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(t->s ? t->s + t->len : NULL, t->s ? t->cap - t->len : 0, fmt, ap);
        va_end(ap);
        if (n < 0) { t->bad = 1; return; }
        if (t->s && (size_t)n < t->cap - t->len) { t->len += (size_t)n; return; }
        size_t cap = t->cap ? t->cap * 2 : 8192;
        while (cap - t->len <= (size_t)n) cap *= 2;
        char *grown = realloc(t->s, cap);
        if (!grown) { t->bad = 1; return; }
        t->s = grown;
        t->cap = cap;
    }
}

/* --- the translation in hand ------------------------------------------------------------ */

typedef struct {
    const gxp_program *p;
    const gxp_hlsl_bind *b;
    text body;                      /* the statements of main, before what goes round them is known */
    usse_state st;                  /* only its repeat increments are used */
    int fragment, serial, failed;
    int sa_written;                 /* the primary part stores to a secondary attribute: they are copied first */
    int top[5];                     /* one past the highest word named of T, P, O, S, I */
    int uses_mem, uses_index, dynamic;
    char why[200];
} gen;

static void fail(gen *g, const char *fmt, ...) {
    if (g->failed) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g->why, sizeof(g->why), fmt, ap);
    va_end(ap);
    g->failed = 1;
}

static int is_float_type(uint8_t t) { return t == USSE_F32 || t == USSE_F16 || t == USSE_C10; }

static int kind_of(uint8_t type) {
    switch (type) {
    case USSE_S8: case USSE_S16: case USSE_S32: return K_SINT;
    case USSE_U8: case USSE_U16: case USSE_U32: return K_UINT;
    default: return K_FLOAT;
    }
}

static const char *kind_name(int kind) { return kind == K_FLOAT ? "float" : kind == K_SINT ? "int" : "uint"; }

/* A float by its bits, so that what the text says is what the interpreter holds. */
static void float_literal(char *out, size_t cap, uint32_t bits) {
    if (bits == 0) snprintf(out, cap, "0.0");
    else if (bits == 0x3F800000u) snprintf(out, cap, "1.0");
    else if (bits == 0x40000000u) snprintf(out, cap, "2.0");
    else if (bits == 0x3F000000u) snprintf(out, cap, "0.5");
    else snprintf(out, cap, "asfloat(0x%08Xu)", bits);
}

static uint32_t bits_of_float(float f) { uint32_t u; memcpy(&u, &f, sizeof(u)); return u; }

/* --- values ---------------------------------------------------------------------------
 * What a load gives and a store takes: up to four components, each the name
 * of a local of main, all floats, all ints or all uints. */

typedef struct { int n, kind; char e[4][24]; } val;

/* A fresh local for component k of the load or result in hand. */
static void local(gen *g, val *v, const char *expr) {
    snprintf(v->e[v->n], sizeof(v->e[0]), "v%d_%d", g->serial, v->n);
    put(&g->body, "        %s %s = %s;\n", kind_name(v->kind), v->e[v->n], expr);
    v->n++;
}

static val new_val(gen *g, int kind) {
    val v;
    memset(&v, 0, sizeof(v));
    v.kind = kind;
    g->serial++;
    return v;
}

/* Component i of a value as its 32 bits. */
static const char *bits(const val *v, int i, char *out, size_t cap) {
    if (v->kind == K_FLOAT) snprintf(out, cap, "asuint(%s)", v->e[i]);
    else if (v->kind == K_SINT) snprintf(out, cap, "uint(%s)", v->e[i]);
    else snprintf(out, cap, "%s", v->e[i]);
    return out;
}

/* The same bits as another kind's number. */
static const char *as_kind(const val *v, int i, int kind, char *out, size_t cap) {
    if (v->kind == kind) snprintf(out, cap, "%s", v->e[i]);
    else if (kind == K_FLOAT) snprintf(out, cap, v->kind == K_SINT ? "asfloat(%s)" : "asfloat(%s)", v->e[i]);
    else if (kind == K_SINT) snprintf(out, cap, v->kind == K_FLOAT ? "asint(%s)" : "int(%s)", v->e[i]);
    else snprintf(out, cap, v->kind == K_FLOAT ? "asuint(%s)" : "uint(%s)", v->e[i]);
    return out;
}

/* --- where an operand's words are --------------------------------------------------------- */

typedef struct { int ok, bank, base, dynamic, index; uint8_t type; } place;

static int bank_slot(int bank) {
    return bank == USSE_TEMP ? 0 : bank == USSE_PA ? 1 : bank == USSE_OUT ? 2 : bank == USSE_SA ? 3 : 4;
}

static place resolve(gen *g, const usse_operand *op, int32_t shift, uint8_t type, int words) {
    place pl;
    memset(&pl, 0, sizeof(pl));
    pl.type = type;
    if (op->bank == USSE_INTERNAL) {
        int64_t base = 4 * ((int64_t)op->num + shift);
        if (base < 0 || base + 4 > 16) return pl;
        /* internal registers hold 32-bit lanes whatever the operand says */
        if (type == USSE_F16) pl.type = USSE_F32;
        else if (type == USSE_S16 || type == USSE_S8) pl.type = USSE_S32;
        else if (type == USSE_U16 || type == USSE_U8) pl.type = USSE_U32;
        pl.ok = 1; pl.bank = USSE_INTERNAL; pl.base = (int)base;
        return pl;
    }
    if (op->bank == USSE_INDEXED1 || op->bank == USSE_INDEXED2) {
        static const uint8_t TARGET[4] = { USSE_TEMP, USSE_OUT, USSE_PA, USSE_SA };
        uint32_t n = (uint32_t)((int32_t)op->num + shift);
        pl.ok = 1; pl.dynamic = 1; pl.index = op->bank - USSE_INDEXED1;
        pl.bank = TARGET[(n >> 5) & 3];
        pl.base = (int)(n & 31);
        g->dynamic = g->uses_index = 1;
        return pl;
    }
    if (op->bank != USSE_TEMP && op->bank != USSE_PA && op->bank != USSE_OUT && op->bank != USSE_SA) return pl;
    int64_t base = (int64_t)op->num + shift;
    if (base < 0 || base + words > USSE_WORDS) return pl;
    pl.ok = 1; pl.bank = op->bank; pl.base = (int)base;
    return pl;
}

/* Word k of a place, as something that can be read and assigned. */
static const char *word(gen *g, const place *pl, int k, char *out, size_t cap) {
    static const char *const NAME[5] = { "T", "P", "O", "S", "I" };
    int slot = bank_slot(pl->bank);
    if (!pl->dynamic) {
        int i = pl->base + k;
        if (i + 1 > g->top[slot]) g->top[slot] = i + 1;
        if (slot == 3 && !g->sa_written) snprintf(out, cap, "SAv[%d].%c", i >> 2, "xyzw"[i & 3]);
        else snprintf(out, cap, "%s[%d]", NAME[slot], i);
    } else {
        /* an index register counts 64-bit units; what it reaches is not known here */
        if (g->top[slot] < 128 && slot != 3) g->top[slot] = 128;
        if (slot == 3 && !g->sa_written) snprintf(out, cap, "sa_at(IX%d * 2 + %d)", pl->index, pl->base + k);
        else snprintf(out, cap, "%s[clamp(IX%d * 2 + %d, 0, %d)]", NAME[slot], pl->index, pl->base + k,
                      slot == 3 ? USSE_WORDS - 1 : 127);
    }
    return out;
}

/* --- loads ------------------------------------------------------------------------------- */

static void modifiers(gen *g, val *v, uint8_t flags) {
    if ((flags & USSE_ABS) && v->kind != K_UINT)
        for (int i = 0; i < v->n; i++) {
            if (v->kind == K_FLOAT) put(&g->body, "        %s = abs(%s);\n", v->e[i], v->e[i]);
            else put(&g->body, "        if (%s < 0) %s = -%s;\n", v->e[i], v->e[i], v->e[i]);
        }
    if (flags & USSE_NEG)
        for (int i = 0; i < v->n; i++) {
            if (v->kind == K_FLOAT) put(&g->body, "        %s = 0.0 - %s;\n", v->e[i], v->e[i]);
            else if (v->kind == K_SINT) put(&g->body, "        %s = -%s;\n", v->e[i], v->e[i]);
            else put(&g->body, "        %s = 0u - %s;\n", v->e[i], v->e[i]);
        }
}

static val load_constant(gen *g, const usse_operand *op, uint8_t mask) {
    static const float CHANNEL[4] = { 0.0f, 1.0f, 2.0f, 0.5f };
    val out = new_val(g, kind_of(op->type));
    const uint32_t *c = usse_constant_words(op->num);
    for (int i = 0; i < 4; i++) {
        if (!((mask >> i) & 1)) continue;
        uint8_t ch = op->sw[i];
        char e[64];
        if (ch >= USSE_ZERO && ch <= USSE_HALF) {
            float k = CHANNEL[ch - USSE_ZERO];
            if (out.kind == K_FLOAT) float_literal(e, sizeof(e), bits_of_float(k));
            else snprintf(e, sizeof(e), "%d", (int)k);
        } else if (ch > USSE_W) {
            snprintf(e, sizeof(e), out.kind == K_FLOAT ? "0.0" : "0");
        } else if (op->type == USSE_F16 || op->type == USSE_U16 || op->type == USSE_S16) {
            /* four halves; two of them the reference's tables correct by hand */
            uint16_t h = (uint16_t)(c[ch >> 1] >> (16 * (ch & 1)));
            if (ch == USSE_X && (op->num & 63) == 12) h = 0x3800;
            if (ch == USSE_X && (op->num & 63) == 16) h = 0x3C00;
            if (op->type == USSE_F16) float_literal(e, sizeof(e), bits_of_float(vita_f16_to_f32(h)));
            else if (op->type == USSE_U16) snprintf(e, sizeof(e), "%uu", (unsigned)h);
            else snprintf(e, sizeof(e), "%d", (int)(int32_t)vita_f16_to_f32(h));
        } else {
            uint32_t w = ch == USSE_Y ? c[1] : c[0];
            if (out.kind == K_FLOAT) float_literal(e, sizeof(e), w);
            else if (out.kind == K_SINT) snprintf(e, sizeof(e), "int(0x%08Xu)", w);
            else snprintf(e, sizeof(e), "0x%08Xu", w);
        }
        local(g, &out, e);
    }
    return out;
}

/* An operand that is a number in the instruction, or a register this has
 * nothing behind: the same for every component. */
static val load_immediate(gen *g, const usse_operand *op, uint8_t mask) {
    val out = new_val(g, kind_of(op->type));
    uint32_t imm = op->bank == USSE_IMMEDIATE ? op->num : 0;
    char e[32];
    if (out.kind == K_FLOAT) float_literal(e, sizeof(e), bits_of_float((float)imm));
    else if (out.kind == K_SINT) snprintf(e, sizeof(e), "int(%uu)", imm);
    else snprintf(e, sizeof(e), "%uu", imm);
    for (int i = 0; i < 4; i++)
        if ((mask >> i) & 1) local(g, &out, e);
    return out;
}

/* The components of an operand whose words are at `pl`, read as `type`. */
static val load_words(gen *g, const usse_operand *op, uint8_t type, const place *pl, uint8_t mask) {
    static const char *const AS_FLOAT[4] = { "0.0", "1.0", "2.0", "0.5" };
    static const char *const AS_INT[4] = { "0", "1", "2", "0" };
    val out = new_val(g, kind_of(type));
    for (int i = 0; i < 4; i++) {
        if (!((mask >> i) & 1)) continue;
        uint8_t ch = op->sw[i];
        char e[160], w[96];
        if (ch >= USSE_ZERO && ch <= USSE_HALF) {
            snprintf(e, sizeof(e), "%s", out.kind == K_FLOAT ? AS_FLOAT[ch - USSE_ZERO] : AS_INT[ch - USSE_ZERO]);
        } else if (!pl->ok || ch > USSE_W) {
            snprintf(e, sizeof(e), out.kind == K_FLOAT ? "0.0" : "0");
        } else {
            switch (type) {
            case USSE_F32: snprintf(e, sizeof(e), "asfloat(%s)", word(g, pl, ch, w, sizeof(w))); break;
            case USSE_U32: snprintf(e, sizeof(e), "%s", word(g, pl, ch, w, sizeof(w))); break;
            case USSE_S32: snprintf(e, sizeof(e), "int(%s)", word(g, pl, ch, w, sizeof(w))); break;
            case USSE_F16:
                word(g, pl, ch >> 1, w, sizeof(w));
                if (ch & 1) snprintf(e, sizeof(e), "f16tof32(%s >> 16)", w); else snprintf(e, sizeof(e), "f16tof32(%s)", w);
                break;
            case USSE_U16: snprintf(e, sizeof(e), "((%s >> %d) & 0xFFFFu)", word(g, pl, ch >> 1, w, sizeof(w)), 16 * (ch & 1)); break;
            case USSE_S16: snprintf(e, sizeof(e), "(int(%s << %d) >> 16)", word(g, pl, ch >> 1, w, sizeof(w)), 16 - 16 * (ch & 1)); break;
            case USSE_U8:  snprintf(e, sizeof(e), "((%s >> %d) & 0xFFu)", word(g, pl, 0, w, sizeof(w)), 8 * ch); break;
            case USSE_S8:  snprintf(e, sizeof(e), "(int(%s << %d) >> 24)", word(g, pl, 0, w, sizeof(w)), 24 - 8 * ch); break;
            case USSE_C10: snprintf(e, sizeof(e), "c10(%s >> %d)", word(g, pl, ch / 3, w, sizeof(w)), 10 * (ch % 3)); break;
            default:       snprintf(e, sizeof(e), "0"); break;
            }
        }
        local(g, &out, e);
    }
    return out;
}

/* Read an operand: for each component the mask asks for, the source channel
 * its swizzle names, or a constant. */
static val load(gen *g, const usse_operand *op, uint8_t mask, int32_t shift) {
    val out;
    if (op->bank == USSE_CONST) { out = load_constant(g, op, mask); modifiers(g, &out, op->flags); return out; }
    if (op->bank == USSE_PREDICATE) {
        char e[32];
        out = new_val(g, K_UINT);
        snprintf(e, sizeof(e), "(PR%d ? 1u : 0u)", op->num & 3);
        local(g, &out, e);
        return out;
    }
    if (op->bank == USSE_INDEX) {
        char e[48];
        out = new_val(g, K_SINT);
        g->uses_index = 1;
        if (op->type == USSE_U8 || op->type == USSE_S8) snprintf(e, sizeof(e), "(IX%d & 0xFF)", (op->num - 1) & 1);
        else if (op->type == USSE_U16 || op->type == USSE_S16) snprintf(e, sizeof(e), "(IX%d & 0xFFFF)", (op->num - 1) & 1);
        else snprintf(e, sizeof(e), "IX%d", (op->num - 1) & 1);
        local(g, &out, e);
        return out;
    }
    if (op->bank == USSE_IMMEDIATE || op->bank == USSE_GLOBAL || op->bank == USSE_SPECIAL || op->bank == USSE_INVALID) {
        out = load_immediate(g, op, mask);
        modifiers(g, &out, op->flags);
        return out;
    }
    place pl = resolve(g, op, shift, op->type, 4);
    out = load_words(g, op, pl.type, &pl, mask);
    modifiers(g, &out, op->flags);
    return out;
}

/* --- stores ------------------------------------------------------------------------------- */

/* Write an operand: the k-th component the mask selects takes the k-th
 * component of the value. A destination's swizzle means nothing. */
static void store(gen *g, const usse_operand *op, const val *v, uint8_t mask, int32_t shift) {
    char b[64], w[96];
    if (!v->n) return;
    if (op->bank == USSE_PREDICATE) {
        put(&g->body, "        PR%d = %s != 0u;\n", op->num & 3, bits(v, 0, b, sizeof(b)));
        return;
    }
    if (op->bank == USSE_INDEX) {
        g->uses_index = 1;
        as_kind(v, 0, K_SINT, b, sizeof(b));
        if (op->type == USSE_U8 || op->type == USSE_S8) put(&g->body, "        IX%d = %s & 0xFF;\n", (op->num - 1) & 1, b);
        else if (op->type == USSE_U16 || op->type == USSE_S16) put(&g->body, "        IX%d = %s & 0xFFFF;\n", (op->num - 1) & 1, b);
        else put(&g->body, "        IX%d = %s;\n", (op->num - 1) & 1, b);
        return;
    }
    if (op->bank != USSE_TEMP && op->bank != USSE_PA && op->bank != USSE_OUT && op->bank != USSE_SA &&
        op->bank != USSE_INTERNAL && op->bank != USSE_INDEXED1 && op->bank != USSE_INDEXED2) return;
    place pl = resolve(g, op, shift, op->type, 4);
    if (!pl.ok) return;
    uint8_t type = pl.type;
    int k = 0;
    if (pl.bank == USSE_INTERNAL || type == USSE_F32 || type == USSE_U32 || type == USSE_S32) {
        /* whole words: floats as floats, integers as their bits */
        for (int p = 0; p < 4; p++) {
            if (!((mask >> p) & 1) || k >= v->n) continue;
            put(&g->body, "        %s = %s;\n", word(g, &pl, p, w, sizeof(w)), bits(v, k, b, sizeof(b)));
            k++;
        }
        return;
    }
    for (int p = 0; p < 4; p++) {
        if (!((mask >> p) & 1)) continue;
        int j = k < v->n ? k : v->n - 1;                /* one value serves every component */
        k++;
        switch (type) {
        case USSE_F16: {
            int at = 16 * (p & 1);
            if (v->kind == K_FLOAT) snprintf(b, sizeof(b), "%s", v->e[j]);
            else if (v->kind == K_SINT) snprintf(b, sizeof(b), "float(%s)", v->e[j]);
            else snprintf(b, sizeof(b), "float(int(%s))", v->e[j]);
            word(g, &pl, p >> 1, w, sizeof(w));
            put(&g->body, "        %s = (%s & 0x%08Xu) | (h16(%s) << %d);\n", w, w, ~(0xFFFFu << at), b, at);
            break;
        }
        case USSE_U16: case USSE_S16: {
            int at = 16 * (p & 1);
            word(g, &pl, p >> 1, w, sizeof(w));
            put(&g->body, "        %s = (%s & 0x%08Xu) | ((%s & 0xFFFFu) << %d);\n", w, w, ~(0xFFFFu << at), bits(v, j, b, sizeof(b)), at);
            break;
        }
        case USSE_U8: case USSE_S8:
            word(g, &pl, 0, w, sizeof(w));
            put(&g->body, "        %s = (%s & 0x%08Xu) | ((%s & 0xFFu) << %d);\n", w, w, ~(0xFFu << (8 * p)), bits(v, j, b, sizeof(b)), 8 * p);
            break;
        case USSE_C10: {
            int at = 10 * (p % 3);
            as_kind(v, j, K_FLOAT, b, sizeof(b));
            word(g, &pl, p / 3, w, sizeof(w));
            put(&g->body, "        %s = (%s & 0x%08Xu) | (to_c10(%s) << %d);\n", w, w, ~(0x3FFu << at), b, at);
            break;
        }
        default: break;
        }
    }
}

/* One 32-bit register of an operand as its bits, `index` registers on. */
static const char *read_word(gen *g, const usse_operand *op, int32_t shift, int index, char *out, size_t cap) {
    if (op->bank == USSE_IMMEDIATE) { snprintf(out, cap, "%uu", (unsigned)op->num); return out; }
    if (op->bank == USSE_CONST) { snprintf(out, cap, "0x%08Xu", usse_constant_words(op->num)[index & 1]); return out; }
    if (op->bank == USSE_INDEX) { g->uses_index = 1; snprintf(out, cap, "uint(IX%d)", (op->num - 1) & 1); return out; }
    place pl = resolve(g, op, shift, USSE_U32, index + 1);
    if (!pl.ok) { snprintf(out, cap, "0u"); return out; }
    return word(g, &pl, index, out, cap);
}

/* One integer element of an operand read as some other type than its own. */
static void integer_element(gen *g, const usse_operand *op, uint8_t type, int element, int32_t shift, char *out, size_t cap) {
    if (op->bank == USSE_IMMEDIATE) { snprintf(out, cap, "int(%uu)", (unsigned)op->num); return; }
    usse_operand o = *op;
    o.type = type;
    o.flags = 0;
    o.sw[0] = (uint8_t)element;
    val v = load(g, &o, 1, shift);
    as_kind(&v, 0, K_SINT, out, cap);
}

static int bits_set(uint8_t mask) {
    int n = 0;
    for (int i = 0; i < 4; i++) n += (mask >> i) & 1;
    return n;
}

/* --- pieces of instructions ---------------------------------------------------------------- */

static float type_max(uint8_t type) {
    return type == USSE_U8 ? 255.0f : type == USSE_S8 ? 127.0f : type == USSE_U16 ? 65535.0f : type == USSE_S16 ? 32767.0f : 1.0f;
}

/* Between floats nothing happens but the re-encoding the store does, and
 * between integers nothing at all. An integer becomes a float as its value,
 * or scaled into 0..1 or -1..1; a float becomes an integer by truncation, or
 * scaled the other way and rounded. */
static val convert(gen *g, const val *v, uint8_t source, uint8_t dest, int scale) {
    int from_float = v->kind == K_FLOAT, to_float = is_float_type(dest);
    char e[160], b[64];
    if (from_float == to_float) {
        if (to_float || v->kind == kind_of(dest)) return *v;
        val out = new_val(g, kind_of(dest));
        for (int i = 0; i < v->n; i++) local(g, &out, as_kind(v, i, out.kind, b, sizeof(b)));
        return out;
    }
    if (to_float) {
        val out = new_val(g, K_FLOAT);
        for (int i = 0; i < v->n; i++) {
            if (!scale) snprintf(e, sizeof(e), "float(%s)", v->e[i]);
            else if (v->kind == K_SINT) snprintf(e, sizeof(e), "max(float(%s) / %.1f, -1.0)", v->e[i], (double)type_max(source));
            else snprintf(e, sizeof(e), "(float(%s) / %.1f)", v->e[i], (double)type_max(source));
            local(g, &out, e);
        }
        return out;
    }
    val out = new_val(g, kind_of(dest));
    for (int i = 0; i < v->n; i++) {
        if (scale) snprintf(e, sizeof(e), "to_int(round(unit(%s, %s) * %.1f))", v->e[i], out.kind == K_SINT ? "-1.0" : "0.0", (double)type_max(dest));
        else snprintf(e, sizeof(e), "to_int(%s)", v->e[i]);
        if (out.kind == K_UINT) { char u[176]; snprintf(u, sizeof(u), "uint(%s)", e); local(g, &out, u); }
        else local(g, &out, e);
    }
    return out;
}

/* An integer expression brought back into the width of its type. */
static void wrapped(char *out, size_t cap, const char *expr, uint8_t type) {
    switch (type) {
    case USSE_S8:  snprintf(out, cap, "(int((%s) << 24) >> 24)", expr); break;
    case USSE_U8:  snprintf(out, cap, "((%s) & 0xFF)", expr); break;
    case USSE_S16: snprintf(out, cap, "(int((%s) << 16) >> 16)", expr); break;
    case USSE_U16: snprintf(out, cap, "((%s) & 0xFFFF)", expr); break;
    default:       snprintf(out, cap, "(%s)", expr); break;
    }
}

/* A test on component i of two loaded values: the operation, then the
 * comparison of its result with zero; or, for a subtraction, the comparison
 * of the two operands themselves. Leaves a bool expression in `out`. */
static int test_component(gen *g, const usse_insn *in, const val *a, const val *b, int i, char *out, size_t cap) {
    int exact = in->c == 1, sign = in->b & 3;
    uint8_t type = in->src1.type;
    char x[64], y[64], v[320], z[64] = "0";
    int n = ++g->serial;
    if (a->kind == K_FLOAT) {
        as_kind(a, i, K_FLOAT, x, sizeof(x));
        as_kind(b, i, K_FLOAT, y, sizeof(y));
        snprintf(z, sizeof(z), "0.0");
        switch (in->a) {
        case T_SUB: snprintf(v, sizeof(v), "%s", x); snprintf(z, sizeof(z), "%s", y); break;
        case T_ADD: snprintf(v, sizeof(v), "%s + %s", x, y); break;
        case T_MUL: snprintf(v, sizeof(v), "%s * %s", x, y); break;
        case T_MIN: snprintf(v, sizeof(v), "(%s < %s ? %s : %s)", y, x, y, x); break;
        case T_MAX: snprintf(v, sizeof(v), "(%s < %s ? %s : %s)", x, y, y, x); break;
        case T_FRC: snprintf(v, sizeof(v), "%s - floor(%s)", x, y); break;
        default: return 0;
        }
        put(&g->body, "        float t%d = %s, z%d = %s;\n", n, v, n, z);
    } else {
        /* in the kind of the first operand; a shift right is of the bits, whatever their kind */
        int kind = a->kind, width = (type == USSE_S8 || type == USSE_U8) ? 8 : (type == USSE_S16 || type == USSE_U16) ? 16 : 32;
        char e[160];
        as_kind(a, i, kind, x, sizeof(x));
        as_kind(b, i, kind, y, sizeof(y));
        switch (in->a) {
        case T_SUB: snprintf(v, sizeof(v), "%s", x); snprintf(z, sizeof(z), "%s", y); break;
        case T_ADD: snprintf(e, sizeof(e), "%s + %s", x, y); wrapped(v, sizeof(v), e, type); break;
        case T_MUL: snprintf(e, sizeof(e), "%s * %s", x, y); wrapped(v, sizeof(v), e, type); break;
        case T_AND: snprintf(v, sizeof(v), "%s & %s", x, y); break;
        case T_OR:  snprintf(v, sizeof(v), "%s | %s", x, y); break;
        case T_XOR: snprintf(v, sizeof(v), "%s ^ %s", x, y); break;
        case T_SHL: {
            char shifted[192];
            snprintf(e, sizeof(e), "%s << (uint(%s) & 31u)", x, y);
            wrapped(shifted, sizeof(shifted), e, type);
            snprintf(v, sizeof(v), "(uint(%s) >= %du ? 0 : %s)", y, width, shifted);
            break;
        }
        case T_SHR:
            kind = K_UINT;
            snprintf(v, sizeof(v), "(uint(%s) >= %du ? 0u : (uint(%s) & 0x%08Xu) >> (uint(%s) & 31u))", y, width, x,
                     width == 32 ? 0xFFFFFFFFu : (1u << width) - 1u, y);
            snprintf(z, sizeof(z), "0u");
            break;
        case T_ASR:
            kind = K_SINT;
            wrapped(e, sizeof(e), x, type == USSE_U8 ? USSE_S8 : type == USSE_U16 ? USSE_S16 : USSE_S32);
            snprintf(v, sizeof(v), "(int(%s) >> min(uint(%s), %du))", e, y, width - 1);
            break;
        default: return 0;
        }
        put(&g->body, "        %s t%d = %s, z%d = %s;\n", kind_name(kind), n, v, n, z);
    }
    switch (sign) {
    case 0:  snprintf(out, cap, exact ? "(t%d == z%d)" : "(t%d < z%d || t%d > z%d)", n, n, n, n); break;
    case 1:  snprintf(out, cap, exact ? "(t%d <= z%d)" : "(t%d < z%d)", n, n); break;
    case 2:  snprintf(out, cap, exact ? "(t%d >= z%d)" : "(t%d > z%d)", n, n); break;
    default: snprintf(out, cap, "false"); break;
    }
    return 1;
}

/* The condition of a conditional move, on one component: against zero. */
static void zero_test(const val *t, int i, uint8_t test, char *out, size_t cap) {
    const char *x = t->e[i];
    if (t->kind == K_FLOAT) {
        if (test == 0) snprintf(out, cap, "(%s == 0.0)", x);
        else if (test == 1) snprintf(out, cap, "(%s < 0.0 || %s > 0.0)", x, x);
        else if (test == 2) snprintf(out, cap, "(%s < 0.0)", x);
        else snprintf(out, cap, "(%s <= 0.0)", x);
    } else if (t->kind == K_SINT) {
        snprintf(out, cap, "(%s %s 0)", x, test == 0 ? "==" : test == 1 ? "!=" : test == 2 ? "<" : "<=");
    } else {
        if (test == 1) snprintf(out, cap, "(%s != 0u)", x);
        else if (test == 2) snprintf(out, cap, "false");
        else snprintf(out, cap, "(%s == 0u)", x);
    }
}

/* What one operation of a dual-issue instruction computes. A dot product
 * and a scalar operation give one number, whatever the mask they are
 * stored under. */
static val dual_result(gen *g, const usse_half *h) {
    val a = load(g, &h->src[0], h->src_mask, 0), b = a, c = a;
    char x[96], y[96], z[96], e[512];
    if (h->sources > 1) b = load(g, &h->src[1], h->src_mask, 0);
    if (h->sources > 2) c = load(g, &h->src[2], h->src_mask, 0);
    if (h->op == DUAL_VMOV) return a;
    val out = new_val(g, K_FLOAT);
    if (!a.n || b.n < a.n || c.n < a.n) { fail(g, "a dual-issue instruction with nothing to read"); return out; }
    if (h->op == DUAL_VDP || h->op == DUAL_VSSQ) {
        /* a sum of squares is the dot product of its one operand with itself */
        size_t at = (size_t)snprintf(e, sizeof(e), "0.0");
        for (int i = 0; i < a.n && at < sizeof(e); i++)
            at += (size_t)snprintf(e + at, sizeof(e) - at, " + %s * %s", as_kind(&a, i, K_FLOAT, x, sizeof(x)), as_kind(&b, i, K_FLOAT, y, sizeof(y)));
        local(g, &out, e);
        return out;
    }
    for (int i = 0; i < a.n; i++) {
        as_kind(&a, i, K_FLOAT, x, sizeof(x));
        as_kind(&b, i, K_FLOAT, y, sizeof(y));
        as_kind(&c, i, K_FLOAT, z, sizeof(z));
        switch (h->op) {
        case DUAL_VMAD: case DUAL_FMAD: snprintf(e, sizeof(e), "%s * %s + %s", x, y, z); break;
        case DUAL_VMUL: case DUAL_FMUL: snprintf(e, sizeof(e), "%s * %s", x, y); break;
        case DUAL_VADD: case DUAL_FADD: snprintf(e, sizeof(e), "%s + %s", x, y); break;
        case DUAL_FSUBFLR:              snprintf(e, sizeof(e), "%s - floor(%s)", x, y); break;
        case DUAL_FRSQ:                 snprintf(e, sizeof(e), "1.0 / sqrt(%s)", x); break;
        case DUAL_FRCP:                 snprintf(e, sizeof(e), "1.0 / %s", x); break;
        case DUAL_FEXP:                 snprintf(e, sizeof(e), "(%s != %s ? 1.0 : exp(%s))", x, x, x); break;
        default:                        snprintf(e, sizeof(e), "log(%s)", x); break;
        }
        local(g, &out, e);
    }
    return out;
}

/* A factor of a sum of products on one channel: nothing, a source's
 * channel or a source's alpha, or one minus that. */
static void sop_factor(uint8_t factor, int channel, const val *s0, const val *s1, const val *s2, char *out, size_t cap) {
    const char *from;
    switch (factor & 7) {
    case SOP_SRC0:       from = s0->e[channel]; break;
    case SOP_SRC0_ALPHA: from = s0->e[3]; break;
    case SOP_SRC1:       from = s1->e[channel]; break;
    case SOP_SRC1_ALPHA: from = s1->e[3]; break;
    case SOP_SRC2:       from = s2->e[channel]; break;
    case SOP_SRC2_ALPHA: from = s2->e[3]; break;
    default:             from = NULL; break;
    }
    if (!from) snprintf(out, cap, (factor & SOP_COMPLEMENT) ? "255u" : "0u");
    else if (factor & SOP_COMPLEMENT) snprintf(out, cap, "(255u - %s)", from);
    else snprintf(out, cap, "%s", from);
}

/* The unit a sample instruction means, by the register it names; -1 for none. */
static int sampler_unit(const gxp_program *p, uint32_t sa_register) {
    for (uint32_t i = 0; i < p->sampler_count; i++)
        if (p->samplers[i].reg == sa_register && p->samplers[i].unit < GXP_TEXTURE_UNITS) return p->samplers[i].unit;
    return -1;
}

/* --- one instruction ----------------------------------------------------------------------- */

#define OFF(operand, r) usse_repeat_offset(&g->st, in, operand, r)

static void emit(gen *g, const usse_insn *in) {
    text *o = &g->body;
    char a_[96], b_[96], c_[96], e[512];
    switch (in->op) {
    case USSE_OP_MAD:
        for (int r = 0; r <= in->repeat; r++) {
            val a = load(g, &in->src0, in->mask, OFF(&in->src0, r));
            val b = load(g, &in->src1, in->mask, OFF(&in->src1, r));
            val c = load(g, &in->src2, in->mask, OFF(&in->src2, r));
            val out = new_val(g, K_FLOAT);
            for (int i = 0; i < a.n; i++) {
                snprintf(e, sizeof(e), "%s * %s + %s", as_kind(&a, i, K_FLOAT, a_, sizeof(a_)), as_kind(&b, i, K_FLOAT, b_, sizeof(b_)),
                         as_kind(&c, i, K_FLOAT, c_, sizeof(c_)));
                local(g, &out, e);
            }
            store(g, &in->dest, &out, in->mask, OFF(&in->dest, r));
        }
        return;
    case USSE_OP_MUL: case USSE_OP_ADD: case USSE_OP_FRC: case USSE_OP_MIN: case USSE_OP_MAX: {
        val a = load(g, &in->src1, in->src_mask, 0), b = load(g, &in->src2, in->src_mask, 0);
        val out = new_val(g, K_FLOAT);
        for (int i = 0; i < a.n; i++) {
            const char *x = as_kind(&a, i, K_FLOAT, a_, sizeof(a_)), *y = as_kind(&b, i, K_FLOAT, b_, sizeof(b_));
            if (in->op == USSE_OP_MUL) snprintf(e, sizeof(e), "%s * %s", x, y);
            else if (in->op == USSE_OP_ADD) snprintf(e, sizeof(e), "%s + %s", x, y);
            else if (in->op == USSE_OP_FRC) snprintf(e, sizeof(e), "%s - floor(%s)", x, y);
            else if (in->op == USSE_OP_MIN) snprintf(e, sizeof(e), "(%s < %s ? %s : %s)", y, x, y, x);
            else snprintf(e, sizeof(e), "(%s < %s ? %s : %s)", x, y, y, x);
            local(g, &out, e);
        }
        store(g, &in->dest, &out, in->mask, 0);
        return;
    }
    case USSE_OP_DOT: {
        val a = load(g, &in->src1, 15, 0), b = load(g, &in->src2, 15, 0);
        val out = new_val(g, K_FLOAT);
        if (a.kind != K_FLOAT || b.kind != K_FLOAT || a.n < 4 || b.n < 4) { fail(g, "a dot product of integers"); return; }
        snprintf(e, sizeof(e), "%s * %s + %s * %s + %s * %s + %s * %s", a.e[0], b.e[0], a.e[1], b.e[1], a.e[2], b.e[2], a.e[3], b.e[3]);
        local(g, &out, e);
        for (int k = 1, n = bits_set(in->mask); k < n; k++) local(g, &out, out.e[0]);
        store(g, &in->dest, &out, in->mask, 0);
        return;
    }
    case USSE_OP_DOT_GPI: {
        uint8_t mask = in->mask;
        for (int r = 0; r <= in->repeat; r++) {
            val a = load(g, &in->src1, in->src_mask, OFF(&in->src1, r));
            val b = load(g, &in->src2, in->src_mask, OFF(&in->src2, r));
            val out = new_val(g, K_FLOAT);
            if (a.kind != K_FLOAT || b.kind != K_FLOAT) { fail(g, "a dot product of integers"); return; }
            size_t at = (size_t)snprintf(e, sizeof(e), "0.0");
            for (int i = 0; i < a.n && at < sizeof(e); i++) at += (size_t)snprintf(e + at, sizeof(e) - at, " + %s * %s", a.e[i], b.e[i]);
            local(g, &out, e);
            for (int k = 1, n = bits_set(mask); k < n; k++) local(g, &out, out.e[0]);
            store(g, &in->dest, &out, mask, 0);
            mask = (uint8_t)(((mask << 1) | (mask >> 3)) & 15);
        }
        return;
    }
    case USSE_OP_RCP: case USSE_OP_RSQ: case USSE_OP_LOG: case USSE_OP_EXP:
        for (int r = 0; r <= in->repeat; r++) {
            val a = load(g, &in->src1, in->src_mask, OFF(&in->src1, r));
            val out = new_val(g, K_FLOAT);
            const char *x = as_kind(&a, 0, K_FLOAT, a_, sizeof(a_));
            if (!a.n) { fail(g, "a unary instruction with nothing to read"); return; }
            if (in->op == USSE_OP_RCP) snprintf(e, sizeof(e), "1.0 / %s", x);
            else if (in->op == USSE_OP_RSQ) snprintf(e, sizeof(e), "1.0 / sqrt(%s)", x);
            else if (in->op == USSE_OP_LOG) snprintf(e, sizeof(e), "log(%s)", x);
            else snprintf(e, sizeof(e), "(%s != %s ? 1.0 : exp(%s))", x, x, x);
            local(g, &out, e);
            for (int k = 1, n = bits_set(in->mask); k < n; k++) local(g, &out, out.e[0]);
            store(g, &in->dest, &out, in->mask, OFF(&in->dest, r));
        }
        return;

    case USSE_OP_MOV: case USSE_OP_MOVC: case USSE_OP_MOVCU8:
        /* a move, or a choice between two values by a third's relation to zero */
        for (int r = 0; r <= in->repeat; r++) {
            val a = load(g, &in->src1, in->mask, OFF(&in->src1, r));
            if (in->op != USSE_OP_MOV) {
                val t = load(g, &in->src0, in->mask, OFF(&in->src0, r));
                val b = load(g, &in->src2, in->mask, OFF(&in->src2, r));
                for (int i = 0; i < a.n && i < t.n && i < b.n; i++) {
                    zero_test(&t, i, in->a, e, sizeof(e));
                    put(o, "        if (!%s) %s = %s;\n", e, a.e[i], as_kind(&b, i, a.kind, b_, sizeof(b_)));
                }
            }
            store(g, &in->dest, &a, in->mask, OFF(&in->dest, r));
        }
        return;

    case USSE_OP_PCK:
        if (in->src1.type == USSE_O8 || in->dest.type == USSE_O8) { fail(g, "a pack to or from the output's bytes"); return; }
        for (int r = 0; r <= in->repeat; r++) {
            int32_t o1 = OFF(&in->src1, r), o2 = OFF(&in->src2, r);
            val v;
            if (in->b) {
                /* four floats from two registers: x and y of one, then of the other */
                v = new_val(g, K_FLOAT);
                for (int i = 0; i < 4; i++) {
                    if (!((in->mask >> i) & 1)) continue;
                    int c = in->src1.sw[i] & 3;
                    if (c < 2) read_word(g, &in->src1, o1, c, a_, sizeof(a_)); else read_word(g, &in->src2, o2, c - 2, a_, sizeof(a_));
                    snprintf(e, sizeof(e), "asfloat(%s)", a_);
                    local(g, &v, e);
                }
            } else {
                v = load(g, &in->src1, in->mask, o1);
            }
            val out = convert(g, &v, in->src1.type, in->dest.type, in->a);
            store(g, &in->dest, &out, in->mask, OFF(&in->dest, r));
        }
        return;

    case USSE_OP_AND: case USSE_OP_OR: case USSE_OP_XOR: case USSE_OP_SHL: case USSE_OP_SHR:
    case USSE_OP_ASR: case USSE_OP_ROL: {
        uint32_t low = in->a ? 0xFFFFu : 0xFFFFFFFFu, width = in->a ? 16 : 32;
        for (int r = 0; r <= in->repeat; r++) {
            val x = load(g, &in->src1, 1, OFF(&in->src1, r));
            int n = ++g->serial;
            if (!x.n) { fail(g, "a bitwise instruction with nothing to read"); return; }
            put(o, "        uint a%d = %s & 0x%08Xu, b%d = %uu;\n", n, bits(&x, 0, a_, sizeof(a_)), low, n, (unsigned)in->imm);
            if (in->b != 1) {
                /* a register: turned, then perhaps complemented, within the instruction's width */
                val y = load(g, &in->src2, 1, OFF(&in->src2, r));
                uint32_t turn = in->c % width;
                if (!y.n) { fail(g, "a bitwise instruction with nothing to read"); return; }
                put(o, "        b%d = %s & 0x%08Xu;\n", n, bits(&y, 0, b_, sizeof(b_)), low);
                if (turn) put(o, "        b%d = (b%d << %u) | (b%d >> %u);\n", n, n, turn, n, width - turn);
                if (in->b == 2) put(o, "        b%d = ~b%d;\n", n, n);
                put(o, "        b%d &= 0x%08Xu;\n", n, low);
            }
            switch (in->op) {
            case USSE_OP_AND: snprintf(e, sizeof(e), "a%d & b%d", n, n); break;
            case USSE_OP_OR:  snprintf(e, sizeof(e), "a%d | b%d", n, n); break;
            case USSE_OP_XOR: snprintf(e, sizeof(e), "a%d ^ b%d", n, n); break;
            case USSE_OP_SHL: snprintf(e, sizeof(e), "(b%d >= %uu ? 0u : a%d << (b%d & 31u))", n, width, n, n); break;
            case USSE_OP_SHR: snprintf(e, sizeof(e), "(b%d >= %uu ? 0u : a%d >> (b%d & 31u))", n, width, n, n); break;
            case USSE_OP_ROL:
                snprintf(e, sizeof(e), "((b%d %% %uu) != 0u ? (a%d << (b%d %% %uu)) | (a%d >> (%uu - b%d %% %uu)) : a%d)",
                         n, width, n, n, width, n, width, n, width, n);
                break;
            default: {
                char wide[64];
                snprintf(wide, sizeof(wide), in->a ? "(int(a%d << 16) >> 16)" : "int(a%d)", n);
                snprintf(e, sizeof(e), "uint(%s >> min(b%d, %uu))", wide, n, width - 1);
                break;
            }
            }
            val out = new_val(g, K_UINT);
            { char masked[560]; snprintf(masked, sizeof(masked), "(%s) & 0x%08Xu", e, low); local(g, &out, masked); }
            store(g, &in->dest, &out, 1, OFF(&in->dest, r));
        }
        return;
    }

    case USSE_OP_TEST: {
        val a = load(g, &in->src1, in->mask, 0), b = load(g, &in->src2, in->mask, 0);
        if (!a.n || !b.n || !test_component(g, in, &a, &b, 0, e, sizeof(e))) { fail(g, "a test of an operation not computed"); return; }
        put(o, "        PR%d = %s;\n", in->dest.num & 3, e);
        return;
    }
    case USSE_OP_TESTMASK: {
        if (!in->imm) return;
        val a = load(g, &in->src1, in->mask, 0), b = load(g, &in->src2, in->mask, 0);
        uint8_t type = in->dest.type;
        for (int i = 0; i < a.n && i < b.n; i++) {
            if (!test_component(g, in, &a, &b, i, e, sizeof(e))) { fail(g, "a test of an operation not computed"); return; }
            if (a.kind == K_FLOAT) put(o, "        %s = %s ? 1.0 : 0.0;\n", a.e[i], e);
            else put(o, "        %s = %s ? %s(0x%Xu) : 0;\n", a.e[i], e, a.kind == K_SINT ? "int" : "uint",
                     type == USSE_U8 ? 0xFFu : type == USSE_U16 ? 0xFFFFu : type == USSE_U32 ? 0xFFFFFFFFu
                     : type == USSE_S8 ? 0x7Fu : type == USSE_S16 ? 0x7FFFu : 0x7FFFFFFFu);
        }
        store(g, &in->dest, &a, in->mask, 0);
        return;
    }

    case USSE_OP_IMAD16: {
        /* src0 times src1, plus src2, in sixteen bits */
        int is_signed = (in->d & 2) != 0;
        uint8_t base = is_signed ? USSE_S16 : USSE_U16, narrow = is_signed ? USSE_S8 : USSE_U8;
        for (int r = 0; r <= in->repeat; r++) {
            integer_element(g, &in->src0, base, 0, OFF(&in->src0, r), a_, sizeof(a_));
            integer_element(g, &in->src1, in->a ? narrow : base, (in->a && (in->c & 1)) ? 1 : 0, OFF(&in->src1, r), b_, sizeof(b_));
            integer_element(g, &in->src2, in->b ? narrow : base, (in->b && (in->c & 2)) ? 1 : 0, OFF(&in->src2, r), c_, sizeof(c_));
            usse_operand dest = in->dest;
            val out = new_val(g, K_SINT);
            snprintf(e, sizeof(e), "int((uint(%s) * uint(%s) + uint(%s)) & 0xFFFFu)", a_, b_, c_);
            local(g, &out, e);
            dest.type = base;
            store(g, &dest, &out, 1, OFF(&in->dest, r));
        }
        return;
    }
    case USSE_OP_IMAD32: {
        /* src0 times src1, sixteen bits each, plus src2, in thirty-two */
        int is_signed = in->d & 1;
        uint8_t half = is_signed ? USSE_S16 : USSE_U16, whole = is_signed ? USSE_S32 : USSE_U32;
        for (int r = 0; r <= in->repeat; r++) {
            integer_element(g, &in->src0, half, in->a & 1, OFF(&in->src0, r), a_, sizeof(a_));
            integer_element(g, &in->src1, half, (in->a >> 1) & 1, OFF(&in->src1, r), b_, sizeof(b_));
            if (in->b == 2) integer_element(g, &in->src2, whole, 0, OFF(&in->src2, r), c_, sizeof(c_));
            else integer_element(g, &in->src2, half, (in->a >> 2) & 1, OFF(&in->src2, r), c_, sizeof(c_));
            usse_operand dest = in->dest;
            val out = new_val(g, is_signed ? K_SINT : K_UINT);
            snprintf(e, sizeof(e), "%s(uint(%s) * uint(%s) + uint(%s))", is_signed ? "int" : "uint", a_, b_, c_);
            local(g, &out, e);
            dest.type = whole;
            store(g, &dest, &out, 1, OFF(&in->dest, r));
        }
        return;
    }
    case USSE_OP_IMAD8:
        /* src1 times src2, a byte from each, plus or minus the addend:
         * whole numbers, cut to a byte or held to one */
        if (!usse_can_run(in)) { fail(g, "an 8-bit multiply-add in a form there is no account of"); return; }
        for (int r = 0; r <= in->repeat; r++) {
            val a = load(g, &in->src0, 15, OFF(&in->src0, r));
            val b = load(g, &in->src1, 15, OFF(&in->src1, r));
            val c = load(g, &in->src2, 15, OFF(&in->src2, r));
            val out = new_val(g, K_UINT);
            if (a.n < 4 || b.n < 4 || c.n < 4) { fail(g, "an 8-bit multiply-add with nothing to read"); return; }
            for (int ch = 0; ch < 4; ch++) {
                const val *from = (ch < 3 ? (in->a & 1) : (in->a & 4)) ? &b : &a;
                const char *addend = from->e[(ch < 3 && (in->a & 2)) ? 3 : ch];
                char sign = (in->b & IMAD8_SUBTRACT) ? '-' : '+';
                if (in->b & IMAD8_SATURATE) snprintf(e, sizeof(e), "uint(clamp(int(%s * %s) %c int(%s), 0, 255))", b.e[ch], c.e[ch], sign, addend);
                else snprintf(e, sizeof(e), "(%s * %s %c %s) & 0xFFu", b.e[ch], c.e[ch], sign, addend);
                local(g, &out, e);
            }
            store(g, &in->dest, &out, 15, OFF(&in->dest, r));
        }
        return;

    case USSE_OP_SOP2: case USSE_OP_SOP2M: case USSE_OP_SOP3:
        /* Bytes are colours from 0 to 1 in 255 steps: the two products are
         * combined in steps of 1/65025, held between nothing and one, and
         * rounded back to a byte. */
        if (!usse_can_run(in)) { fail(g, "a sum of products in a form there is no account of"); return; }
        for (int r = 0; r <= in->repeat; r++) {
            val s1 = load(g, &in->src1, 15, OFF(&in->src1, r));
            val s2 = load(g, &in->src2, 15, OFF(&in->src2, r));
            val s0 = in->op == USSE_OP_SOP3 ? load(g, &in->src0, 15, OFF(&in->src0, r)) : s1;      /* the others have no source 0 */
            if (s0.n < 4 || s1.n < 4 || s2.n < 4) { fail(g, "a sum of products with nothing to read"); return; }
            if (in->d & SOP_SRC1_INVERSE)
                for (int ch = 0; ch < 4; ch++) put(o, "        %s = 255u - %s;\n", s1.e[ch], s1.e[ch]);
            val out = new_val(g, K_UINT);
            /* every channel the mask keeps is worked out, the colour's three one way and the alpha another */
            for (int ch = 0; ch < 4; ch++) {
                uint8_t factors = ch < 3 ? in->a : in->b, op = (uint8_t)((ch < 3 ? in->c : in->c >> 2) & 3);
                int n = ++g->serial;
                if (!((in->mask >> ch) & 1)) continue;
                sop_factor(factors, ch, &s0, &s1, &s2, a_, sizeof(a_));
                sop_factor((uint8_t)(factors >> 4), ch, &s0, &s1, &s2, b_, sizeof(b_));
                put(o, "        uint p%d = %s * %s, q%d = %s * %s;\n", n, a_, s1.e[ch], n, b_, s2.e[ch]);
                if (op == 0) snprintf(e, sizeof(e), "(min(p%d + q%d, 65025u) + 127u) / 255u", n, n);
                else if (op == 1) snprintf(e, sizeof(e), "((p%d > q%d ? p%d - q%d : 0u) + 127u) / 255u", n, n, n, n);
                else if (op == 2) snprintf(e, sizeof(e), "(min(p%d, q%d) + 127u) / 255u", n, n);
                else snprintf(e, sizeof(e), "(max(p%d, q%d) + 127u) / 255u", n, n);
                local(g, &out, e);
            }
            store(g, &in->dest, &out, in->mask, OFF(&in->dest, r));
        }
        return;

    case USSE_OP_DUAL: {
        /* both operations read what was there before either writes */
        usse_half half[2];
        val out[2];
        usse_dual_halves(in, half);
        for (int k = 0; k < 2; k++) {
            out[k] = dual_result(g, &half[k]);
            if (g->failed) return;
        }
        for (int k = 0; k < 2; k++) store(g, &half[k].dest, &out[k], half[k].mask, 0);
        return;
    }

    case USSE_OP_DEPTHF:
        /* the tests would have to be made after the program, on what it leaves: nothing here sets that up */
        fail(g, "a depth of the program's own");
        return;

    case USSE_OP_LIMM: {
        val out = new_val(g, K_UINT);
        snprintf(e, sizeof(e), "0x%08Xu", in->imm);
        local(g, &out, e);
        store(g, &in->dest, &out, 1, 0);
        return;
    }

    case USSE_OP_LDR: {
        /* Words of memory into registers. The base register holds an
         * address less four, which the instruction puts back unless told
         * the address is already whole. */
        int n = ++g->serial;
        uint32_t bytes = (uint32_t)in->a * in->b;
        g->uses_mem = 1;
        read_word(g, &in->src0, 0, 0, a_, sizeof(a_));
        if (in->src1.bank == USSE_IMMEDIATE) snprintf(b_, sizeof(b_), "%uu", (unsigned)in->src1.num);
        else { char w[96]; snprintf(b_, sizeof(b_), "(%s - 0x10000u)", read_word(g, &in->src1, 0, 0, w, sizeof(w))); }
        read_word(g, &in->src2, 0, 0, c_, sizeof(c_));
        put(o, "        uint m%d = %s + %s + %uu + %s;\n", n, a_, b_, in->c ? 0u : 4u, c_);
        for (uint32_t j = 0; 4 * j < bytes; j++) {
            uint32_t left = bytes - 4 * j;
            usse_operand dest = in->dest;
            uint8_t type = USSE_U32;
            place pl = resolve(g, &dest, 0, type, (int)j + 1);
            char w[96];
            if (!pl.ok) continue;
            word(g, &pl, (int)j, w, sizeof(w));
            if (left < 4) {
                uint32_t keep = 0xFFFFFFFFu << (8 * left);
                put(o, "        %s = (mem(m%d + %uu) & 0x%08Xu) | (%s & 0x%08Xu);\n", w, n, 4 * j, ~keep, w, keep);
            } else {
                put(o, "        %s = mem(m%d + %uu);\n", w, n, 4 * j);
            }
        }
        return;
    }

    case USSE_OP_SMP: {
        val c = load(g, &in->src0, in->src_mask, 0);
        int unit = sampler_unit(g->p, in->src1.num), n = ++g->serial;
        char lod[64] = "0.0";
        if (in->b == 3) { fail(g, "a sample with explicit gradients"); return; }
        if (c.kind != K_FLOAT) { fail(g, "a sample at an integer coordinate"); return; }
        if (in->c == 1 || in->c == 3) {
            /* the four texels of the footprint, first component only, and
             * after them the weight a bilinear filter would give each, as
             * halves */
            int bound = unit >= 0 && g->b->tex_components[unit] && !g->b->tex_cube[unit];
            if (!is_float_type(in->dest.type)) { fail(g, "a gathering sample into integers"); return; }
            if (!bound) {
                put(o, "        float4 s%d = float4(0.0, 0.0, 0.0, 0.0);\n        float2 f%d = float2(0.0, 0.0);\n", n, n);
            } else {
                put(o, "        float2 f%d;\n", n);
                put(o, "        float4 s%d = gather_at(TEX%d, float2(%s, %s), %d, %d, f%d);\n", n, unit, c.n > 0 ? c.e[0] : "0.0",
                    c.n > 1 ? c.e[1] : "0.0", g->b->tex_wrap[unit] & 15, g->b->tex_wrap[unit] >> 4, n);
            }
            val out = new_val(g, K_FLOAT);
            for (int i = 0; i < 4; i++) { snprintf(e, sizeof(e), "s%d.%c", n, "xyzw"[i]); local(g, &out, e); }
            store(g, &in->dest, &out, 15, 0);
            if (in->c == 3) {
                usse_operand weights = in->dest;
                weights.type = USSE_F16;
                weights.num = (uint16_t)(weights.num + (in->dest.type == USSE_F32 ? 4 : 2));
                val wt = new_val(g, K_FLOAT);
                snprintf(e, sizeof(e), "(1.0 - f%d.x) * (1.0 - f%d.y)", n, n); local(g, &wt, e);
                snprintf(e, sizeof(e), "f%d.x * (1.0 - f%d.y)", n, n);         local(g, &wt, e);
                snprintf(e, sizeof(e), "f%d.x * f%d.y", n, n);                 local(g, &wt, e);
                snprintf(e, sizeof(e), "(1.0 - f%d.x) * f%d.y", n, n);         local(g, &wt, e);
                store(g, &weights, &wt, 15, 0);
            }
            return;
        }
        if (in->c != 0) { fail(g, "a sample mode the interpreter has no means for either"); return; }
        if (in->b) {
            usse_operand bias = in->src2;
            bias.type = in->src0.type;
            val l = load(g, &bias, 1, 0);
            if (l.n) as_kind(&l, 0, K_FLOAT, lod, sizeof(lod));
        }
        int bound = unit >= 0 && g->b->tex_components[unit], comps = bound ? g->b->tex_components[unit] : 4;
        if (comps < 1 || comps > 4) comps = 4;
        if (!bound) {
            put(o, "        float4 s%d = float4(0.0, 0.0, 0.0, 0.0);\n", n);
        } else if (g->b->tex_cube[unit]) {
            put(o, "        float4 s%d = TEX%d.SampleLevel(SMP%d, cube_at(float3(%s, %s, %s)), %s);\n", n, unit, unit,
                c.n > 0 ? c.e[0] : "0.0", c.n > 1 ? c.e[1] : "0.0", c.n > 2 ? c.e[2] : "0.0", lod);
        } else if (g->b->tex_point[unit] && g->b->larger > 1 && g->b->tex_locked[unit] && g->fragment) {
            put(o, "        float4 s%d = texel_locked(TEX%d, float2(%s, %s), i.pos.xy, %d, ((asuint(VP[2].w) >> %d) & 1u) != 0u, %d, %d);\n", n, unit,
                c.n > 0 ? c.e[0] : "0.0", c.n > 1 ? c.e[1] : "0.0", g->b->larger, unit, g->b->tex_wrap[unit] & 15, g->b->tex_wrap[unit] >> 4);
        } else if (g->b->tex_point[unit]) {
            put(o, "        float4 s%d = texel_at(TEX%d, float2(%s, %s), %d, %d);\n", n, unit, c.n > 0 ? c.e[0] : "0.0",
                c.n > 1 ? c.e[1] : "0.0", g->b->tex_wrap[unit] & 15, g->b->tex_wrap[unit] >> 4);
        } else {
            put(o, "        float4 s%d = TEX%d.SampleLevel(SMP%d, float2(%s, %s), %s);\n", n, unit, unit,
                c.n > 0 ? c.e[0] : "0.0", c.n > 1 ? c.e[1] : "0.0", lod);
        }
        val out = new_val(g, K_FLOAT);
        for (int i = 0; i < comps; i++) { snprintf(e, sizeof(e), "s%d.%c", n, "xyzw"[i]); local(g, &out, e); }
        if (!is_float_type(in->dest.type)) out = convert(g, &out, USSE_F32, in->dest.type, 1);
        store(g, &in->dest, &out, (uint8_t)((1u << comps) - 1), 0);
        return;
    }

    case USSE_OP_SMLSI:
        usse_apply_smlsi(&g->st, in);
        return;
    case USSE_OP_KILL:
        /* the predicate it names is the one that spares the fragment */
        if (!g->fragment) return;
        if (in->a == 1) put(o, "        if (!PR0) discard;\n");
        else if (in->a == 2) put(o, "        if (!PR1) discard;\n");
        else if (in->a == 3) put(o, "        if (PR0) discard;\n");
        else put(o, "        discard;\n");
        return;
    case USSE_OP_PHAS: case USSE_OP_NOP: case USSE_OP_SPEC:
        return;
    case USSE_OP_BR:                /* gxp_hlsl makes the `if` */
        return;
    default:
        fail(g, "an instruction the interpreter has no means for either (operation %d)", in->op);
        return;
    }
}

/* --- what goes round the instructions -------------------------------------------------------- */

static const char PRELUDE[] =
    "// Written by the toolkit from a program of the title's: the interpreter's steps, as text.\n"
    "cbuffer draw : register(b0) { uint4 SAv[66]; float4 VP[3]; uint4 MEMR[8]; };\n"
    "Buffer<uint> MEM : register(t16);\n"
    "uint sa_at(int i) { uint u = uint(clamp(i, 0, 263)); return SAv[u >> 2][u & 3u]; }\n"
    /* The first of the eight runs that holds the address, looked for from the last so that the first is what is
     * left, and one load at the end. Written as a loop that returns from its middle, the same thing took the
     * compiler four to six seconds for a program with fifty loads: it is unrolled into each of them. */
    "uint mem(uint address) {\n"
    "    int word = -1;\n"
    "    for (int k = 7; k >= 0; k--) {\n"
    "        uint at = address - MEMR[k].x;\n"
    "        if (at < MEMR[k].y) word = int(MEMR[k].z + (at >> 2));\n"
    "    }\n"
    "    if (word < 0) return 0u;\n"
    "    return MEM.Load(word);\n"
    "}\n"
    /* A float as a half, to the nearest and to the even one of two as near:
     * what the interpreter does. The card's own conversion cuts the bits
     * off, which over a program's worth of halves is a different picture:
     * a coordinate a step short samples somewhere else. So the card's
     * answer is taken and put right from its neighbour.
     *
     * It is written as choices and not as ways out: what is not a number
     * or is past the largest half is worked through like the rest and the
     * answer chosen last. A program stores a hundred halves and more, this
     * is written out for each, and a function left from its middle is one
     * the compiler takes a second over a hundred of. */
    "uint h16(float f) {\n"
    "    uint h = f32tof16(f), m = h & 0x7FFFu;\n"
    "    float a = abs(f), at = f16tof32(m);\n"
    "    uint other = at > a ? m - 1u : m + 1u;\n"
    "    float near = abs(a - at), far = abs(f16tof32(other) - a);\n"
    "    uint best = (far < near || (far == near && (other & 1u) == 0u)) ? other : m;\n"
    "    best = a >= 65520.0 ? 0x7C00u : best;\n"
    "    best = m >= 0x7C00u ? m : best;\n"
    "    return (h & 0x8000u) | best;\n"
    "}\n"
    /* A flat texture's texel by number, wrapped as the console's mode says,
     * and where a point lies among the texels: on 256 steps to a texel, as
     * gxm_tex.c has it. One texel with no filter, or the four a bilinear
     * filter would blend with how far the point lies among them. */
    "int wrapped(int i, int n, int mode) {\n"
    "    if (mode == 0 || mode == 4) { i %= n; return i < 0 ? i + n : i; }\n"
    "    if (mode == 1) { i %= 2 * n; if (i < 0) i += 2 * n; return i < n ? i : 2 * n - 1 - i; }\n"
    "    if (mode == 3) { if (i < 0) i = -1 - i; return i >= n ? n - 1 : i; }\n"
    "    return i < 0 ? 0 : i >= n ? n - 1 : i;\n"
    "}\n"
    "float4 texel_at(Texture2D t, float2 uv, int across, int down) {\n"
    "    uint w, h;\n"
    "    t.GetDimensions(w, h);\n"
    "    if (uv.x != uv.x || uv.y != uv.y) return float4(0.0, 0.0, 0.0, 0.0);\n"
    "    if (abs(uv.x) > 32768.0) uv.x = 0.0;\n"
    "    if (abs(uv.y) > 32768.0) uv.y = 0.0;\n"
    "    float2 p = floor(floor(uv * float2(float(w), float(h)) * 256.0 + 0.5) / 256.0);\n"
    "    return t.Load(int3(wrapped(int(p.x), int(w), across), wrapped(int(p.y), int(h), down), 0));\n"
    "}\n"
    "float4 gather_at(Texture2D t, float2 uv, int across, int down, out float2 among) {\n"
    "    uint w, h;\n"
    "    t.GetDimensions(w, h);\n"
    "    if (uv.x != uv.x || uv.y != uv.y || abs(uv.x) > 32768.0 || abs(uv.y) > 32768.0) uv = float2(0.0, 0.0);\n"
    "    float2 p = floor((uv * float2(float(w), float(h)) - 0.5) * 256.0 + 0.5) / 256.0, first = floor(p);\n"
    "    int x0 = wrapped(int(first.x), int(w), across), x1 = wrapped(int(first.x) + 1, int(w), across);\n"
    "    int y0 = wrapped(int(first.y), int(h), down), y1 = wrapped(int(first.y) + 1, int(h), down);\n"
    "    among = p - first;\n"
    "    return float4(t.Load(int3(x0, y1, 0)).x, t.Load(int3(x1, y1, 0)).x, t.Load(int3(x1, y0, 0)).x, t.Load(int3(x0, y0, 0)).x);\n"
    "}\n"
    /* The face of a cube a direction points at and where on it, as
     * gxm_tex.c's cube_face has it: a cube is six flat faces, and a sample
     * near the edge of one is of that face's texels and no other's. A card
     * left to it blends across the edge, which the console does not. */
    "float3 cube_at(float3 d) {\n"
    "    float3 a = abs(d);\n"
    "    float face, sc, tc, ma;\n"
    "    if (a.x >= a.y && a.x >= a.z) {\n"
    "        ma = a.x;\n"
    "        if (d.x >= 0.0) { face = 0.0; sc = -d.z; tc = -d.y; } else { face = 1.0; sc = d.z; tc = -d.y; }\n"
    "    } else if (a.y >= a.z) {\n"
    "        ma = a.y;\n"
    "        if (d.y >= 0.0) { face = 2.0; sc = d.x; tc = d.z; } else { face = 3.0; sc = d.x; tc = -d.z; }\n"
    "    } else {\n"
    "        ma = a.z;\n"
    "        if (d.z >= 0.0) { face = 4.0; sc = d.x; tc = -d.y; } else { face = 5.0; sc = -d.x; tc = -d.y; }\n"
    "    }\n"
    "    if (!(ma > 0.0)) return float3(0.5, 0.5, 0.0);\n"
    "    return float3((sc / ma + 1.0) * 0.5, (tc / ma + 1.0) * 0.5, face);\n"
    "}\n"
    "float c10(uint field) { int f = int(field & 0x3FFu); if ((f & 0x200) != 0) f -= 0x400; return max(float(f) / 255.0, -2.0); }\n"
    "uint to_c10(float f) { return uint(int(round(clamp(f, -2.0, 2.0) * 255.0))) & 0x3FFu; }\n"
    "float unit(float f, float low) { return f != f ? 0.0 : f < low ? low : f > 1.0 ? 1.0 : f; }\n"
    "int to_int(float f) {\n"
    "    int i = int(f);\n"
    "    i = f <= -2147483648.0 ? int(0x80000000u) : i;\n"
    "    i = f >= 2147483520.0 ? 2147483520 : i;\n"
    "    return f != f ? 0 : i;\n"
    "}\n";

/* A float into the registers as its type packs it: for the inputs. */
static void pack_line(text *o, const char *bank, int reg, unsigned type, unsigned k, const char *value) {
    switch (type) {
    case USSE_F16: {
        int at = 16 * (int)(k & 1);
        put(o, "    %s[%d] = (%s[%d] & 0x%08Xu) | (h16(%s) << %d);\n", bank, reg + (int)(k >> 1), bank, reg + (int)(k >> 1),
            ~(0xFFFFu << at), value, at);
        break;
    }
    case USSE_U8: case USSE_S8:
        put(o, "    %s[%d] = (%s[%d] & 0x%08Xu) | ((uint(int(%s)) & 0xFFu) << %d);\n", bank, reg + (int)(k >> 2), bank,
            reg + (int)(k >> 2), ~(0xFFu << (8 * (k & 3))), value, 8 * (int)(k & 3));
        break;
    default:
        put(o, "    %s[%d] = asuint(%s);\n", bank, reg + (int)k, value);
        break;
    }
}

static void varyings(text *o, uint32_t slots) {
    for (unsigned slot = 0; slot < GXP_SLOT_POSITION; slot++)
        if ((slots >> slot) & 1) put(o, "    float4 s%u : TEXCOORD%u;\n", slot, slot);
}

/* The four floats of an interpolated quantity, as the fragment program's
 * main names them. */
static const char *slot_text(const gen *g, unsigned slot, char *out, size_t cap) {
    if (slot == GXP_SLOT_POSITION) snprintf(out, cap, "i.pos");
    else if (slot < GXP_SLOT_POSITION && ((g->b->slots >> slot) & 1)) snprintf(out, cap, "i.s%u", slot);
    else snprintf(out, cap, "float4(0.0, 0.0, 0.0, 1.0)");
    return out;
}

char *gxp_hlsl(const gxp_program *p, const gxp_hlsl_bind *bind, char *why, size_t why_size) {
    gen g;
    text out;
    memset(&g, 0, sizeof(g));
    memset(&out, 0, sizeof(out));
    g.p = p;
    g.b = bind;
    g.fragment = (p->flags & GXP_FLAG_FRAGMENT) != 0;
    usse_reset(&g.st);
    if (why && why_size) why[0] = '\0';

    int reads_pixel = g.fragment && (p->flags & GXP_FLAG_FRAGCOLOR) && bind->now;
    if (p->undecoded) fail(&g, "%u instructions that could not be read", p->undecoded);
    if (g.fragment && (p->flags & GXP_FLAG_DEPTH)) fail(&g, "a program that replaces the depth");

    /* a store to a secondary attribute makes them all locals */
    for (uint32_t i = 0; i < p->primary_count; i++) {
        const usse_operand *d = &p->primary[i].dest;
        if (d->bank == USSE_SA || d->bank == USSE_INDEXED1 || d->bank == USSE_INDEXED2) g.sa_written = 1;
    }

    /* A branch forward passes over the instructions up to its target: they
     * are put in an `if` that the target closes. */
    uint32_t closes[16];
    int open = 0;
    for (uint32_t i = 0; i <= p->primary_count && !g.failed; i++) {
        while (open && closes[open - 1] == i) { put(&g.body, "    }\n"); open--; }
        if (i == p->primary_count) break;
        const usse_insn *in = &p->primary[i];
        char line[1024];
        usse_state shown = g.st;                    /* the text of an instruction, with the increments as they stand */
        size_t n = usse_disassemble(in, &shown, line, sizeof(line));
        for (size_t at = 0; at < n;) {
            size_t end = at;
            while (end < n && line[end] != '\n') end++;
            put(&g.body, "    // %.*s\n", (int)(end - at), line + at);
            at = end + 1;
        }
        if (in->op == USSE_OP_PHAS || in->op == USSE_OP_NOP || in->op == USSE_OP_SPEC) continue;
        if (in->op == USSE_OP_BR) {
            int64_t target = (int64_t)i + (int32_t)in->imm;
            if ((int32_t)in->imm <= 0) { fail(&g, "a branch backward"); break; }
            if (target > (int64_t)p->primary_count || open == 16 || (open && target > (int64_t)closes[open - 1])) {
                fail(&g, "a branch out of what an earlier branch passes over");
                break;
            }
            switch (in->pred) {
            case USSE_PRED_P0: case USSE_PRED_P1: case USSE_PRED_P2: case USSE_PRED_P3:
                put(&g.body, "    if (!PR%d) {\n", in->pred - USSE_PRED_P0);
                break;
            case USSE_PRED_NOT_P0: put(&g.body, "    if (PR0) {\n"); break;
            case USSE_PRED_NOT_P1: put(&g.body, "    if (PR1) {\n"); break;
            default: put(&g.body, "    if (false) {\n"); break;
            }
            closes[open++] = (uint32_t)target;
            continue;
        }
        switch (in->pred) {
        case USSE_PRED_P0: case USSE_PRED_P1: case USSE_PRED_P2: case USSE_PRED_P3:
            put(&g.body, "    if (PR%d) {\n", in->pred - USSE_PRED_P0);
            break;
        case USSE_PRED_NOT_P0: put(&g.body, "    if (!PR0) {\n"); break;
        case USSE_PRED_NOT_P1: put(&g.body, "    if (!PR1) {\n"); break;
        default: put(&g.body, "    {\n"); break;
        }
        emit(&g, in);
        put(&g.body, "    }\n");
    }

    if (g.failed || g.body.bad) {
        if (why && why_size) snprintf(why, why_size, "%s", g.failed ? g.why : "out of memory");
        free(g.body.s);
        return NULL;
    }

    /* the registers the inputs and the results lie in are named too */
    const char *result = (g.fragment && !(p->flags & GXP_FLAG_NATIVECOLOR)) ? "P" : "O";
    int result_slot = result[0] == 'P' ? 1 : 2;
    if (g.fragment) {
        for (uint32_t i = 0; i < p->input_count; i++) {
            const gxp_input *in = &p->inputs[i];
            if (in->has_iterator && in->iterator_reg + 4 > g.top[1]) g.top[1] = in->iterator_reg + 4;
            if (in->has_texture && in->texture_reg + 4 > g.top[1]) g.top[1] = in->texture_reg + 4;
        }
        if (g.top[result_slot] < 4) g.top[result_slot] = 4;
        if (reads_pixel && g.top[2] < 4) g.top[2] = 4;
    } else {
        if ((int)bind->pa_words > g.top[1]) g.top[1] = (int)bind->pa_words;
        if (g.top[2] < 4) g.top[2] = 4;
        for (unsigned slot = 0; slot < GXP_SLOT_POSITION; slot++)
            if (((bind->slots >> slot) & 1) && p->out_reg[slot] + p->out_count[slot] > g.top[2]) g.top[2] = p->out_reg[slot] + p->out_count[slot];
    }
    for (int k = 0; k < 3; k++) if (g.top[k] > USSE_WORDS) g.top[k] = USSE_WORDS;

    put(&out, "%s", PRELUDE);
    int locked = 0;
    if (g.fragment && bind->larger > 1)
        for (unsigned unit = 0; unit < GXP_TEXTURE_UNITS; unit++) locked |= bind->tex_locked[unit] && bind->tex_point[unit];
    if (locked)
        put(&out, "float4 texel_locked(Texture2D t, float2 uv, float2 pos, int n, bool kept, int across, int down) {\n"
                  "    uint w, h;\n"
                  "    t.GetDimensions(w, h);\n"
                  "    int2 part = int2(pos) %% n, size = int2(w, h) / (kept ? n : 1);\n"
                  "    float2 back = float2(n - 1, n - 1) * 0.5 - float2(part);\n"
                  "    uv += ddx_fine(uv) * back.x + ddy_fine(uv) * back.y;\n"
                  "    bool none = uv.x != uv.x || uv.y != uv.y;\n"
                  "    uv.x = abs(uv.x) > 32768.0 ? 0.0 : uv.x;\n"
                  "    uv.y = abs(uv.y) > 32768.0 ? 0.0 : uv.y;\n"
                  "    float2 p = floor(floor(uv * float2(size) * 256.0 + 0.5) / 256.0);\n"
                  "    int2 texel = int2(wrapped(int(p.x), size.x, across), wrapped(int(p.y), size.y, down));\n"
                  "    float4 found = t.Load(int3(kept ? texel * n + part : texel, 0));\n"
                  "    return none ? float4(0.0, 0.0, 0.0, 0.0) : found;\n"
                  "}\n");
    for (unsigned unit = 0; unit < GXP_TEXTURE_UNITS; unit++) {
        if (!bind->tex_components[unit] || !((p->textures_used >> unit) & 1)) continue;
        put(&out, "%s TEX%u : register(t%u);\nSamplerState SMP%u : register(s%u);\n", bind->tex_cube[unit] ? "Texture2DArray" : "Texture2D",
            unit, unit, unit, unit);
    }
    int ordered = reads_pixel && bind->ordered && bind->now == 1 && !bind->result_words;
    if (ordered) put(&out, "RasterizerOrderedTexture2D<float4> NOW : register(u0);\n");
    else if (reads_pixel) put(&out, "%s NOW : register(t17);\n", bind->now == 2 ? "Texture2D<uint4>" : "Texture2D<float4>");
    static const char *const NAME[5] = { "T", "P", "O", "S", "I" };
    for (int k = 0; k < 3; k++) if (g.top[k]) put(&out, "static uint %s[%d];\n", NAME[k], g.top[k]);
    if (g.sa_written) put(&out, "static uint S[%d];\n", USSE_WORDS);
    if (g.top[4]) put(&out, "static uint I[16];\n");
    put(&out, "static bool PR0, PR1, PR2, PR3;\n");
    if (g.uses_index) put(&out, "static int IX0, IX1;\n");

    if (!g.fragment) {
        put(&out, "struct VIN {\n");
        for (uint32_t k = 0; k < bind->pa_words / 4; k++) put(&out, "    uint4 pa%u : PA%u;\n", k, k);
        if (!bind->pa_words) put(&out, "    uint nothing : PA0;\n");
        put(&out, "};\nstruct VOUT {\n    float4 pos : SV_Position;\n");
        varyings(&out, bind->slots);
        put(&out, "    float beyond : SV_ClipDistance0;\n};\n\nVOUT main(VIN i) {\n");
        for (uint32_t k = 0; k < bind->pa_words; k++) put(&out, "    P[%u] = i.pa%u.%c;\n", k, k / 4, "xyzw"[k & 3]);
    } else {
        put(&out, "struct PIN {\n    float4 pos : SV_Position;\n");
        varyings(&out, bind->slots);
        /* a program that writes the target itself is not run for a pixel the tests turn away */
        if (ordered) put(&out, "};\n\n[earlydepthstencil]\nvoid main(PIN i) {\n    float4 was = NOW[int2(i.pos.xy)];\n");
        else put(&out, "};\n\n%s main(PIN i) : SV_Target {\n", bind->result_words ? "uint4" : "float4");

        /* the primary attributes, from the program's input list */
        char v[64];
        for (uint32_t k = 0; k < p->input_count; k++) {
            const gxp_input *in = &p->inputs[k];
            if (in->has_iterator && in->iterator_reg < USSE_WORDS - 4) {
                put(&out, "    {\n        float4 v = %s;\n", slot_text(&g, in->source, v, sizeof(v)));
                for (unsigned c = 0; c < in->components && c < 4; c++) {
                    char value[64];
                    if (in->type == USSE_F32 || in->type == USSE_F16) snprintf(value, sizeof(value), "v.%c", "xyzw"[c]);
                    else snprintf(value, sizeof(value), "round(unit(v.%c, 0.0) * 255.0)", "xyzw"[c]);
                    put(&out, "    ");
                    pack_line(&out, "P", in->iterator_reg, in->type == USSE_F32 ? USSE_F32 : in->type == USSE_F16 ? USSE_F16 : USSE_U8, c, value);
                }
                put(&out, "    }\n");
            }
            if (in->has_texture && in->texture_reg < USSE_WORDS - 4) {
                unsigned unit = in->unit;
                int bound = unit < GXP_TEXTURE_UNITS && bind->tex_components[unit];
                int n = bound ? bind->tex_components[unit] : 4, reg = in->texture_reg;
                uint8_t type = bound ? bind->tex_type[unit] : USSE_U8;
                if (n < 1 || n > 4) n = 4;
                put(&out, "    {\n");
                if (!bound) {
                    put(&out, "        float4 s = float4(0.0, 0.0, 0.0, 0.0);\n");
                } else {
                    put(&out, "        float4 v = %s;\n", slot_text(&g, in->coord, v, sizeof(v)));
                    char at[32];
                    if (in->projection && !bind->tex_cube[unit]) snprintf(at, sizeof(at), "v.xy / v.%c", in->projection == 1 ? 'w' : 'z');
                    else snprintf(at, sizeof(at), "v.xy");
                    /* a cube is read from its first level unless the code names another: gxm_draw.c's sample_input */
                    if (bind->tex_cube[unit]) put(&out, "        float4 s = TEX%u.SampleLevel(SMP%u, cube_at(v.xyz), 0.0);\n", unit, unit);
                    else if (bind->tex_point[unit] && bind->larger > 1 && bind->tex_locked[unit])
                        put(&out, "        float4 s = texel_locked(TEX%u, %s, i.pos.xy, %d, ((asuint(VP[2].w) >> %u) & 1u) != 0u, %d, %d);\n", unit, at,
                            bind->larger, unit, bind->tex_wrap[unit] & 15, bind->tex_wrap[unit] >> 4);
                    else if (bind->tex_point[unit])
                        put(&out, "        float4 s = texel_at(TEX%u, %s, %d, %d);\n", unit, at, bind->tex_wrap[unit] & 15, bind->tex_wrap[unit] >> 4);
                    else put(&out, "        float4 s = TEX%u.Sample(SMP%u, %s);\n", unit, unit, at);
                }
                /* into the registers in the form the program was compiled to find there */
                if (in->result == 3) {
                    if (in->single) n = 1;
                    for (int c = 0; c < 4; c++) {
                        if (c < n) put(&out, "        P[%d] = asuint(s.%c);\n", reg + c, "xyzw"[c]);
                        else if (!in->single) put(&out, "        P[%d] = 0u;\n", reg + c);
                    }
                } else if (in->result == 0 && bound && type == USSE_F32) {
                    for (int c = 0; c < n; c++) put(&out, "        P[%d] = asuint(s.%c);\n", reg + c, "xyzw"[c]);
                } else if (in->result == 0 && (!bound || type == USSE_U8 || type == USSE_S8)) {
                    put(&out, "        P[%d] = 0u;\n", reg);
                    for (int c = 0; c < n; c++) {
                        char value[64];
                        if (bound && type == USSE_S8) snprintf(value, sizeof(value), "round(clamp(s.%c, -1.0, 1.0) * 127.0)", "xyzw"[c]);
                        else snprintf(value, sizeof(value), "round(unit(s.%c, 0.0) * 255.0)", "xyzw"[c]);
                        put(&out, "    ");
                        pack_line(&out, "P", reg, USSE_U8, (unsigned)c, value);
                    }
                } else {
                    put(&out, "        P[%d] = 0u;\n        P[%d] = 0u;\n", reg, reg + 1);
                    for (int c = 0; c < n; c++) {
                        char value[16];
                        snprintf(value, sizeof(value), "s.%c", "xyzw"[c]);
                        put(&out, "    ");
                        pack_line(&out, "P", reg, USSE_F16, (unsigned)c, value);
                    }
                }
                put(&out, "    }\n");
            }
        }
    }
    if (g.sa_written) put(&out, "    for (int q = 0; q < %d; q++) S[q] = SAv[q >> 2][q & 3];\n", USSE_WORDS);
    if (reads_pixel) {
        /* A program that blends for itself is given the pixel as it stands,
         * in the registers its own result will be left in. */
        if (bind->now == 2) {
            put(&out, "    {\n        uint4 now = NOW.Load(int3(int2(i.pos.xy), 0));\n");
            for (int k = 0; k < 4; k++) {
                if (k < bind->now_words) put(&out, "        O[%d] = now.%c;\n", k, "xyzw"[k]);
                else put(&out, "        O[%d] = 0u;\n", k);
            }
            put(&out, "    }\n");
        } else {
            put(&out, ordered ? "    {\n        float4 now = was;\n" : "    {\n        float4 now = NOW.Load(int3(int2(i.pos.xy), 0));\n");
            if (p->result_type == GXP_F16) {
                put(&out, "        O[0] = h16(now.x) | (h16(now.y) << 16);\n"
                          "        O[1] = h16(now.z) | (h16(now.w) << 16);\n");
            } else if (p->result_type == GXP_F32) {
                for (int k = 0; k < 4; k++) put(&out, "        O[%d] = asuint(now.%c);\n", k, "xyzw"[k]);
            } else {
                put(&out, "        O[0] = 0u;\n");
                for (unsigned k = 0; k < 4; k++) {
                    char value[64];
                    snprintf(value, sizeof(value), "round(unit(now.%c, 0.0) * 255.0)", "xyzw"[k]);
                    put(&out, "    ");
                    pack_line(&out, "O", 0, USSE_U8, k, value);
                }
            }
            put(&out, "    }\n");
        }
    }

    put(&out, "%s", g.body.s ? g.body.s : "");
    free(g.body.s);

    if (!g.fragment) {
        put(&out, "    VOUT o;\n"
                  "    float4 c = float4(asfloat(O[0]), asfloat(O[1]), asfloat(O[2]), asfloat(O[3]));\n"
                  "    // window = offset + scale * (clip / w), on a viewport of the whole target, y down;\n"
                  "    // and a corner goes where the console's rasteriser has it, on a sixteenth of a pixel:\n"
                  "    // the card's own steps are finer, and what is interpolated across a triangle follows its corners\n"
                  "    float2 at = float2(VP[1].x * c.w + VP[0].x * c.x, VP[1].y * c.w + VP[0].y * c.y);\n"
                  "    if (c.w > 0.0) at = floor(at / c.w * 16.0 + 0.5) * (c.w / 16.0);\n"
                  "    o.pos.x = at.x * VP[2].x - c.w;\n"
                  "    o.pos.y = c.w - at.y * VP[2].y;\n"
                  "    o.pos.z = VP[1].z * c.w + VP[0].z * c.z;\n"
                  "    o.pos.w = c.w;\n"
                  "    // cut where the depth passes 1; nearer than the near plane is drawn, at depth 0\n"
                  "    o.beyond = c.w - o.pos.z;\n");
        for (unsigned slot = 0; slot < GXP_SLOT_POSITION; slot++) {
            if (!((bind->slots >> slot) & 1)) continue;
            put(&out, "    o.s%u = float4(", slot);
            for (unsigned k = 0; k < 4; k++) {
                if (k < p->out_count[slot]) put(&out, "asfloat(O[%d])", p->out_reg[slot] + (int)k);
                else put(&out, k == 3 ? "1.0" : "0.0");
                put(&out, k == 3 ? ");\n" : ", ");
            }
        }
        put(&out, "    return o;\n}\n");
    } else if (bind->result_words) {
        put(&out, "    return uint4(");
        for (int k = 0; k < 4; k++) {
            if (k < bind->result_words && k < g.top[result_slot]) put(&out, "%s[%d]", result, k); else put(&out, "0u");
            put(&out, k == 3 ? ");\n}\n" : ", ");
        }
    } else {
        int words = p->result_type == GXP_F32 ? 4 : (p->result_type == GXP_U8 || p->result_type == GXP_S8) ? 1 : 2;
        if (g.top[result_slot] < words) put(&out, "    // (the result's registers were never named)\n");
        put(&out, "    float4 colour;\n");
        for (int k = 0; k < 4; k++) {
            switch (p->result_type) {
            case GXP_F32: put(&out, "    colour.%c = asfloat(%s[%d]);\n", "xyzw"[k], result, k); break;
            case GXP_U8:  put(&out, "    colour.%c = float((%s[0] >> %d) & 0xFFu) / 255.0;\n", "xyzw"[k], result, 8 * k); break;
            case GXP_S8:  put(&out, "    colour.%c = max(float(int(%s[0] << %d) >> 24) / 127.0, -1.0);\n", "xyzw"[k], result, 24 - 8 * k); break;
            case GXP_U16: put(&out, "    colour.%c = float((%s[%d] >> %d) & 0xFFFFu) / 65535.0;\n", "xyzw"[k], result, k >> 1, 16 * (k & 1)); break;
            case GXP_S16: put(&out, "    colour.%c = max(float(int(%s[%d] << %d) >> 16) / 32767.0, -1.0);\n", "xyzw"[k], result, k >> 1, 16 - 16 * (k & 1)); break;
            default:      put(&out, "    colour.%c = f16tof32(%s[%d] >> %d);\n", "xyzw"[k], result, k >> 1, 16 * (k & 1)); break;
            }
        }
        if (bind->clamp) put(&out, "    colour = float4(unit(colour.x, 0.0), unit(colour.y, 0.0), unit(colour.z, 0.0), unit(colour.w, 0.0));\n");
        if (ordered)
            put(&out, "    uint keep = asuint(VP[2].z);\n"
                      "    NOW[int2(i.pos.xy)] = float4((keep & 1u) != 0u ? colour.x : was.x, (keep & 2u) != 0u ? colour.y : was.y,\n"
                      "                                 (keep & 4u) != 0u ? colour.z : was.z, (keep & 8u) != 0u ? colour.w : was.w);\n}\n");
        else put(&out, "    return colour;\n}\n");
    }

    if (out.bad) {
        if (why && why_size) snprintf(why, why_size, "out of memory");
        free(out.s);
        return NULL;
    }
    return out.s;
}
