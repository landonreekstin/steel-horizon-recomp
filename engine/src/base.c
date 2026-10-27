/* Shipyards and game modes (design; the original has no bases in its battles).
 *
 * A shipyard is a ship that doesn't move or float: it uses the same compartments,
 * armour, modules and guns, but can't flood, list or sink by water, and goes when its
 * structure fails or its ammunition dump explodes. While it stands it earns build
 * points, builds ships on its slipway and repairs friendly ships moored by its
 * drydock: structure, modules (even ones lost at sea), crew, fires and flooding. */
#include <math.h>
#include <string.h>

#include "math3d.h"
#include "sim_internal.h"

#define INCOME 4.0f            /* build points per second */
#define START_BUDGET 1200.0f
#define MOORED_SPEED 4.0f
#define SKIRMISH_LIMIT (90 * 60.0f)   /* seconds */      /* slower than this counts as alongside for repairs */

int is_shore(const ship *s)
{
    return s->cls->hull_type == HULL_SHIPYARD;
}

float repair_radius(const ship *yard)
{
    return yard->cls->half_length + 900;
}

int shipyard_spawn(world *w, const ship_class *yard, int side, float x, float z, float heading)
{
    int id = world_spawn(w, yard, side, x, z, heading);
    if (id < 0) return -1;
    side_state *st = &w->sides[side & 1];
    st->yard = id;
    st->budget = START_BUDGET;
    st->income = INCOME;
    return id;
}

int order_build(world *w, int side, const ship_class *c)
{
    side_state *st = &w->sides[side & 1];
    if (st->yard < 0 || !w->ships[st->yard].alive || st->queue_count == MAX_QUEUE) return -1;
    float cost = class_cost(c);
    if (st->budget < cost) return -1;
    st->budget -= cost;
    st->queue[st->queue_count++] = c;
    return 0;
}

int at_yard(const world *w, int i)
{
    const ship *s = &w->ships[i];
    int y = w->sides[s->side & 1].yard;
    if (y < 0 || y == i || is_shore(s)) return 0;
    const ship *yard = &w->ships[y];
    if (!yard->alive || module_health(yard, MOD_DRYDOCK, -1) < 0.3f) return 0;
    return s->speed < MOORED_SPEED && hypotf(s->x - yard->x, s->z - yard->z) < repair_radius(yard);
}

float mode_time_left(const world *w)
{
    if (w->mode == MODE_SKIRMISH || w->time_limit <= 0) return 0;
    return fmaxf(0, w->time_limit - (float)w->tick * SIM_DT);
}

static void repair_alongside(world *w, int i, float rate)
{
    ship *s = &w->ships[i];
    float dt = SIM_DT * rate;
    for (int k = 0; k < s->cls->comp_count; k++) {
        compartment *c = &s->comp[k];
        c->structure = fminf(1, c->structure + 0.004f * dt);
        c->fire = fmaxf(0, c->fire - 0.1f * dt);
        for (int sd = 0; sd < 2; sd++) {
            c->leak[sd] = fmaxf(0, c->leak[sd] - 0.01f * dt);
            c->flood[sd] = fmaxf(0, c->flood[sd] - 0.01f * dt);
        }
    }
    for (int m = 0; m < s->cls->module_count; m++) {
        /* The yard's workshops can rebuild what was lost at sea, slowly. */
        float add = (s->module_lost[m] ? 0.003f : 0.01f) * dt;
        s->module_hp[m] = fminf(1, s->module_hp[m] + add);
        if (s->module_lost[m] && s->module_hp[m] >= 0.1f) s->module_lost[m] = 0;
    }
    s->crew = fminf(s->cls->complement, s->crew + s->cls->complement * 0.002f * dt);
    /* Rearming: a magazine's worth every 2 minutes alongside. */
    if (w->tick % SIM_HZ == 0)                           /* once a second, 1/120 of a load */
        for (int m = 0; m < s->cls->mount_count; m++) {
            int cap = ammo_capacity(s->cls, m), add = (cap + 119) / 120;
            s->turrets[m].ammo = s->turrets[m].ammo + add < cap ? s->turrets[m].ammo + add : cap;
        }
}

/* Keep ships out of the shipyard's ground: push them back to its waterline. */
static void keep_off_land(world *w, int y)
{
    const ship *yard = &w->ships[y];
    float r = hypotf(yard->cls->half_length, yard->cls->half_beam) * 1.05f;
    for (int i = 0; i < w->count; i++) {
        ship *s = &w->ships[i];
        if (i == y || is_shore(s) || s->cls->submerged) continue;
        float dx = s->x - yard->x, dz = s->z - yard->z, d = hypotf(dx, dz), need = r + s->cls->half_beam;
        if (d >= need || d < 1e-3f) continue;
        s->x = yard->x + dx / d * need;
        s->z = yard->z + dz / d * need;
        s->speed *= 0.5f;
    }
}

