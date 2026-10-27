#include "sim.h"

#include <math.h>
#include <string.h>

#include "math3d.h"

#define ARRIVE_RADIUS 120.0f   /* waypoint reached (about 0.6 battleship lengths) */
#define RUDDER_GAIN 0.35f      /* heading error (rad) that gives full rudder */

void world_init(world *w)
{
    memset(w, 0, sizeof *w);
    world_seed(w, 1);
}

int world_spawn(world *w, const ship_class *cls, int side, float x, float z, float heading)
{
    if (w->count == MAX_SHIPS || !cls) return -1;
    ship *s = &w->ships[w->count];
    memset(s, 0, sizeof *s);
    s->cls = cls;
    s->side = side;
    s->alive = 1;
    s->x = s->prev_x = x;
    s->z = s->prev_z = z;
    s->heading = s->prev_heading = heading;
    s->max_speed = cls->speed * SPEED_SCALE;
    /* Bigger hulls take longer to get under way: battleship ~30 s, PT boat ~17 s. */
    s->accel = s->max_speed / (10.0f + cls->size_mod * 0.2f);
    /* The original's turn_rate grows with hull size (BB 3, DD 2, PT 1): treat it as
     * sluggishness. 12 deg/s / (1 + turn_rate): BB 3, DD 4, PT 6 deg/s. */
    s->max_yaw = (12.0f / (1.0f + cls->turn_rate)) * PI_F / 180.0f;
    s->hp = cls->max_health;
    s->stage = 1;
    for (int b = 0; b < BATTERIES; b++) s->order_target[b] = -1;
    damage_init(s);
    for (int m = 0; m < cls->mount_count; m++) {
        s->turrets[m].yaw = cls->mounts[m].arc_center;
        s->turrets[m].target = -1;
    }
    int id = w->count++;
    if (is_carrier(s)) air_spawn_group(w, id);
    return id;
}

/* Top speed with machinery damage and the drag of water aboard. */
static float top_speed(const ship *s)
{
    float engines = module_health(s, MOD_ENGINE, -1);
    return s->max_speed * clampf(engines * 1.25f, 0, 1) * clampf(1 - s->water * 1.5f, 0.2f, 1);
}

static void steer(world *w, int i)
{
    ship *s = &w->ships[i];
    float target_speed = 0, rudder_target = 0, vmax = top_speed(s);
    while (s->wp_count) {
        float dx = s->wp[0].x - s->x, dz = s->wp[0].z - s->z;
        float dist = sqrtf(dx * dx + dz * dz);
        if (dist < ARRIVE_RADIUS) {
            memmove(s->wp, s->wp + 1, sizeof(waypoint) * (size_t)--s->wp_count);
            continue;
        }
        float err = wrap_angle(atan2f(dx, dz) - s->heading);
        rudder_target = clampf(err / RUDDER_GAIN, -1, 1);
        target_speed = vmax;
        if (s->wp_count == 1) {
            /* Slow down to stop near the last waypoint: v = sqrt(2 a d). */
            float v = sqrtf(2.0f * s->accel * fmaxf(dist - ARRIVE_RADIUS * 0.5f, 0));
            target_speed = fminf(target_speed, fmaxf(v, vmax * 0.15f));
        }
        if (fabsf(err) > PI_F / 2) target_speed *= 0.6f;   /* tighten hard turns */
        break;
    }
    if (s->evade > 0) {
        /* Evasive: comb torpedoes coming her way, otherwise zig-zag 35 degrees either
         * side of her course, changing every 12 s; full speed throughout. */
        s->evade -= SIM_DT;
        float comb;
        if (torpedo_threat(w, i, &comb)) s->evade_heading = comb;
        else {
            float base = s->wp_count ? atan2f(s->wp[0].x - s->x, s->wp[0].z - s->z) : s->evade_base;
            int leg = (int)(w->tick / (12 * SIM_HZ)) & 1;
            s->evade_heading = wrap_angle(base + (leg ? 0.6f : -0.6f));
        }
        rudder_target = clampf(wrap_angle(s->evade_heading - s->heading) / RUDDER_GAIN, -1, 1);
        target_speed = vmax;
    }
    /* Steering gear out: the rudder stays where it was. Bridge gone: conned from
     * the after station, the helm answers at half rate. */
    float rate = RUDDER_RATE * (module_health(s, MOD_BRIDGE, -1) < 0.3f ? 0.5f : 1);
    if (!s->jammed) s->rudder += clampf(rudder_target - s->rudder, -rate * SIM_DT, rate * SIM_DT);
    float dv = clampf(target_speed - s->speed, -s->accel * SIM_DT, s->accel * SIM_DT);
    s->speed = fmaxf(0, s->speed + dv);
}

