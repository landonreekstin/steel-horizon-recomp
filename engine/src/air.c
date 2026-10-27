/* Carrier air groups: launch, fly, fight, bomb, land and rearm. Shipboard AA.
 *
 * From the original (notes/battle-logic.md, "Aircraft"): fighters 600 HP, bombers
 * 800 HP, speeds 400 / 300, guns "Aircraft Guns1" (30 dmg, every 0.2 s), bomb
 * "Aircraft Bomb1" (100 dmg), AA from the ship's aa_damage. The air group size,
 * flight model, AA rules and rearm time are design choices. */
#include <math.h>
#include <string.h>

#include "math3d.h"
#include "sim_internal.h"

#define DEG (PI_F / 180.0f)
#define FUEL_S 700.0f          /* endurance: a 12,000-unit strike and back, with 90 s spare */
#define LAUNCH_INTERVAL 2.0f
#define ATTACK_RUN 1000.0f     /* bombers start their dive this far from the target */
#define DIVE_ALT 130.0f
#define GUN_RANGE 300.0f
#define GUN_CONE (12 * DEG)
#define GUN_HIT 0.25f
#define GUN_DAMAGE 30.0f
#define GUN_INTERVAL 0.2f
#define BOMB_DAMAGE 100.0f
#define BOMB_PEN 2.4f          /* through cruiser and carrier decks, sometimes a battleship's */
#define AA_HIT 0.30f
#define AA_INTERVAL 0.5f
#define INTERCEPT_RADIUS 2500.0f
#define CAP_RADIUS 600.0f

int is_carrier(const ship *s)
{
    return s->cls->hull_type == 1;
}

static int airborne(const aircraft *a)
{
    return a->state == AIR_OUT || a->state == AIR_ATTACK || a->state == AIR_RETURN;
}

void air_spawn_group(world *w, int carrier)
{
    const ship *c = &w->ships[carrier];
    for (int k = 0; k < AIR_GROUP_FIGHTERS + AIR_GROUP_BOMBERS && w->air_count < MAX_AIRCRAFT; k++) {
        aircraft *a = &w->air[w->air_count++];
        memset(a, 0, sizeof *a);
        a->type = k < AIR_GROUP_FIGHTERS ? PLANE_FIGHTER : PLANE_BOMBER;
        a->state = AIR_HANGAR;
        a->carrier = carrier;
        a->side = c->side;
        a->target = -1;
        a->max_hp = a->hp = a->type == PLANE_FIGHTER ? 600 : 800;
        a->stage = 1;
    }
}

int hangar_count(const world *w, int carrier, plane_type type)
{
    int n = 0;
    for (int k = 0; k < w->air_count; k++) {
        const aircraft *a = &w->air[k];
        n += a->carrier == carrier && a->type == type && a->state == AIR_HANGAR && a->mission == MISSION_NONE;
    }
    return n;
}

/* ---- orders ------------------------------------------------------------------------ */

void order_strike(world *w, int carrier, int target)
{
    if (target < 0 || !w->ships[target].alive || w->ships[target].cls->submerged) return;
    int fighters = hangar_count(w, carrier, PLANE_FIGHTER), escorts = (fighters + 1) / 2;
    for (int k = 0; k < w->air_count; k++) {
        aircraft *a = &w->air[k];
        if (a->carrier != carrier || a->state != AIR_HANGAR || a->mission != MISSION_NONE) continue;
        if (a->type == PLANE_BOMBER) {
            a->mission = MISSION_STRIKE;
            a->target = target;
        } else if (escorts > 0) {
            a->mission = MISSION_ESCORT;
            escorts--;
        }
    }
}

void order_cap(world *w, int carrier)
{
    for (int k = 0; k < w->air_count; k++) {
        aircraft *a = &w->air[k];
        if (a->carrier == carrier && a->type == PLANE_FIGHTER && a->state == AIR_HANGAR && a->mission == MISSION_NONE)
            a->mission = MISSION_CAP;
    }
}

void order_recall(world *w, int carrier)
{
    for (int k = 0; k < w->air_count; k++) {
        aircraft *a = &w->air[k];
        if (a->carrier != carrier) continue;
        if (airborne(a)) a->state = AIR_RETURN;
        else if (a->state == AIR_HANGAR) a->mission = MISSION_NONE;   /* cancel queued launches */
    }
}