static void produce(world *w, int side)
{
    side_state *st = &w->sides[side];
    if (st->yard < 0) return;
    ship *yard = &w->ships[st->yard];
    if (!yard->alive) return;
    st->budget += st->income * SIM_DT;
    float slip = module_health(yard, MOD_SLIPWAY, -1);
    if (!st->queue_count || slip < 0.3f) return;
    st->progress += SIM_DT * slip;
    const ship_class *c = st->queue[0];
    if (st->progress < build_time(c)) return;
    /* Launch off the slipway, seaward of the yard, staggered so they don't stack. */
    float off = ((st->built % 3) - 1) * 220.0f, fwd = yard->cls->half_length + 300;
    float sn = sinf(yard->heading), cs = cosf(yard->heading);
    float x = yard->x + sn * fwd + cs * off, z = yard->z + cs * fwd - sn * off;
    int id = world_spawn(w, c, side, x, z, yard->heading);
    if (id >= 0) {
        w->ships[id].ai = yard->ai;
        st->built++;
    }
    st->progress = 0;
    memmove(st->queue, st->queue + 1, sizeof st->queue[0] * (size_t)--st->queue_count);
}

static void reinforce(world *w, int side)
{
    side_state *st = &w->sides[side];
    if (st->reinforce_every <= 0 || !st->option_count) return;
    st->reinforce_timer += SIM_DT;
    if (st->reinforce_timer < st->reinforce_every) return;
    st->reinforce_timer = 0;
    /* The wave: the first two options, in line abreast, heading for the enemy. */
    int ey = w->sides[1 - side].yard;
    float hd = ey >= 0 ? atan2f(w->ships[ey].x - st->entry_x, w->ships[ey].z - st->entry_z) : 0;
    for (int k = 0; k < 2 && k < st->option_count; k++) {
        float off = ((float)k - 0.5f) * 400;
        int id = world_spawn(w, st->options[k], side, st->entry_x + cosf(hd) * off, st->entry_z - sinf(hd) * off, hd);
        if (id >= 0) {
            int any_ai = 0;
            for (int i = 0; i < w->count; i++) any_ai |= w->ships[i].side == side && w->ships[i].ai;
            w->ships[id].ai = any_ai;
            st->built++;
        }
    }
}

void base_step(world *w)
{
    for (int side = 0; side < 2; side++) {
        produce(w, side);
        reinforce(w, side);
        int y = w->sides[side].yard;
        if (y < 0 || !w->ships[y].alive) continue;
        keep_off_land(w, y);
        float dock = module_health(&w->ships[y], MOD_DRYDOCK, -1);
        for (int i = 0; i < w->count; i++)
            if (w->ships[i].alive && at_yard(w, i)) repair_alongside(w, i, dock);
    }
}

/* Can side still hurt the enemy: a gun, tube or rack with ammunition, or a strike
 * aircraft not lost? */
static int can_fight(const world *w, int side)
{
    int enemy_subs = 0;
    for (int j = 0; j < w->count; j++)
        enemy_subs |= w->ships[j].alive && w->ships[j].side != side && w->ships[j].cls->submerged;
    for (int i = 0; i < w->count; i++) {
        const ship *s = &w->ships[i];
        if (!s->alive || s->side != side) continue;
        for (int m = 0; m < s->cls->mount_count; m++) {
            ammo_kind a = s->cls->weapons[s->cls->mounts[m].weapon].ammo;
            if (s->turrets[m].ammo <= 0 || a == AMMO_NONE || module_health(s, MOD_TURRET, m) <= 0) continue;
            if (a == AMMO_DC && !enemy_subs) continue;       /* racks can't hurt surface ships */
            return 1;
        }
    }
    for (int k = 0; k < w->air_count; k++)
        if (w->air[k].side == side && is_strike_plane(w->air[k].type) && w->air[k].state != AIR_LOST &&
            w->air[k].state != AIR_FALLING) return 1;
    return 0;
}

int world_winner(const world *w)
{
    /* Neither side has anything left to fight with: a draw (2). */
    if (!can_fight(w, 0) && !can_fight(w, 1)) {
        int afloat = 0;
        for (int i = 0; i < w->count; i++) afloat += w->ships[i].alive;
        if (afloat) return 2;
    }
    int alive[2] = {0, 0};
    for (int i = 0; i < w->count; i++)
        if (w->ships[i].alive && !is_shore(&w->ships[i])) alive[w->ships[i].side & 1]++;
    int yard_up[2];
    for (int s = 0; s < 2; s++) {
        int y = w->sides[s].yard;
        yard_up[s] = y >= 0 && w->ships[y].alive;
    }
    switch (w->mode) {
    case MODE_ASSAULT:
        /* First to destroy the other's shipyard; a side whose yard stands can rebuild. */
        if (!yard_up[0] && yard_up[1]) return 1;
        if (!yard_up[1] && yard_up[0]) return 0;
        if (!yard_up[0] && !yard_up[1]) return alive[0] > alive[1] ? 0 : alive[1] > alive[0] ? 1 : -1;
        if (w->time_limit > 0 && mode_time_left(w) <= 0)   /* on time: points */
            return w->score[0] > w->score[1] ? 0 : w->score[1] > w->score[0] ? 1 : -1;
        return -1;
    case MODE_DEFEND: {
        int d = w->defender & 1, a = 1 - d;
        if (!yard_up[d]) return a;
        if (!alive[a] || mode_time_left(w) <= 0) return d;
        return -1;
    }
    default: {
        int r = alive[0] && !alive[1] ? 0 : alive[1] && !alive[0] ? 1 : -1;
        /* A safety net for chases nobody can finish: after 90 minutes, points
         * (the value of enemy ships sunk) decide; level points are a draw. */
        if (r < 0 && (float)w->tick * SIM_DT >= SKIRMISH_LIMIT)
            r = w->score[0] > w->score[1] ? 0 : w->score[1] > w->score[0] ? 1 : 2;
        return r;
    }
    }
}
