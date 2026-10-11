/* raster.c — triangles to pixels, in software. See raster.h. */

#include "raster.h"

#include <math.h>
#include <string.h>

#define W_MIN      1e-6f
#define GUARD      4096.0f          /* clip x and y to this many viewport widths, so fixed point holds */
#define SUB        16               /* sub-pixel steps per pixel */
#define POLY_MAX   16

/* --- clipping -------------------------------------------------------------------
 * Sutherland-Hodgman against one plane at a time, in clip space, where the
 * planes are linear in the vertex and the varyings interpolate linearly. */

enum { P_W = 0, P_Z_NEAR, P_Z_FAR, P_LEFT, P_RIGHT, P_TOP, P_BOTTOM };

static float distance(const raster_vertex *v, int plane) {
    switch (plane) {
    case P_W:      return v->pos[3] - W_MIN;
    case P_Z_NEAR: return v->pos[2];
    case P_Z_FAR:  return v->pos[3] - v->pos[2];
    case P_LEFT:   return v->pos[0] + GUARD * v->pos[3];
    case P_RIGHT:  return GUARD * v->pos[3] - v->pos[0];
    case P_TOP:    return v->pos[1] + GUARD * v->pos[3];
    default:       return GUARD * v->pos[3] - v->pos[1];
    }
}

static void lerp(raster_vertex *out, const raster_vertex *a, const raster_vertex *b, float t, int varyings) {
    for (int i = 0; i < 4; i++) out->pos[i] = a->pos[i] + (b->pos[i] - a->pos[i]) * t;
    for (int i = 0; i < varyings; i++) out->var[i] = a->var[i] + (b->var[i] - a->var[i]) * t;
}

static int clip(const raster_vertex *in, int n, raster_vertex *out, int plane, int varyings) {
    int m = 0;
    for (int i = 0; i < n && m < POLY_MAX - 1; i++) {
        const raster_vertex *a = &in[i], *b = &in[(i + 1) % n];
        float da = distance(a, plane), db = distance(b, plane);
        if (da >= 0) out[m++] = *a;
        if ((da >= 0) != (db >= 0)) lerp(&out[m++], a, b, da / (da - db), varyings);
    }
    return m;
}

/* --- one triangle in window space --------------------------------------------- */

typedef struct { float x, y, z, q; const float *var; } screen_vertex;

static int depth_passes(int func, float incoming, float stored) {
    switch (func) {
    case RASTER_NEVER:         return 0;
    case RASTER_LESS:          return incoming <  stored;
    case RASTER_EQUAL:         return incoming == stored;
    case RASTER_LESS_EQUAL:    return incoming <= stored;
    case RASTER_GREATER:       return incoming >  stored;
    case RASTER_NOT_EQUAL:     return incoming != stored;
    case RASTER_GREATER_EQUAL: return incoming >= stored;
    default:                   return 1;
    }
}

/* Whether a pixel exactly on the edge from (x0, y0) to (x1, y1) belongs to
 * the triangle: top edges and left edges do, for a triangle wound clockwise
 * with y down. */
static int owns_edge(int64_t x0, int64_t y0, int64_t x1, int64_t y1) {
    int64_t dx = x1 - x0, dy = y1 - y0;
    return dy < 0 || (dy == 0 && dx > 0);
}

static int64_t floor_div(int64_t a, int64_t b) {       /* b > 0 */
    int64_t q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}

