/* Cross-section damage model (design; the original has one HP pool per ship).
 *
 * The hull is a row of compartments stern to bow, each split port / starboard for
 * flooding. A hit lands in one compartment and either penetrates (belt for flat
 * fire, deck for plunging fire and bombs) or doesn't. Penetrations wreck structure,
 * knock out the modules there, start fires and, at the waterline, open leaks.
 * Water spreads the list; fires spread and cook off magazines. A ship goes down
 * when the water exceeds her reserve buoyancy, she capsizes from list, a magazine
 * goes up, or her structure fails. Damage-control teams fight fires, plug leaks,
 * pump and patch modules, by a priority policy.
 *
 * Units: structure and module hp are 0..1; flood is the fraction of a half-compartment
 * full; leak is that fraction per second; armour and shell_pen share one scale
 * (a 16in shell at point blank is ~3.6, a 5in ~1.6). */
#include <math.h>

#include "math3d.h"
#include "sim_internal.h"

#define COMP_HP_SCALE 1.2f     /* one compartment's structure = max_health / n * this */
#define CAPSIZE_LIST 0.13f     /* list fraction (~22 deg) at which she rolls over */
#define LIST_TO_RAD 3.0f
#define AUTO_COUNTER 0.058f    /* list fraction (~10 deg) at which the crew counter-floods unasked */
#define STRUCTURE_FAIL 0.2f
#define BROKEN_BACK 0.55f      /* fraction of compartments wrecked that breaks the hull */
#define POWER_PUMPS 0.0012f    /* per second, every compartment, while machinery runs */
#define CREW_PUMP 0.004f
#define CREW_PATCH 0.0025f     /* leak plugged per second per team */
#define CREW_FIRE 0.03f
#define CREW_REPAIR 0.01f
#define REPAIR_CAP 0.7f        /* field repairs bring a module back this far */
#define FIRE_GROWTH 0.012f
#define FIRE_SPREAD 0.03f      /* chance per second for a big fire to reach a neighbour */
#define MAG_COOKOFF 0.004f     /* chance per second at full fire next to a dry magazine */
#define MAG_HIT 0.02f          /* chance a penetrating 90-damage hit sets the magazine off */
#define CRITICAL 0.1f          /* a module knocked below this is lost for the battle */
#define ABANDON 0.08f          /* crew fraction at which she is abandoned */

static float toughness(module_kind k)
{
    switch (k) {
    case MOD_TURRET: return 180;
    case MOD_ENGINE: return 220;
    case MOD_STEERING: return 120;
    case MOD_BRIDGE: return 110;
    case MOD_RADAR: return 60;
    case MOD_AA: return 90;
    case MOD_FLIGHT_DECK: return 160;
    case MOD_HANGAR_LIFT: return 100;
    default: return 1e9f;
    }
}

void damage_init(ship *s)
{
    for (int k = 0; k < MAX_COMPARTMENTS; k++) {
        compartment *c = &s->comp[k];
        c->structure = 1;
        c->flood[0] = c->flood[1] = c->leak[0] = c->leak[1] = c->fire = 0;
        c->crews = c->mag_flooded = 0;
        c->counter_flood = -1;
    }
    for (int m = 0; m < MAX_MODULES; m++) { s->module_hp[m] = 1; s->module_lost[m] = 0; }
    for (int b = 0; b < BATTERIES; b++) s->aim[b] = -1;
    s->crew = s->cls->complement;
    s->evade = 0;
    s->list = s->trim = s->water = 0;
    s->jammed = 0;
    s->cause = 0;
}

float shell_pen(const class_weapon *wp, float dist)
{
    /* Penetration grows with calibre (damage as proxy) and falls off with range. */
    float r = wp->max_range > 0 ? clampf(dist / wp->max_range, 0, 1) : 0;
    return wp->damage / 25.0f * (1 - 0.35f * r);
}

