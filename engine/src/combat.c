/* Fire control, projectiles and damage. Called from world_step at the fixed tick.
 *
 * Turrets train inside their arcs at a calibre-dependent rate and fire when on
 * bearing and loaded. Shells fly analytic ballistic arcs at a lead point with a
 * Gaussian aim error; whatever hull the shell lands in takes the hit, so aspect,
 * evasive turns and friendly fire all fall out of the geometry. */
#include <math.h>
#include <string.h>

#include "math3d.h"
#include "sim_internal.h"

#define DEG (PI_F / 180.0f)
#define ON_BEARING (1.5f * DEG)
#define RETARGET_S 2.0f
#define LEAD_ERROR 0.15f       /* fraction of target speed misjudged when leading */
#define TORP_SPREAD (3.0f * DEG)
#define TORP_ARM_S 3.0f        /* a torpedo can't hit its own side before this */
#define DC_SINK_S 2.0f         /* depth charge: seconds from splash to detonation */

/* ---- random numbers (xorshift64*, per world: deterministic) ---------------------- */

void world_seed(world *w, unsigned long long seed)
{
    w->rng = seed ? seed : 0x9e3779b97f4a7c15ULL;
}

float rand01(world *w)
{
    w->rng ^= w->rng >> 12;
    w->rng ^= w->rng << 25;
    w->rng ^= w->rng >> 27;
    return (float)((w->rng * 0x2545f4914f6cdd1dULL) >> 40) / (float)(1 << 24);
}

float gauss(world *w)
{
    float u = fmaxf(rand01(w), 1e-7f), v = rand01(w);
    return sqrtf(-2.0f * logf(u)) * cosf(2 * PI_F * v);
}

/* ---- geometry ---------------------------------------------------------------------- */

static void mount_world(const ship *s, const turret_mount *m, float *x, float *y, float *z)
{
    float c = cosf(s->heading), sn = sinf(s->heading);
    *x = s->x + m->pos[0] * c + m->pos[2] * sn;
    *y = m->pos[1];
    *z = s->z - m->pos[0] * sn + m->pos[2] * c;
}

/* Distance from (x, z) to ship s's hull outline, 0 inside. The footprint is the model's
 * bbox with the beam tapering toward bow and stern. */
float hull_distance(const ship *s, float x, float z)
{
    float dx = x - s->x, dz = z - s->z, c = cosf(s->heading), sn = sinf(s->heading);
    float along = fabsf(dx * sn + dz * c), across = fabsf(dx * c - dz * sn);
    float L = s->cls->half_length, u = fminf(along / L, 1);
    float B = s->cls->half_beam * (1 - 0.6f * u * u * u);
    return hypotf(fmaxf(along - L, 0), fmaxf(across - B, 0));
}

/* Time for a projectile at speed v from (mx, mz) to meet a target at P moving at V. */
float intercept(float mx, float mz, float px, float pz, float vx, float vz, float v)
{
    float dx = px - mx, dz = pz - mz;
    float a = vx * vx + vz * vz - v * v, b = 2 * (dx * vx + dz * vz), c = dx * dx + dz * dz;
    if (a > -1e-3f) return sqrtf(c) / v;        /* target as fast as the projectile */
    float disc = b * b - 4 * a * c;
    return (-b - sqrtf(fmaxf(disc, 0))) / (2 * a);
}

static float traverse_rate(const class_weapon *w)
{
    if (w->ammo == AMMO_TORPEDO) return 20 * DEG;
    if (w->ammo == AMMO_DC) return 360 * DEG;
    return w->damage >= 80 ? 5 * DEG : w->damage >= 55 ? 8 * DEG : 15 * DEG;
}

float shell_speed(const class_weapon *w)
{
    /* Big guns lob (12 s to 3200), small ones are flatter and quicker. */
    return w->max_range >= 3000 ? 260.0f : w->max_range >= 2500 ? 290.0f : 330.0f;
}