void world_step(world *w)
{
    /* Damage first, so this tick's movement and gunnery see its effects. */
    for (int i = 0; i < w->count; i++) damage_step(w, i);
    for (int i = 0; i < w->count; i++) {
        ship *s = &w->ships[i];
        s->prev_x = s->x;
        s->prev_z = s->z;
        s->prev_heading = s->heading;
        if (!s->alive) {                    /* sinking: coasts to a stop, rudder frozen */
            s->sink += SIM_DT;
            s->speed *= 0.98f;
            s->x += sinf(s->heading) * s->speed * SIM_DT;
            s->z += cosf(s->heading) * s->speed * SIM_DT;
            continue;
        }
        steer(w, i);
        /* A ship needs way on to turn: rudder authority scales with speed. */
        float authority = clampf(s->speed / fmaxf(s->max_speed, 1e-3f) * 1.5f, 0.15f, 1.0f);
        s->heading = wrap_angle(s->heading + s->rudder * s->max_yaw * authority * SIM_DT);
        s->x += sinf(s->heading) * s->speed * SIM_DT;
        s->z += cosf(s->heading) * s->speed * SIM_DT;
    }
    combat_step(w);
    air_step(w);
    if (w->tick % (2 * SIM_HZ) == 0) ai_step(w);
    w->tick++;
}

static int g_append;   /* order_move's append flag, for travel() */

static float travel(const world *w, int id, waypoint p)
{
    const ship *s = &w->ships[id];
    /* From the ship's last queued waypoint when queueing, else from where it is. */
    int from_wp = g_append && s->wp_count;
    float sx = from_wp ? s->wp[s->wp_count - 1].x : s->x, sz = from_wp ? s->wp[s->wp_count - 1].z : s->z;
    /* Euclidean, not squared: a minimum total-distance assignment never has two
     * crossing paths (uncrossing a pair always shortens it, by the triangle inequality). */
    return hypotf(p.x - sx, p.z - sz);
}

static void permute(const world *w, const int *ids, int n, const waypoint *slot, int k,
                    int *cur, int *used, float cost, float *best, int *out)
{
    if (cost >= *best) return;
    if (k == n) {
        *best = cost;
        memcpy(out, cur, sizeof(int) * (size_t)n);
        return;
    }
    for (int j = 0; j < n; j++) {
        if (used[j]) continue;
        used[j] = 1;
        cur[k] = j;
        permute(w, ids, n, slot, k + 1, cur, used, cost + travel(w, ids[k], slot[j]), best, out);
        used[j] = 0;
    }
}

static void assign_slots(const world *w, const int *ids, int n, const waypoint *slot, int *out)
{
    int used[MAX_SHIPS] = {0}, cur[MAX_SHIPS];
    if (n <= 7) {
        float best = INFINITY;
        permute(w, ids, n, slot, 0, cur, used, 0, &best, out);
        return;
    }
    int done[MAX_SHIPS] = {0};
    for (int round = 0; round < n; round++) {
        int bi = -1, bj = -1;
        float bd = INFINITY;
        for (int i = 0; i < n; i++)
            for (int j = 0; !done[i] && j < n; j++)
                if (!used[j] && travel(w, ids[i], slot[j]) < bd) { bd = travel(w, ids[i], slot[j]); bi = i; bj = j; }
        done[bi] = used[bj] = 1;
        out[bi] = bj;
    }
}

void order_move(world *w, const int *ids, int n, float x, float z, int append)
{
    if (n <= 0) return;
    /* Group centre and mean facing (circular mean of headings). */
    float cx = 0, cz = 0, hs = 0, hc = 0;
    for (int i = 0; i < n; i++) {
        const ship *s = &w->ships[ids[i]];
        cx += s->x;
        cz += s->z;
        hs += sinf(s->heading);
        hc += cosf(s->heading);
    }
    cx /= (float)n;
    cz /= (float)n;
    float facing = atan2f(hs, hc);
    float move_dir = atan2f(x - cx, z - cz);
    /* Keep the formation shape, turned so its front faces the direction of travel. */
    float rot = n > 1 ? wrap_angle(move_dir - facing) : 0;
    float c = cosf(rot), sn = sinf(rot);
    waypoint slot[MAX_SHIPS];
    for (int i = 0; i < n; i++) {
        const ship *s = &w->ships[ids[i]];
        float ox = s->x - cx, oz = s->z - cz;
        slot[i].x = x + ox * c + oz * sn;
        slot[i].z = z - ox * sn + oz * c;
    }
    /* Hand the slots out so ships don't cross each other's paths: minimise total
     * travel distance (exact for small groups, greedy nearest pair beyond that). */
    int assign[MAX_SHIPS];
    g_append = append;
    assign_slots(w, ids, n, slot, assign);
    for (int i = 0; i < n; i++) {
        ship *s = &w->ships[ids[i]];
        if (!append) s->wp_count = 0;
        if (s->wp_count < MAX_WAYPOINTS) s->wp[s->wp_count++] = slot[assign[i]];
    }
}

void order_stop(world *w, const int *ids, int n)
{
    for (int i = 0; i < n; i++) w->ships[ids[i]].wp_count = 0;
}