float module_health(const ship *s, module_kind kind, int mount)
{
    float sum = 0;
    int n = 0;
    for (int m = 0; m < s->cls->module_count; m++) {
        const module_def *d = &s->cls->modules[m];
        if (d->kind != kind || (mount >= 0 && d->mount != mount)) continue;
        sum += s->module_hp[m];
        n++;
    }
    return n ? sum / (float)n : 1;
}

float condition(const ship *s)
{
    if (!s->alive) return 0;
    int n = s->cls->comp_count;
    if (n == 0) return 1;
    float st = 0;
    for (int k = 0; k < n; k++) st += s->comp[k].structure;
    st /= (float)n;
    float structural = (st - STRUCTURE_FAIL) / (1 - STRUCTURE_FAIL);
    /* Broken back: too much of the hull wrecked, however sound the ends are. */
    int wrecked = 0;
    for (int k = 0; k < n; k++) wrecked += s->comp[k].structure < 0.05f;
    structural = fminf(structural, 1 - (float)wrecked / (BROKEN_BACK * (float)n));
    float buoyancy = 1 - s->water / s->cls->reserve_buoyancy;
    float listing = 1 - fabsf(s->list) / (CAPSIZE_LIST * LIST_TO_RAD);
    return clampf(fminf(structural, fminf(buoyancy, listing)), 0, 1);
}

float crew_factor(const ship *s)
{
    return s->cls->complement > 0 ? clampf(s->crew / s->cls->complement, 0, 1) : 1;
}

/* Casualties: `share` of the men in one compartment. */
static void casualties(ship *s, float share)
{
    s->crew = fmaxf(0, s->crew - s->cls->complement / (float)s->cls->comp_count * share);
}

static void hurt_module(ship *s, int m, float amount)
{
    s->module_hp[m] = fmaxf(0, s->module_hp[m] - amount);
    if (s->module_hp[m] < CRITICAL) { s->module_hp[m] = 0; s->module_lost[m] = 1; }
}

static int comp_at(const ship_class *c, float z)
{
    int k = 0;
    while (k < c->comp_count - 1 && z > c->comp_z[k + 1]) k++;
    return k;
}

static int has_magazine(const ship *s, int k)
{
    for (int m = 0; m < s->cls->module_count; m++)
        if (s->cls->modules[m].kind == MOD_MAGAZINE && s->cls->modules[m].comp == k) return 1;
    return 0;
}

static void go_down(world *w, int j, int cause)
{
    ship *s = &w->ships[j];
    if (!s->alive) return;
    s->alive = 0;
    s->hp = 0;
    s->stage = 3;
    s->wp_count = 0;
    s->cause = cause;
    w->score[1 - (s->side & 1)] += class_cost(s->cls);
    fx(w, cause == 3 ? FX_MAGAZINE : FX_SINK, s->x, 0, s->z);
    if (cause == 3) w->magazine_explosions++;
}

static void ignite(world *w, ship *s, int k, float amount)
{
    compartment *c = &s->comp[k];
    if (c->fire <= 0) w->fires_started++;
    c->fire = fminf(1, c->fire + amount);
}

/* Structural damage to compartment k; what a wrecked compartment can't absorb carries
 * into its neighbours, and a wrecked compartment that keeps being hit is holed. */
static void wreck(ship *s, int k, float amount, int side)
{
    compartment *cp = &s->comp[k];
    float over = amount - cp->structure;
    cp->structure = fmaxf(0, cp->structure - amount);
    if (over <= 0) return;
    cp->leak[side] += over * 0.02f;
    /* Half each way, passing through already-wrecked compartments. */
    int n = s->cls->comp_count;
    for (int dir = -1; dir <= 1; dir += 2) {
        float rest = over * 0.5f;
        for (int q = k + dir; q >= 0 && q < n && rest > 0; q += dir) {
            float took = fminf(rest, s->comp[q].structure);
            s->comp[q].structure -= took;
            rest -= took;
        }
    }
}

