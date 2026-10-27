/* Captain's reports: what each ship's captain recommends doing right now, so the
 * player can act on a ship without selecting it. Raised from the ship's state; acting
 * on one (or dismissing it) quiets that kind of report for a while. */
#include <math.h>

#include "math3d.h"
#include "sim_internal.h"

#define FIRE_NEAR_MAGAZINE 0.25f
#define LIST_WARN (6 * PI_F / 180)
#define ACT_COOLDOWN 45.0f
#define DISMISS_COOLDOWN 60.0f

_Static_assert(REP_KINDS <= REP_KINDS_MAX, "ship.report_cool too small");

static int has_magazine(const ship *s, int k)
{
    for (int m = 0; m < s->cls->module_count; m++)
        if (s->cls->modules[m].kind == MOD_MAGAZINE && s->cls->modules[m].comp == k) return 1;
    return 0;
}

static int nearest_enemy(const world *w, int i, int surface_only, float *dist)
{
    const ship *s = &w->ships[i];
    int best = -1;
    float bd = INFINITY;
    for (int j = 0; j < w->count; j++) {
        const ship *t = &w->ships[j];
        if (!t->alive || t->side == s->side || (surface_only && t->cls->submerged)) continue;
        float d = hypotf(t->x - s->x, t->z - s->z);
        if (d < bd) { bd = d; best = j; }
    }
    if (dist) *dist = bd;
    return best;
}

static int add(captain_report *out, int n, int max, int ship, report_kind kind, float urgency)
{
    if (n >= max) return n;
    out[n].ship = ship;
    out[n].kind = kind;
    out[n].urgency = urgency;
    return n + 1;
}

int captain_reports(const world *w, int side, captain_report *out, int max)
{
    int n = 0;
    for (int i = 0; i < w->count; i++) {
        const ship *s = &w->ships[i];
        if (!s->alive || s->side != side) continue;
        const float *cool = s->report_cool;
        /* Bridge destroyed: no captain to report. The bar says so, once, low down. */
        if (!s->cls->submerged && module_health(s, MOD_BRIDGE, -1) <= 0) {
            n = add(out, n, max, i, REP_BRIDGE_LOST, 1);
            continue;
        }
        /* A fire next to a dry magazine: flood it before she blows up. */
        float worst = 0;
        int fires = 0, counter = 0;
        for (int k = 0; k < s->cls->comp_count; k++) {
            const compartment *cp = &s->comp[k];
            fires += cp->fire > 0;
            counter |= cp->counter_flood >= 0;
            if (cp->fire > FIRE_NEAR_MAGAZINE && !cp->mag_flooded && has_magazine(s, k)) worst = fmaxf(worst, cp->fire);
        }
        if (worst > 0 && cool[REP_FLOOD_MAGAZINE] <= 0) n = add(out, n, max, i, REP_FLOOD_MAGAZINE, 100 + 50 * worst);
        float comb;
        if (s->evade <= 0 && cool[REP_EVADE_TORPEDO] <= 0 && torpedo_threat(w, i, &comb))
            n = add(out, n, max, i, REP_EVADE_TORPEDO, 95);
        if (fabsf(s->list) > LIST_WARN && !counter && cool[REP_COUNTER_FLOOD] <= 0)
            n = add(out, n, max, i, REP_COUNTER_FLOOD, 60 + fabsf(s->list) * 180 / PI_F);
        if (s->water > 0.4f * s->cls->reserve_buoyancy && s->policy != DC_FLOODING && cool[REP_FIGHT_FLOODING] <= 0)
            n = add(out, n, max, i, REP_FIGHT_FLOODING, 50 + 50 * s->water / s->cls->reserve_buoyancy);
        if (fires >= 2 && s->policy != DC_FIRES && cool[REP_FIGHT_FIRES] <= 0)
            n = add(out, n, max, i, REP_FIGHT_FIRES, 40 + 5 * (float)fires);
        float d;
        if (s->hp < 0.3f * s->cls->max_health && cool[REP_WITHDRAW] <= 0 && nearest_enemy(w, i, 1, &d) >= 0 && d < 3500)
            n = add(out, n, max, i, REP_WITHDRAW, 30);
        int y = w->sides[side & 1].yard;
        if (!is_shore(s) && y >= 0 && w->ships[y].alive && s->hp < 0.5f * s->cls->max_health && !at_yard(w, i) &&
            module_health(&w->ships[y], MOD_DRYDOCK, -1) >= 0.3f && cool[REP_RETURN_TO_YARD] <= 0 &&
            hypotf(w->ships[y].x - s->x, w->ships[y].z - s->z) > repair_radius(&w->ships[y]))
            n = add(out, n, max, i, REP_RETURN_TO_YARD, 25);
        if (is_carrier(s) && !s->ai && cool[REP_LAUNCH_STRIKE] <= 0 && strike_ready(w, i) >= 3 &&
            nearest_enemy(w, i, 1, &d) >= 0 && d < 12000)
            n = add(out, n, max, i, REP_LAUNCH_STRIKE, 20);
    }
    /* Most urgent first (insertion sort: a handful of entries). */
    for (int a = 1; a < n; a++)
        for (int b = a; b > 0 && out[b].urgency > out[b - 1].urgency; b--) {
            captain_report tmp = out[b];
            out[b] = out[b - 1];
            out[b - 1] = tmp;
        }
    return n;
}

void captain_act(world *w, const captain_report *r)
{
    ship *s = &w->ships[r->ship];
    if (!s->alive) return;
    int id = r->ship;
    switch (r->kind) {
    case REP_FLOOD_MAGAZINE:
        for (int k = 0; k < s->cls->comp_count; k++)
            if (s->comp[k].fire > FIRE_NEAR_MAGAZINE) order_flood_magazine(w, id, k);
        break;
    case REP_EVADE_TORPEDO: order_evade(w, &id, 1, 40); break;
    case REP_COUNTER_FLOOD: order_counter_flood(w, &id, 1); break;
    case REP_FIGHT_FLOODING: order_dc_policy(w, &id, 1, DC_FLOODING); break;
    case REP_FIGHT_FIRES: order_dc_policy(w, &id, 1, DC_FIRES); break;
    case REP_WITHDRAW: {
        float d;
        int e = nearest_enemy(w, id, 1, &d);
        if (e >= 0) {
            float away = atan2f(s->x - w->ships[e].x, s->z - w->ships[e].z);
            order_move(w, &id, 1, s->x + sinf(away) * 3000, s->z + cosf(away) * 3000, 0);
            order_evade(w, &id, 1, 60);
        }
        break;
    }
    case REP_LAUNCH_STRIKE: {
        float d;
        order_strike(w, id, nearest_enemy(w, id, 1, &d));
        break;
    }
    case REP_BRIDGE_LOST: return;              /* nothing to do */
    case REP_RETURN_TO_YARD: {
        const ship *yd = &w->ships[w->sides[s->side & 1].yard];
        float fwd = yd->cls->half_length + 450;
        order_move(w, &id, 1, yd->x + sinf(yd->heading) * fwd, yd->z + cosf(yd->heading) * fwd, 0);
        break;
    }
    default: break;
    }
    s->report_cool[r->kind] = ACT_COOLDOWN;
}

void captain_dismiss(world *w, const captain_report *r)
{
    w->ships[r->ship].report_cool[r->kind] = DISMISS_COOLDOWN;
}
