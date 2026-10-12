/* gpu_tween.c — the frame between two frames. See gpu_tween.h. */

#include "gpu_tween.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- what was asked ------------------------------------------------------------------------- */

static int g_on = -1, g_says;
static char g_dump_dir[900];
static const char *g_dump;
static unsigned g_dump_from, g_dump_to = ~0u;   /* the turns of the display whose pictures are written */
static unsigned g_turn;                         /* counts them: one each time a picture is asked to be shown */

int gpu_tween_on(void) {
    if (g_on >= 0) return g_on;
    const char *e = getenv("VITARECOMP_TWEEN");
    g_on = e && *e && *e != '0';
    e = getenv("VITARECOMP_TWEEN_DUMP");
    if (e && *e) {
        /* <directory>, or <directory>@FROM-TO for the pictures of those turns alone */
        snprintf(g_dump_dir, sizeof(g_dump_dir), "%s", e);
        char *at = strrchr(g_dump_dir, '@');
        if (at && sscanf(at + 1, "%u-%u", &g_dump_from, &g_dump_to) == 2) *at = '\0';
        else { g_dump_from = 0; g_dump_to = ~0u; }
        g_dump = g_dump_dir;
    }
    e = getenv("VITARECOMP_TWEEN_LOG");
    g_says = e && *e && *e != '0';
    if (g_on) fprintf(stderr, "gpu tween: a frame is shown between each two of the title's\n");
    return g_on;
}

/* --- the surfaces beside the card's ---------------------------------------------------------- */

/* A surface of the title's on the card, and the one beside it that the
 * frames between are drawn to: made when first wanted, as the first then
 * stands. `gone` when gxm_gpu.c has let go of the surface and what was
 * written down may still name it. */
typedef struct {
    gpu_surface *real, *twin;
    unsigned format;
    uint32_t width, height;
    uint32_t drawn;                         /* the going-through that last put something in the twin */
    int gone;
} pair;

/* The same of a texture that is filled from a surface (gpu_convert). */
typedef struct {
    gpu_texture *real, *twin;
    unsigned width, height;
    int gone;
} blank;

static pair *g_pairs;
static unsigned g_pair_count, g_pair_cap;
static blank *g_blanks;
static unsigned g_blank_count, g_blank_cap;
static uint32_t g_going = 1;                /* counts the times a frame was gone through */

static pair *pair_of(const gpu_surface *s) {
    for (unsigned i = 0; i < g_pair_count; i++)
        if (g_pairs[i].real == s) return &g_pairs[i];
    return NULL;
}

static blank *blank_of(const gpu_texture *t) {
    for (unsigned i = 0; i < g_blank_count; i++)
        if (g_blanks[i].real == t) return &g_blanks[i];
    return NULL;
}

static gpu_surface *beside(gpu_surface *s) {
    pair *p = s ? pair_of(s) : NULL;
    if (!p) return NULL;
    if (!p->twin) {
        p->twin = gpu_surface_make(p->format, p->width, p->height);
        if (p->twin) gpu_surface_copy(p->twin, p->real);
    }
    if (p->twin) p->drawn = g_going;
    return p->twin;
}

/* What gxm_gpu.c has let go of is let go of when nothing written down can
 * name it any more: after the frame it was let go in has been gone
 * through. */
enum { A_SURFACE = 0, A_TEXTURE, A_MESH };
typedef struct { int what; void *it; } leaving;
static leaving *g_leaving;
static unsigned g_leaving_count, g_leaving_cap;

static void leave(int what, void *it) {
    if (!it) return;
    if (g_leaving_count == g_leaving_cap) {
        unsigned cap = g_leaving_cap ? g_leaving_cap * 2 : 64;
        leaving *grown = realloc(g_leaving, cap * sizeof(*grown));
        if (!grown) return;                 /* kept for good, which costs memory and harms nothing */
        g_leaving = grown;
        g_leaving_cap = cap;
    }
    g_leaving[g_leaving_count].what = what;
    g_leaving[g_leaving_count++].it = it;
}

static void left(void) {
    for (unsigned i = 0; i < g_leaving_count; i++) {
        void *it = g_leaving[i].it;
        if (g_leaving[i].what == A_SURFACE) {
            pair *p = pair_of(it);
            if (p) {
                gpu_surface_free(p->twin);
                *p = g_pairs[--g_pair_count];
            }
            gpu_surface_free(it);
        } else if (g_leaving[i].what == A_TEXTURE) {
            blank *k = blank_of(it);
            if (k) {
                if (k->twin) gpu_texture_free(k->twin);
                *k = g_blanks[--g_blank_count];
            }
            gpu_texture_free(it);
        } else {
            gpu_mesh_free(it);
        }
    }
    g_leaving_count = 0;
}

/* --- what moves ------------------------------------------------------------------------------ */

/* Of a vertex program: which of its secondary attributes may be put half
 * way between two frames, which words of a vertex, and which words of the
 * buffers it reads from memory. */
typedef struct { uint8_t buffer; uint32_t first, words; } far_range;

