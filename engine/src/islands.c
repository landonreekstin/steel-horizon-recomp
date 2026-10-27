/* Islands (design): round, seeded, placed in the battle area away from the fleets'
 * start lines. They stop ships and torpedoes and block line of sight between ships
 * (aircraft see over them); move orders route round them. */
#include <math.h>
#include <string.h>

#include "math3d.h"
#include "sim_internal.h"

static unsigned next(unsigned *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

void islands_generate(world *w, unsigned seed, int count, float x0, float z0, float x1, float z1)
{
    unsigned s = seed * 2654435761u + 1;
    w->isle_count = 0;
    for (int tries = 0; tries < count * 20 && w->isle_count < count && w->isle_count < MAX_ISLANDS; tries++) {
        float r = 250 + (float)(next(&s) % 650);
        float x = x0 + (x1 - x0) * (float)(next(&s) % 1000) / 1000;
        float z = z0 + (z1 - z0) * (float)(next(&s) % 1000) / 1000;
        int clash = 0;
        for (int k = 0; k < w->isle_count; k++)
            clash |= hypotf(w->isles[k].x - x, w->isles[k].z - z) < w->isles[k].radius + r + 600;
        for (int i = 0; i < w->count; i++)          /* keep clear of ships already placed */
            clash |= hypotf(w->ships[i].x - x, w->ships[i].z - z) < r + 900;
        if (clash) continue;
        island *is = &w->isles[w->isle_count++];
        is->x = x;
        is->z = z;
        is->radius = r;
        is->height = 40 + (float)(next(&s) % 120);
        is->seed = next(&s);
    }
}

int island_at(const world *w, float x, float z, float margin)
{
    for (int k = 0; k < w->isle_count; k++)
        if (hypotf(w->isles[k].x - x, w->isles[k].z - z) < w->isles[k].radius + margin) return k;
    return -1;
}

/* Does the segment a-b pass through an island (shrunk a little: beaches don't hide)? */
int island_blocks(const world *w, float ax, float az, float bx, float bz)
{
    float dx = bx - ax, dz = bz - az, l2 = dx * dx + dz * dz;
    for (int k = 0; k < w->isle_count; k++) {
        const island *is = &w->isles[k];
        float t = l2 > 0 ? ((is->x - ax) * dx + (is->z - az) * dz) / l2 : 0;
        t = clampf(t, 0, 1);
        if (hypotf(ax + dx * t - is->x, az + dz * t - is->z) < is->radius * 0.85f) return 1;
    }
    return 0;
}

void islands_step(world *w)
{
    for (int i = 0; i < w->count; i++) {
        ship *s = &w->ships[i];
        if (is_shore(s)) continue;
        for (int k = 0; k < w->isle_count; k++) {
            const island *is = &w->isles[k];
            float dx = s->x - is->x, dz = s->z - is->z, d = hypotf(dx, dz), need = is->radius + s->cls->half_beam + 10;
            if (d >= need || d < 1e-3f) continue;
            s->x = is->x + dx / d * need;              /* aground: pushed back to the shore */
            s->z = is->z + dz / d * need;
            s->speed *= 0.6f;
        }
    }
}

int island_route(const world *w, float x0, float z0, float x1, float z1, waypoint *out, int max)
{
    /* Up to a few detours: past the first island in the way, on the side nearer the
     * straight line, then try again from there. */
    int n = 0;
    float cx = x0, cz = z0;
    for (int hop = 0; hop < 4 && n < max - 1; hop++) {
        int hit = -1;
        float best_t = 2;
        float dx = x1 - cx, dz = z1 - cz, l2 = dx * dx + dz * dz;
        for (int k = 0; k < w->isle_count; k++) {
            const island *is = &w->isles[k];
            float t = l2 > 0 ? ((is->x - cx) * dx + (is->z - cz) * dz) / l2 : 0;
            if (t <= 0 || t >= 1) continue;
            float px = cx + dx * t, pz = cz + dz * t;
            if (hypotf(px - is->x, pz - is->z) < is->radius + 200 && t < best_t) { best_t = t; hit = k; }
        }
        if (hit < 0) break;
        const island *is = &w->isles[hit];
        float l = sqrtf(l2), nx = -dz / l, nz = dx / l;  /* the line's normal */
        float px = cx + dx * best_t, pz = cz + dz * best_t;
        float side = (px - is->x) * nx + (pz - is->z) * nz >= 0 ? 1 : -1;
        float r = is->radius + 450;
        out[n].x = is->x + nx * side * r;
        out[n].z = is->z + nz * side * r;
        cx = out[n].x;
        cz = out[n].z;
        n++;
    }
    out[n].x = x1;
    out[n].z = z1;
    return n + 1;
}