float hit_chance(int fh, int th, float d)
{
    /* Range bands and hull counters as read from the original (notes/battle-logic.md);
     * flagships and boss hulls count as their base types here. */
    int small = fh == 6 || fh == 7 || fh == 15;
    int big = th == 1 || th == 2 || th == 3 || th == 14 || th == 17 || th == 20 || th == 21;
    float p = d < 2400 ? 75 : d < 4800 ? 55 : d < 6000 ? 45 : 15;
    if (small && big) p -= 40;
    else if (small && (th == 12 || th == 13)) p += 40;
    else if (fh == 1 && (th == 2 || th == 3)) p += 15;
    else if (fh == 1 && (th == 1 || th == 5)) p -= 15;
    else p += 10;
    return clampf(p, 10, 95);
}

/* ---- events and damage ------------------------------------------------------------- */

void fx(world *w, fx_kind kind, float x, float y, float z)
{
    fx_mag(w, kind, x, y, z, 0);
}

void fx_mag(world *w, fx_kind kind, float x, float y, float z, float mag)
{
    fx_event *e = &w->fx[w->fx_head];
    e->mag = mag;
    e->kind = kind;
    e->x = x;
    e->y = y;
    e->z = z;
    e->tick = w->tick;
    w->fx_head = (w->fx_head + 1) % MAX_FX;
}

/* A mount whose turret is wrecked, or whose magazine is flooded, can't fire. */
static int mount_ready(const ship *s, int m)
{
    if (module_health(s, MOD_TURRET, m) <= 0) return 0;
    const turret_mount *mt = &s->cls->mounts[m];
    if (mt->bat != BAT_MAIN && mt->bat != BAT_TORPEDO) return 1;
    for (int q = 0; q < s->cls->module_count; q++) {
        const module_def *d = &s->cls->modules[q];
        if (d->kind == MOD_TURRET && d->mount == m) return !s->comp[d->comp].mag_flooded;
    }
    return 1;
}

/* Fire-control quality: radar and bridge damage widen the spread. */
static float fire_control_factor(const ship *s)
{
    return (module_health(s, MOD_RADAR, -1) < 0.3f ? 1.4f : 1) * (module_health(s, MOD_BRIDGE, -1) < 0.3f ? 1.2f : 1);
}

/* ---- accuracy ---------------------------------------------------------------------- */

/* Where in the target's hull a macro target sits (model space z), or 0 = amidships. */
static float aim_offset(const ship *t, int kind)
{
    if (kind < 0) return 0;
    const ship_class *c = t->cls;
    for (int pass = 0; pass < 2; pass++)              /* prefer one that still works */
        for (int m = 0; m < c->module_count; m++) {
            const module_def *d = &c->modules[m];
            if ((int)d->kind != kind || (pass == 0 && t->module_hp[m] <= 0)) continue;
            if (kind == MOD_TURRET && c->mounts[d->mount].bat != BAT_MAIN) continue;
            return (c->comp_z[d->comp] + c->comp_z[d->comp + 1]) / 2;
        }
    return 0;
}

/* Aim point (lead included) and the spread around it, per axis, for one shell from
 * mount m of ship i at ship j. The spread combines: the gun's dispersion at this
 * range (scaled by the original's accuracy table), fire control, crew, aiming at a
 * part of the ship, evasive steering, the misjudged relative speed of the two
 * ships, the target's own turning over the time of flight, and the firer's turning. */