typedef struct {
    const uint8_t *data;                    /* the program it is of, and a number made of its tables */
    uint32_t size;
    uint64_t stamp;
    uint8_t  sa[USSE_WORDS];
    uint32_t sa_words;                      /* one past the last that may move; 0, none does */
    uint8_t  pa[64];
    uint32_t pa_words;
    far_range far[16];
    uint32_t far_count;
} motion;

#define MATRIX_WORDS 12                     /* a uniform of so many floats or more is a matrix, or several */
#define SEMANTIC_POSITION 11

static motion g_motions[128];
static unsigned g_motion_count;

static uint32_t word_at(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static uint64_t stamp_of(const gxp_program *p) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < p->param_count * 16; i++) h = (h ^ p->params[i]) * 1099511628211ull;
    h = (h ^ p->sa_count) * 1099511628211ull;
    h = (h ^ p->block_count) * 1099511628211ull;
    h = (h ^ p->base_count) * 1099511628211ull;
    return h;
}

static const motion *motion_of(const gxp_program *p) {
    uint64_t stamp = stamp_of(p);
    motion *m = NULL;
    for (unsigned i = 0; i < g_motion_count; i++)
        if (g_motions[i].data == p->data && g_motions[i].size == p->size) {
            if (g_motions[i].stamp == stamp) return &g_motions[i];
            m = &g_motions[i];              /* another program where that one was */
            break;
        }
    if (!m) {
        if (g_motion_count == sizeof(g_motions) / sizeof(g_motions[0])) return NULL;
        m = &g_motions[g_motion_count++];
    }
    memset(m, 0, sizeof(*m));
    m->data = p->data;
    m->size = p->size;
    m->stamp = stamp;

    /* what the secondary program works out: the registers nothing else fills */
    uint8_t filled[USSE_WORDS] = { 0 };
    for (uint32_t i = 0; i < p->block_count; i++)
        for (uint32_t w = 0; w < p->blocks[i].words && p->blocks[i].reg + w < USSE_WORDS; w++) filled[p->blocks[i].reg + w] = 1;
    for (uint32_t i = 0; i < p->base_count; i++)
        if (p->bases[i].reg < USSE_WORDS) filled[p->bases[i].reg] = 1;
    for (uint32_t i = 0; i < p->literal_count; i++)
        if (p->literals[i].reg < USSE_WORDS) filled[p->literals[i].reg] = 1;
    for (uint32_t i = 0; i < p->sampler_count; i++)
        if (p->samplers[i].reg < USSE_WORDS) filled[p->samplers[i].reg] = 1;
    if (p->secondary_count)
        for (uint32_t i = 0; i < p->sa_count && i < USSE_WORDS; i++) m->sa[i] = !filled[i];

    int position = -1, first = -1;
    uint32_t position_words = 0, first_words = 0;
    for (uint32_t i = 0; i < p->param_count; i++) {
        const uint8_t *e = p->params + i * 16;
        uint32_t components = (uint32_t)gxp_param_components(e), count = word_at(e + 8), at = word_at(e + 12);
        int container = gxp_param_container(e);
        if (gxp_param_category(e) == GXP_ATTRIBUTE) {
            if (e[6] == SEMANTIC_POSITION && position < 0) { position = (int)at; position_words = components; }
            if (first < 0 || (int)at < first) { first = (int)at; first_words = components; }
            continue;
        }
        if (gxp_param_category(e) != GXP_UNIFORM || gxp_param_type(e) != GXP_F32) continue;
        uint32_t words = components * (count ? count : 1);
        if (words < MATRIX_WORDS) continue;
        int in_registers = 0;
        for (uint32_t k = 0; k < p->block_count; k++) {
            const gxp_block *b = &p->blocks[k];
            if (b->buffer != container) continue;
            in_registers = 1;
            for (uint32_t w = 0; w < words && at + w < b->words && b->reg + at + w < USSE_WORDS; w++) m->sa[b->reg + at + w] = 1;
        }
        if (!in_registers && m->far_count < sizeof(m->far) / sizeof(m->far[0])) {
            m->far[m->far_count].buffer = (uint8_t)container;
            m->far[m->far_count].first = at;
            m->far[m->far_count++].words = words;
        }
    }
    for (uint32_t i = 0; i < USSE_WORDS; i++)
        if (m->sa[i]) m->sa_words = i + 1;
    /* a vertex's position: the attribute that says it is, or the first there is */
    if (position < 0) { position = first; position_words = first_words; }
    if (position >= 0)
        for (uint32_t w = 0; w < (position_words ? position_words : 4) && (uint32_t)position + w < 64; w++) {
            m->pa[(uint32_t)position + w] = 1;
            m->pa_words = (uint32_t)position + w + 1;
        }
    return m;
}

/* --- a frame, written down ------------------------------------------------------------------- */

/* The bytes a frame's draws are kept in: never moved, so what is written
 * down can point into them. */
typedef struct chunk { struct chunk *next; size_t used, size; } chunk;
typedef struct { chunk *first, *at; size_t held; } heap;

#define CHUNK_BYTES ((size_t)4 << 20)
#define HEAP_MOST   ((size_t)768 << 20)     /* more than this for one frame, and the frame is given up */

