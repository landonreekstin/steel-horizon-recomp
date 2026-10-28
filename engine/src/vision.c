/* Fog of war (design): what each side can see. A ship is seen by the other side
 * while within lookout range of one of its ships (farther with working radar),
 * within sight of one of its aircraft, or for a few seconds after firing within gun-
 * flash range. Submerged boats are only found by sonar, close. Shore installations
 * are always known. */
#include <math.h>
#include <string.h>

#include "math3d.h"
#include "sim_internal.h"

static void mark(world *w, int side, int j)
{
    w->seen[side][j / 64] |= 1ull << (j % 64);
}

int visible_to(const world *w, int side, int j)
{
    const ship *t = &w->ships[j];
    if (!w->fog || t->side == side || is_shore(t)) return 1;
    return (int)((w->seen[side & 1][j / 64] >> (j % 64)) & 1);
}

void vision_step(world *w)
{
    memset(w->seen, 0, sizeof w->seen);
    for (int j = 0; j < w->count; j++) {
        ship *t = &w->ships[j];
        t->reveal = fmaxf(0, t->reveal - SIM_DT);
        t->recon_cool = fmaxf(0, t->recon_cool - SIM_DT);
        if (!t->alive) continue;
        int side = 1 - (t->side & 1);                   /* who might see her */
        int seen = 0;
        for (int i = 0; i < w->count && !seen; i++) {
            const ship *s = &w->ships[i];
            if (!s->alive || (s->side & 1) != side) continue;
            float d = hypotf(s->x - t->x, s->z - t->z);
            if (w->isle_count && island_blocks(w, s->x, s->z, t->x, t->z)) continue;   /* behind an island */
            if (is_submerged(t)) {
                /* Sonar: escorts and anything with depth charges hear farther; a deep
                 * boat is heard at half the range. */
                int asw = 0;
                for (int m = 0; m < s->cls->mount_count; m++) asw |= s->cls->mounts[m].bat == BAT_ASW;
                seen = d < SUB_DETECT * (asw ? 1.5f : 1) * (t->depth == DEPTH_DEEP ? 0.5f : 1);
            } else {
                float range = module_health(s, MOD_RADAR, -1) >= 0.3f && s->cls->module_count ? RADAR_RANGE : VIS_RANGE;
                if (is_shore(s)) range = RADAR_RANGE;
                if (t->cls->submerged) range *= 0.6f;   /* a surfaced boat: low in the water */
                seen = d < range || (t->reveal > 0 && d < FLASH_RANGE);
            }
        }
        for (int k = 0; k < w->air_count && !seen; k++) {
            const aircraft *a = &w->air[k];
            if (a->side != side || (a->state != AIR_OUT && a->state != AIR_ATTACK && a->state != AIR_RETURN)) continue;
            /* From the air: a periscope and wake close by; nothing of a deep boat. */
            if (is_submerged(t) && t->depth == DEPTH_DEEP) continue;
            seen = hypotf(a->x - t->x, a->z - t->z) < (is_submerged(t) ? AIR_VIS * 0.4f : AIR_VIS);
        }
        if (seen) {
            mark(w, side, j);
            w->last_x[side][j] = t->x;
            w->last_z[side][j] = t->z;
            w->last_seen[side][j] = w->tick;
        }
    }
}

int can_recon(const ship *s)
{
    int h = s->cls->hull_type;
    return s->alive && s->recon_cool <= 0 && (h == 1 || h == 2 || h == 3 || h == 5 || h == 14 || h == 20) &&
           module_health(s, MOD_BRIDGE, -1) > 0;
}

int order_recon(world *w, int i, float x, float z)
{
    ship *s = &w->ships[i];
    if (!can_recon(s) || w->air_count >= MAX_AIRCRAFT) return -1;
    aircraft *a = &w->air[w->air_count++];
    memset(a, 0, sizeof *a);
    a->type = PLANE_RECON;
    a->mission = MISSION_RECON;
    a->state = AIR_OUT;
    a->carrier = i;
    a->side = s->side;
    a->target = -1;
    a->max_hp = a->hp = 400;
    a->stage = 1;
    a->x = a->prev_x = s->x;
    a->z = a->prev_z = s->z;
    a->y = a->prev_y = 20;
    a->heading = a->prev_heading = atan2f(x - s->x, z - s->z);
    a->fuel = 600;
    a->timer = 0;
    a->gun_reload = 0;
    a->vy = 0;
    a->recon_x = x;
    a->recon_z = z;
    s->recon_cool = RECON_COOLDOWN;
    return 0;
}
