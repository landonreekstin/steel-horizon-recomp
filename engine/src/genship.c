/* Generated ship models (design; no game assets). Proportions are typical of WWII
 * warships relative to the hull's half-length L and half-beam B: freeboard, draft,
 * sheer, flare, superstructure heights. Placement of the bridge, funnels and after
 * superstructure follows the class's turret mounts, so the model agrees with the
 * simulation's layout. */
#include "genship.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { P_BOOT, P_HULL, P_DECK, P_UPPER, P_DARK, P_GUN, P_COUNT };
static const float COLOURS[P_COUNT][3] = {
    {0.45f, 0.17f, 0.14f},   /* boot-topping / anti-fouling below the waterline */
    {0.50f, 0.53f, 0.57f},   /* hull grey */
    {0.50f, 0.44f, 0.35f},   /* teak deck */
    {0.60f, 0.62f, 0.66f},   /* superstructure */
    {0.13f, 0.13f, 0.14f},   /* funnel caps, masts */
    {0.48f, 0.50f, 0.54f},   /* turret and barrels */
};

static void grow(gen_part *p, int nv, int ni)
{
    if (p->nv + nv > p->cap_v) {
        p->cap_v = (p->nv + nv) * 2;
        p->v = realloc(p->v, sizeof(vertex) * (size_t)p->cap_v);
    }
    if (p->ni + ni > p->cap_i) {
        p->cap_i = (p->ni + ni) * 2;
        p->idx = realloc(p->idx, sizeof(uint32_t) * (size_t)p->cap_i);
    }
}

/* A flat quad (a, b, c, d in order around it). */
static void quad(gen_part *p, const float *a, const float *b, const float *c, const float *d)
{
    float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {d[0] - a[0], d[1] - a[1], d[2] - a[2]};
    float n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (l < 1e-9f) l = 1;
    grow(p, 4, 6);
    const float *pts[4] = {a, b, c, d};
    for (int k = 0; k < 4; k++) {
        vertex *vx = &p->v[p->nv + k];
        memcpy(vx->pos, pts[k], sizeof vx->pos);
        for (int q = 0; q < 3; q++) vx->normal[q] = n[q] / l;
        vx->uv[0] = vx->uv[1] = 0;
    }
    static const int t[6] = {0, 1, 2, 0, 2, 3};
    for (int k = 0; k < 6; k++) p->idx[p->ni + k] = (uint32_t)(p->nv + t[k]);
    p->nv += 4;
    p->ni += 6;
}

static void box(gen_part *p, float x0, float y0, float z0, float x1, float y1, float z1)
{
    float c[8][3] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0},
                     {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}};
    quad(p, c[0], c[1], c[2], c[3]);
    quad(p, c[5], c[4], c[7], c[6]);
    quad(p, c[4], c[0], c[3], c[7]);
    quad(p, c[1], c[5], c[6], c[2]);
    quad(p, c[3], c[2], c[6], c[7]);
    quad(p, c[4], c[5], c[1], c[0]);
}

/* An upright n-sided prism (funnels, masts): centre (x, z), radii rx, rz. */
static void prism(gen_part *p, float x, float z, float rx, float rz, float y0, float y1, int n)
{
    for (int k = 0; k < n; k++) {
        float a0 = 6.2831853f * (float)k / (float)n, a1 = 6.2831853f * (float)(k + 1) / (float)n;
        float p0[3] = {x + cosf(a0) * rx, y0, z + sinf(a0) * rz}, p1[3] = {x + cosf(a1) * rx, y0, z + sinf(a1) * rz};
        float p2[3] = {p1[0], y1, p1[2]}, p3[3] = {p0[0], y1, p0[2]};
        quad(p, p0, p1, p2, p3);
        float c[3] = {x, y1, z};
        quad(p, p3, p2, c, c);
    }
}

static int is_big(int h) { return h == 2 || h == 3 || h == 14 || h == 17 || h == 20 || h == 21; }
static int is_light(int h) { return h == 6 || h == 7 || h == 15 || h == 10; }

typedef struct { float freeboard, draft, sheer, transom, flare; } hull_form;