static void *room(heap *h, size_t bytes) {
    bytes = (bytes + 15) & ~(size_t)15;
    while (h->at && h->at->size - h->at->used < bytes) h->at = h->at->next;
    if (!h->at) {
        size_t size = bytes > CHUNK_BYTES ? bytes : CHUNK_BYTES;
        if (h->held + size > HEAP_MOST) return NULL;
        chunk *c = malloc(sizeof(chunk) + size);
        if (!c) return NULL;
        c->next = NULL;
        c->used = 0;
        c->size = size;
        h->held += size;
        if (!h->first) h->first = c;
        else {
            chunk *last = h->first;
            while (last->next) last = last->next;
            last->next = c;
        }
        h->at = c;
    }
    void *out = (uint8_t *)(h->at + 1) + h->at->used;      /* words: a chunk's own three leave them where words may lie */
    h->at->used += bytes;
    return out;
}

static const void *kept(heap *h, const void *from, size_t bytes) {
    void *to = room(h, bytes ? bytes : 1);
    if (to && bytes) memcpy(to, from, bytes);
    return to;
}

static void emptied(heap *h) {
    for (chunk *c = h->first; c; c = c->next) c->used = 0;
    h->at = h->first;
}

enum { PUT = 0, CLEAR, COPY, CORNER, DEPTH_CORNER, CONVERT, BEGIN, DRAW, END };

typedef struct {
    uint8_t  what, stencil;
    gpu_surface *a, *b;                     /* into and from; a list's colour and depth */
    gpu_texture *texture;
    gpu_shader *program;
    const void *data;                       /* rows, or a program's constants */
    uint32_t bytes, pitch, width, height;
    float    depth;
    uint32_t draw;
} step;

typedef struct {
    gpu_draw_call call;                     /* as it was made, its bytes kept */
    const motion *moves;
    uint8_t  range_buffer[8];               /* each range of its memory words: which buffer, or 255 */
    uint32_t range_first[8], range_words[8];
    uint64_t key;
    int32_t  was;                           /* the draw of the frame before that this one is, or -1 */
} drawn;

typedef struct { uint32_t first, count; unsigned format; uint32_t width, height; } listed;

typedef struct {
    step   *steps;
    uint32_t step_count, step_cap;
    drawn  *draws;
    uint32_t draw_count, draw_cap;
    listed *lists;
    uint32_t list_count, list_cap;
    heap   bytes;
    int    whole;                           /* nothing of it is missing */
} frame;

static frame g_frames[2] = { { .whole = 1 }, { .whole = 1 } };
static int g_now;                           /* the one being written; the other is the frame before */
static int g_before;                        /* and it is one that was written whole */
static int g_listing;                       /* between tween_begin and tween_end */
static int g_told;                          /* gpu_tween_flip has been called: frames end there */
static int g_ready;                         /* the surfaces beside hold the frame between, for the frame now to be shown */
static uint32_t g_moved, g_found, g_drawn;  /* of the frame last gone through: draws that moved, that had a draw before, in all */

static struct { const motion *moves; uint8_t buffer[8]; uint32_t first[8], words[8]; int said; } g_about;

static void *grown(void *list, uint32_t *cap, uint32_t count, size_t each) {
    if (count < *cap) return list;
    uint32_t more = *cap ? *cap * 2 : 256;
    void *to = realloc(list, (size_t)more * each);
    if (to) *cap = more;
    return to;
}

static step *stepped(int what) {
    frame *f = &g_frames[g_now];
    if (!f->whole) return NULL;
    step *list = grown(f->steps, &f->step_cap, f->step_count, sizeof(step));
    if (!list || f->step_count >= f->step_cap) { f->whole = 0; return NULL; }
    f->steps = list;
    step *s = &f->steps[f->step_count++];
    memset(s, 0, sizeof(*s));
    s->what = (uint8_t)what;
    return s;
}

static void frame_emptied(frame *f) {
    f->step_count = f->draw_count = f->list_count = 0;
    f->whole = 1;
    emptied(&f->bytes);
}

void gpu_tween_about(const gxp_program *vertex, const uint32_t buffers[GXP_BUFFERS], const uint32_t memory[8][4]) {
    if (!gpu_tween_on()) return;
    g_about.moves = vertex ? motion_of(vertex) : NULL;
    for (int k = 0; k < 8; k++) {
        g_about.buffer[k] = 255;
        g_about.first[k] = memory ? memory[k][2] : 0;
        g_about.words[k] = memory ? memory[k][1] / 4 : 0;
        if (!memory || !memory[k][1] || !buffers) continue;
        for (unsigned b = 0; b < GXP_BUFFERS; b++)
            if (buffers[b] && buffers[b] == memory[k][0]) { g_about.buffer[k] = (uint8_t)b; break; }
    }
    g_about.said = 1;
}

/* --- the card's functions, written down ------------------------------------------------------ */

gpu_surface *tween_surface_make(unsigned format, uint32_t width, uint32_t height) {
    gpu_surface *s = gpu_surface_make(format, width, height);
    if (!s || !gpu_tween_on()) return s;
    if (g_pair_count == g_pair_cap) {
        unsigned cap = g_pair_cap ? g_pair_cap * 2 : 64;
        pair *more = realloc(g_pairs, cap * sizeof(*more));
        if (!more) return s;                /* a surface with none beside it: its lists are not drawn between */
        g_pairs = more;
        g_pair_cap = cap;
    }
    pair *p = &g_pairs[g_pair_count++];
    memset(p, 0, sizeof(*p));
    p->real = s;
    p->format = format;
    p->width = width;
    p->height = height;
    return s;
}

