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

/* Weight of a ship's gunfire: damage per second of every shell-firing mount. */
static float firepower(const ship *s)
{
    float f = 0;
    for (int m = 0; m < s->cls->mount_count; m++) {
        const class_weapon *wp = &s->cls->weapons[s->cls->mounts[m].weapon];
        if (wp->ammo == AMMO_SHELL && wp->fire_rate > 0) f += wp->damage / (wp->fire_rate * RELOAD_SCALE);
    }
    return f;
}

/* AI specials: used when they'd count. */
static void ai_special(world *w, int i, int target)
{
    ship *s = &w->ships[i];
    special_kind k = special_of(s->cls);
    if (k == SP_NONE || s->special_cool > 0) return;
    int in_range = 0;
    for (int m = 0; target >= 0 && m < s->cls->mount_count && !in_range; m++) in_range = can_engage(w, i, m, target);
    switch (k) {
    case SP_FOCUS_FIRE: case SP_RAPID_FIRE: case SP_ACCURATE_FIRE: case SP_TARGETING_TORPEDO:
        if (in_range) order_special(w, i, target);
        break;
    case SP_HEDGEHOG: order_special(w, i, -1); break;   /* goes only with a submarine near */
    case SP_EVASIVE_ACTION: {
        float comb;
        if (torpedo_threat(w, i, &comb) || (target >= 0 && s->hp < 0.8f * s->cls->max_health && in_range)) order_special(w, i, -1);
        break;
    }
    case SP_AA_BLAST: case SP_ACE_PILOT:
        for (int k2 = 0; k2 < w->air_count; k2++) {
            const aircraft *a = &w->air[k2];
            if (a->side != s->side && (a->state == AIR_OUT || a->state == AIR_ATTACK) &&
                hypotf(a->x - s->x, a->z - s->z) < (k == SP_AA_BLAST ? 1300 : 6000)) { order_special(w, i, -1); break; }
        }
        break;
    default: break;
    }
}

/* AI captains act on their own reports, as an attentive player would. */
static void ai_captains(world *w)
{
    for (int side = 0; side < 2; side++) {
        captain_report rep[64];
        int n = captain_reports(w, side, rep, 64);
        for (int k = 0; k < n; k++) {
            const ship *s = &w->ships[rep[k].ship];
            if (!s->ai || rep[k].kind == REP_BRIDGE_LOST || rep[k].kind == REP_WITHDRAW ||
                rep[k].kind == REP_RETURN_TO_YARD || rep[k].kind == REP_LAUNCH_STRIKE || rep[k].urgency < 40) continue;
            captain_act(w, &rep[k]);
        }
    }
}

/* Concentration of fire: each AI fleet's main batteries on one target, the enemy most
 * of its ships can reach, weighted toward damaged and valuable ships. Every 10 s. */
static void ai_focus(world *w)
{
    if (w->tick % (10 * SIM_HZ) != 0) return;
    for (int p = 0; p < FLEET_PLAYERS + 2; p++) {
        int best = -1;
        float bscore = 0;
        for (int j = 0; j < w->count; j++) {
            const ship *t = &w->ships[j];
            if (!t->alive || t->cls->submerged) continue;
            int reach = 0, side = -1;
            for (int i = 0; i < w->count; i++) {
                const ship *s = &w->ships[i];
                if (s->owner != p || !s->alive || !s->ai || s->side == t->side) continue;
                side = s->side;
                for (int m = 0; m < s->cls->mount_count; m++)
                    if (s->cls->mounts[m].bat == BAT_MAIN && can_engage(w, i, m, j)) { reach++; break; }
            }
            if (side < 0 || !reach) continue;
            float cond = t->hp / t->cls->max_health;
            float score = (float)reach * (1.5f - cond) * (0.5f + class_cost(t->cls) / 1500);
            if (score > bscore) { bscore = score; best = j; }
        }
        for (int i = 0; i < w->count; i++) {
            ship *s = &w->ships[i];
            if (s->owner == p && s->alive && s->ai) s->order_target[BAT_MAIN] = best;
        }
    }
}

/* Every fourth ship of a side (by its order within that side's fleet) guards home:
 * the ship index itself won't do, since two yards building in turn give each side
 * indices of one parity. */
static int home_guard(const world *w, int i)
{
    int ord = 0;
    for (int j = 0; j < i; j++) ord += w->ships[j].side == w->ships[i].side && !is_shore(&w->ships[j]);
    return ord % 4 == 0;
}