static void gun_solution(const world *w, int i, int m, int j, float *ax, float *az, float *sd, float *T_out)
{
    const ship *s = &w->ships[i], *t = &w->ships[j];
    const turret_mount *mt = &s->cls->mounts[m];
    const class_weapon *wp = &s->cls->weapons[mt->weapon];
    float mx, my, mz;
    mount_world(s, mt, &mx, &my, &mz);
    float off = aim_offset(t, s->aim[mt->bat]);
    float px = t->x + sinf(t->heading) * off, pz = t->z + cosf(t->heading) * off;
    float tvx = sinf(t->heading) * t->speed, tvz = cosf(t->heading) * t->speed;
    float fvx = sinf(s->heading) * s->speed, fvz = cosf(s->heading) * s->speed;
    float d = hypotf(px - mx, pz - mz), v = shell_speed(wp), T = intercept(mx, mz, px, pz, tvx, tvz, v);
    float sigma = d * DISPERSION * 85.0f / hit_chance(s->cls->hull_type, t->cls->hull_type, d);
    sigma *= fire_control_factor(s) * (1 + 0.6f * (1 - crew_factor(s))) * (s->aim[mt->bat] >= 0 ? 1.25f : 1)
           * (s->evade > 0 ? 1.3f : 1) * (1 + 0.5f * fabsf(s->rudder) * s->speed / fmaxf(s->max_speed, 1));
    sigma *= command_accuracy(w, i) / (1 + 0.15f * officer_level(s, OFF_FIRE_CONTROL));
    float rel = hypotf(tvx - fvx, tvz - fvz);
    float lead = T * LEAD_ERROR * fmaxf(rel, t->speed * 0.5f);
    float turn = 0.25f * fabsf(t->rudder) * t->max_yaw * t->speed * T * T;
    *ax = px + tvx * T;
    *az = pz + tvz * T;
    *sd = sqrtf(sigma * sigma + lead * lead + turn * turn);
    if (T_out) *T_out = T;
}

float hit_probability(const world *w, int i, int m, int j)
{
    const ship *s = &w->ships[i], *t = &w->ships[j];
    if (s->cls->weapons[s->cls->mounts[m].weapon].ammo != AMMO_SHELL) return 0;
    float ax, az, sd;
    gun_solution(w, i, m, j, &ax, &az, &sd, NULL);
    /* The aim error is the same in every direction, so work in the target's own frame:
     * land within the (tapered) beam and within the length. Aspect drops out. */
    /* Integrate along the length over the same tapered outline hull_distance uses. */
    float L = t->cls->half_length, B = t->cls->half_beam, p = 0;
    const int N = 24;
    for (int q = 0; q < N; q++) {
        float z = -L + 2 * L * ((float)q + 0.5f) / (float)N, u = fabsf(z) / L;
        float bz = B * (1 - 0.6f * u * u * u);
        float pz = expf(-z * z / (2 * sd * sd)) / (sd * 2.50662827f) * (2 * L / (float)N);
        p += pz * erff(bz / (sd * 1.41421356f));
    }
    return p;
}

/* ---- targeting --------------------------------------------------------------------- */

int can_engage(const world *w, int i, int m, int j)
{
    const ship *s = &w->ships[i], *t = &w->ships[j];
    if (i == j || !t->alive || t->side == s->side) return 0;
    const turret_mount *mt = &s->cls->mounts[m];
    const class_weapon *wp = &s->cls->weapons[mt->weapon];
    if ((wp->ammo == AMMO_DC) != (t->cls->submerged != 0)) return 0;
    float mx, my, mz;
    mount_world(s, mt, &mx, &my, &mz);
    float dx = t->x - mx, dz = t->z - mz, d = sqrtf(dx * dx + dz * dz);
    if (d > wp->max_range || d < wp->min_range) return 0;
    float rel = wrap_angle(atan2f(dx, dz) - s->heading);
    return fabsf(wrap_angle(rel - mt->arc_center)) <= mt->arc_half;
}

static int pick_target(const world *w, int i, int m)
{
    const ship *s = &w->ships[i];
    int ordered = s->order_target[s->cls->mounts[m].bat];
    if (ordered >= 0 && can_engage(w, i, m, ordered)) return ordered;
    int best = -1;
    float bd = INFINITY;
    for (int j = 0; j < w->count; j++) {
        if (!can_engage(w, i, m, j)) continue;
        float d = hypotf(w->ships[j].x - s->x, w->ships[j].z - s->z);
        if (d < bd) { bd = d; best = j; }
    }
    return best;
}

/* ---- firing ------------------------------------------------------------------------ */