static void hit_modules(world *w, ship *s, int k, float dmg, int exposed_only)
{
    for (int m = 0; m < s->cls->module_count; m++) {
        const module_def *d = &s->cls->modules[m];
        if (d->comp != k || d->kind == MOD_MAGAZINE) continue;
        int exposed = d->kind == MOD_RADAR || d->kind == MOD_AA || d->kind == MOD_BRIDGE || d->kind == MOD_TURRET;
        if (exposed_only && !exposed) continue;
        float chance = exposed_only ? (d->kind == MOD_TURRET ? 0.08f : 0.25f) : 0.5f;
        if (rand01(w) < chance) hurt_module(s, m, dmg * (exposed_only ? 0.5f : 1) / toughness(d->kind));
    }
}

void damage_hit(world *w, int j, hit_kind kind, float x, float z, float dmg, float pen, int plunging)
{
    ship *s = &w->ships[j];
    if (!s->alive) return;
    const ship_class *c = s->cls;
    float dx = x - s->x, dz = z - s->z, cs = cosf(s->heading), sn = sinf(s->heading);
    float along = dx * sn + dz * cs, across = dx * cs - dz * sn;
    int k = comp_at(c, along), side = across >= 0;
    compartment *cp = &s->comp[k];
    float comp_hp = c->max_health / (float)c->comp_count * COMP_HP_SCALE;
    if (is_shore(s)) {                                   /* nothing to flood ashore */
        if (kind == HIT_NEAR_MISS) return;
        if (kind == HIT_TORPEDO) {                       /* blasts the quay */
            kind = HIT_SHELL;
            pen = 9;
            plunging = 0;
        }
    }

    /* Flat fire strikes the hull at a height: the upperworks (thin plating, the
     * bridge, radar and AA, no flooding), the belt, or below the waterline (flooding). */
    int zone = 1;                                        /* 0 upperworks, 1 belt, 2 underwater */
    if (kind == HIT_SHELL && !plunging && !is_shore(s)) {
        float u = rand01(w);
        zone = u < 0.3f ? 0 : u < 0.85f ? 1 : 2;
    }
    switch (kind) {
    case HIT_SHELL: case HIT_BOMB: {
        float armour = plunging || kind == HIT_BOMB ? c->deck : zone == 0 ? c->deck * 0.5f : zone == 2 ? c->belt * 0.8f : c->belt;
        if (zone == 0) {                                 /* upperworks: wreck what's up there */
            w->upperworks++;
            wreck(s, k, dmg * 0.5f / comp_hp, side);
            hit_modules(w, s, k, dmg * 1.4f, 1);
            casualties(s, 0.12f * dmg / comp_hp);
            if (rand01(w) < 0.2f) ignite(w, s, k, 0.25f);
            break;
        }
        int through = pen * (0.85f + 0.3f * rand01(w)) > armour;
        if (!through) {                                  /* splinters on the upperworks */
            w->ricochets++;
            wreck(s, k, dmg * 0.12f / comp_hp, side);
            hit_modules(w, s, k, dmg, 1);
            casualties(s, 0.02f * dmg / comp_hp);
            if (rand01(w) < 0.05f) ignite(w, s, k, 0.2f);
            fx(w, FX_RICOCHET, x, 10, z);
            break;
        }
        w->penetrations++;
        wreck(s, k, dmg / comp_hp, side);
        hit_modules(w, s, k, dmg, 0);
        casualties(s, 0.25f * dmg / comp_hp);
        if (rand01(w) < (kind == HIT_BOMB ? 0.5f : 0.25f) * fminf(dmg / 60.0f, 1.5f)) ignite(w, s, k, 0.3f);
        if (!plunging && kind == HIT_SHELL && rand01(w) < (zone == 2 ? 0.8f : 0.3f)) cp->leak[side] += dmg / (zone == 2 ? 1500.0f : 3000.0f);
        if (has_magazine(s, k) && !cp->mag_flooded && rand01(w) < MAG_HIT * dmg / 90.0f) go_down(w, j, 3);
        break;
    }
    case HIT_NEAR_MISS:
        if (rand01(w) < 0.3f) cp->leak[side] += dmg / 8000.0f;
        hit_modules(w, s, k, dmg * 0.5f, 1);
        break;
    case HIT_TORPEDO: {
        float bulge = c->belt >= 2 ? 0.6f : 1.0f;        /* capital ships' torpedo protection */
        cp->leak[side] += dmg / 800.0f * bulge;
        if (k > 0) s->comp[k - 1].leak[side] += dmg / 2500.0f * bulge;
        if (k < c->comp_count - 1) s->comp[k + 1].leak[side] += dmg / 2500.0f * bulge;
        wreck(s, k, dmg * 0.5f / comp_hp, side);
        hit_modules(w, s, k, dmg * 0.6f, 0);
        casualties(s, 0.15f * dmg / comp_hp);
        if (rand01(w) < 0.15f) ignite(w, s, k, 0.3f);
        if (has_magazine(s, k) && !cp->mag_flooded && rand01(w) < MAG_HIT) go_down(w, j, 3);
        break;
    }
    case HIT_DEPTH_CHARGE:
        cp->leak[side] += dmg / 400.0f;
        wreck(s, k, dmg * 0.6f / comp_hp, side);
        hit_modules(w, s, k, dmg, 0);
        break;
    }
    if (is_shore(s)) for (int q = 0; q < c->comp_count; q++) s->comp[q].leak[0] = s->comp[q].leak[1] = 0;
    if (s->alive) {
        s->hp = condition(s) * c->max_health;
        float f = s->hp / c->max_health;
        s->stage = f < 1.0f / 3 ? 3 : f < 2.0f / 3 ? 2 : 1;
        if (s->hp <= 0) go_down(w, j, 4);
    }
}

