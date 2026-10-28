/* AI fleet commander: one per AI fleet (skirmish), a two-state machine run every two
 * seconds by ai_step before the ships' own logic (ai.c), which still fights each ship.
 *
 *   APPROACH  nothing in sight close by: the fleet steams in formation at its slowest
 *             ship's speed toward the latest sighting (else where the enemy is thought
 *             to be). The battle line abreast, the screen 1,500 ahead, carriers and
 *             auxiliaries 2,500 astern; ships behind their station catch up, ships
 *             ahead ease off. One ship sends recon ahead.
 *   ENGAGE    an enemy surface ship within reach (9,000; carriers' strike range when
 *             the fleet has carriers): ships fight on their own, but the commander picks
 *             a priority target (valuable, damaged, close) for the carriers, and sends
 *             the screen's torpedo craft at the enemy's biggest ship. Carriers fly
 *             their strikes together: each waits until every carrier is ready, or
 *             90 s after the first one was.
 *
 * Back to APPROACH after 40 s with no enemy surface ship in sight. Submarines and ships
 * heading home for repairs are left to their own logic. Everything here is in the
 * world, so lockstep peers agree on it. */
#include <math.h>

#include "math3d.h"
#include "sim.h"

#define ENGAGE_RANGE 9000.0f
#define CARRIER_ENGAGE 12000.0f
#define LOST_CONTACT_S 40
#define STRIKE_WAIT_S 90

typedef enum { ROLE_MAIN, ROLE_SCREEN, ROLE_REAR, ROLE_FREE } role;

static role role_of(const ship *s)
{
    int h = s->cls->hull_type;
    if (s->cls->submerged || is_shore(s) || s->returning) return ROLE_FREE;
    if (is_carrier(s)) return ROLE_REAR;
    if (h == 6 || h == 7 || h == 13 || h == 15) return ROLE_SCREEN;   /* DD, DE, PT, flagship DD */
    if (h == 4 || h == 8 || h == 9 || h == 10 || h == 11) return ROLE_REAR;   /* cargo, hospital, landing, minelayer, repair */
    return ROLE_MAIN;
}

static int has_torpedoes(const ship *s)
{
    for (int m = 0; m < s->cls->mount_count; m++)
        if (s->cls->mounts[m].bat == BAT_TORPEDO) return 1;
    return 0;
}

/* Where the enemy is: the latest sighting, else the centre of their fleet. */
static int enemy_estimate(const world *w, int side, float *x, float *z)
{
    long latest = -1;
    for (int j = 0; j < w->count; j++)
        if (w->ships[j].side != side && w->ships[j].alive && w->last_seen[side & 1][j] > latest) {
            latest = w->last_seen[side & 1][j];
            *x = w->last_x[side & 1][j];
            *z = w->last_z[side & 1][j];
        }
    if (latest >= 0) return 1;
    int n = 0;
    float sx = 0, sz = 0;
    for (int j = 0; j < w->count; j++)
        if (w->ships[j].side != side && w->ships[j].alive && !is_shore(&w->ships[j])) { sx += w->ships[j].x; sz += w->ships[j].z; n++; }
    if (!n) return 0;
    *x = sx / (float)n;
    *z = sz / (float)n;
    return 1;
}

static void approach(world *w, int p, const int *ids, int n, int side)
{
    float cx = 0, cz = 0, vmin = INFINITY;
    int nm = 0;
    for (int q = 0; q < n; q++) {                   /* the guide: the battle line's centre */
        const ship *s = &w->ships[ids[q]];
        if (role_of(s) == ROLE_MAIN) { cx += s->x; cz += s->z; nm++; }
    }
    if (!nm)
        for (int q = 0; q < n; q++) { cx += w->ships[ids[q]].x; cz += w->ships[ids[q]].z; nm++; }
    cx /= (float)nm;
    cz /= (float)nm;
    for (int q = 0; q < n; q++) {
        const ship *s = &w->ships[ids[q]];
        vmin = fminf(vmin, s->max_speed * clampf(module_health(s, MOD_ENGINE, -1) * 1.25f, 0.3f, 1));
    }
    float ex, ez;
    if (!enemy_estimate(w, side, &ex, &ez)) return;
    float course = atan2f(ex - cx, ez - cz);
    /* A fleet with bombers to fly holds once the enemy is in strike distance and lets
     * the scouts find them; the others keep closing. */
    int bombers = 0;
    for (int k = 0; k < w->air_count; k++)
        bombers += w->air[k].carrier >= 0 && w->ships[w->air[k].carrier].owner == p && is_strike_plane(w->air[k].type) &&
                   w->air[k].state != AIR_LOST && w->air[k].state != AIR_FALLING;
    float lead = bombers && hypotf(ex - cx, ez - cz) < 9000 ? 0 : 800;
    float fx = sinf(course), fz = cosf(course), rx = fz, rz = -fx;   /* forward, starboard */
    int count[3] = {0}, seen[3] = {0};
    for (int q = 0; q < n; q++) {
        role r = role_of(&w->ships[ids[q]]);
        if (r != ROLE_FREE) count[r]++;
    }
    int recon_sent = 0;
    for (int q = 0; q < n; q++) {
        int i = ids[q];
        ship *s = &w->ships[i];
        role r = role_of(s);
        if (r == ROLE_FREE) continue;
        static const float spacing[3] = {500, 700, 800}, ahead[3] = {0, 1500, -2500};
        float across = ((float)seen[r]++ - (float)(count[r] - 1) / 2) * spacing[r];
        float sx = cx + fx * ahead[r] + rx * across, sz = cz + fz * ahead[r] + rz * across;
        /* Off station along the course: ease off when ahead, open up when behind. */
        float along = (s->x - sx) * fx + (s->z - sz) * fz;
        s->cruise = along > 300 ? vmin * 0.6f : along < -300 ? 0 : vmin * 0.9f;
        int id = i;
        if (lead > 0 || hypotf(sx - s->x, sz - s->z) > 250) order_move(w, &id, 1, sx + fx * lead, sz + fz * lead, 0);
        else order_stop(w, &id, 1);
        s->in_formation = 1;
        if (!recon_sent && can_recon(s)) { order_recon(w, i, ex, ez); recon_sent = 1; }
    }
}