static hull_form form_for(const ship_class *c)
{
    int h = c->hull_type;
    if (is_big(h)) return (hull_form){0.55f, 0.65f, 0.6f, 0.65f, 0.05f};
    if (h == 1) return (hull_form){0.95f, 0.6f, 0.3f, 0.75f, 0.12f};
    if (h == 5) return (hull_form){0.6f, 0.6f, 0.7f, 0.6f, 0.05f};
    if (is_light(h)) return (hull_form){0.7f, 0.55f, 0.9f, 0.55f, 0.06f};
    if (h == 13) return (hull_form){0.6f, 0.4f, 0.4f, 0.8f, 0.1f};
    return (hull_form){0.75f, 0.6f, 0.5f, 0.75f, 0.04f};      /* auxiliaries: full-bodied */
}

/* The lofted hull: 48 stations, each a polyline keel -> bilge -> waterline -> deck
 * edge, mirrored; quads between stations; a teak deck; a transom at the stern. */
static void hull(gen_model *m, const ship_class *c, float L, float B)
{
    hull_form f = form_for(c);
    enum { S = 48, K = 6 };
    float sec[S + 1][K][3];
    for (int i = 0; i <= S; i++) {
        float u = -1 + 2 * (float)i / (float)S, z = u * L;
        float b = u > 0 ? B * powf(fmaxf(0, 1 - powf(u, 2.2f)), 0.6f) : B * (1 - (1 - f.transom) * powf(-u, 3));
        float bd = b * (1 + f.flare) + (u > 0.9f ? 0 : B * 0.02f);
        float fb = B * f.freeboard * (1 + f.sheer * fmaxf(u, 0) * fmaxf(u, 0) + 0.15f * fmaxf(-u, 0) * fmaxf(-u, 0));
        float d = B * f.draft * (1 - 0.35f * powf(fabsf(u), 8));
        float pts[K][2] = {{0, -d}, {0.55f * b, -0.92f * d}, {0.9f * b, -0.6f * d}, {b, 0}, {(b + bd) / 2, fb * 0.5f}, {bd, fb}};
        for (int k = 0; k < K; k++) { sec[i][k][0] = pts[k][0]; sec[i][k][1] = pts[k][1]; sec[i][k][2] = z; }
    }
    for (int i = 0; i < S; i++)
        for (int k = 0; k < K - 1; k++)
            for (int side = -1; side <= 1; side += 2) {
                float a[3] = {side * sec[i][k][0], sec[i][k][1], sec[i][k][2]};
                float b[3] = {side * sec[i + 1][k][0], sec[i + 1][k][1], sec[i + 1][k][2]};
                float cc[3] = {side * sec[i + 1][k + 1][0], sec[i + 1][k + 1][1], sec[i + 1][k + 1][2]};
                float d[3] = {side * sec[i][k + 1][0], sec[i][k + 1][1], sec[i][k + 1][2]};
                gen_part *p = &m->part[k < 3 ? P_BOOT : P_HULL];
                quad(p, a, b, cc, d);
            }
    for (int i = 0; i < S; i++) {                        /* deck */
        float a[3] = {-sec[i][K - 1][0], sec[i][K - 1][1], sec[i][K - 1][2]};
        float b[3] = {sec[i][K - 1][0], sec[i][K - 1][1], sec[i][K - 1][2]};
        float cc[3] = {sec[i + 1][K - 1][0], sec[i + 1][K - 1][1], sec[i + 1][K - 1][2]};
        float d[3] = {-sec[i + 1][K - 1][0], sec[i + 1][K - 1][1], sec[i + 1][K - 1][2]};
        quad(&m->part[P_DECK], a, b, cc, d);
    }
    for (int k = 0; k < K - 1; k++) {                    /* transom */
        float a[3] = {-sec[0][k][0], sec[0][k][1], sec[0][k][2]}, b[3] = {sec[0][k][0], sec[0][k][1], sec[0][k][2]};
        float cc[3] = {sec[0][k + 1][0], sec[0][k + 1][1], sec[0][k + 1][2]};
        float d[3] = {-sec[0][k + 1][0], sec[0][k + 1][1], sec[0][k + 1][2]};
        quad(&m->part[k < 3 ? P_BOOT : P_HULL], a, b, cc, d);
    }
}

static float deck_at(const ship_class *c, float z, float L, float B)
{
    hull_form f = form_for(c);
    float u = z / L;
    return B * f.freeboard * (1 + f.sheer * fmaxf(u, 0) * fmaxf(u, 0) + 0.15f * fmaxf(-u, 0) * fmaxf(-u, 0));
}