/* ---- damage control ------------------------------------------------------------- */

static void assign_crews(ship *s)
{
    const ship_class *c = s->cls;
    static const float weight[DC_POLICIES][3] = {   /* fire, flooding, repair */
        {1, 1, 1}, {3, 0.5f, 0.5f}, {0.5f, 3, 0.5f}, {0.7f, 0.7f, 2.5f}};
    float score[MAX_COMPARTMENTS] = {0};
    for (int k = 0; k < c->comp_count; k++) {
        const compartment *cp = &s->comp[k];
        float fire = cp->fire * (has_magazine(s, k) && !cp->mag_flooded ? 6 : 2);
        float flood = (cp->leak[0] + cp->leak[1]) * 400 + (cp->flood[0] + cp->flood[1]) * 0.5f;
        float repair = 0;
        for (int m = 0; m < c->module_count; m++) {
            const module_def *d = &c->modules[m];
            if (d->comp != k || d->kind == MOD_MAGAZINE || s->module_lost[m] || s->module_hp[m] >= REPAIR_CAP) continue;
            float wgt = d->kind == MOD_ENGINE || d->kind == MOD_STEERING ? 1.5f
                      : d->kind == MOD_TURRET || d->kind == MOD_RADAR ? (s->policy == DC_WEAPONS ? 1.2f : 0.6f) : 0.4f;
            repair += (REPAIR_CAP - s->module_hp[m]) * wgt;
        }
        const float *pw = weight[s->policy];
        score[k] = (fire * pw[0] + flood * pw[1] + repair * pw[2]) * (cp->priority ? 4 : 1) + (cp->priority ? 0.5f : 0);
        s->comp[k].crews = 0;
    }
    /* Short-handed ships man fewer teams. */
    int teams = (int)ceilf((float)c->dc_crews * fminf(1, crew_factor(s) * 1.4f))
              + (int)officer_level(s, OFF_DAMAGE_CONTROL);      /* a DC officer organises another team */
    for (int t = 0; t < teams; t++) {
        int best = -1;
        float bs = 0.01f;
        for (int k = 0; k < c->comp_count; k++) {
            float v = score[k] / (float)(1 + s->comp[k].crews);
            if (v > bs) { bs = v; best = k; }
        }
        if (best < 0) break;
        s->comp[best].crews++;
    }
}