projectile *new_projectile(world *w)
{
    for (int k = 0; k < MAX_PROJECTILES; k++)
        if (!w->proj[k].active) {
            memset(&w->proj[k], 0, sizeof w->proj[k]);
            w->proj[k].active = 1;
            w->proj[k].tick0 = w->tick;
            return &w->proj[k];
        }
    return NULL;
}

/* A lobbed round from (mx, my, mz) landing at (ax, az) after T seconds. */
void launch_ballistic(projectile *p, float mx, float my, float mz, float ax, float az, float T)
{
    p->x0 = mx;
    p->y0 = my;
    p->z0 = mz;
    p->vx = (ax - mx) / T;
    p->vz = (az - mz) / T;
    p->vy = (0.5f * GRAVITY * T * T - my) / T;
    p->flight = T;
}

static void fire(world *w, int i, int m, int j, float aim_yaw)
{
    ship *s = &w->ships[i];
    const ship *t = &w->ships[j];
    const turret_mount *mt = &s->cls->mounts[m];
    const class_weapon *wp = &s->cls->weapons[mt->weapon];
    turret *tu = &s->turrets[m];
    float mx, my, mz;
    mount_world(s, mt, &mx, &my, &mz);
    float d = hypotf(t->x - mx, t->z - mz);

    if (wp->ammo == AMMO_TORPEDO) {
        /* A spread of three running straight down the bearing the tube trained to. */
        for (int k = -1; k <= 1; k++) {
            projectile *p = new_projectile(w);
            if (!p) break;
            float hd = s->heading + aim_yaw + (float)k * TORP_SPREAD;
            p->ammo = AMMO_TORPEDO;
            p->owner = i;
            p->side = s->side;
            p->damage = wp->damage;
            p->x0 = p->px = mx;
            p->z0 = p->pz = mz;
            p->vx = sinf(hd) * TORP_SPEED;
            p->vz = cosf(hd) * TORP_SPEED;
            p->flight = wp->max_range / TORP_SPEED;
            w->torps++;
        }
        tu->reload = wp->fire_rate * TORP_RELOAD_SCALE * (module_health(s, MOD_TURRET, m) < 0.5f ? 1.8f : 1);
        fx(w, FX_TORP_LAUNCH, mx, my, mz);
        return;
    }

    projectile *p = new_projectile(w);
    if (!p) return;
    p->ammo = wp->ammo;
    p->owner = i;
    p->side = s->side;
    p->damage = wp->damage;
    if (wp->ammo == AMMO_DC) {
        /* Thrown or rolled over the boat's position: short lob, then sinks. */
        float ax = t->x + gauss(w) * 20, az = t->z + gauss(w) * 20;
        launch_ballistic(p, mx, my, mz, ax, az, fmaxf(1.5f, d / 150.0f));
    } else {
        float v = shell_speed(wp), ax, az, sd;
        gun_solution(w, i, m, j, &ax, &az, &sd, NULL);
        ax += gauss(w) * sd;
        az += gauss(w) * sd;
        launch_ballistic(p, mx, my, mz, ax, az, hypotf(ax - mx, az - mz) / v);
        p->pen = shell_pen(wp, d);
        p->plunging = d > 0.55f * wp->max_range;
        w->shots++;
    }
    /* Damaged turrets and thinned-out gun crews load slower. */
    tu->reload = fmaxf(2.0f, wp->fire_rate * RELOAD_SCALE) * (module_health(s, MOD_TURRET, m) < 0.5f ? 1.8f : 1)
               / (0.4f + 0.6f * crew_factor(s)) / (1 + 0.2f * officer_level(s, OFF_GUNNERY));
    if (wp->ammo == AMMO_DC) fx(w, FX_DC_LAUNCH, mx, my, mz);
    else fx_mag(w, FX_MUZZLE, mx, my, mz, wp->damage);
}