static void submarine(gen_model *m, float L, float B, float zt)   /* zt: conning tower centre */
{
    /* A cigar: round sections, mostly awash; a conning tower amidships forward. */
    enum { S = 32, K = 10 };
    for (int i = 0; i < S; i++)
        for (int k = 0; k < K; k++) {
            float u0 = -1 + 2 * (float)i / (float)S, u1 = -1 + 2 * (float)(i + 1) / (float)S;
            float r0 = B * 0.95f * powf(fmaxf(0, 1 - powf(fabsf(u0), 3)), 0.5f), r1 = B * 0.95f * powf(fmaxf(0, 1 - powf(fabsf(u1), 3)), 0.5f);
            float a0 = 6.2831853f * (float)k / (float)K, a1 = 6.2831853f * (float)(k + 1) / (float)K;
            float yc = -0.35f * B;
            float p0[3] = {cosf(a0) * r0, yc + sinf(a0) * r0, u0 * L}, p1[3] = {cosf(a0) * r1, yc + sinf(a0) * r1, u1 * L};
            float p2[3] = {cosf(a1) * r1, yc + sinf(a1) * r1, u1 * L}, p3[3] = {cosf(a1) * r0, yc + sinf(a1) * r0, u0 * L};
            quad(&m->part[sinf((a0 + a1) / 2) < -0.2f ? P_BOOT : P_HULL], p0, p1, p2, p3);
        }
    box(&m->part[P_HULL], -0.3f * B, 0.3f * B, zt - 0.125f * L, 0.3f * B, 1.5f * B, zt + 0.125f * L);
    box(&m->part[P_DARK], -0.05f * B, 1.5f * B, zt - 0.025f * L, 0.05f * B, 2.4f * B, zt - 0.005f * L);   /* periscopes */
}

/* The real ship's layout (standalone classes): each feature where her profile has it. */
static void from_plan(gen_model *m, const ship_class *c, float L, float B)
{
    const ship_plan *pl = &c->plan;
    gen_part *up = &m->part[P_UPPER], *dark = &m->part[P_DARK];
    int h = c->hull_type;
    if (h == 1) {                                        /* carrier: flight deck, island to starboard */
        float fd = deck_at(c, 0, L, B) + 0.45f * B;
        box(&m->part[P_DECK], -1.2f * B, fd - 0.15f * B, -0.97f * L, 1.2f * B, fd, 0.98f * L);
        box(up, -1.2f * B, deck_at(c, 0, L, B), -0.9f * L, 1.2f * B, fd - 0.15f * B, 0.9f * L);
        float z0 = pl->island_z0, z1 = pl->island_z1;
        if (z1 <= z0) { z0 = -0.05f * L; z1 = 0.18f * L; }
        box(up, 0.85f * B, fd, z0, 1.15f * B, fd + 1.0f * B, z1);
        box(up, 0.9f * B, fd + 1.0f * B, z0 + 0.25f * (z1 - z0), 1.12f * B, fd + 1.45f * B, z1 - 0.15f * (z1 - z0));
        for (int k = 0; k < pl->funnels; k++)            /* the funnel trunked into the island */
            box(up, 0.88f * B, fd + 0.6f * B, pl->funnel_z[k] - 0.035f * L, 1.13f * B, fd + pl->funnel_h[k], pl->funnel_z[k] + 0.035f * L);
        if (pl->mast_h > 0)
            box(dark, 0.98f * B, fd, pl->mast_z - 0.004f * L, 1.02f * B, fd + pl->mast_h, pl->mast_z + 0.004f * L);
        return;
    }
    float d0 = deck_at(c, 0, L, B), th = pl->tower_h, zt = pl->tower_z;
    if (th > 0) {
        if (pl->tower == TOWER_PAGODA) {
            /* Many stepped platforms, each narrower, on a tall core (Japanese style). */
            box(up, -0.35f * B, d0, zt - 0.03f * L, 0.35f * B, d0 + th, zt + 0.03f * L);
            for (int lv = 0; lv < 7; lv++) {
                float y = d0 + th * (0.15f + 0.12f * (float)lv), w = B * (0.9f - 0.09f * (float)lv), l = L * (0.08f - 0.007f * (float)lv);
                box(up, -w, y, zt - l / 2, w, y + th * 0.05f, zt + l / 2);
            }
            box(up, -0.5f * B, d0 + th * 0.97f, zt - 0.02f * L, 0.5f * B, d0 + th * 1.05f, zt + 0.02f * L);   /* rangefinder */
        } else {
            /* Bridge block, then a narrower fire-control tower on top. */
            int lv_n = th > 15 ? 4 : th > 6 ? 3 : 2;
            float base = th * (lv_n == 4 ? 0.45f : 0.75f);
            for (int lv = 0; lv < lv_n; lv++) {
                float w = B * (0.8f - 0.12f * (float)lv), y0 = d0 + base * (float)lv / (float)lv_n, y1 = d0 + base * (float)(lv + 1) / (float)lv_n;
                float len = L * (0.11f - 0.015f * (float)lv) * (h == 13 ? 1.6f : 1);
                box(up, -w, y0, zt - len * 0.7f, w, y1, zt + len * 0.3f);
            }
            if (lv_n == 4) {
                box(up, -0.32f * B, d0 + base, zt - 0.03f * L, 0.32f * B, d0 + th * 0.8f, zt + 0.025f * L);
                box(up, -0.22f * B, d0 + th * 0.8f, zt - 0.02f * L, 0.22f * B, d0 + th * 0.92f, zt + 0.015f * L);
                box(up, -0.55f * B, d0 + th * 0.92f, zt - 0.02f * L, 0.55f * B, d0 + th, zt + 0.02f * L);   /* director */
            }
        }
        if (h != 13)                                     /* foremast */
            box(dark, -0.03f * B, d0 + th, zt - 0.004f * L, 0.03f * B, d0 + th * 1.35f + 2, zt + 0.004f * L);
    }
    for (int k = 0; k < pl->funnels; k++) {
        float rx = B * (is_big(h) ? 0.3f : 0.24f), rz = L * (is_big(h) ? 0.04f : 0.035f), top = d0 + pl->funnel_h[k];
        prism(up, 0, pl->funnel_z[k], rx, rz, d0, top, 10);
        prism(dark, 0, pl->funnel_z[k], rx * 1.02f, rz * 1.02f, top, top + 0.12f * B, 10);
    }
    if (pl->aft_h > 0) {
        box(up, -0.65f * B, d0, pl->aft_z0, 0.65f * B, d0 + pl->aft_h, pl->aft_z1);
        box(up, -0.35f * B, d0 + pl->aft_h, pl->aft_z0 + 0.3f * (pl->aft_z1 - pl->aft_z0), 0.35f * B, d0 + pl->aft_h * 1.5f,
            pl->aft_z1 - 0.2f * (pl->aft_z1 - pl->aft_z0));
    }
    if (pl->mast_h > 0)
        box(dark, -0.035f * B, d0, pl->mast_z - 0.005f * L, 0.035f * B, d0 + pl->mast_h, pl->mast_z + 0.005f * L);
}

