/* Minimal enemy commander (the full fleet AI is phase 8). Every two seconds each
 * ship with ai set closes on the nearest enemy until inside its best weapon's
 * range, then turns to steam across the bearing so every turret bears. Carriers
 * stand off and fly strikes; one with no bombers left fights with its guns. */
#include <math.h>

#include "math3d.h"
#include "sim.h"

#define STRIKE_RANGE 12000.0f /* carriers launch at targets this close */

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

/* An AI shipyard keeps two ships on the slipway, cycling through its options. */
static void ai_build(world *w)
{
    for (int side = 0; side < 2; side++) {
        side_state *st = &w->sides[side];
        if (st->yard < 0 || !w->ships[st->yard].alive || !w->ships[st->yard].ai || !st->option_count) continue;
        if (st->queue_count >= 2) continue;
        const ship_class *c = st->options[(st->built + st->queue_count) % st->option_count];
        order_build(w, side, c);
    }
}

/* AI admirals: a flagship (the biggest ship), officers posted to the biggest ships. */
static void ai_command(world *w)
{
    for (int p = 0; p < FLEET_PLAYERS; p++) {
        int best = -1, side = -1, any_ai = 0;
        float bc = -1;
        for (int i = 0; i < w->count; i++) {
            const ship *s = &w->ships[i];
            if (s->owner != p || !s->alive || is_shore(s)) continue;
            any_ai |= s->ai;
            side = s->side & 1;
            float c = class_cost(s->cls) + s->hp * 0.01f;
            if (c > bc) { bc = c; best = i; }
        }
        if (!any_ai || best < 0) continue;
        if (w->flagship_of[p] < 0) order_flagship(w, p, best);
        for (int k = 0; k < w->officer_count[side]; k++)
            if (w->officer_ship[side][k] == -1) {
                officer_kind kind = w->officer_kind_of[side][k];
                /* Aviation officers to a carrier if there is one. */
                int to = best;
                if (kind == OFF_AVIATION)
                    for (int i = 0; i < w->count; i++)
                        if (w->ships[i].owner == p && w->ships[i].alive && is_carrier(&w->ships[i])) { to = i; break; }
                order_officer(w, p, k, to);
            }
    }
}

