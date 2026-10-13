/* gpu_tween.c — the frame between two frames. See gpu_tween.h. */

#include "gpu_tween.h"
#include "clock.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- what was asked ------------------------------------------------------------------------- */

static int g_on = -1, g_says;
static unsigned g_only_from, g_only_to = ~0u, g_list_at;   /* VITARECOMP_TWEEN_LISTS=A-B: only the draws of those lists of a frame move */
static int g_still;                         /* nothing is put half way: a frame gone through again should then be the frame */
static int g_after = 1;                     /* whether a fragment program's matrices move */
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
    e = getenv("VITARECOMP_TWEEN_LISTS");
    if (e && sscanf(e, "%u-%u", &g_only_from, &g_only_to) != 2) { g_only_from = 0; g_only_to = ~0u; }
    e = getenv("VITARECOMP_TWEEN_STILL");
    g_still = e && *e && *e != '0';
    e = getenv("VITARECOMP_TWEEN_FRAGMENT");
    if (e && *e) g_after = *e != '0';
    e = getenv("VITARECOMP_TWEEN_LOG");
    g_says = e && *e ? atoi(e) : 0;
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

/* --- the geometry kept on the card ----------------------------------------------------------- */

/* A mesh gxm_gpu.c keeps on the card. Its vertices are there and nowhere
 * else, and a title that writes what moves into two buffers turn and turn
 * about draws a thing from one mesh in this frame and from another in the
 * next: to tell that the two are the same thing, and to draw it half way,
 * its vertices have to be at hand. So those of a small mesh are kept here
 * as it is made. `steady` says the mesh was drawn in the frame before as
 * well: it is then its own like, and nothing of it need be looked at. */
typedef struct {
    gpu_mesh *mesh;
    uint32_t *vertices, *indices;           /* NULL: more than is kept of one */
    uint32_t vertex_count, stride, index_count;
    uint32_t seen;                          /* the frame it was last drawn in */
    uint8_t  steady;
} known;

#define KNOWN_SLOTS    32768u               /* a power of two, several times the meshes gxm_gpu.c keeps */
#define KNOWN_VERTICES 256u                 /* a mesh of more vertices than this is not kept here */
#define KNOWN_BYTES    ((size_t)48 << 20)   /* nor is any, once this much is */
static known *g_known[KNOWN_SLOTS];
static known g_known_gone;                  /* stands where a mesh was: looked past, and used again */
static uint32_t g_known_filled;             /* slots that are not empty */
static size_t g_known_bytes;
static uint32_t g_frame_no = 2;             /* counts the frames written down */

static uint32_t known_slot(const gpu_mesh *m) {
    return (uint32_t)(((uintptr_t)m >> 4) * 2654435761u) & (KNOWN_SLOTS - 1);
}

static known *known_of(const gpu_mesh *m) {
    for (uint32_t at = known_slot(m), tries = 0; tries < KNOWN_SLOTS; tries++, at = (at + 1) & (KNOWN_SLOTS - 1)) {
        if (!g_known[at]) return NULL;
        if (g_known[at]->mesh == m) return g_known[at];
    }
    return NULL;
}

static int known_put(known *k) {
    if (g_known_filled > KNOWN_SLOTS / 4 * 3) {
        /* too many of the slots stand for meshes that have gone: the table is made again of those that have not */
        static known *live[KNOWN_SLOTS];
        uint32_t n = 0;
        for (uint32_t i = 0; i < KNOWN_SLOTS; i++)
            if (g_known[i] && g_known[i] != &g_known_gone) live[n++] = g_known[i];
        memset(g_known, 0, sizeof(g_known));
        g_known_filled = 0;
        if (n > KNOWN_SLOTS / 2) return 1;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t at = known_slot(live[i]->mesh);
            while (g_known[at]) at = (at + 1) & (KNOWN_SLOTS - 1);
            g_known[at] = live[i];
            g_known_filled++;
        }
    }
    uint32_t at = known_slot(k->mesh);
    while (g_known[at] && g_known[at] != &g_known_gone) at = (at + 1) & (KNOWN_SLOTS - 1);
    if (!g_known[at]) g_known_filled++;
    g_known[at] = k;
    return 0;
}