void genship_build(const ship_class *c, gen_model *out)
{
    memset(out, 0, sizeof *out);
    out->count = P_COUNT;
    for (int p = 0; p < P_COUNT; p++) memcpy(out->part[p].color, COLOURS[p], sizeof COLOURS[p]);
    float L = c->half_length > 1 ? c->half_length : 60, B = c->half_beam > 0.5f ? c->half_beam : L / 8;
    int h = c->hull_type;
    if (c->submerged) {
        submarine(out, L, B, c->plan.has && c->plan.tower_h > 0 ? c->plan.tower_z : 0.175f * L);
        return;
    }
    hull(out, c, L, B);
    if (c->plan.has) { from_plan(out, c, L, B); return; }
    /* Where the guns are: the superstructure goes between the fore and aft groups. */
    float fore = L * 0.45f, aft = -L * 0.45f;
    int nf = 0, na = 0;
    for (int k = 0; k < c->mount_count; k++) {
        if (!c->mounts[k].name[0]) continue;
        float z = c->mounts[k].pos[2];
        if (z > 0) { fore = nf ? fminf(fore, z) : z; nf++; }
        else { aft = na ? fmaxf(aft, z) : z; na++; }
    }
    float mid0 = aft + 0.12f * L, mid1 = fore - 0.1f * L;   /* the clear stretch amidships */
    if (mid1 - mid0 < 0.2f * L) { mid0 = -0.2f * L; mid1 = 0.25f * L; }
    gen_part *up = &out->part[P_UPPER], *dark = &out->part[P_DARK];
    if (h == 1) {
        /* Carrier: a flight deck over the whole hull, the island to starboard. */
        float fd = deck_at(c, 0, L, B) + 0.45f * B;
        box(&out->part[P_DECK], -1.2f * B, fd - 0.15f * B, -0.97f * L, 1.2f * B, fd, 0.98f * L);
        box(up, -1.2f * B, deck_at(c, 0, L, B), -0.9f * L, 1.2f * B, fd - 0.15f * B, 0.9f * L);
        box(up, 0.85f * B, fd, -0.05f * L, 1.15f * B, fd + 1.3f * B, 0.18f * L);
        prism(up, B, 0.02f * L, 0.18f * B, 0.06f * L, fd + 1.3f * B, fd + 1.9f * B, 8);
        box(dark, 0.98f * B, fd + 1.3f * B, 0.12f * L, 1.02f * B, fd + 2.6f * B, 0.135f * L);
        return;
    }
    float d0 = deck_at(c, (mid0 + mid1) / 2, L, B);
    float tower = is_big(h) ? 2.3f : h == 5 ? 1.9f : is_light(h) ? 1.5f : h == 13 ? 0.9f : 1.3f;
    float zb = mid1 - 0.06f * L;                         /* bridge just abaft the fore guns */
    if (h == 4 || h == 8 || h == 9 || h == 11 || h == 18) zb = -0.55f * L;   /* merchant-style: aft */
    /* Bridge and tower: stacked, narrowing levels; a pole mast on top. */
    for (int lv = 0; lv < 4; lv++) {
        float w = B * (0.85f - 0.15f * (float)lv), y0 = d0 + B * tower * (float)lv / 4, y1 = d0 + B * tower * (float)(lv + 1) / 4;
        float len = L * (0.16f - 0.025f * (float)lv) * (h == 13 ? 0.6f : 1);
        box(up, -w, y0, zb - len / 2, w, y1, zb + len / 2);
    }
    box(dark, -0.04f * B, d0 + B * tower, zb - 0.01f * L, 0.04f * B, d0 + B * (tower + 1.1f), zb + 0.01f * L);
    if (h == 13) return;                                 /* PT boat: just the cabin */
    /* Funnels abaft the bridge, then the after superstructure and mainmast. */
    int funnels = h == 7 || h == 10 || h == 4 || h == 8 || h == 9 || h == 11 ? 1 : 2;
    for (int k = 0; k < funnels; k++) {
        float zf = zb - 0.12f * L - (float)k * 0.11f * L;
        if (zb < 0) zf = zb + 0.12f * L;                 /* merchant: funnel ahead of the bridge block */
        float rx = B * (is_big(h) ? 0.28f : 0.22f), rz = L * 0.035f, top = d0 + B * (is_big(h) ? 1.8f : 1.4f);
        prism(up, 0, zf, rx, rz, d0, top, 10);
        prism(dark, 0, zf, rx * 1.02f, rz * 1.02f, top, top + 0.15f * B, 10);
    }
    float za = zb - 0.12f * L - (float)funnels * 0.11f * L - 0.06f * L;
    if (za > aft + 0.06f * L && zb > 0) {
        box(up, -0.6f * B, d0, za - 0.06f * L, 0.6f * B, d0 + 0.7f * B, za + 0.05f * L);
        box(dark, -0.035f * B, d0 + 0.7f * B, za - 0.008f * L, 0.035f * B, d0 + 1.9f * B, za + 0.008f * L);
    }
    if (zb < 0)                                          /* merchant: kingposts by the holds */
        for (int k = 0; k < 2; k++) box(dark, -0.05f * B, deck_at(c, 0, L, B), 0.2f * L + 0.3f * L * (float)k - 0.01f * L,
                                        0.05f * B, deck_at(c, 0, L, B) + 1.4f * B, 0.2f * L + 0.3f * L * (float)k + 0.01f * L);
}