static void fire_control(world *w, int i)
{
    ship *s = &w->ships[i];
    for (int m = 0; m < s->cls->mount_count; m++) {
        const turret_mount *mt = &s->cls->mounts[m];
        const class_weapon *wp = &s->cls->weapons[mt->weapon];
        turret *tu = &s->turrets[m];
        tu->reload = fmaxf(0, tu->reload - SIM_DT);
        tu->retarget -= SIM_DT;
        int ordered = s->order_target[mt->bat];
        if (s->hold_fire) tu->target = -1;
        else if (tu->target < 0 || tu->retarget <= 0 || !can_engage(w, i, m, tu->target) ||
                 (ordered >= 0 && tu->target != ordered && can_engage(w, i, m, ordered))) {
            tu->target = pick_target(w, i, m);
            tu->retarget = RETARGET_S;
        }
        /* Train toward the lead bearing (or back to rest), never through the dead arc:
         * angles are measured from the arc centre and clamped to it. */
        float want = mt->arc_center;
        if (tu->target >= 0) {
            const ship *t = &w->ships[tu->target];
            float mx, my, mz;
            mount_world(s, mt, &mx, &my, &mz);
            float tvx = sinf(t->heading) * t->speed, tvz = cosf(t->heading) * t->speed;
            float v = wp->ammo == AMMO_TORPEDO ? TORP_SPEED : wp->ammo == AMMO_DC ? 1e6f : shell_speed(wp);
            float T = intercept(mx, mz, t->x, t->z, tvx, tvz, v);
            want = wrap_angle(atan2f(t->x + tvx * T - mx, t->z + tvz * T - mz) - s->heading);
        }
        float cur = wrap_angle(tu->yaw - mt->arc_center);
        float goal = clampf(wrap_angle(want - mt->arc_center), -mt->arc_half, mt->arc_half);
        float step = traverse_rate(wp) * SIM_DT * (module_health(s, MOD_TURRET, m) < 0.5f ? 0.5f : 1)
                   * (1 + 0.3f * officer_level(s, OFF_GUNNERY));
        cur += clampf(goal - cur, -step, step);
        tu->yaw = wrap_angle(cur + mt->arc_center);
        if (tu->target >= 0 && tu->reload <= 0 && fabsf(wrap_angle(want - tu->yaw)) < ON_BEARING && mount_ready(s, m))
            fire(w, i, m, tu->target, tu->yaw);
    }
}

/* ---- projectiles ------------------------------------------------------------------- */

static void shell_impact(world *w, projectile *p)
{
    float x = p->x0 + p->vx * p->flight, z = p->z0 + p->vz * p->flight;
    int best = -1;
    float bd = INFINITY;
    for (int j = 0; j < w->count; j++) {
        const ship *t = &w->ships[j];
        if (j == p->owner || !t->alive || t->cls->submerged) continue;
        float d = hull_distance(t, x, z);
        if (d < bd) { bd = d; best = j; }
    }
    if (best >= 0 && bd <= 0) {
        damage_hit(w, best, p->ammo == AMMO_BOMB ? HIT_BOMB : HIT_SHELL, x, z, p->damage, p->pen, p->plunging);
        if (p->ammo == AMMO_BOMB) w->bomb_hits++;
        else w->hits++;
        fx_mag(w, FX_HIT, x, 8, z, p->damage);
    } else if (best >= 0 && bd <= NEAR_MISS) {
        damage_hit(w, best, HIT_NEAR_MISS, x, z, p->damage, 0, 0);
        fx_mag(w, FX_NEAR_MISS, x, 0, z, p->damage);
    } else {
        fx_mag(w, FX_SPLASH, x, 0, z, p->damage);
    }
}