/* Value of shooting at j: what she's worth, more when hurt, less when far. */
static float target_value(const world *w, float cx, float cz, int j)
{
    const ship *t = &w->ships[j];
    float d = hypotf(t->x - cx, t->z - cz);
    return class_cost(t->cls) * (1.5f - t->hp / t->cls->max_health) / (1 + d / 6000);
}

void ai_commander(world *w)
{
    if (w->mode != MODE_SKIRMISH) return;
    for (int p = 0; p < FLEET_PLAYERS; p++) {
        commander *c = &w->cmdr[p];
        int ids[MAX_SHIPS], n = 0, side = -1, carriers = 0;
        float cx = 0, cz = 0;
        for (int i = 0; i < w->count; i++) {
            ship *s = &w->ships[i];
            s->in_formation = s->owner == p ? 0 : s->in_formation;
            if (s->owner != p || !s->alive || !s->ai || is_shore(s)) continue;
            s->cruise = 0;
            s->ai_target = -1;
            ids[n++] = i;
            side = s->side;
            cx += s->x;
            cz += s->z;
            carriers += is_carrier(s);
        }
        if (n < 2 || (w->no_commander >> p & 1)) continue;   /* a lone ship needs no commander */
        cx /= (float)n;
        cz /= (float)n;
        /* Contact: an enemy surface ship in sight and within reach. */
        float reach = carriers ? CARRIER_ENGAGE : ENGAGE_RANGE, best = 0, big = 0;
        c->target = c->capital = -1;
        for (int j = 0; j < w->count; j++) {
            const ship *t = &w->ships[j];
            if (!t->alive || t->side == side || is_submerged(t) || !visible_to(w, side, j)) continue;
            float d = hypotf(t->x - cx, t->z - cz);
            if (d > reach && !is_shore(t)) continue;
            if (d <= reach) c->contact_tick = w->tick;
            float v = target_value(w, cx, cz, j);
            if (v > best) { best = v; c->target = j; }
            if (!is_shore(t) && class_cost(t->cls) > big) { big = class_cost(t->cls); c->capital = j; }
        }
        if (c->target >= 0) c->state = CMDR_ENGAGE;
        else if (c->state == CMDR_ENGAGE && w->tick - c->contact_tick > LOST_CONTACT_S * SIM_HZ) c->state = CMDR_APPROACH;
        if (c->state == CMDR_APPROACH) { approach(w, p, ids, n, side); continue; }
        /* Engaged: the screen's torpedo craft go for the biggest ship. */
        for (int q = 0; q < n; q++) {
            ship *s = &w->ships[ids[q]];
            if (role_of(s) == ROLE_SCREEN && has_torpedoes(s) && c->capital >= 0) s->ai_target = c->capital;
            if (is_carrier(s) && c->target >= 0) s->ai_target = c->target;
        }
    }
}

/* Carriers of one fleet strike together: go when all are ready, or after a wait. */
int commander_strike_ok(const world *w, int carrier)
{
    const ship *cv = &w->ships[carrier];
    if (w->mode != MODE_SKIRMISH || cv->owner < 0 || cv->owner >= FLEET_PLAYERS) return 1;
    commander *c = (commander *)&w->cmdr[cv->owner];   /* the wait clock lives in the world */
    int others = 0, ready = 1;
    for (int i = 0; i < w->count; i++) {
        const ship *s = &w->ships[i];
        if (i == carrier || s->owner != cv->owner || !s->alive || !s->ai || !is_carrier(s)) continue;
        int left = 0;
        for (int k = 0; k < w->air_count; k++)
            left += w->air[k].carrier == i && is_strike_plane(w->air[k].type) &&
                    w->air[k].state != AIR_LOST && w->air[k].state != AIR_FALLING;
        if (!left) continue;
        others++;
        ready &= strike_ready(w, i) >= (left < 3 ? left : 3);
    }
    if (!others || ready) { c->strike_wait = -1; return 1; }
    if (c->strike_wait < 0) c->strike_wait = w->tick;
    if (w->tick - c->strike_wait > STRIKE_WAIT_S * SIM_HZ) { c->strike_wait = -1; return 1; }
    return 0;
}