void genship_gun(const ship_class *c, gen_model *out)
{
    memset(out, 0, sizeof *out);
    out->count = P_COUNT;
    for (int p = 0; p < P_COUNT; p++) memcpy(out->part[p].color, COLOURS[p], sizeof COLOURS[p]);
    float dmg = 40;
    for (int k = 0; k < c->mount_count; k++)
        if (c->mounts[k].bat == BAT_MAIN) { dmg = c->weapons[c->mounts[k].weapon].damage; break; }
    float R = 2.5f + dmg * 0.065f;                       /* 16in: ~8.4, half a battleship's beam wide */
    int barrels = dmg >= 55 ? 3 : dmg >= 30 ? 2 : 1;
    float len = 1.5f * R + dmg * 0.05f, t = 0.11f * R;
    gen_part *g = &out->part[P_GUN];
    box(g, -R, 0, -0.9f * R, R, 0.55f * R, 0.75f * R);   /* gunhouse */
    box(g, -0.8f * R, 0.55f * R, -0.7f * R, 0.8f * R, 0.65f * R, 0.4f * R);   /* roof and rangefinder */
    for (int k = 0; k < barrels; k++) {
        float x = ((float)k - (float)(barrels - 1) / 2) * 0.55f * R;
        box(g, x - t, 0.25f * R - t, 0.7f * R, x + t, 0.25f * R + t, 0.7f * R + len);
    }
}

