/* Minimal enemy commander (the full fleet AI is phase 8). Every two seconds each
 * ship with ai set closes on the nearest enemy until inside its best weapon's
 * range, then turns to steam across the bearing so every turret bears. Carriers
 * stand off and fly strikes; one with no bombers left fights with its guns. */
#include <math.h>

#include "math3d.h"
#include "sim.h"

static float engage_range(const ship_class *c)
{
    float gun = 0, torp = 0;
    for (int m = 0; m < c->mount_count; m++) {
        const class_weapon *w = &c->weapons[c->mounts[m].weapon];
        if (w->ammo == AMMO_SHELL) gun = fmaxf(gun, w->max_range);
        if (w->ammo == AMMO_TORPEDO) torp = fmaxf(torp, w->max_range);
    }
    /* Torpedo craft run in close; gunships stand off near their maximum range. */
    return torp > 0 && torp * 0.5f > gun * 0.5f ? torp * 0.5f : gun > 0 ? gun * 0.8f : 1500;
}

void ai_step(world *w)
{
    for (int i = 0; i < w->count; i++) {
        ship *s = &w->ships[i];
        if (!s->ai || !s->alive) continue;
        int best = -1;
        float bd = INFINITY;
        for (int j = 0; j < w->count; j++) {
            const ship *t = &w->ships[j];
            if (!t->alive || t->side == s->side) continue;
            float d = hypotf(t->x - s->x, t->z - s->z) + (t->cls->submerged ? 4000 : 0);
            if (d < bd) { bd = d; best = j; }
        }
        if (best < 0) {
            order_stop(w, &i, 1);
            continue;
        }
        const ship *t = &w->ships[best];
        int bombers_left = 0;
        for (int k = 0; k < w->air_count; k++)
            bombers_left += w->air[k].carrier == i && w->air[k].type == PLANE_BOMBER &&
                            w->air[k].state != AIR_LOST && w->air[k].state != AIR_FALLING;
        if (is_carrier(s) && bombers_left) {
            /* Strike anything within reach, cover the fleet when enemy planes are up,
             * and keep well clear of the gun line. */
            int ready = hangar_count(w, i, PLANE_BOMBER);     /* a full deck load, or all that's left */
            if (ready >= (bombers_left < 3 ? bombers_left : 3) && bd < 9000) order_strike(w, i, best);
            for (int k = 0; k < w->air_count; k++) {
                const aircraft *a = &w->air[k];
                if (a->side != s->side && (a->state == AIR_OUT || a->state == AIR_ATTACK) &&
                    hypotf(a->x - s->x, a->z - s->z) < 5000) { order_cap(w, i); break; }
            }
            float d = hypotf(t->x - s->x, t->z - s->z);
            if (d < 5000) {
                float away = atan2f(s->x - t->x, s->z - t->z);
                order_move(w, &i, 1, s->x + sinf(away) * 800, s->z + cosf(away) * 800, 0);
            } else if (s->wp_count == 0 && d > 8000) {
                float to = atan2f(t->x - s->x, t->z - s->z);
                order_move(w, &i, 1, s->x + sinf(to) * 800, s->z + cosf(to) * 800, 0);
            }
            continue;
        }
        float dx = t->x - s->x, dz = t->z - s->z, d = hypotf(dx, dz), R = engage_range(s->cls);
        float bearing = atan2f(dx, dz), go;
        if (d > R || t->cls->submerged) {
            go = bearing;                                   /* close the range */
        } else {
            /* Beam on: whichever perpendicular is nearer the current heading; edge in
             * or out a little to hold the range. */
            float a = bearing + PI_F / 2, b = bearing - PI_F / 2;
            go = fabsf(wrap_angle(a - s->heading)) < fabsf(wrap_angle(b - s->heading)) ? a : b;
            go += (d > R * 0.8f ? 1 : -1) * wrap_angle(bearing - go) * 0.2f;
        }
        float reach = t->cls->submerged ? fminf(d, 800) : 800;
        order_move(w, &i, 1, s->x + sinf(go) * reach, s->z + cosf(go) * reach, 0);
    }
}