void tween_surface_free(gpu_surface *s) {
    if (!s) return;
    if (!gpu_tween_on()) { gpu_surface_free(s); return; }
    pair *p = pair_of(s);
    if (p) p->gone = 1;
    leave(A_SURFACE, s);
}

int tween_surface_put(gpu_surface *s, const void *rows, uint32_t pitch) {
    int r = gpu_surface_put(s, rows, pitch);
    if (r || !gpu_tween_on()) return r;
    pair *p = pair_of(s);
    step *n = p ? stepped(PUT) : NULL;
    if (!n) return r;
    n->a = s;
    n->pitch = pitch;
    n->data = kept(&g_frames[g_now].bytes, rows, (size_t)pitch * p->height);
    if (!n->data) g_frames[g_now].whole = 0;
    return r;
}

void tween_surface_clear(gpu_surface *s, float depth, uint8_t stencil) {
    gpu_surface_clear(s, depth, stencil);
    step *n = gpu_tween_on() ? stepped(CLEAR) : NULL;
    if (!n) return;
    n->a = s;
    n->depth = depth;
    n->stencil = stencil;
}

int tween_surface_copy(gpu_surface *into, gpu_surface *from) {
    int r = gpu_surface_copy(into, from);
    step *n = !r && gpu_tween_on() ? stepped(COPY) : NULL;
    if (n) { n->a = into; n->b = from; }
    return r;
}

int tween_surface_copy_corner(gpu_surface *into, gpu_surface *from, uint32_t width, uint32_t height) {
    int r = gpu_surface_copy_corner(into, from, width, height);
    step *n = !r && gpu_tween_on() ? stepped(CORNER) : NULL;
    if (n) { n->a = into; n->b = from; n->width = width; n->height = height; }
    return r;
}

int tween_surface_depth_corner(gpu_surface *into, gpu_surface *from, uint8_t stencil) {
    int r = gpu_surface_depth_corner(into, from, stencil);
    step *n = !r && gpu_tween_on() ? stepped(DEPTH_CORNER) : NULL;
    if (n) { n->a = into; n->b = from; n->stencil = stencil; }
    return r;
}

gpu_texture *tween_texture_blank(unsigned width, unsigned height) {
    gpu_texture *t = gpu_texture_blank(width, height);
    if (!t || !gpu_tween_on()) return t;
    if (g_blank_count == g_blank_cap) {
        unsigned cap = g_blank_cap ? g_blank_cap * 2 : 64;
        blank *more = realloc(g_blanks, cap * sizeof(*more));
        if (!more) return t;
        g_blanks = more;
        g_blank_cap = cap;
    }
    blank *k = &g_blanks[g_blank_count++];
    memset(k, 0, sizeof(*k));
    k->real = t;
    k->width = width;
    k->height = height;
    return t;
}

void tween_texture_free(gpu_texture *t) {
    if (!t) return;
    if (!gpu_tween_on()) { gpu_texture_free(t); return; }
    blank *k = blank_of(t);
    if (k) k->gone = 1;
    leave(A_TEXTURE, t);
}

void tween_mesh_free(gpu_mesh *m) {
    if (!m) return;
    if (!gpu_tween_on()) { gpu_mesh_free(m); return; }
    leave(A_MESH, m);
}

int tween_convert(gpu_texture *into, gpu_surface *from, gpu_shader *program, const void *constants, uint32_t constants_bytes) {
    int r = gpu_convert(into, from, program, constants, constants_bytes);
    step *n = !r && gpu_tween_on() ? stepped(CONVERT) : NULL;
    if (!n) return r;
    n->texture = into;
    n->b = from;
    n->program = program;
    n->bytes = constants_bytes;
    n->data = kept(&g_frames[g_now].bytes, constants, constants_bytes);
    if (!n->data) g_frames[g_now].whole = 0;
    return r;
}

int tween_begin(gpu_surface *colour, gpu_surface *depth) {
    int r = gpu_begin(colour, depth);
    if (r || !gpu_tween_on()) return r;
    frame *f = &g_frames[g_now];
    g_listing = 1;
    step *n = stepped(BEGIN);
    if (!n) return r;
    n->a = colour;
    n->b = depth;
    listed *lists = grown(f->lists, &f->list_cap, f->list_count, sizeof(listed));
    if (!lists || f->list_count >= f->list_cap) { f->whole = 0; return r; }
    f->lists = lists;
    const pair *p = pair_of(colour ? colour : depth);
    listed *l = &f->lists[f->list_count++];
    l->first = f->draw_count;
    l->count = 0;
    l->format = p ? p->format : 0;
    l->width = p ? p->width : 0;
    l->height = p ? p->height : 0;
    return r;
}