void genship_plane(const char *nation, gen_model *out)
{
    memset(out, 0, sizeof *out);
    out->count = 2;
    const float *paint = !strcmp(nation, "Japanese") ? (const float[3]){0.38f, 0.44f, 0.3f}
                       : !strcmp(nation, "British") ? (const float[3]){0.44f, 0.46f, 0.38f}
                       : !strcmp(nation, "German") ? (const float[3]){0.5f, 0.52f, 0.55f} : (const float[3]){0.42f, 0.48f, 0.58f};
    memcpy(out->part[0].color, paint, sizeof(float) * 3);
    memcpy(out->part[1].color, COLOURS[P_DARK], sizeof COLOURS[P_DARK]);
    gen_part *b = &out->part[0];
    box(b, -0.6f, -0.5f, -4.0f, 0.6f, 0.6f, 3.2f);       /* fuselage */
    box(b, -5.0f, -0.1f, -0.4f, 5.0f, 0.15f, 1.4f);      /* wings */
    box(b, -2.0f, 0.0f, -4.0f, 2.0f, 0.12f, -3.2f);      /* tailplane */
    box(b, -0.08f, 0.1f, -4.0f, 0.08f, 1.7f, -3.1f);     /* fin */
    box(&out->part[1], -0.75f, -0.6f, 3.2f, 0.75f, 0.7f, 4.0f);   /* cowling */
    box(&out->part[1], -0.35f, 0.6f, -0.6f, 0.35f, 1.0f, 1.2f);   /* canopy */
}

void genship_island(unsigned seed, float R, float H, gen_model *out)
{
    memset(out, 0, sizeof *out);
    out->count = 3;
    const float cols[3][3] = {{0.76f, 0.7f, 0.52f}, {0.32f, 0.45f, 0.24f}, {0.45f, 0.42f, 0.38f}};   /* sand, grass, rock */
    for (int p = 0; p < 3; p++) memcpy(out->part[p].color, cols[p], sizeof cols[p]);
    enum { A = 40, RINGS = 10 };
    float amp[5], ph[5];
    unsigned s = seed;
    for (int k = 0; k < 5; k++) {
        s = s * 1664525u + 1013904223u;
        amp[k] = (float)(s >> 24) / 255.0f * 0.18f;
        ph[k] = (float)((s >> 8) & 0xffff) / 65536.0f * 6.2831853f;
    }
    float pt[RINGS + 2][A][3];
    for (int ring = 0; ring <= RINGS + 1; ring++)
        for (int a = 0; a < A; a++) {
            float ang = 6.2831853f * (float)a / A, wob = 1;
            for (int k = 0; k < 5; k++) wob += amp[k] * sinf((float)(k + 2) * ang + ph[k]);
            float u = ring <= RINGS ? (float)ring / RINGS : 1.12f;   /* the last ring: underwater skirt */
            float r = R * u * wob;
            float h = ring <= RINGS ? H * powf(fmaxf(0, 1 - u * u), 1.4f) * (0.85f + 0.3f * wob - 0.3f) + 3 : -25;
            if (ring == RINGS) h = 2;                    /* the beach line */
            pt[ring][a][0] = cosf(ang) * r;
            pt[ring][a][1] = h;
            pt[ring][a][2] = sinf(ang) * r;
        }
    for (int ring = 0; ring <= RINGS; ring++)
        for (int a = 0; a < A; a++) {
            int b = (a + 1) % A;
            const float *p0 = pt[ring][a], *p1 = pt[ring][b], *p2 = pt[ring + 1][b], *p3 = pt[ring + 1][a];
            float hmean = (p0[1] + p1[1] + p2[1] + p3[1]) / 4;
            int part = hmean < 8 ? 0 : hmean > H * 0.7f ? 2 : 1;
            quad(&out->part[part], p0, p3, p2, p1);
        }
}

void genship_free(gen_model *m)
{
    for (int p = 0; p < GEN_PARTS; p++) { free(m->part[p].v); free(m->part[p].idx); }
    memset(m, 0, sizeof *m);
}