/* ---- flight ------------------------------------------------------------------------ */

static float plane_speed(const aircraft *a)
{
    return (a->type == PLANE_FIGHTER ? 400.0f : 300.0f) * AIR_SPEED_SCALE;
}

static void fly_to(aircraft *a, float tx, float ty, float tz, float climb, float speed)
{
    float max_turn = (a->type == PLANE_FIGHTER ? 35.0f : 20.0f) * DEG;
    float err = wrap_angle(atan2f(tx - a->x, tz - a->z) - a->heading);
    float turn = clampf(err, -max_turn * SIM_DT, max_turn * SIM_DT);
    a->heading = wrap_angle(a->heading + turn);
    a->bank = -turn / (max_turn * SIM_DT) * 0.7f;
    a->vy = clampf((ty - a->y) * 0.8f, -climb, climb);
    a->speed = speed;
    a->pitch = atan2f(a->vy, a->speed);
    a->x += sinf(a->heading) * a->speed * SIM_DT;
    a->z += cosf(a->heading) * a->speed * SIM_DT;
    a->y += a->vy * SIM_DT;
}

/* The carrier to land on: its own, else the nearest friendly carrier afloat. */
static int home_carrier(const world *w, const aircraft *a)
{
    if (w->ships[a->carrier].alive) return a->carrier;
    int best = -1;
    float bd = INFINITY;
    for (int i = 0; i < w->count; i++) {
        const ship *s = &w->ships[i];
        if (!s->alive || s->side != a->side || !is_carrier(s)) continue;
        float d = hypotf(s->x - a->x, s->z - a->z);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static void plane_damage(world *w, aircraft *a, float dmg, int by_fighter)
{
    if (!airborne(a)) return;
    a->hp -= dmg;
    a->stage = a->hp < a->max_hp / 3 ? 3 : a->hp < a->max_hp * 2 / 3 ? 2 : 1;
    if (a->hp <= 0) {
        a->state = AIR_FALLING;
        if (by_fighter) w->planes_lost_fighters++;
        else w->planes_lost_aa++;
        fx(w, FX_PLANE_DOWN, a->x, a->y, a->z);
    }
}

static int nearest_enemy_ship(const world *w, const aircraft *a, float max_d)
{
    int best = -1;
    float bd = max_d;
    for (int i = 0; i < w->count; i++) {
        const ship *s = &w->ships[i];
        if (!s->alive || s->side == a->side || s->cls->submerged) continue;
        float d = hypotf(s->x - a->x, s->z - a->z);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static void bomber_step(world *w, aircraft *a)
{
    if (a->target < 0 || !w->ships[a->target].alive) a->target = nearest_enemy_ship(w, a, 3000);
    if (a->target < 0) { a->state = AIR_RETURN; return; }
    const ship *t = &w->ships[a->target];
    /* Lead for the bomb's fall from the current height. */
    float tf = sqrtf(2 * fmaxf(a->y, 1) / GRAVITY);
    float px = t->x + sinf(t->heading) * t->speed * tf, pz = t->z + cosf(t->heading) * t->speed * tf;
    float d = hypotf(px - a->x, pz - a->z);
    if (a->state == AIR_OUT && d < ATTACK_RUN) a->state = AIR_ATTACK;
    float alt = a->state == AIR_ATTACK ? DIVE_ALT : CRUISE_ALT;
    fly_to(a, px, alt, pz, a->state == AIR_ATTACK ? 80 : 30, plane_speed(a));
    float err = fabsf(wrap_angle(atan2f(px - a->x, pz - a->z) - a->heading));
    if (a->state == AIR_ATTACK && a->y < DIVE_ALT + 30 && err < 10 * DEG && d <= a->speed * tf + 15) {
        projectile *p = new_projectile(w);
        if (p) {
            float sigma = 10 + 0.05f * a->y;
            p->ammo = AMMO_BOMB;
            p->owner = -1;
            p->side = a->side;
            p->damage = BOMB_DAMAGE;
            p->pen = BOMB_PEN;
            p->plunging = 1;
            launch_ballistic(p, a->x, a->y, a->z, px + gauss(w) * sigma, pz + gauss(w) * sigma, tf);
            w->bombs++;
        }
        a->state = AIR_RETURN;
    }
}

static void fighter_step(world *w, int k)
{
    aircraft *a = &w->air[k];
    /* Point to guard: the escorted strike's centre, else the carrier. */
    float gx = 0, gz = 0;
    int n = 0;
    if (a->mission == MISSION_ESCORT)
        for (int q = 0; q < w->air_count; q++) {
            const aircraft *b = &w->air[q];
            if (b->carrier == a->carrier && b->mission == MISSION_STRIKE && b->type == PLANE_BOMBER &&
                (b->state == AIR_OUT || b->state == AIR_ATTACK)) { gx += b->x; gz += b->z; n++; }
        }
    if (n) { gx /= (float)n; gz /= (float)n; }
    else {
        int home = home_carrier(w, a);
        if (home < 0) { gx = a->x; gz = a->z; }
        else { gx = w->ships[home].x; gz = w->ships[home].z; }
        if (a->mission == MISSION_ESCORT) a->mission = MISSION_CAP;    /* strike over: patrol home */
    }
    /* Pick the nearest enemy plane near the guarded point. */
    a->timer -= SIM_DT;
    if (a->target >= 0 && !airborne(&w->air[a->target])) a->target = -1;
    if (a->timer <= 0) {
        a->timer = 1.0f;
        int best = -1;
        float bd = INFINITY;
        for (int q = 0; q < w->air_count; q++) {
            const aircraft *b = &w->air[q];
            if (b->side == a->side || !airborne(b) || hypotf(b->x - gx, b->z - gz) > INTERCEPT_RADIUS) continue;
            float d = hypotf(b->x - a->x, b->z - a->z) + (b->type == PLANE_BOMBER ? 0 : 300);  /* bombers first */
            if (d < bd) { bd = d; best = q; }
        }
        a->target = best;
    }
    if (a->target >= 0) {
        aircraft *b = &w->air[a->target];
        /* Close at full speed, then sit on its tail: match speed plus a little. */
        float dx = b->x - a->x, dy = b->y - a->y, dz = b->z - a->z, d = sqrtf(dx * dx + dy * dy + dz * dz);
        float lead = 0.5f, v = d < 400 ? fminf(plane_speed(a), b->speed + 4 + (d - 150) * 0.05f) : plane_speed(a);
        fly_to(a, b->x + sinf(b->heading) * b->speed * lead, b->y, b->z + cosf(b->heading) * b->speed * lead, 40, v);
        float off = fabsf(wrap_angle(atan2f(dx, dz) - a->heading));
        a->gun_reload -= SIM_DT;
        if (d < GUN_RANGE && off < GUN_CONE && a->gun_reload <= 0) {
            a->gun_reload = GUN_INTERVAL;
            if ((w->tick & 3) == 0) fx(w, FX_AIR_GUN, a->x, a->y, a->z);
            if (rand01(w) < GUN_HIT) plane_damage(w, b, GUN_DAMAGE, 1);
        }
    } else {
        /* Orbit the guarded point. */
        float ang = atan2f(a->x - gx, a->z - gz) + 0.35f;
        fly_to(a, gx + sinf(ang) * CAP_RADIUS, CRUISE_ALT + 50, gz + cosf(ang) * CAP_RADIUS, 30, plane_speed(a));
    }
}

static void return_step(world *w, aircraft *a)
{
    int home = home_carrier(w, a);
    if (home < 0) {                                   /* nowhere to land: circle until dry */
        fly_to(a, a->x + sinf(a->heading + 0.3f) * 500, CRUISE_ALT, a->z + cosf(a->heading + 0.3f) * 500, 30, plane_speed(a));
        return;
    }
    const ship *c = &w->ships[home];
    float d = hypotf(c->x - a->x, c->z - a->z);
    fly_to(a, c->x, d < 800 ? 25 : CRUISE_ALT, c->z, 30, plane_speed(a));
    if (d < 120 && a->y < 60 && module_health(c, MOD_FLIGHT_DECK, -1) >= 0.4f) {
        a->state = AIR_DECK;
        a->carrier = home;
        a->mission = MISSION_NONE;
        a->target = -1;
        a->timer = REARM_S;                           /* rearmed and patched up below */
    }
}

static void aa_step(world *w, int i)
{
    ship *s = &w->ships[i];
    if (!s->alive || s->cls->submerged || s->cls->aa_damage <= 0 || s->hold_fire) return;
    s->aa_reload -= SIM_DT;
    if (s->aa_reload > 0) return;
    int best = -1;
    float bd = AA_RANGE;
    for (int k = 0; k < w->air_count; k++) {
        const aircraft *a = &w->air[k];
        if (a->side == s->side || !airborne(a)) continue;
        float dx = a->x - s->x, dz = a->z - s->z, d = sqrtf(dx * dx + a->y * a->y + dz * dz);
        if (d < bd) { bd = d; best = k; }
    }
    if (best < 0) return;
    s->aa_reload = AA_INTERVAL;
    aircraft *a = &w->air[best];
    fx(w, FX_FLAK, a->x + gauss(w) * 18, a->y + gauss(w) * 10, a->z + gauss(w) * 18);
    if (rand01(w) < AA_HIT * (1 - 0.5f * bd / AA_RANGE))
        plane_damage(w, a, s->cls->aa_damage * module_health(s, MOD_AA, -1), 0);
}

void air_step(world *w)
{
    /* Launches: one plane every two seconds off each carrier's bow. */
    for (int i = 0; i < w->count; i++) {
        ship *c = &w->ships[i];
        if (!c->alive || !is_carrier(c) || module_health(c, MOD_FLIGHT_DECK, -1) < 0.4f) continue;
        c->launch_timer -= SIM_DT;
        if (c->launch_timer > 0) continue;
        for (int k = 0; k < w->air_count; k++) {
            aircraft *a = &w->air[k];
            if (a->carrier != i || a->state != AIR_HANGAR || a->mission == MISSION_NONE) continue;
            float L = c->cls->half_length * 0.6f;
            a->x = c->x + sinf(c->heading) * L;
            a->z = c->z + cosf(c->heading) * L;
            a->y = 15;
            a->heading = c->heading;
            a->speed = plane_speed(a);
            a->fuel = FUEL_S;
            a->state = AIR_OUT;
            a->prev_x = a->x;
            a->prev_y = a->y;
            a->prev_z = a->z;
            a->prev_heading = a->heading;
            c->launch_timer = LAUNCH_INTERVAL;
            break;
        }
    }
    for (int k = 0; k < w->air_count; k++) {
        aircraft *a = &w->air[k];
        a->prev_x = a->x;
        a->prev_y = a->y;
        a->prev_z = a->z;
        a->prev_heading = a->heading;
        switch (a->state) {
        case AIR_OUT: case AIR_ATTACK: case AIR_RETURN:
            a->fuel -= SIM_DT;
            if (a->fuel <= 0) { a->state = AIR_FALLING; w->planes_ditched++; break; }   /* out of fuel */
            if (a->state != AIR_RETURN && (a->fuel < 90 || home_carrier(w, a) < 0)) a->state = AIR_RETURN;
            if (a->state == AIR_RETURN) return_step(w, a);
            else if (a->type == PLANE_BOMBER) bomber_step(w, a);
            else fighter_step(w, k);
            break;
        case AIR_DECK:
            /* Rearming and repairs happen below: no lifts, no turnaround. */
            if (module_health(&w->ships[a->carrier], MOD_HANGAR_LIFT, -1) >= 0.3f) a->timer -= SIM_DT;
            if (a->timer <= 0) {
                a->state = AIR_HANGAR;
                a->hp = a->max_hp;
                a->stage = 1;
            }
            if (!w->ships[a->carrier].alive) a->state = AIR_LOST;   /* went down with her */
            break;
        case AIR_HANGAR:
            if (!w->ships[a->carrier].alive) a->state = AIR_LOST;
            break;
        case AIR_FALLING:
            a->vy -= GRAVITY * SIM_DT;
            a->pitch = atan2f(a->vy, a->speed);
            a->bank += 2.5f * SIM_DT;
            a->x += sinf(a->heading) * a->speed * SIM_DT;
            a->z += cosf(a->heading) * a->speed * SIM_DT;
            a->y += a->vy * SIM_DT;
            if (a->y <= 0) {
                a->state = AIR_LOST;
                fx(w, FX_SPLASH, a->x, 0, a->z);
            }
            break;
        default:
            break;
        }
    }
    for (int i = 0; i < w->count; i++) aa_step(w, i);
}