static void known_gone(const gpu_mesh *m) {
    for (uint32_t at = known_slot(m), tries = 0; tries < KNOWN_SLOTS && g_known[at]; tries++, at = (at + 1) & (KNOWN_SLOTS - 1)) {
        known *k = g_known[at];
        if (k->mesh != m) continue;
        if (k->vertices) g_known_bytes -= (size_t)k->vertex_count * k->stride + (size_t)k->index_count * 4;
        free(k->vertices);
        free(k);
        g_known[at] = &g_known_gone;
        return;
    }
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
            known_gone(it);
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

static motion g_motions[512];
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

/* Whether two programs' are the same in all but where the program lies. A
 * title may register one program's bytes more than once, at an address
 * each: the card is given one program for them (gxm_gpu.c knows a program
 * by what it says), and a draw with one is the draw the frame before made
 * with the other. Taken for different, two such draws had nothing to be
 * told apart by, and a light was put half way to the light beside it. */
static int alike(const motion *a, const motion *b) {
    return a == b || (a && b && a->stamp == b->stamp && a->size == b->size);
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
    const motion *moves_after;              /* the same of its fragment program, where one was told of */
    uint8_t  after_buffer[8];
    uint32_t after_first[8], after_words[8];
    uint64_t key;
    int32_t  was;                           /* the draw of the frame before that this one is, or -1 */
    /* its vertices and indices, where they are at hand: its own, or those of a small mesh that was not drawn in the
     * frame before; NULL where they are on the card alone */
    const uint32_t *vertices, *indices;
    uint32_t vertex_count, index_count;
    float    middle[4], size;               /* where those vertices are (placed) */
    uint8_t  steady;                        /* its geometry is on the card, and was drawn from in the frame before */
    float    gap;                           /* how far it is from the draw it was (apart) */
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
static uint32_t g_untold;                   /* and those from geometry on the card that the frame before did not draw from, with no more known of it */
static float g_furthest;                    /* and the furthest a draw was from the one it was taken for */

static struct { const motion *moves; uint8_t buffer[8]; uint32_t first[8], words[8], shape; int said; } g_about;
static struct { const motion *moves; uint8_t buffer[8]; uint32_t first[8], words[8]; } g_about_after;

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

void gpu_tween_about(const gxp_program *vertex, const uint32_t buffers[GXP_BUFFERS], const uint32_t memory[8][4], uint32_t shape) {
    if (!gpu_tween_on()) return;
    g_about.moves = vertex ? motion_of(vertex) : NULL;
    g_about.shape = shape;
    g_about_after.moves = NULL;
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

void gpu_tween_about_fragment(const gxp_program *fragment, const uint32_t buffers[GXP_BUFFERS], const uint32_t memory[8][4]) {
    if (!gpu_tween_on()) return;
    g_about_after.moves = fragment ? motion_of(fragment) : NULL;
    for (int k = 0; k < 8; k++) {
        g_about_after.buffer[k] = 255;
        g_about_after.first[k] = memory ? memory[k][2] : 0;
        g_about_after.words[k] = memory ? memory[k][1] / 4 : 0;
        if (!memory || !memory[k][1] || !buffers) continue;
        for (unsigned b = 0; b < GXP_BUFFERS; b++)
            if (buffers[b] && buffers[b] == memory[k][0]) { g_about_after.buffer[k] = (uint8_t)b; break; }
    }
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

gpu_mesh *tween_mesh_make(const void *vertices, uint32_t vertex_count, uint32_t stride, const uint32_t *indices, uint32_t index_count) {
    gpu_mesh *m = gpu_mesh_make(vertices, vertex_count, stride, indices, index_count);
    if (!m || !gpu_tween_on()) return m;
    known *k = calloc(1, sizeof(*k));
    if (!k) return m;
    size_t vertex_bytes = (size_t)vertex_count * stride, index_bytes = (size_t)index_count * 4;
    k->mesh = m;
    k->vertex_count = vertex_count;
    k->stride = stride;
    k->index_count = index_count;
    k->seen = g_frame_no - 2;
    if (vertex_count && vertex_count <= KNOWN_VERTICES && !(stride & 3) && g_known_bytes + vertex_bytes + index_bytes <= KNOWN_BYTES &&
        (k->vertices = malloc(vertex_bytes + index_bytes)) != NULL) {
        k->indices = (uint32_t *)((uint8_t *)k->vertices + vertex_bytes);
        memcpy(k->vertices, vertices, vertex_bytes);
        memcpy(k->indices, indices, index_bytes);
        g_known_bytes += vertex_bytes + index_bytes;
    }
    if (known_put(k)) {
        if (k->vertices) g_known_bytes -= vertex_bytes + index_bytes;
        free(k->vertices);
        free(k);
    }
    return m;
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

/* What a draw has to share with a draw of the frame before to be the same
 * thing, before anything of the two is looked at (apart): its programs,
 * the form of its geometry and how it is put on the target. Not where its
 * geometry lies: a title that writes what moves into two buffers turn and
 * turn about draws each thing from another place than the frame before,
 * and from the same as the frame before that. */
static uint64_t key_of(const gpu_draw_call *d, uint32_t shape) {
    uint64_t h = 1469598103934665603ull;
#define MIX(v) h = (h ^ (uint64_t)(v)) * 1099511628211ull
    MIX((uintptr_t)d->vertex); MIX((uintptr_t)d->fragment); MIX(shape);
    if (!shape) { MIX(d->mesh != NULL); MIX(d->vertex_count); MIX(d->index_count); }
    MIX(d->vertex_stride); MIX(d->strip);
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
    w->key = key_of(d, said ? g_about.shape : 0);
    if (d->mesh) {
        known *k = known_of(d->mesh);
        w->call.vertices = w->call.indices = NULL;
        if (k && k->seen != g_frame_no) {
            k->steady = k->seen + 1 == g_frame_no;
            k->seen = g_frame_no;
        }
        w->steady = k && k->steady;
        if (k && !k->steady && k->vertices && k->stride == d->vertex_stride) {
            w->vertices = kept(&f->bytes, k->vertices, (size_t)k->vertex_count * k->stride);
            w->indices = kept(&f->bytes, k->indices, (size_t)k->index_count * 4);
            if (w->vertices && w->indices) { w->vertex_count = k->vertex_count; w->index_count = k->index_count; }
            else w->vertices = w->indices = NULL;
        }
    } else {
        w->call.vertices = kept(&f->bytes, d->vertices, (size_t)d->vertex_count * d->vertex_stride);
        w->call.indices = kept(&f->bytes, d->indices, (size_t)d->index_count * d->index_size);
        if (!w->call.vertices || !w->call.indices) f->whole = 0;
        if (d->index_size == 4 && !(d->vertex_stride & 3)) {
            w->vertices = w->call.vertices;
            w->indices = w->call.indices;
            w->vertex_count = d->vertex_count;
            w->index_count = d->index_count;
        }
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
    memset(w->after_buffer, 255, sizeof(w->after_buffer));
    if (said) {
        w->moves = g_about.moves;
        memcpy(w->range_buffer, g_about.buffer, sizeof(w->range_buffer));
        memcpy(w->range_first, g_about.first, sizeof(w->range_first));
        memcpy(w->range_words, g_about.words, sizeof(w->range_words));
        if (g_about_after.moves) {
            w->moves_after = g_about_after.moves;
            memcpy(w->after_buffer, g_about_after.buffer, sizeof(w->after_buffer));
            memcpy(w->after_first, g_about_after.first, sizeof(w->after_first));
            memcpy(w->after_words, g_about_after.words, sizeof(w->after_words));
        }
    } else {
        memset(w->range_buffer, 255, sizeof(w->range_buffer));
    }
    g_about_after.moves = NULL;
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

#define FAR_ENOUGH 0.25f                    /* further than this in a thirtieth of a second is a jump, not a move */

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
static const uint32_t *far_words_of(const drawn *d, int stage, const far_range *r, uint32_t *words) {
    const uint8_t *buffer = stage ? d->after_buffer : d->range_buffer;
    const uint32_t *firsts = stage ? d->after_first : d->range_first, *counts = stage ? d->after_words : d->range_words;
    *words = 0;
    if (!d->call.memory[stage]) return NULL;
    for (int k = 0; k < 8; k++) {
        if (buffer[k] != r->buffer || counts[k] <= r->first) continue;
        uint32_t first = firsts[k] + r->first, n = counts[k] - r->first;
        if (n > r->words) n = r->words;
        if (first >= d->call.memory_words[stage]) return NULL;
        if (first + n > d->call.memory_words[stage]) n = d->call.memory_words[stage] - first;
        *words = n;
        return d->call.memory[stage] + first;
    }
    return NULL;
}

static const uint32_t *far_words(const drawn *d, const far_range *r, uint32_t *words) { return far_words_of(d, 0, r, words); }

/* Whether the vertices of two draws are at hand, and one for one. */
static int vertices_alike(const drawn *now, const drawn *was) {
    return now->vertices && was->vertices && now->vertex_count && now->vertex_count == was->vertex_count &&
           now->call.vertex_stride == was->call.vertex_stride;
}

/* Where a draw's vertices are, taken together: the middle of them, and the
 * length of the box around them, over the words of a vertex that are its
 * place. */
static void placed(drawn *d) {
    const motion *m = d->moves;
    uint32_t stride = d->call.vertex_stride / 4, look = m->pa_words < stride ? m->pa_words : stride, dims = 0;
    float size = 0.0f;
    for (uint32_t k = 0; k < look && dims < 4; k++) {
        if (!m->pa[k]) continue;
        float lo = 0.0f, hi = 0.0f;
        double sum = 0.0;
        uint32_t n = 0;
        for (uint32_t v = 0; v < d->vertex_count; v++) {
            float x;
            memcpy(&x, &d->vertices[(size_t)v * stride + k], 4);
            if (!sane(x)) continue;
            if (!n || x < lo) lo = x;
            if (!n || x > hi) hi = x;
            sum += x;
            n++;
        }
        d->middle[dims++] = n ? (float)(sum / n) : 0.0f;
        size += (hi - lo) * (hi - lo);
    }
    d->size = sqrtf(size);
}

/* How far one vertex is from another, over the words that are its place. */
static float moved_by(const uint32_t *was, const uint32_t *now, const uint8_t *place, uint32_t place_words) {
    float far = 0.0f;
    for (uint32_t k = 0; k < place_words; k++) {
        float x, y;
        if (!place[k]) continue;
        memcpy(&x, &was[k], 4);
        memcpy(&y, &now[k], 4);
        if (sane(x) && sane(y)) far += (x - y) * (x - y);
    }
    return sqrtf(far);
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

/* How far the vertices of a draw have gone, taken together, from those of
 * a draw of the frame before: FAR_ENOUGH at half their own size. A thing
 * does not go further than that in a thirtieth of a second; what lies
 * further off is another thing of the same kind, the next of a row of
 * people's shadows. */
static float shifted(const drawn *now, const drawn *was) {
    float far = 0.0f, size = now->size > was->size ? now->size : was->size;
    for (int k = 0; k < 4; k++) far += (now->middle[k] - was->middle[k]) * (now->middle[k] - was->middle[k]);
    far = sqrtf(far);
    if (far == 0.0f) return 0.0f;
    return size > 0.0f ? 2.0f * FAR_ENOUGH * far / size : 2.0f * FAR_ENOUGH;
}

/* How far a draw is from one of the frame before; more than FAR_ENOUGH
 * when nothing says the two are the same thing. They are when both draw
 * the same geometry on the card, or when the vertices of both are at hand
 * and lie where the others lay. Geometry on the card that is not the
 * other's, and of which no more is known, is another thing: a title draws
 * many things of one form with the same programs, and one that comes into
 * view put half way to another is seen. */
static float apart(const drawn *now, const drawn *was) {
    const motion *m = now->moves;
    float most = 0.0f, d;
    if (!m || !alike(was->moves, m)) return 0.0f;
    if (!now->call.mesh || now->call.mesh != was->call.mesh) {
        if (!vertices_alike(now, was)) return 2.0f * FAR_ENOUGH;
        if (m->pa_words) most = shifted(now, was);
        if (most > FAR_ENOUGH) return most;
    }
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
    return most;
}

#define LOOK 16                             /* how far either side of where a draw is expected its like is looked for */

static int g_telling;                       /* this going-through is one that is told draw by draw */

/* VITARECOMP_TWEEN_LOG=2: why a draw found none of the frame before. */
static void why_not(const frame *f, const frame *b, const listed *o, uint32_t i, uint32_t lo, uint32_t hi, const uint8_t *taken) {
    const drawn *d = &f->draws[i];
    unsigned near = 0, near_taken = 0, in_list = 0, anywhere = 0, programs = 0, mesh = 0;
    float nearest = -1.0f;
    for (uint32_t c = 0; c < b->draw_count; c++) {
        const drawn *w = &b->draws[c];
        int within = c >= o->first && c < o->first + o->count;
        if (within && w->call.vertex == d->call.vertex && w->call.fragment == d->call.fragment) programs++;
        if (within && w->call.mesh == d->call.mesh && d->call.mesh) mesh++;
        if (w->key != d->key) continue;
        anywhere++;
        if (!within) continue;
        in_list++;
        if (c < lo || c >= hi) continue;
        near++;
        if (taken[c]) { near_taken++; continue; }
        float gap = apart(d, w);
        if (nearest < 0.0f || gap < nearest) nearest = gap;
    }
    fprintf(stderr, "tween draw %3u: none: looked at %u to %u of the list at %u (%u draws); the same key %u times in the frame, %u in the list, "
                    "%u near, %u of those taken, the nearest left %.3f away; the same programs %u times in the list, the same mesh %u\n",
            i, lo, hi, o->first, o->count, anywhere, in_list, near, near_taken, nearest, programs, mesh);
}

/* What a draw would be of the frame before, were it asked alone. */
typedef struct { float score, gap; uint32_t draw, lo, hi, expect; int32_t best; } wish;

static int by_score(const void *a, const void *b) {
    const wish *x = a, *y = b;
    if (x->score != y->score) return x->score < y->score ? -1 : 1;
    return x->draw < y->draw ? -1 : x->draw > y->draw;
}

/* The draw of the frame before, of those from `lo` up to `hi` that none
 * has taken, that a draw is likeliest to be: one of its kind (key_of) that
 * it is not far from (apart), the nearest, with the same picture on it and
 * the same place in the list counting for more than a little distance.
 * -1 when there is none. */
static int32_t likeliest(const drawn *d, const frame *b, uint32_t lo, uint32_t hi, uint32_t expect, const uint8_t *taken, float *score,
                         float *gap_of) {
    int32_t best = -1;
    for (uint32_t c = lo; c < hi; c++) {
        const drawn *w = &b->draws[c];
        if ((taken && taken[c]) || w->key != d->key) continue;
        float gap = apart(d, w);
        if (gap > FAR_ENOUGH) continue;
        float s = gap + (w->call.textures[1][0] == d->call.textures[1][0] ? 0.0f : FAR_ENOUGH) +
                  1e-5f * (float)(c > expect ? c - expect : expect - c);
        if (best < 0 || s < *score) { best = (int32_t)c; *score = s; *gap_of = gap; }
    }
    return best;
}

/* For each draw of a frame, the draw of the frame before that it is: of
 * the list in the same place among lists of the same form, the likeliest
 * near the same place in it. The draws that are surest of theirs choose
 * first: one that has just come into view finds among the frame before's
 * only things that are others, and asked in its turn it would take from
 * the thing behind it the draw that thing was. */
static void match(frame *f, const frame *b) {
    static uint8_t *taken;
    static uint32_t taken_cap;
    static wish *wishes;
    static uint32_t wish_cap;
    g_found = 0;
    g_drawn = f->draw_count;
    g_untold = 0;
    g_furthest = 0.0f;
    for (uint32_t i = 0; i < f->draw_count; i++) {
        drawn *d = &f->draws[i];
        d->was = -1;
        if (d->moves && d->moves->pa_words && d->vertices) placed(d);
        if (d->call.mesh && !d->vertices && !d->steady) g_untold++;
    }
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
        uint32_t expect = o->first, end = o->first + o->count, count = 0;
        if (l->count > wish_cap) {
            wish *more = realloc(wishes, (size_t)l->count * sizeof(*more));
            if (!more) return;
            wishes = more;
            wish_cap = l->count;
        }
        /* what each would be, asked alone, and where the next is then looked for */
        for (uint32_t i = l->first; i < l->first + l->count; i++) {
            wish *n = &wishes[count];
            n->draw = i;
            n->expect = expect;
            n->lo = expect > o->first + LOOK ? expect - LOOK : o->first;
            n->hi = expect + LOOK < end ? expect + LOOK : end;
            n->score = n->gap = 0.0f;
            n->best = likeliest(&f->draws[i], b, n->lo, n->hi, expect, NULL, &n->score, &n->gap);
            if (n->best < 0) {
                if (g_telling) why_not(f, b, o, i, n->lo, n->hi, taken);
                /* its like of the frame before went unfound as it did: the next is looked for one further on */
                if (expect < end) expect++;
                continue;
            }
            expect = (uint32_t)n->best + 1;
            count++;
        }
        /* and the surest first: one whose draw another was surer of looks again among what is left */
        qsort(wishes, count, sizeof(*wishes), by_score);
        for (uint32_t k = 0; k < count; k++) {
            wish *n = &wishes[k];
            drawn *d = &f->draws[n->draw];
            if (taken[n->best]) n->best = likeliest(d, b, n->lo, n->hi, n->expect, taken, &n->score, &n->gap);
            if (n->best < 0) {
                if (g_telling) why_not(f, b, o, n->draw, n->lo, n->hi, taken);
                continue;
            }
            d->was = n->best;
            taken[n->best] = 1;
            g_found++;
            d->gap = n->gap;
            if (n->gap > g_furthest) g_furthest = n->gap;
        }
    }
}

/* --- a frame gone through again -------------------------------------------------------------- */

static uint8_t *g_spare[5];
static size_t g_spare_cap[5];

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
    if (g_still || g_list_at < g_only_from || g_list_at > g_only_to) { gpu_draw(&call); return; }
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
        if (m->pa_words && vertices_alike(d, w) && !(d->call.mesh && d->call.mesh == w->call.mesh)) {
            uint32_t stride = call.vertex_stride / 4, look = m->pa_words < stride ? m->pa_words : stride, count = d->vertex_count;
            uint32_t *v = spare(2, d->vertices, (size_t)count * call.vertex_stride);
            if (v) {
                const uint32_t *a = w->vertices;
                uint32_t same = 0, went_half = 0;
                for (uint32_t i = 0; i < count; i++) {
                    const uint32_t *was = a + (size_t)i * stride;
                    uint32_t *now = v + (size_t)i * stride;
                    if (!same_vertex(was, now, m->pa, look, stride)) continue;
                    same++;
                    /* one that went further than half the size of the whole is one that jumped */
                    if (moved_by(was, now, m->pa, look) * 2.0f > d->size) continue;
                    went_half += halved(now, was, m->pa, look);
                }
                if (same < count && count <= 4) {
                    /* one sprite, showing another picture of itself: its vertices are not what they were, and it is
                     * the thing it was all the same (match saw to that). The later frame's shape, put half way back
                     * to where the earlier one was. */
                    uint32_t dim = 0;
                    memcpy(v, d->vertices, (size_t)count * call.vertex_stride);
                    went_half = 0;
                    for (uint32_t k = 0; k < look && dim < 4; k++) {
                        if (!m->pa[k]) continue;
                        float back = 0.5f * (d->middle[dim] - w->middle[dim]);
                        dim++;
                        if (back == 0.0f) continue;
                        for (uint32_t i = 0; i < count; i++) {
                            float x;
                            memcpy(&x, &v[(size_t)i * stride + k], 4);
                            if (!sane(x)) continue;
                            x -= back;
                            memcpy(&v[(size_t)i * stride + k], &x, 4);
                            went_half++;
                        }
                    }
                }
                if (went_half) {
                    call.vertices = v;
                    call.vertex_count = count;
                    if (call.mesh) {
                        /* geometry on the card cannot be put elsewhere: drawn from its vertices here, this once */
                        call.mesh = NULL;
                        call.indices = d->indices;
                        call.index_count = d->index_count;
                        call.index_size = 4;
                    }
                    changed += went_half;
                }
            }
        }
        if (changed) g_moved++;
    }
    /* And the fragment program's matrices, which go half way too unless
     * they went further than a thing goes in a thirtieth of a second: that
     * is a cut, and half way between two cameras is nowhere.
     * VITARECOMP_TWEEN_FRAGMENT=0 leaves a fragment program as the later
     * frame has it. */
    const motion *after = d->moves_after;
    if (g_after && d->was >= 0 && after && alike(b->draws[d->was].moves_after, after)) {
        const drawn *w = &b->draws[d->was];
        const int in_registers = after->sa_words && call.constants[1] && w->call.constants[1] &&
                                 call.constants_bytes[1] >= after->sa_words * 4 && w->call.constants_bytes[1] >= after->sa_words * 4;
        float far = in_registers ? went(w->call.constants[1], call.constants[1], after->sa, after->sa_words) : 0.0f;
        for (uint32_t i = 0; i < after->far_count; i++) {
            uint32_t a_words, b_words;
            const uint32_t *a = far_words_of(w, 1, &after->far[i], &a_words), *now = far_words_of(d, 1, &after->far[i], &b_words);
            if (!a || !now) continue;
            float f = went(a, now, NULL, a_words < b_words ? a_words : b_words);
            if (f > far) far = f;
        }
        if (far > 0.0f && far <= FAR_ENOUGH) {
            if (in_registers) {
                uint32_t *c = spare(3, call.constants[1], call.constants_bytes[1]);
                if (c) {
                    halved(c, w->call.constants[1], after->sa, after->sa_words);
                    call.constants[1] = c;
                }
            }
            if (after->far_count && call.memory[1]) {
                uint32_t *words = spare(4, call.memory[1], (size_t)call.memory_words[1] * 4);
                for (uint32_t i = 0; words && i < after->far_count; i++) {
                    uint32_t a_words, b_words;
                    const uint32_t *a = far_words_of(w, 1, &after->far[i], &a_words), *now = far_words_of(d, 1, &after->far[i], &b_words);
                    if (!a || !now) continue;
                    halved(words + (now - d->call.memory[1]), a, NULL, a_words < b_words ? a_words : b_words);
                }
                if (words) call.memory[1] = words;
            }
        }
    }
    if (g_telling) {
        /* VITARECOMP_TWEEN_LOG=2: a frame now and then, draw by draw: what it was taken for, and what of it differed */
        unsigned differ = 0, may = 0, vertices_differ = 0;
        int first = -1;
        if (d->was >= 0 && call.constants_bytes[0] && d->call.constants[0] && b->draws[d->was].call.constants[0]) {
            const uint32_t *now = d->call.constants[0], *was = b->draws[d->was].call.constants[0];
            for (unsigned k = 0; k < USSE_WORDS && k * 4 < d->call.constants_bytes[0]; k++) {
                if (now[k] == was[k]) continue;
                differ++;
                if (first < 0) first = (int)k;
                if (m && k < m->sa_words && m->sa[k]) may++;
            }
        }
        if (d->was >= 0 && vertices_alike(d, &b->draws[d->was]))
            vertices_differ = memcmp(d->vertices, b->draws[d->was].vertices, (size_t)d->vertex_count * d->call.vertex_stride) != 0;
        fprintf(stderr, "tween draw %3u: was %4d, %s, %u vertices of %u words, %u indices; may move: sa to %u, pa to %u, %u far; "
                        "%u words of its constants differ (%u that may move, the first sa%d)%s%s\n",
                (unsigned)(d - g_frames[g_now].draws), d->was,
                !d->call.mesh ? "own vertices" : d->steady ? "kept mesh" : d->vertices ? "mesh new to the frame" : "mesh new to the frame and large",
                d->vertex_count, d->call.vertex_stride / 4, d->index_count, m ? m->sa_words : 0, m ? m->pa_words : 0, m ? m->far_count : 0,
                differ, may, first, vertices_differ ? ", its vertices differ" : "", call.mesh != d->call.mesh ? ", drawn from its vertices" : "");
        if (d->was >= 0 && d->gap > 0.0f) {
            const drawn *w = &b->draws[d->was];
            fprintf(stderr, "tween draw %3u: %.3f from the one it was; its vertices %.2f large about %.2f %.2f %.2f, the other's %.2f about "
                            "%.2f %.2f %.2f\n", (unsigned)(d - g_frames[g_now].draws), d->gap, d->size, d->middle[0], d->middle[1],
                    d->middle[2], w->size, w->middle[0], w->middle[1], w->middle[2]);
        }
        if (d->was >= 0 && d->call.constants[1] && b->draws[d->was].call.constants[1]) {
            /* and its fragment program's constants: which differ from the draw it was, and which of those may move */
            const uint32_t *now = d->call.constants[1], *was = b->draws[d->was].call.constants[1];
            unsigned differ_after = 0, may_after = 0, memory_differ = 0;
            char which[160] = "";
            int n = 0;
            for (unsigned k = 0; k < USSE_WORDS && k * 4 < d->call.constants_bytes[1] && k * 4 < b->draws[d->was].call.constants_bytes[1]; k++) {
                if (now[k] == was[k]) continue;
                differ_after++;
                if (after && k < after->sa_words && after->sa[k]) may_after++;
                else if (n < 140) n += snprintf(which + n, sizeof(which) - (size_t)n, " sa%u", k);
            }
            if (d->call.memory[1] && b->draws[d->was].call.memory[1]) {
                uint32_t words = d->call.memory_words[1] < b->draws[d->was].call.memory_words[1] ? d->call.memory_words[1]
                                                                                             : b->draws[d->was].call.memory_words[1];
                for (uint32_t k = 0; k < words; k++) memory_differ += d->call.memory[1][k] != b->draws[d->was].call.memory[1][k];
            }
            if (differ_after || memory_differ)
                fprintf(stderr, "tween draw %3u: fragment program %p: %u words of its constants differ, %u that may move (sa to %u, %u far), "
                                "%u words of the memory it reads differ; those that stay:%s\n", (unsigned)(d - g_frames[g_now].draws),
                        (void *)d->call.fragment, differ_after, may_after, after ? after->sa_words : 0, after ? after->far_count : 0,
                        memory_differ, which);
        }
        if (g_says >= 3 && d->was >= 0 && m && m->sa_words && d->call.constants[0] && b->draws[d->was].call.constants[0]) {
            const uint32_t *now = d->call.constants[0], *was = b->draws[d->was].call.constants[0];
            char text[1400];
            int n = 0;
            for (unsigned k = 0; k < m->sa_words && k < 40 && k * 4 < d->call.constants_bytes[0] && n < 1300; k++) {
                float x, y;
                if (!m->sa[k]) continue;
                memcpy(&x, &was[k], 4);
                memcpy(&y, &now[k], 4);
                n += snprintf(text + n, sizeof(text) - (size_t)n, " %u:%g>%g", k, (double)x, (double)y);
            }
            fprintf(stderr, "tween draw %3u: moves:%s\n", (unsigned)(d - g_frames[g_now].draws), text);
        }
        if (g_says >= 3)
            fprintf(stderr, "tween draw %3u: is mesh %p, programs %p %p, texture %p\n", (unsigned)(d - g_frames[g_now].draws),
                    (void *)d->call.mesh, (void *)d->call.vertex, (void *)d->call.fragment, (void *)d->call.textures[1][0]);
    }
    gpu_draw(&call);
}

static void go_through(const frame *f, const frame *b) {
    int listing = 0;
    g_going++;
    g_moved = 0;
    g_list_at = 0;
    if (g_telling) fprintf(stderr, "tween frame %u, draw by draw:\n", g_going);
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
            if (g_telling && g_list_at < f->list_count)
                fprintf(stderr, "tween list %u: %u by %u, format %u, %u draws from %u%s\n", g_list_at, f->lists[g_list_at].width,
                        f->lists[g_list_at].height, f->lists[g_list_at].format, f->lists[g_list_at].count, f->lists[g_list_at].first,
                        s->a ? "" : ", depth alone");
            break;
        case DRAW:
            if (listing) draw_between(b, &f->draws[s->draw]);
            break;
        case END:
            if (listing) gpu_end(NULL, 0);
            listing = 0;
            g_list_at++;
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
    static uint32_t frames, found, moved, drawn_in_all, given_up, untold;
    static float furthest;
    if (!g_says) return;
    frames++;
    found += g_found;
    moved += g_moved;
    drawn_in_all += g_drawn;
    untold += g_untold;
    if (g_furthest > furthest) furthest = g_furthest;
    if (!g_ready) given_up++;
    if (frames < 30) return;
    fprintf(stderr, "gpu tween: %u frames: %u draws a frame, %u of them found in the frame before and %u moved; %u frames not gone "
                    "through; %u pictures shown, %u of them frames between; %u draws a frame from geometry new on the card and too "
                    "large to tell by; the furthest a draw went %.3f\n",
            frames, drawn_in_all / frames, found / frames, moved / frames, given_up, g_pictures[0] + g_pictures[1], g_pictures[1],
            untold / frames, furthest);
    frames = found = moved = drawn_in_all = given_up = untold = 0;
    furthest = 0.0f;
    g_pictures[0] = g_pictures[1] = 0;
}

/* The frame that was being written is whole: gone through, and the next
 * begun. */
/* --- giving way ------------------------------------------------------------------------------
 * A frame between is the title's frame drawn twice, and a card that has
 * not the time for both makes the title wait for it: thirty frames a
 * second become twenty-four, and a title that counts its time in frames
 * runs slow, its sound with it. That is worse than thirty pictures.
 *
 * What a title's own pace is cannot be told by looking: it draws sixty
 * frames a second on one screen and thirty on the next, and fewer while it
 * loads. So it is told by trying. The time the title's frames take is
 * measured, sixty frames together, and their pace is the mean of the
 * fifty-four quickest: a place that loads, a menu that opens or a program
 * compiled is a few long frames and no change of pace, and a card that is
 * behind is behind on most. When the pace is slower than the one last
 * found to be the title's own, no frames between are made for the next
 * sixty, and the two are compared. No faster without them: that is the
 * title's pace now, and they go on. Faster: they were holding it up, and
 * none are made for twenty seconds; then they are tried again, against
 * what the title did without, and each time that fails the rest is twice
 * as long, up to two minutes. A rest also ends when the title's frames
 * are of another make than when it began, sixty running with another
 * number of lists of draws: a menu has closed, a place has been left,
 * and what held the title up there may not here. (Its pace without them
 * says nothing of that: without them it was never held up.)
 *
 * The time is the runtime's clock. On the virtual clock a frame takes the
 * time the title gives it and no more, and what is tried is always found
 * to make no difference. VITARECOMP_TWEEN_GIVE_WAY=0: none of this, and
 * a frame between each two whatever it costs. */

#define PACE_FRAMES 60u                     /* frames whose time is taken together, */
#define PACE_LONG   6u                      /* and how many of them, the longest, are left out of their pace */
#define PACE_SLOWER 1.04f                   /* more than this many times another pace is slower */
#define REST_FIRST  600u                    /* frames */
#define REST_MOST   3600u

static int g_gives_way = -1;
static uint64_t g_frame_at;                 /* when the title's last frame was shown, in the clock's microseconds */
static uint32_t g_pace_frames;              /* of the frames being taken together: how many, */
static uint32_t g_pace_times[PACE_FRAMES];  /* and how long each took */
static float g_pace_own;                    /* microseconds a frame, with frames between, at a pace they were found not to slow; 0, none yet */
static float g_pace_without;                /* and over the last sixty frames that had none */
static uint32_t g_lists_now;                /* how many lists of draws the title's last frame was */
static uint32_t g_rest_lists, g_rest_unlike;    /* the same when the rest in progress began, and frames of the sixty that were not so */
static uint32_t g_rest, g_rest_next = REST_FIRST;   /* frames left with none between; how long the next rest is */
static int g_trying;                        /* the frames between have just come back, and are being held against going without */
static int g_rested;                        /* a rest has just ended: the surfaces beside are behind */

static int by_time(const void *a, const void *b) {
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* The pace of the frames taken together, and the count begun again. */
static float pace_taken(void) {
    uint64_t sum = 0;
    uint32_t n = g_pace_frames > PACE_LONG * 2 ? g_pace_frames - PACE_LONG : g_pace_frames;
    qsort(g_pace_times, g_pace_frames, sizeof(g_pace_times[0]), by_time);
    for (uint32_t i = 0; i < n; i++) sum += g_pace_times[i];
    g_pace_frames = 0;
    return n ? (float)sum / (float)n : 0.0f;
}

static void rest_over(float pace) {
    g_pace_without = pace;
    g_rest = 0;
    g_rested = g_trying = 1;
}

/* The title has shown a frame. */
static void paced(void) {
    if (g_gives_way < 0) { const char *e = getenv("VITARECOMP_TWEEN_GIVE_WAY"); g_gives_way = !(e && *e == '0'); }
    if (!g_gives_way) return;
    const uint64_t now = vita_clock_us(), took = now - g_frame_at;
    const int first = !g_frame_at;
    g_frame_at = now;
    if (first || took > 10000000u) return;          /* the first, or one after a long silence */
    g_pace_times[g_pace_frames++] = (uint32_t)took;
    if (g_pace_frames == 1) g_rest_unlike = 0;
    if (g_rest && g_lists_now != g_rest_lists) g_rest_unlike++;
    if (g_rest && !--g_rest) {
        /* a rest is over: what the title did in the last of it is what the frames between are held against */
        rest_over(pace_taken());
        return;
    }
    if (g_pace_frames < PACE_FRAMES) return;
    const float pace = pace_taken();
    if (g_rest) {
        /* resting: and if none of these sixty frames was of the make the rest began with, the rest was for somewhere else */
        if (g_rest_unlike >= PACE_FRAMES && g_rest > PACE_FRAMES) rest_over(pace);
        return;
    }
    if (g_trying) {
        g_trying = 0;
        if (pace > g_pace_without * PACE_SLOWER) {
            g_rest = g_rest_next;
            g_rest_lists = g_lists_now;
            fprintf(stderr, "gpu tween: the title's frames take %.1f ms with a frame between each two and %.1f without; none are made "
                            "for the next %u frames\n", (double)pace * 1e-3, (double)g_pace_without * 1e-3, g_rest);
            g_rest_next = g_rest_next * 2 < REST_MOST ? g_rest_next * 2 : REST_MOST;
        } else {
            g_pace_own = pace;
            g_rest_next = REST_FIRST;
        }
    } else if (g_pace_own == 0.0f) {
        g_pace_own = pace;                          /* the first pace there is: taken for the title's own until one is slower */
    } else if (pace > g_pace_own * PACE_SLOWER) {
        g_rest = PACE_FRAMES;                       /* slower than it was: sixty frames without, to see whose doing it is */
        g_rest_lists = g_lists_now;
    } else if (pace < g_pace_own) {
        g_pace_own = pace;
    }
}

static void settle(void) {
    frame *f = &g_frames[g_now], *b = &g_frames[!g_now];
    g_ready = 0;
    if (g_listing) return;
    if (f->step_count || !f->whole) {
        g_lists_now = f->list_count;
        if (f->whole && g_rest) {
            /* resting: the frame is kept for the next to be told by, and not gone through */
            g_found = g_moved = 0;
        } else if (f->whole) {
            if (g_rested) { made_alike(); g_rested = 0; }
            g_telling = g_says >= 2 && (g_going + 1) % 90 < (g_says >= 3 ? 4u : 1u);
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
        g_frame_no++;
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
        /* a surface several times the title's size is written at about the title's: each pixel the mean of a square */
        uint32_t by = p->width / 960 ? p->width / 960 : 1, w = p->width / by, h = p->height / by;
        uint8_t *line = malloc((size_t)w * 3);
        fprintf(out, "P6\n%u %u\n255\n", (unsigned)w, (unsigned)h);
        for (uint32_t y = 0; line && y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                uint32_t sum[3] = { 0, 0, 0 };
                for (uint32_t j = 0; j < by; j++)
                    for (uint32_t i = 0; i < by; i++) {
                        const uint8_t *px = rows + ((size_t)(y * by + j) * p->width + x * by + i) * 4;
                        sum[0] += px[0]; sum[1] += px[1]; sum[2] += px[2];
                    }
                for (int c = 0; c < 3; c++) line[x * 3 + c] = (uint8_t)(sum[c] / (by * by));
            }
            fwrite(line, 1, (size_t)w * 3, out);
        }
        free(line);
        fclose(out);
    }
    free(rows);
}

void gpu_tween_pictures(const char *directory, unsigned count) {
    if (!gpu_tween_on() || !directory || !*directory) return;
    snprintf(g_dump_dir, sizeof(g_dump_dir), "%s", directory);
    g_dump = g_dump_dir;
    g_dump_from = g_turn + 1;
    g_dump_to = g_turn + (count ? count : 1);
}

/* VITARECOMP_SHOW_LOG=1: once a second, how the pictures shown were spaced.
 * A count of pictures a second hides what is seen as a hitch: one picture
 * that stood for three vertical blanks among sixty. So each wait between
 * two pictures is counted by how many blanks long it was, and how far the
 * worst of them was off a whole number of blanks is said with it. */
static void spaced(int between) {
    static int asked, wanted;
    static uint64_t last, since;
    static uint32_t pictures, betweens, by_blanks[4];
    static double off_most, longest;
    struct timespec t;
    if (!asked) { const char *e = getenv("VITARECOMP_SHOW_LOG"); asked = 1; wanted = e && *e && *e != '0'; }
    if (!wanted) return;
    timespec_get(&t, TIME_UTC);
    uint64_t now = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
    if (last) {
        double ms = (double)(now - last) * 1e-6, blank = 1000.0 / 60.0;
        int blanks = (int)(ms / blank + 0.5);
        double off = ms - blanks * blank;
        if (off < 0.0) off = -off;
        by_blanks[blanks < 1 ? 0 : blanks > 3 ? 3 : blanks]++;
        if (off > off_most && blanks >= 1 && blanks <= 3) off_most = off;
        if (ms > longest) longest = ms;
    } else {
        since = now;
    }
    last = now;
    pictures++;
    if (between) betweens++;
    if (now - since < 1000000000ull) return;
    fprintf(stderr, "show: %u pictures (%u between) in %.2f s: %u a blank apart, %u two, %u three or more, %u less than one; "
                    "the longest wait %.1f ms, the worst off a blank's time by %.1f ms\n",
            pictures, betweens, (double)(now - since) * 1e-9, by_blanks[1], by_blanks[2], by_blanks[3], by_blanks[0], longest, off_most);
    since = now;
    pictures = betweens = 0;
    memset(by_blanks, 0, sizeof(by_blanks));
    off_most = longest = 0.0;
}

static int shown(void *window, gpu_surface *s, int fresh, int between) {
    if (fresh) spaced(between);
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
    if (!gpu_tween_on() || g_listing) {
        if (fresh) spaced(0);
        return gpu_show(window, s, fresh);
    }
    g_turn++;
    if (fresh && !g_told) settle();         /* nobody says when a frame ends: it ends where it is shown */
    if (fresh) paced();
    if (fresh) {
        const pair *p = s ? pair_of(s) : NULL;
        /* the frame between, when there is one and something in it is not where the title's frame has it */
        gpu_surface *between = g_ready && (g_moved || g_still) && p && p->twin && p->drawn == g_going && !written_since(s) ? p->twin : NULL;
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