static void fill(raster *r, const screen_vertex *a, const screen_vertex *b, const screen_vertex *c) {
    int64_t ax = (int64_t)llroundf(a->x * SUB), ay = (int64_t)llroundf(a->y * SUB);
    int64_t bx = (int64_t)llroundf(b->x * SUB), by = (int64_t)llroundf(b->y * SUB);
    int64_t cx = (int64_t)llroundf(c->x * SUB), cy = (int64_t)llroundf(c->y * SUB);

    int64_t area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (area == 0) return;
    int clockwise = area > 0;
    if ((r->cull == RASTER_CULL_CW && clockwise) || (r->cull == RASTER_CULL_CCW && !clockwise)) return;
    if (!clockwise) {
        const screen_vertex *t = b; b = c; c = t;
        int64_t tx = bx, ty = by; bx = cx; by = cy; cx = tx; cy = ty;
        area = -area;
    }

    int64_t min_x = ax < bx ? (ax < cx ? ax : cx) : (bx < cx ? bx : cx);
    int64_t max_x = ax > bx ? (ax > cx ? ax : cx) : (bx > cx ? bx : cx);
    int64_t min_y = ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy);
    int64_t max_y = ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy);
    /* pixels whose centre (x + 1/2, y + 1/2) lies in the box */
    int64_t px0 = floor_div(min_x - SUB / 2 + SUB - 1, SUB), px1 = floor_div(max_x - SUB / 2, SUB);
    int64_t py0 = floor_div(min_y - SUB / 2 + SUB - 1, SUB), py1 = floor_div(max_y - SUB / 2, SUB);
    if (px0 < r->clip_x0) px0 = r->clip_x0;
    if (py0 < r->clip_y0) py0 = r->clip_y0;
    if (px1 >= r->clip_x1) px1 = r->clip_x1 - 1;
    if (py1 >= r->clip_y1) py1 = r->clip_y1 - 1;
    if (px0 > px1 || py0 > py1) return;
    r->triangles_drawn++;

    /* Edge functions at the first pixel's centre, positive inside. A pixel
     * on an edge the triangle does not own is pushed just outside. */
    int64_t X = px0 * SUB + SUB / 2, Y = py0 * SUB + SUB / 2;
    int64_t bias_ab = owns_edge(ax, ay, bx, by) ? 0 : 1;
    int64_t bias_bc = owns_edge(bx, by, cx, cy) ? 0 : 1;
    int64_t bias_ca = owns_edge(cx, cy, ax, ay) ? 0 : 1;
    int64_t e_ab = (bx - ax) * (Y - ay) - (by - ay) * (X - ax) - bias_ab;
    int64_t e_bc = (cx - bx) * (Y - by) - (cy - by) * (X - bx) - bias_bc;
    int64_t e_ca = (ax - cx) * (Y - cy) - (ay - cy) * (X - cx) - bias_ca;
    int64_t ab_dx = -(by - ay) * SUB, ab_dy = (bx - ax) * SUB;
    int64_t bc_dx = -(cy - by) * SUB, bc_dy = (cx - bx) * SUB;
    int64_t ca_dx = -(ay - cy) * SUB, ca_dy = (ax - cx) * SUB;

    /* Everything interpolated is a value at a, plus its differences to b and
     * c weighted by their barycentrics; the varyings as value over w, which
     * is what is linear on the screen. */
    int   nvar = r->varyings;
    float inv_area = 1.0f / (float)area;
    float qa = a->q, qb = b->q - a->q, qc = c->q - a->q;
    float za = a->z, zb = b->z - a->z, zc = c->z - a->z;
    float ua[RASTER_MAX_VARYINGS], ub[RASTER_MAX_VARYINGS], uc[RASTER_MAX_VARYINGS];
    for (int i = 0; i < nvar; i++) {
        ua[i] = a->var[i] * a->q;
        ub[i] = b->var[i] * b->q - ua[i];
        uc[i] = c->var[i] * c->q - ua[i];
    }
    /* how the barycentrics of b and c change per pixel */
    float lb_dx = (float)ca_dx * inv_area, lb_dy = (float)ca_dy * inv_area;
    float lc_dx = (float)ab_dx * inv_area, lc_dy = (float)ab_dy * inv_area;
    float q_dx = qb * lb_dx + qc * lc_dx, q_dy = qb * lb_dy + qc * lc_dy;
    float u_dx[RASTER_MAX_VARYINGS], u_dy[RASTER_MAX_VARYINGS];
    if (r->want_derivatives)
        for (int i = 0; i < nvar; i++) {
            u_dx[i] = ub[i] * lb_dx + uc[i] * lc_dx;
            u_dy[i] = ub[i] * lb_dy + uc[i] * lc_dy;
        }

    float var[RASTER_MAX_VARYINGS], ddx[RASTER_MAX_VARYINGS], ddy[RASTER_MAX_VARYINGS];
    for (int64_t y = py0; y <= py1; y++) {
        int64_t w_ab = e_ab, w_bc = e_bc, w_ca = e_ca;
        float *depth_row = r->depth ? r->depth + (size_t)y * (size_t)r->width : NULL;
        for (int64_t x = px0; x <= px1; x++, w_ab += ab_dx, w_bc += bc_dx, w_ca += ca_dx) {
            if ((w_ab | w_bc | w_ca) < 0) continue;
            /* the weights of b and c, from the edges opposite them, without the bias */
            float lb = (float)(w_ca + bias_ca) * inv_area;
            float lc = (float)(w_ab + bias_ab) * inv_area;
            float z = za + zb * lb + zc * lc;
            if (depth_row && !depth_passes(r->depth_func, z, depth_row[x])) continue;
            float q = qa + qb * lb + qc * lc;
            float w = 1.0f / q;
            for (int i = 0; i < nvar; i++) var[i] = (ua[i] + ub[i] * lb + uc[i] * lc) * w;
            if (r->want_derivatives)
                for (int i = 0; i < nvar; i++) {
                    ddx[i] = (u_dx[i] - var[i] * q_dx) * w;
                    ddy[i] = (u_dy[i] - var[i] * q_dy) * w;
                }
            r->pixels_shaded++;
            if (r->shade && !r->shade(r->user, (int)x, (int)y, z, clockwise, var, ddx, ddy)) continue;
            if (depth_row && r->depth_write) depth_row[x] = z;
        }
        e_ab += ab_dy; e_bc += bc_dy; e_ca += ca_dy;
    }
}

