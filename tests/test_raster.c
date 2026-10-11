/* test_raster.c — the software rasteriser's promises.
 *
 * A rasteriser is wrong in ways that look almost right: a seam of pixels
 * drawn twice where two triangles meet, a texture that swims because the
 * interpolation forgot the perspective, a triangle that vanishes when one
 * corner goes behind the eye. Each of those is a rule, and each is here.
 */

#include "../src/raster.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define W 16
#define H 16

static int   g_count[H][W];
static float g_var[H][W][4];
static float g_ddx[H][W][4];
static int   g_cw[H][W];
static int   g_discard_odd;

static int shade(void *user, int x, int y, float depth, int clockwise,
                 const float *var, const float *ddx, const float *ddy) {
    (void)user; (void)depth; (void)ddy;
    assert(x >= 0 && x < W && y >= 0 && y < H);
    if (g_discard_odd && ((x + y) & 1)) return 0;
    g_count[y][x]++;
    g_cw[y][x] = clockwise;
    for (int i = 0; i < 4; i++) { g_var[y][x][i] = var[i]; g_ddx[y][x][i] = ddx[i]; }
    return 1;
}

/* A viewport that maps clip x, y in [0, W] and [0, H] straight to pixels. */
static void setup(raster *r, float *depth) {
    memset(r, 0, sizeof(*r));
    r->width = W; r->height = H;
    r->clip_x1 = W; r->clip_y1 = H;
    r->depth = depth;
    r->x_scale = r->y_scale = r->z_scale = 1.0f;
    r->depth_func = RASTER_ALWAYS;
    r->varyings = 4;
    r->want_derivatives = 1;
    r->shade = shade;
    memset(g_count, 0, sizeof(g_count));
    memset(g_var, 0, sizeof(g_var));
    g_discard_odd = 0;
}

static raster_vertex vtx(float x, float y, float z, float w, float v0, float v1) {
    raster_vertex v;
    memset(&v, 0, sizeof(v));
    v.pos[0] = x * w; v.pos[1] = y * w; v.pos[2] = z * w; v.pos[3] = w;
    v.var[0] = v0; v.var[1] = v1;
    return v;
}

/* Two triangles that share a diagonal cover a square once: no pixel on the
 * shared edge is drawn twice or left out, and none outside is touched. */
static void test_fill_rule(void) {
    raster r;
    setup(&r, NULL);
    raster_vertex a = vtx(2, 2, 0, 1, 0, 0), b = vtx(12, 2, 0, 1, 0, 0);
    raster_vertex c = vtx(12, 12, 0, 1, 0, 0), d = vtx(2, 12, 0, 1, 0, 0);
    raster_triangle(&r, &a, &b, &c);
    raster_triangle(&r, &a, &c, &d);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            assert(g_count[y][x] == ((x >= 2 && x < 12 && y >= 2 && y < 12) ? 1 : 0));
    assert(r.pixels_shaded == 100 && r.triangles_drawn == 2);

    /* A fan around a centre, eight slivers meeting at a point. */
    setup(&r, NULL);
    static const float ring[9][2] = { {0,0}, {8,0}, {16,0}, {16,8}, {16,16}, {8,16}, {0,16}, {0,8}, {0,0} };
    raster_vertex centre = vtx(7.3f, 9.1f, 0, 1, 0, 0);
    for (int i = 0; i < 8; i++) {
        raster_vertex p = vtx(ring[i][0], ring[i][1], 0, 1, 0, 0), q = vtx(ring[i + 1][0], ring[i + 1][1], 0, 1, 0, 0);
        raster_triangle(&r, &centre, &p, &q);
    }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            assert(g_count[y][x] == 1);
    printf("  fill rule                      ok\n");
}

/* With w equal at every corner a varying is a plane: here it is the pixel's
 * own centre, and its change per pixel is one. */
static void test_affine(void) {
    raster r;
    setup(&r, NULL);
    raster_vertex a = vtx(0, 0, 0, 1, 0, 0), b = vtx(16, 0, 0, 1, 16, 0), c = vtx(0, 16, 0, 1, 0, 16);
    raster_triangle(&r, &a, &b, &c);
    int seen = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            if (!g_count[y][x]) continue;
            seen++;
            assert(fabsf(g_var[y][x][0] - (x + 0.5f)) < 1e-4f);
            assert(fabsf(g_var[y][x][1] - (y + 0.5f)) < 1e-4f);
            assert(fabsf(g_ddx[y][x][0] - 1.0f) < 1e-4f && fabsf(g_ddx[y][x][1]) < 1e-4f);
        }
    assert(seen == 120);          /* the pixels above the diagonal; those on it are not this triangle's */
    printf("  affine varyings                ok\n");
}

/* Across an edge from w = 1 to w = 4, a varying that runs 0 to 1 is not
 * half-way at the middle of the screen: the far end is squeezed. */