static void projectiles_step(world *w)
{
    float now = (float)(w->tick + 1);
    for (int k = 0; k < MAX_PROJECTILES; k++) {
        projectile *p = &w->proj[k];
        if (!p->active) continue;
        float t = (now - (float)p->tick0) * SIM_DT;
        if (p->ammo == AMMO_SHELL || p->ammo == AMMO_BOMB) {
            if (t >= p->flight) {
                shell_impact(w, p);
                p->active = 0;
            }
        } else if (p->ammo == AMMO_TORPEDO) {
            float x = p->x0 + p->vx * t, z = p->z0 + p->vz * t;
            for (int j = 0; j < w->count; j++) {
                const ship *s = &w->ships[j];
                if (j == p->owner || !s->alive || s->cls->submerged) continue;
                if (s->side == p->side && t < TORP_ARM_S) continue;
                if (hull_distance(s, x, z) <= 0) {
                    damage_hit(w, j, HIT_TORPEDO, x, z, p->damage, 0, 0);
                    w->torp_hits++;
                    fx(w, FX_TORP_HIT, x, 0, z);
                    p->active = 0;
                    break;
                }
            }
            p->px = x;
            p->pz = z;
            if (t >= p->flight) p->active = 0;
        } else if (p->ammo == AMMO_DC) {
            float x = p->x0 + p->vx * p->flight, z = p->z0 + p->vz * p->flight;
            if (t - SIM_DT < p->flight && t >= p->flight) fx(w, FX_SPLASH, x, 0, z);
            if (t >= p->flight + DC_SINK_S) {
                for (int j = 0; j < w->count; j++) {
                    const ship *s = &w->ships[j];
                    if (s->alive && s->cls->submerged && s->side != p->side && hull_distance(s, x, z) <= DC_RADIUS) {
                        damage_hit(w, j, HIT_DEPTH_CHARGE, s->x, s->z, p->damage, 0, 0);
                        w->dc_hits++;
                    }
                }
                fx(w, FX_DC_BLAST, x, 0, z);
                p->active = 0;
            }
        }
    }
}

void combat_step(world *w)
{
    for (int i = 0; i < w->count; i++)
        if (w->ships[i].alive) fire_control(w, i);
    projectiles_step(w);
}

/* ---- orders ------------------------------------------------------------------------ */

void order_attack(world *w, const int *ids, int n, int target, unsigned mask)
{
    for (int i = 0; i < n; i++)
        for (int b = 0; b < BATTERIES; b++)
            if (mask & (1u << b)) w->ships[ids[i]].order_target[b] = target;
}

void order_aim(world *w, const int *ids, int n, int kind, unsigned mask)
{
    for (int i = 0; i < n; i++)
        for (int b = 0; b < BATTERIES; b++)
            if (mask & (1u << b)) w->ships[ids[i]].aim[b] = kind;
}

void order_evade(world *w, const int *ids, int n, float seconds)
{
    for (int i = 0; i < n; i++) {
        ship *s = &w->ships[ids[i]];
        s->evade = seconds;
        s->evade_heading = s->evade_base = s->heading;
    }
}

int torpedo_threat(const world *w, int i, float *comb)
{
    const ship *s = &w->ships[i];
    float now = (float)w->tick, svx = sinf(s->heading) * s->speed, svz = cosf(s->heading) * s->speed;
    float worst = INFINITY;
    int found = 0;
    for (int k = 0; k < MAX_PROJECTILES; k++) {
        const projectile *p = &w->proj[k];
        if (!p->active || p->ammo != AMMO_TORPEDO || p->side == s->side) continue;
        float tt = (now - (float)p->tick0) * SIM_DT;
        float rx = p->x0 + p->vx * tt - s->x, rz = p->z0 + p->vz * tt - s->z;
        float vx = p->vx - svx, vz = p->vz - svz, vv = vx * vx + vz * vz;
        float tc = vv > 0 ? -(rx * vx + rz * vz) / vv : -1;
        if (tc <= 0 || tc > 45 || tc > p->flight - tt) continue;
        float miss = hypotf(rx + vx * tc, rz + vz * tc);
        if (miss > s->cls->half_length + 60 || tc > worst) continue;
        worst = tc;
        found = 1;
        /* Comb the track: turn parallel to it, whichever way is the smaller turn. */
        float h = atan2f(p->vx, p->vz);
        *comb = fabsf(wrap_angle(h - s->heading)) < fabsf(wrap_angle(h + PI_F - s->heading)) ? h : wrap_angle(h + PI_F);
    }
    return found;
}

void order_hold_fire(world *w, const int *ids, int n, int hold)
{
    for (int i = 0; i < n; i++) w->ships[ids[i]].hold_fire = hold;
}