void ai_step(world *w)
{
    ai_build(w);
    ai_command(w);
    ai_captains(w);
    ai_focus(w);
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
            if (!t->alive || t->side == s->side || !visible_to(w, s->side, j)) continue;
            float d = hypotf(t->x - s->x, t->z - s->z) + (t->cls->submerged ? 4000 : 0);
            if (d < bd) { bd = d; best = j; }
        }
        if (best < 0) {
            /* Nothing in sight: search. Toward the latest sighting, else toward where
             * intelligence puts the enemy (their fleet's centre); send recon ahead. */
            int sd = s->side & 1;
            long latest = -1;
            float gx = 0, gz = 0;
            for (int j = 0; j < w->count; j++)
                if (w->ships[j].side != s->side && w->ships[j].alive && w->last_seen[sd][j] > latest) {
                    latest = w->last_seen[sd][j];
                    gx = w->last_x[sd][j];
                    gz = w->last_z[sd][j];
                }
            if (latest < 0 || hypotf(gx - s->x, gz - s->z) < 600) {
                int n = 0;
                gx = gz = 0;
                for (int j = 0; j < w->count; j++)
                    if (w->ships[j].side != s->side && w->ships[j].alive) { gx += w->ships[j].x; gz += w->ships[j].z; n++; }
                if (!n) { order_stop(w, &i, 1); continue; }
                gx /= (float)n;
                gz /= (float)n;
            }
            if (can_recon(s)) order_recon(w, i, gx, gz);
            /* Carriers close to strike distance of the guess, then wait for the scouts. */
            if (is_carrier(s) && hypotf(gx - s->x, gz - s->z) < 9000) { order_stop(w, &i, 1); continue; }
            float to = atan2f(gx - s->x, gz - s->z);
            order_move(w, &i, 1, s->x + sinf(to) * 800, s->z + cosf(to) * 800, 0);
            continue;
        }
        const ship *t = &w->ships[best];
        ai_special(w, i, best);
        /* Objectives. Attacking (assault, or the attacker in defend mode): make for the
         * enemy shipyard, fighting only ships that come within 3,500. In assault every
         * fourth ship stays home as a guard. */
        int ey = w->sides[1 - (s->side & 1)].yard;
        int attacking = ey >= 0 && w->ships[ey].alive &&
                        (w->mode == MODE_ASSAULT || (w->mode == MODE_DEFEND && (s->side & 1) != (w->defender & 1)));
        int guard = (w->mode == MODE_DEFEND && (s->side & 1) == (w->defender & 1)) ||
                    (w->mode == MODE_ASSAULT && home_guard(w, i));
        if (attacking && !guard && !s->committed && y >= 0 && w->ships[y].alive) {
            /* Muster off the home yard; the wave sails when big enough (4, then 2 more
             * each time up to 8, so a stalled assault escalates), or as soon as it
             * outnumbers the enemy's whole fleet by two. */
            const ship *hy = &w->ships[y];
            float fwd = hy->cls->half_length + 2500;
            float rx = hy->x + sinf(hy->heading) * fwd, rz = hy->z + cosf(hy->heading) * fwd;
            int mustered = 0;
            for (int j = 0; j < w->count; j++) {
                const ship *o = &w->ships[j];
                mustered += o->alive && o->ai && o->side == s->side && !is_shore(o) && !o->committed && !o->returning &&
                            !home_guard(w, j) && hypotf(o->x - rx, o->z - rz) < 1500;
            }
            side_state *own = &w->sides[s->side & 1];
            int enemies = 0;
            for (int j = 0; j < w->count; j++)
                enemies += w->ships[j].alive && w->ships[j].side != s->side && !is_shore(&w->ships[j]);
            int need = 4 + 2 * own->waves;
            if (need > 8) need = 8;                       /* waves grow, but not past eight */
            if (mustered >= need || mustered >= enemies + 2) {
                own->waves++;
                for (int j = 0; j < w->count; j++) {
                    ship *o = &w->ships[j];
                    if (o->alive && o->ai && o->side == s->side && !is_shore(o) && !home_guard(w, j) && hypotf(o->x - rx, o->z - rz) < 1500)
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
                if (!o->alive || o->side == s->side || is_shore(o) || o->cls->submerged || !visible_to(w, s->side, j)) continue;
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
            /* Run from ships that outgun her; another carrier, or anything no better
             * armed, she stands and fights with her own guns while the planes work. */
            if (d < 5000 && firepower(t) > 1.2f * firepower(s)) {
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