static void test_perspective(void) {
    raster r;
    setup(&r, NULL);
    raster_vertex a = vtx(0, 0, 0, 1, 0, 0), b = vtx(16, 0, 0, 4, 1, 0), c = vtx(0, 16, 0, 1, 0, 0);
    raster_triangle(&r, &a, &b, &c);
    /* at pixel (7, 0) the centre is 7.5 / 16 of the way along the top edge
     * and half a pixel down it, towards c, which shares a's value and w */
    float lb = 7.5f / 16.0f, lc = 0.5f / 16.0f, la = 1.0f - lb - lc;
    float expect = (lb / 4.0f) / (la + lb / 4.0f + lc);
    assert(g_count[0][7] == 1);
    assert(fabsf(g_var[0][7][0] - expect) < 1e-4f);
    assert(g_var[0][7][0] < 0.25f);
    printf("  perspective                    ok\n");
}

static void test_cull_and_winding(void) {
    raster r;
    raster_vertex a = vtx(1, 1, 0, 1, 0, 0), b = vtx(9, 1, 0, 1, 0, 0), c = vtx(1, 9, 0, 1, 0, 0);
    setup(&r, NULL);
    raster_triangle(&r, &a, &b, &c);                    /* right, then down: clockwise on a y-down screen */
    assert(g_count[2][2] == 1 && g_cw[2][2] == 1);
    setup(&r, NULL);
    raster_triangle(&r, &a, &c, &b);
    assert(g_count[2][2] == 1 && g_cw[2][2] == 0);      /* the other winding covers the same pixels */
    setup(&r, NULL);
    r.cull = RASTER_CULL_CW;
    raster_triangle(&r, &a, &b, &c);
    assert(r.pixels_shaded == 0);
    raster_triangle(&r, &a, &c, &b);
    assert(r.pixels_shaded > 0);
    setup(&r, NULL);
    r.cull = RASTER_CULL_CCW;
    raster_triangle(&r, &a, &c, &b);
    assert(r.pixels_shaded == 0);
    printf("  winding and culling            ok\n");
}

static void test_depth(void) {
    raster r;
    float depth[W * H];
    for (int i = 0; i < W * H; i++) depth[i] = 1.0f;
    setup(&r, depth);
    r.depth_func = RASTER_LESS;
    r.depth_write = 1;
    raster_vertex a = vtx(0, 0, 0.5f, 1, 0, 0), b = vtx(16, 0, 0.5f, 1, 0, 0), c = vtx(0, 16, 0.5f, 1, 0, 0);
    raster_triangle(&r, &a, &b, &c);
    assert(g_count[1][1] == 1 && depth[1 * W + 1] == 0.5f);
    a.pos[2] = b.pos[2] = c.pos[2] = 0.75f;             /* behind: fails */
    raster_triangle(&r, &a, &b, &c);
    assert(g_count[1][1] == 1);
    a.pos[2] = b.pos[2] = c.pos[2] = 0.25f;             /* in front: passes, and a discarded pixel keeps its depth */
    g_discard_odd = 1;
    raster_triangle(&r, &a, &b, &c);
    assert(g_count[2][2] == 2 && depth[2 * W + 2] == 0.25f);
    assert(g_count[1][2] == 1 && depth[1 * W + 2] == 0.5f);
    printf("  depth test                     ok\n");
}

/* A corner behind the eye: the triangle is cut where w runs out, and what
 * is left is drawn inside the target and nowhere else. */
static void test_clipping(void) {
    raster r;
    setup(&r, NULL);
    raster_vertex a = vtx(2, 2, 0, 1, 0, 0), b = vtx(14, 2, 0, 1, 0, 0), c;
    memset(&c, 0, sizeof(c));
    c.pos[0] = 8; c.pos[1] = 30; c.pos[3] = -1;         /* behind */
    raster_triangle(&r, &a, &b, &c);
    assert(r.pixels_shaded > 0);
    assert(g_count[2][8] == 1);

    /* Coordinates far outside any fixed-point range do not crash or wrap. */
    setup(&r, NULL);
    raster_vertex p = vtx(-1e9f, -1e9f, 0, 1, 0, 0), q = vtx(1e9f, -1e9f, 0, 1, 0, 0), s = vtx(8, 1e9f, 0, 1, 0, 0);
    raster_triangle(&r, &p, &q, &s);
    assert(g_count[8][8] == 1 && g_count[0][0] == 1 && g_count[H - 1][W - 1] == 1);

    /* With the depth planes on, what lies beyond z = w is cut away. */
    setup(&r, NULL);
    r.clip_z = 1;
    raster_vertex n = vtx(0, 0, 0, 1, 0, 0), f = vtx(16, 0, 2, 1, 0, 0), g = vtx(0, 16, 0, 1, 0, 0);
    raster_triangle(&r, &n, &f, &g);
    assert(g_count[0][2] == 1 && g_count[0][12] == 0);
    printf("  clipping                       ok\n");
}

int main(void) {
    printf("raster:\n");
    test_fill_rule();
    test_affine();
    test_perspective();
    test_cull_and_winding();
    test_depth();
    test_clipping();
    printf("all raster tests passed\n");
    return 0;
}