static uint64_t key_of(const gpu_draw_call *d) {
    uint64_t h = 1469598103934665603ull;
#define MIX(v) h = (h ^ (uint64_t)(v)) * 1099511628211ull
    MIX((uintptr_t)d->vertex); MIX((uintptr_t)d->fragment); MIX((uintptr_t)d->mesh);
    MIX(d->vertex_count); MIX(d->index_count); MIX(d->vertex_stride); MIX(d->strip);
    MIX(d->constants_bytes[0]); MIX(d->memory_words[0]);
    MIX(d->blend); MIX(d->cull); MIX(d->depth_func | (unsigned)d->depth_write << 4);
#undef MIX
    return h;
}

int tween_draw(const gpu_draw_call *d) {
    int r = gpu_draw(d);
    int said = g_about.said;
    g_about.said = 0;
    if (!gpu_tween_on() || !g_listing) return r;
    frame *f = &g_frames[g_now];
    if (!f->whole) return r;
    drawn *draws = grown(f->draws, &f->draw_cap, f->draw_count, sizeof(drawn));
    if (!draws || f->draw_count >= f->draw_cap) { f->whole = 0; return r; }
    f->draws = draws;
    step *n = stepped(DRAW);
    if (!n) return r;
    n->draw = f->draw_count;
    drawn *w = &f->draws[f->draw_count++];
    memset(w, 0, sizeof(*w));
    w->call = *d;
    w->call.count = 0;
    w->was = -1;
    w->key = key_of(d);
    if (d->mesh) {
        w->call.vertices = w->call.indices = NULL;
    } else {
        w->call.vertices = kept(&f->bytes, d->vertices, (size_t)d->vertex_count * d->vertex_stride);
        w->call.indices = kept(&f->bytes, d->indices, (size_t)d->index_count * d->index_size);
        if (!w->call.vertices || !w->call.indices) f->whole = 0;
    }
    for (int k = 0; k < 2; k++) {
        if (d->constants[k] && d->constants_bytes[k]) {
            w->call.constants[k] = kept(&f->bytes, d->constants[k], d->constants_bytes[k]);
            if (!w->call.constants[k]) f->whole = 0;
        }
        if (d->memory[k] && d->memory_words[k]) {
            w->call.memory[k] = kept(&f->bytes, d->memory[k], (size_t)d->memory_words[k] * 4);
            if (!w->call.memory[k]) f->whole = 0;
        }
    }
    if (said) {
        w->moves = g_about.moves;
        memcpy(w->range_buffer, g_about.buffer, sizeof(w->range_buffer));
        memcpy(w->range_first, g_about.first, sizeof(w->range_first));
        memcpy(w->range_words, g_about.words, sizeof(w->range_words));
    } else {
        memset(w->range_buffer, 255, sizeof(w->range_buffer));
    }
    return r;
}

int tween_end(uint64_t *counts, unsigned max_counts) {
    int r = gpu_end(counts, max_counts);
    if (!gpu_tween_on() || !g_listing) return r;
    frame *f = &g_frames[g_now];
    g_listing = 0;
    if (stepped(END) && f->list_count) f->lists[f->list_count - 1].count = f->draw_count - f->lists[f->list_count - 1].first;
    return r;
}

/* --- which draw is which, and how far it went ------------------------------------------------ */

/* A number that is a place, a length or a part of a matrix, and not a
 * count, an address or a handful of bits read as a float. */
static int sane(float x) {
    float a = fabsf(x);
    return x == x && (a == 0.0f || (a >= 1e-6f && a <= 1e12f));
}

/* How far some words went from one frame to the next, as a part of how
 * large they are: the most of any four of them, a row of a matrix or a
 * place. `may` says which words to look at; NULL, all. */
static float went(const uint32_t *was, const uint32_t *now, const uint8_t *may, uint32_t words) {
    float most = 0.0f;
    for (uint32_t i = 0; i < words; i += 4) {
        float far = 0.0f, size = 0.0f;
        for (uint32_t k = i; k < i + 4 && k < words; k++) {
            float x, y;
            if (may && !may[k]) continue;
            memcpy(&x, &was[k], 4);
            memcpy(&y, &now[k], 4);
            if (!sane(x) || !sane(y)) continue;
            float d = fabsf(x - y), ax = fabsf(x), ay = fabsf(y);
            if (d > far) far = d;
            if (ax > size) size = ax;
            if (ay > size) size = ay;
        }
        if (far > 0.0f && far / (1.0f + size) > most) most = far / (1.0f + size);
    }
    return most;
}

/* The same words half way: `out` holds the later frame's and is given the
 * mean of the two where both are numbers. Returns how many changed. */
static uint32_t halved(uint32_t *out, const uint32_t *was, const uint8_t *may, uint32_t words) {
    uint32_t changed = 0;
    for (uint32_t k = 0; k < words; k++) {
        float x, y;
        if ((may && !may[k]) || was[k] == out[k]) continue;
        memcpy(&x, &was[k], 4);
        memcpy(&y, &out[k], 4);
        if (!sane(x) || !sane(y)) continue;
        x = (x + y) * 0.5f;
        memcpy(&out[k], &x, 4);
        changed++;
    }
    return changed;
}

/* Where a draw's memory words have the words of a matrix it reads from a
 * buffer, and how many of them are there. */