void damage_step(world *w, int i)
{
    ship *s = &w->ships[i];
    const ship_class *c = s->cls;
    int n = c->comp_count;
    if (!s->alive || n == 0) return;
    float dt = SIM_DT;
    s->dc_timer -= dt;
    for (int r = 0; r < REP_KINDS_MAX; r++) s->report_cool[r] = fmaxf(0, s->report_cool[r] - dt);
    if (s->dc_timer <= 0) { assign_crews(s); s->dc_timer = 1.0f; }
    float machinery = module_health(s, MOD_ENGINE, -1);
    if (is_shore(s))
        for (int k = 0; k < n; k++) {
            s->comp[k].leak[0] = s->comp[k].leak[1] = s->comp[k].flood[0] = s->comp[k].flood[1] = 0;
            s->comp[k].counter_flood = -1;
        }
    float water = 0, list = 0, trim = 0;
    for (int k = 0; k < n; k++) {
        compartment *cp = &s->comp[k];
        float teams = (float)cp->crews * (0.5f + 0.5f * crew_factor(s))    /* tired, short-handed */
                    * (1 + 0.4f * officer_level(s, OFF_DAMAGE_CONTROL)) * (s->flagship ? 1.2f : 1);
        float was = cp->flood[0] + cp->flood[1];
        for (int sd = 0; sd < 2; sd++) {
            /* Leaks fill the space that's still dry; teams plug, pumps drain. */
            cp->flood[sd] += cp->leak[sd] * dt * (1 - cp->flood[sd]);
            cp->leak[sd] = fmaxf(0, cp->leak[sd] - CREW_PATCH * teams * dt);
            float pump = (machinery > 0.1f ? POWER_PUMPS : 0) + CREW_PUMP * teams;
            if (cp->counter_flood == sd) cp->flood[sd] += 0.02f * dt;
            else cp->flood[sd] -= pump * dt * (cp->leak[sd] > 0 ? 0.3f : 1);
            /* A wrecked, flooded compartment spills into its neighbours. */
            if (cp->flood[sd] > 0.85f && cp->structure < 0.5f) {
                if (k > 0) s->comp[k - 1].flood[sd] += 0.004f * dt;
                if (k < n - 1) s->comp[k + 1].flood[sd] += 0.004f * dt;
            }
            cp->flood[sd] = clampf(cp->flood[sd], 0, 1);
        }
        /* Men caught by the water, and by the fire. */
        float rose = cp->flood[0] + cp->flood[1] - was;
        if (rose > 0 && cp->counter_flood < 0) casualties(s, rose * 0.1f);
        if (cp->fire > 0) casualties(s, 0.003f * cp->fire * dt);
        /* Counter-flooding stops once the list is gone. */
        if (cp->counter_flood >= 0 && fabsf(s->list) < 0.01f) cp->counter_flood = -1;
        /* Fire grows, wrecks, spreads; water and teams put it out. */
        if (cp->fire > 0) {
            if (cp->flood[0] + cp->flood[1] > 1.0f || cp->structure <= 0.02f) cp->fire = 0;
            cp->fire = clampf(cp->fire + FIRE_GROWTH * dt - CREW_FIRE * teams * dt, 0, 1);
            cp->structure = fmaxf(0, cp->structure - 0.002f * cp->fire * dt);
            for (int m = 0; m < c->module_count; m++)
                if (c->modules[m].comp == k && c->modules[m].kind != MOD_MAGAZINE && !s->module_lost[m])
                    hurt_module(s, m, 0.006f * cp->fire * dt);
            if (cp->fire > 0.6f && rand01(w) < FIRE_SPREAD * dt) {
                int nb = k + (rand01(w) < 0.5f ? -1 : 1);
                if (nb >= 0 && nb < n) ignite(w, s, nb, 0.15f);
            }
            if (has_magazine(s, k) && !cp->mag_flooded && cp->fire > 0.4f && rand01(w) < MAG_COOKOFF * cp->fire * dt) {
                go_down(w, i, 3);
                return;
            }
        }
        /* Field repairs up to 70%; a module past the critical point stays lost. */
        if (teams > 0)
            for (int m = 0; m < c->module_count; m++) {
                if (c->modules[m].comp != k || c->modules[m].kind == MOD_MAGAZINE || s->module_lost[m]) continue;
                if (s->module_hp[m] < REPAIR_CAP)
                    s->module_hp[m] = fminf(REPAIR_CAP, s->module_hp[m] + CREW_REPAIR * teams * dt);
            }
        float wk = (cp->flood[0] + cp->flood[1]) / 2;
        water += wk;
        list += cp->flood[1] - cp->flood[0];
        trim += wk * ((float)k + 0.5f - (float)n / 2) / ((float)n / 2);
    }
    s->water = water / (float)n;
    float list_frac = list / (2 * (float)n);
    s->list = list_frac * LIST_TO_RAD;
    s->trim = trim / (float)n * 0.6f;
    s->jammed = module_health(s, MOD_STEERING, -1) < 0.25f;
    if (fabsf(list_frac) > AUTO_COUNTER && !is_shore(s)) {
        int i1 = i;
        order_counter_flood(w, &i1, 1);
    }
    s->hp = condition(s) * c->max_health;
    float f = s->hp / c->max_health;
    s->stage = f < 1.0f / 3 ? 3 : f < 2.0f / 3 ? 2 : 1;
    if (crew_factor(s) < ABANDON) go_down(w, i, 5);
    else if (s->water >= c->reserve_buoyancy) go_down(w, i, 1);
    else if (fabsf(list_frac) > CAPSIZE_LIST) go_down(w, i, 2);
    else if (s->hp <= 0) go_down(w, i, 4);
}