void ai_step(world *w)
{
    ai_build(w);
    ai_command(w);
    for (int i = 0; i < w->count; i++) {
        ship *s = &w->ships[i];
        if (!s->ai || !s->alive || is_shore(s)) continue;
        float comb;
        if (s->evade <= 0 && torpedo_threat(w, i, &comb)) {   /* AI captains comb torpedoes too */
            order_evade(w, &i, 1, 30);
            continue;
        }
        /* Badly hurt: home to the drydock, back out once patched up. */
        int y = w->sides[s->side & 1].yard;
        int dock = y >= 0 && w->ships[y].alive && module_health(&w->ships[y], MOD_DRYDOCK, -1) >= 0.3f;
        float cond = s->hp / s->cls->max_health;
        if (dock && cond < 0.35f) s->returning = 1;
        if (s->returning && (!dock || cond > 0.85f)) s->returning = 0;
        if (s->returning) {
            const ship *yd = &w->ships[y];
            float fwd = yd->cls->half_length + 450;
            float px = yd->x + sinf(yd->heading) * fwd, pz = yd->z + cosf(yd->heading) * fwd;
            if (hypotf(px - s->x, pz - s->z) > 250) order_move(w, &i, 1, px, pz, 0);
            else order_stop(w, &i, 1);
            continue;
        }
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
        /* Objectives. Attacking (assault, or the attacker in defend mode): make for the
         * enemy shipyard, fighting only ships that come within 3,500. In assault every
         * fourth ship stays home as a guard. */
        int ey = w->sides[1 - (s->side & 1)].yard;
        int attacking = ey >= 0 && w->ships[ey].alive &&
                        (w->mode == MODE_ASSAULT || (w->mode == MODE_DEFEND && (s->side & 1) != (w->defender & 1)));
        int guard = (w->mode == MODE_DEFEND && (s->side & 1) == (w->defender & 1)) ||
                    (w->mode == MODE_ASSAULT && i % 4 == 0);
        if (attacking && !guard && !s->committed && y >= 0 && w->ships[y].alive) {
            /* Muster off the home yard; the wave sails when big enough (4, then 2 more
             * each time, so a stalled assault escalates). */
            const ship *hy = &w->ships[y];
            float fwd = hy->cls->half_length + 2500;
            float rx = hy->x + sinf(hy->heading) * fwd, rz = hy->z + cosf(hy->heading) * fwd;
            int mustered = 0;
            for (int j = 0; j < w->count; j++) {
                const ship *o = &w->ships[j];
                mustered += o->alive && o->ai && o->side == s->side && !is_shore(o) && !o->committed && !o->returning &&
                            j % 4 != 0 && hypotf(o->x - rx, o->z - rz) < 1500;
            }
            side_state *own = &w->sides[s->side & 1];
            if (mustered >= 4 + 2 * own->waves) {
                own->waves++;
                for (int j = 0; j < w->count; j++) {
                    ship *o = &w->ships[j];
                    if (o->alive && o->ai && o->side == s->side && !is_shore(o) && j % 4 != 0 && hypotf(o->x - rx, o->z - rz) < 1500)
                        o->committed = 1;
                }
            } else {
                if (hypotf(rx - s->x, rz - s->z) > 300) order_move(w, &i, 1, rx, rz, 0);
                continue;
            }
        }
        if (attacking && !guard) {
            float near = INFINITY;
            int nb = -1;
            for (int j = 0; j < w->count; j++) {
                const ship *o = &w->ships[j];
                if (!o->alive || o->side == s->side || is_shore(o) || o->cls->submerged) continue;
                float d = hypotf(o->x - s->x, o->z - s->z);
                if (d < near) { near = d; nb = j; }
            }
            best = nb >= 0 && near < 3500 ? nb : ey;
            t = &w->ships[best];
            bd = hypotf(t->x - s->x, t->z - s->z);
        }
        /* Guarding: stay on station off the yard until the enemy comes within 6,000. */
        if (guard && y >= 0 && w->ships[y].alive) {
            const ship *yd = &w->ships[y];
            float near = INFINITY;
            for (int j = 0; j < w->count; j++)
                if (w->ships[j].alive && w->ships[j].side != s->side)
                    near = fminf(near, hypotf(w->ships[j].x - yd->x, w->ships[j].z - yd->z));
            if (near > 6000) {
                float fwd = yd->cls->half_length + 1500;
                float px = yd->x + sinf(yd->heading) * fwd, pz = yd->z + cosf(yd->heading) * fwd;
                if (hypotf(px - s->x, pz - s->z) > 400) order_move(w, &i, 1, px, pz, 0);
                continue;
            }
        }
        int bombers_left = 0;
        for (int k = 0; k < w->air_count; k++)
            bombers_left += w->air[k].carrier == i && is_strike_plane(w->air[k].type) &&
                            w->air[k].state != AIR_LOST && w->air[k].state != AIR_FALLING;
        if (is_carrier(s) && bombers_left) {
            /* Strike anything within reach, cover the fleet when enemy planes are up,
             * and keep well clear of the gun line. */
            int ready = strike_ready(w, i);                   /* a full deck load, or all that's left */
            if (ready >= (bombers_left < 3 ? bombers_left : 3) && bd < STRIKE_RANGE) order_strike(w, i, best);
            for (int k = 0; k < w->air_count; k++) {
                const aircraft *a = &w->air[k];
                if (a->side != s->side && (a->state == AIR_OUT || a->state == AIR_ATTACK) &&
                    hypotf(a->x - s->x, a->z - s->z) < 5000) { order_cap(w, i); break; }
            }
            float d = hypotf(t->x - s->x, t->z - s->z);
            if (d < 5000) {
                float away = atan2f(s->x - t->x, s->z - t->z);
                order_move(w, &i, 1, s->x + sinf(away) * 800, s->z + cosf(away) * 800, 0);
            } else if (s->wp_count == 0 && d > STRIKE_RANGE * 0.9f) {
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