static const uint32_t *far_words(const drawn *d, const far_range *r, uint32_t *words) {
    *words = 0;
    if (!d->call.memory[0]) return NULL;
    for (int k = 0; k < 8; k++) {
        if (d->range_buffer[k] != r->buffer || d->range_words[k] <= r->first) continue;
        uint32_t first = d->range_first[k] + r->first, n = d->range_words[k] - r->first;
        if (n > r->words) n = r->words;
        if (first >= d->call.memory_words[0]) return NULL;
        if (first + n > d->call.memory_words[0]) n = d->call.memory_words[0] - first;
        *words = n;
        return d->call.memory[0] + first;
    }
    return NULL;
}

static int vertices_alike(const drawn *now, const drawn *was) {
    return !now->call.mesh && !was->call.mesh && now->call.vertex_count == was->call.vertex_count &&
           now->call.vertex_stride == was->call.vertex_stride && now->call.vertices && was->call.vertices;
}

/* Whether a vertex is the vertex it was: everything of it but its place is
 * what it was, its texture coordinates and its colour to the bit. A row of
 * letters that gains a letter keeps its count of vertices and hands each
 * place on to the next letter: such a vertex is another one, and is drawn
 * where the later frame has it. */
static int same_vertex(const uint32_t *was, const uint32_t *now, const uint8_t *place, uint32_t place_words, uint32_t stride) {
    for (uint32_t k = 0; k < stride; k++)
        if (!(k < place_words && place[k]) && was[k] != now[k]) return 0;
    return 1;
}

#define FAR_ENOUGH 0.25f                    /* further than this in a thirtieth of a second is a jump, not a move */

/* How far a draw is from one of the frame before. */
static float apart(const drawn *now, const drawn *was) {
    const motion *m = now->moves;
    float most = 0.0f, d;
    if (!m || was->moves != m) return 0.0f;
    if (m->sa_words && now->call.constants[0] && was->call.constants[0] && now->call.constants_bytes[0] >= m->sa_words * 4 &&
        was->call.constants_bytes[0] >= m->sa_words * 4) {
        d = went(was->call.constants[0], now->call.constants[0], m->sa, m->sa_words);
        if (d > most) most = d;
    }
    for (uint32_t i = 0; i < m->far_count; i++) {
        uint32_t a_words, b_words;
        const uint32_t *a = far_words(was, &m->far[i], &a_words), *b = far_words(now, &m->far[i], &b_words);
        if (!a || !b) continue;
        d = went(a, b, NULL, a_words < b_words ? a_words : b_words);
        if (d > most) most = d;
    }
    if (m->pa_words && vertices_alike(now, was)) {
        uint32_t stride = now->call.vertex_stride / 4, look = m->pa_words < stride ? m->pa_words : stride;
        const uint32_t *a = was->call.vertices, *b = now->call.vertices;
        for (uint32_t v = 0; v < now->call.vertex_count && most <= FAR_ENOUGH; v++) {
            if (!same_vertex(a + (size_t)v * stride, b + (size_t)v * stride, m->pa, look, stride)) continue;
            d = went(a + (size_t)v * stride, b + (size_t)v * stride, m->pa, look);
            if (d > most) most = d;
        }
    }
    return most;
}

#define LOOK 16                             /* how far either side of where a draw is expected its like is looked for */

/* For each draw of a frame, the draw of the frame before that it is: of
 * the list in the same place among lists of the same form, a draw with the
 * same programs and geometry near the same place in it, the nearest in
 * what it holds, and with the same texture first. */
static void match(frame *f, const frame *b) {
    static uint8_t *taken;
    static uint32_t taken_cap;
    g_found = 0;
    g_drawn = f->draw_count;
    for (uint32_t i = 0; i < f->draw_count; i++) f->draws[i].was = -1;
    if (!b || !b->draw_count) return;
    if (b->draw_count > taken_cap) {
        uint8_t *more = realloc(taken, b->draw_count);
        if (!more) return;
        taken = more;
        taken_cap = b->draw_count;
    }
    memset(taken, 0, b->draw_count);
    for (uint32_t li = 0; li < f->list_count; li++) {
        const listed *l = &f->lists[li], *o = NULL;
        uint32_t turn = 0, seen = 0;
        for (uint32_t k = 0; k < li; k++)
            if (f->lists[k].format == l->format && f->lists[k].width == l->width && f->lists[k].height == l->height) turn++;
        for (uint32_t k = 0; k < b->list_count && !o; k++) {
            const listed *c = &b->lists[k];
            if (c->format != l->format || c->width != l->width || c->height != l->height) continue;
            if (seen++ == turn) o = c;
        }
        if (!o) continue;
        uint32_t expect = o->first, end = o->first + o->count;
        for (uint32_t i = l->first; i < l->first + l->count; i++) {
            drawn *d = &f->draws[i];
            uint32_t lo = expect > o->first + LOOK ? expect - LOOK : o->first, hi = expect + LOOK < end ? expect + LOOK : end;
            int32_t best = -1;
            float best_score = 0.0f;
            for (uint32_t c = lo; c < hi; c++) {
                const drawn *w = &b->draws[c];
                if (taken[c] || w->key != d->key) continue;
                float gap = apart(d, w);
                if (gap > FAR_ENOUGH) continue;
                /* the same picture on it counts for more than a little distance, and so does the same place in the list */
                float score = gap + (w->call.textures[1][0] == d->call.textures[1][0] ? 0.0f : FAR_ENOUGH) +
                              1e-5f * (float)(c > expect ? c - expect : expect - c);
                if (best < 0 || score < best_score) { best = (int32_t)c; best_score = score; }
            }
            if (best < 0) continue;
            d->was = best;
            taken[best] = 1;
            expect = (uint32_t)best + 1;
            g_found++;
        }
    }
}