/* ---- orders ------------------------------------------------------------------------ */

void order_dc_policy(world *w, const int *ids, int n, dc_policy p)
{
    for (int i = 0; i < n; i++) w->ships[ids[i]].policy = p;
}

void order_flood_magazine(world *w, int ship_id, int k)
{
    ship *s = &w->ships[ship_id];
    if (!has_magazine(s, k) || s->comp[k].mag_flooded) return;
    s->comp[k].mag_flooded = 1;
    s->comp[k].flood[0] = fmaxf(s->comp[k].flood[0], 0.5f);
    s->comp[k].flood[1] = fmaxf(s->comp[k].flood[1], 0.5f);
}

void order_flood_magazines(world *w, const int *ids, int n)
{
    for (int i = 0; i < n; i++)
        for (int k = 0; k < w->ships[ids[i]].cls->comp_count; k++) order_flood_magazine(w, ids[i], k);
}

void order_dc_priority(world *w, int ship_id, int k)
{
    ship *s = &w->ships[ship_id];
    if (k < 0 || k >= s->cls->comp_count) return;
    s->comp[k].priority = !s->comp[k].priority;
    s->dc_timer = 0;                                     /* reassign now */
}

void order_counter_flood(world *w, const int *ids, int n)
{
    for (int i = 0; i < n; i++) {
        ship *s = &w->ships[ids[i]];
        if (fabsf(s->list) < 0.01f) continue;
        int dry = s->list > 0 ? 0 : 1;                  /* listing to starboard: flood port */
        for (int k = 0; k < s->cls->comp_count; k++)
            if (s->comp[k].flood[dry] < 0.6f && !s->comp[k].mag_flooded) s->comp[k].counter_flood = dry;
    }
}