void raster_triangle(raster *r, const raster_vertex *a, const raster_vertex *b, const raster_vertex *c) {
    raster_vertex poly[2][POLY_MAX];
    int n = 3, cur = 0, varyings = r->varyings;
    if (varyings > RASTER_MAX_VARYINGS) varyings = r->varyings = RASTER_MAX_VARYINGS;
    r->triangles_in++;
    poly[0][0] = *a; poly[0][1] = *b; poly[0][2] = *c;

    /* Always against w, so the divide is safe; against the depth planes
     * when asked; against the guard band only if a vertex is beyond it. */
    int planes[7], np = 0;
    planes[np++] = P_W;
    if (r->clip_z) { planes[np++] = P_Z_NEAR; planes[np++] = P_Z_FAR; }
    for (int i = 0; i < 3; i++) {
        const float *p = poly[0][i].pos;
        float lim = GUARD * (p[3] > 0 ? p[3] : -p[3]);
        if (p[0] < -lim || p[0] > lim || p[1] < -lim || p[1] > lim || p[3] <= W_MIN) {
            planes[np++] = P_LEFT; planes[np++] = P_RIGHT; planes[np++] = P_TOP; planes[np++] = P_BOTTOM;
            break;
        }
    }
    for (int k = 0; k < np && n >= 3; k++) {
        int inside = 0;
        for (int i = 0; i < n; i++) inside += distance(&poly[cur][i], planes[k]) >= 0;
        if (inside == n) continue;
        if (inside == 0) return;
        n = clip(poly[cur], n, poly[cur ^ 1], planes[k], varyings);
        cur ^= 1;
    }
    if (n < 3) return;

    screen_vertex sv[POLY_MAX];
    for (int i = 0; i < n; i++) {
        const raster_vertex *v = &poly[cur][i];
        float q = 1.0f / v->pos[3];
        sv[i].x = r->x_offset + r->x_scale * v->pos[0] * q;
        sv[i].y = r->y_offset + r->y_scale * v->pos[1] * q;
        sv[i].z = r->z_offset + r->z_scale * v->pos[2] * q;
        sv[i].q = q;
        sv[i].var = v->var;
    }
    for (int i = 1; i + 1 < n; i++) fill(r, &sv[0], &sv[i], &sv[i + 1]);
}