/* --- a frame gone through again -------------------------------------------------------------- */

static uint8_t *g_spare[3];
static size_t g_spare_cap[3];

static void *spare(int which, const void *from, size_t bytes) {
    if (bytes > g_spare_cap[which]) {
        uint8_t *more = realloc(g_spare[which], bytes + 64);
        if (!more) return NULL;
        g_spare[which] = more;
        g_spare_cap[which] = bytes + 64;
    }
    memcpy(g_spare[which], from, bytes);
    return g_spare[which];
}

static void draw_between(const frame *b, const drawn *d) {
    gpu_draw_call call = d->call;
    /* a texture that was made from a surface: the one made from the surface beside it */
    for (int stage = 0; stage < 2; stage++)
        for (int unit = 0; unit < 16; unit++) {
            const blank *k = call.textures[stage][unit] ? blank_of(call.textures[stage][unit]) : NULL;
            if (k && k->twin) call.textures[stage][unit] = k->twin;
        }
    const motion *m = d->moves;
    if (d->was >= 0 && m) {
        const drawn *w = &b->draws[d->was];
        uint32_t changed = 0;
        if (m->sa_words && call.constants[0] && w->call.constants[0] && call.constants_bytes[0] >= m->sa_words * 4 &&
            w->call.constants_bytes[0] >= m->sa_words * 4) {
            uint32_t *c = spare(0, call.constants[0], call.constants_bytes[0]);
            if (c) {
                changed += halved(c, w->call.constants[0], m->sa, m->sa_words);
                call.constants[0] = c;
            }
        }
        if (m->far_count && call.memory[0]) {
            uint32_t *words = spare(1, call.memory[0], (size_t)call.memory_words[0] * 4);
            for (uint32_t i = 0; words && i < m->far_count; i++) {
                uint32_t a_words, b_words;
                const uint32_t *a = far_words(w, &m->far[i], &a_words), *now = far_words(d, &m->far[i], &b_words);
                if (!a || !now) continue;
                changed += halved(words + (now - d->call.memory[0]), a, NULL, a_words < b_words ? a_words : b_words);
            }
            if (words) call.memory[0] = words;
        }
        if (m->pa_words && vertices_alike(d, w)) {
            uint32_t stride = call.vertex_stride / 4, look = m->pa_words < stride ? m->pa_words : stride;
            uint32_t *v = spare(2, call.vertices, (size_t)call.vertex_count * call.vertex_stride);
            if (v) {
                const uint32_t *a = w->call.vertices;
                for (uint32_t i = 0; i < call.vertex_count; i++)
                    if (same_vertex(a + (size_t)i * stride, v + (size_t)i * stride, m->pa, look, stride))
                        changed += halved(v + (size_t)i * stride, a + (size_t)i * stride, m->pa, look);
                call.vertices = v;
            }
        }
        if (changed) g_moved++;
    }
    gpu_draw(&call);
}

static void go_through(const frame *f, const frame *b) {
    int listing = 0;
    g_going++;
    g_moved = 0;
    for (uint32_t i = 0; i < f->step_count; i++) {
        const step *s = &f->steps[i];
        gpu_surface *into, *from;
        switch (s->what) {
        case PUT:
            if ((into = beside(s->a)) != NULL) gpu_surface_put(into, s->data, s->pitch);
            break;
        case CLEAR:
            if ((into = beside(s->a)) != NULL) gpu_surface_clear(into, s->depth, s->stencil);
            break;
        case COPY:
        case CORNER:
        case DEPTH_CORNER:
            from = beside(s->b);
            into = beside(s->a);
            if (!into || !from) break;
            if (s->what == COPY) gpu_surface_copy(into, from);
            else if (s->what == CORNER) gpu_surface_copy_corner(into, from, s->width, s->height);
            else gpu_surface_depth_corner(into, from, s->stencil);
            break;
        case CONVERT: {
            blank *k = blank_of(s->texture);
            from = beside(s->b);
            if (!k || !from) break;
            if (!k->twin) k->twin = gpu_texture_blank(k->width, k->height);
            if (k->twin && gpu_convert(k->twin, from, s->program, s->data, s->bytes)) {
                gpu_texture_free(k->twin);
                k->twin = NULL;
            }
            break;
        }
        case BEGIN:
            into = s->a ? beside(s->a) : NULL;
            from = beside(s->b);
            listing = from && (!s->a || into) && !gpu_begin(into, from);
            break;
        case DRAW:
            if (listing) draw_between(b, &f->draws[s->draw]);
            break;
        case END:
            if (listing) gpu_end(NULL, 0);
            listing = 0;
            break;
        }
    }
    if (listing) gpu_end(NULL, 0);
}

/* A frame that could not be written down whole is not gone through: the
 * surfaces beside are made what the card's are, and what was made from
 * them is made again when it is next wanted. */
static void made_alike(void) {
    for (unsigned i = 0; i < g_pair_count; i++)
        if (g_pairs[i].twin && !g_pairs[i].gone) gpu_surface_copy(g_pairs[i].twin, g_pairs[i].real);
    for (unsigned i = 0; i < g_blank_count; i++)
        if (g_blanks[i].twin) {
            gpu_texture_free(g_blanks[i].twin);
            g_blanks[i].twin = NULL;
        }
}

static uint32_t g_pictures[2];              /* shown since it was last said: the title's own frames, frames between */

static void said(void) {
    static uint32_t frames, found, moved, drawn_in_all, given_up;
    if (!g_says) return;
    frames++;
    found += g_found;
    moved += g_moved;
    drawn_in_all += g_drawn;
    if (!g_ready) given_up++;
    if (frames < 30) return;
    fprintf(stderr, "gpu tween: %u frames: %u draws a frame, %u of them found in the frame before and %u moved; %u frames not gone "
                    "through; %u pictures shown, %u of them frames between\n",
            frames, drawn_in_all / frames, found / frames, moved / frames, given_up, g_pictures[0] + g_pictures[1], g_pictures[1]);
    frames = found = moved = drawn_in_all = given_up = 0;
    g_pictures[0] = g_pictures[1] = 0;
}

/* The frame that was being written is whole: gone through, and the next
 * begun. */
static void settle(void) {
    frame *f = &g_frames[g_now], *b = &g_frames[!g_now];
    g_ready = 0;
    if (g_listing) return;
    if (f->step_count || !f->whole) {
        if (f->whole) {
            match(f, g_before ? b : NULL);
            go_through(f, b);
            g_ready = 1;
        } else {
            static int told;
            if (!told) fprintf(stderr, "gpu tween: a frame was more than is kept of one; it is shown as the title drew it\n");
            told = 1;
            made_alike();
            g_found = g_moved = 0;
        }
        said();
        g_before = f->whole;
        g_now = !g_now;
        frame_emptied(&g_frames[g_now]);
    }
    left();
}

void gpu_tween_flip(void) {
    if (!gpu_tween_on()) return;
    g_told = 1;
    settle();
}

/* --- the pictures shown ---------------------------------------------------------------------- */

static void dump(gpu_surface *s, int between) {
    const pair *p = NULL;
    if (g_turn < g_dump_from || g_turn > g_dump_to) return;
    for (unsigned i = 0; i < g_pair_count && !p; i++)
        if (g_pairs[i].real == s || g_pairs[i].twin == s) p = &g_pairs[i];
    if (!p || (p->format != GPU_RGBA8 && p->format != GPU_RGBA8_SRGB)) return;
    uint8_t *rows = malloc((size_t)p->width * p->height * 4);
    char path[1024];
    snprintf(path, sizeof(path), "%s/tween-%06u-%c.ppm", g_dump, g_turn, between ? 't' : 'r');
    FILE *out = rows && !gpu_surface_get(s, rows, p->width * 4) ? fopen(path, "wb") : NULL;
    if (out) {
        fprintf(out, "P6\n%u %u\n255\n", (unsigned)p->width, (unsigned)p->height);
        for (size_t i = 0, n = (size_t)p->width * p->height; i < n; i++) fwrite(rows + i * 4, 1, 3, out);
        fclose(out);
    }
    free(rows);
}

static int shown(void *window, gpu_surface *s, int fresh, int between) {
    if (fresh) g_pictures[between ? 1 : 0]++;
    if (fresh && g_dump && s) dump(s, between);
    return window ? gpu_show(window, s, fresh) : 0;
}

/* Whether something written down since the last frame ended puts
 * something in a surface: the surface beside it is then behind. */
static int written_since(const gpu_surface *s) {
    const frame *f = &g_frames[g_now];
    for (uint32_t i = 0; i < f->step_count; i++)
        if (f->steps[i].a == s && f->steps[i].what != CONVERT) return 1;
    return 0;
}

int tween_show(void *window, gpu_surface *s, int fresh) {
    static gpu_surface *owed;               /* the title's frame, to follow the frame between that was shown for it */
    if (!gpu_tween_on() || g_listing) return gpu_show(window, s, fresh);
    g_turn++;
    if (fresh && !g_told) settle();         /* nobody says when a frame ends: it ends where it is shown */
    if (fresh) {
        const pair *p = s ? pair_of(s) : NULL;
        /* the frame between, when there is one and something in it is not where the title's frame has it */
        gpu_surface *between = g_ready && g_moved && p && p->twin && p->drawn == g_going && !written_since(s) ? p->twin : NULL;
        g_ready = 0;
        owed = between ? s : NULL;
        return shown(window, between ? between : s, 1, between != NULL);
    }
    if (owed && owed == s) {
        owed = NULL;
        return shown(window, s, 1, 0);
    }
    return shown(window, s, 0, 0);
}
