/* Fleet command (design): flagships and distinguished officers.
 *
 * Flagship: one per player. Her hand-picked crew shoots 12% tighter and fights damage
 * 20% faster; ships of her fleet within 3,000 shoot 5% tighter; losing her rattles the
 * fleet (15% wider spread for two minutes).
 * Officers: a pool per team, posted to ships; several can serve on one ship, each extra
 * one of a speciality counting half as much as the one before. */
#include <math.h>
#include <string.h>

#include "math3d.h"
#include "sim_internal.h"

#define AURA_RANGE 3000.0f
#define SHAKEN_S 120.0f

void order_flagship(world *w, int player, int ship_id)
{
    if (player < 0 || player >= FLEET_PLAYERS || ship_id < 0 || ship_id >= w->count) return;
    ship *s = &w->ships[ship_id];
    if (!s->alive || s->owner != player || s->cls->hull_type == HULL_SHIPYARD) return;
    int old = w->flagship_of[player];
    if (old >= 0 && old < w->count) w->ships[old].flagship = 0;
    s->flagship = 1;
    w->flagship_of[player] = ship_id;
}

int officer_add(world *w, int side, officer_kind kind)
{
    side &= 1;
    if (w->officer_count[side] == MAX_OFFICERS) return -1;
    int k = w->officer_count[side]++;
    w->officer_kind_of[side][k] = kind;
    w->officer_ship[side][k] = -1;
    return k;
}

static void recount(world *w, int side)
{
    for (int i = 0; i < w->count; i++)
        if (w->ships[i].side == side) memset(w->ships[i].officers, 0, sizeof w->ships[i].officers);
    for (int k = 0; k < w->officer_count[side]; k++) {
        int i = w->officer_ship[side][k];
        if (i >= 0) w->ships[i].officers[w->officer_kind_of[side][k]]++;
    }
}

void order_officer(world *w, int player, int officer, int ship_id)
{
    /* The player's team is that of any ship he owns. */
    int side = -1;
    for (int i = 0; i < w->count && side < 0; i++)
        if (w->ships[i].owner == player) side = w->ships[i].side & 1;
    if (side < 0 || officer < 0 || officer >= w->officer_count[side] || w->officer_ship[side][officer] == -2) return;
    if (ship_id >= 0) {
        if (ship_id >= w->count) return;
        const ship *s = &w->ships[ship_id];
        if (!s->alive || s->side != side || s->owner != player) return;
    }
    w->officer_ship[side][officer] = ship_id;
    recount(w, side);
}

float officer_level(const ship *s, officer_kind k)
{
    float lvl = 0, add = 1;
    for (int q = 0; q < s->officers[k]; q++, add *= 0.5f) lvl += add;
    return lvl;
}

float command_accuracy(const world *w, int i)
{
    const ship *s = &w->ships[i];
    float f = s->flagship ? 0.88f : 1;
    int p = s->owner;
    if (p >= 0 && p < FLEET_PLAYERS) {
        int fl = w->flagship_of[p];
        if (fl >= 0 && fl != i && w->ships[fl].alive && hypotf(w->ships[fl].x - s->x, w->ships[fl].z - s->z) < AURA_RANGE)
            f *= 0.95f;
        if (w->shaken[p] > 0) f *= 1.15f;
    }
    return f;
}

void fleet_step(world *w)
{
    for (int p = 0; p < FLEET_PLAYERS; p++) {
        w->shaken[p] = fmaxf(0, w->shaken[p] - SIM_DT);
        int fl = w->flagship_of[p];
        if (fl >= 0 && !w->ships[fl].alive) {
            w->shaken[p] = SHAKEN_S;
            w->ships[fl].flagship = 0;
            w->flagship_of[p] = -1;
        }
    }
    /* Officers go down with their ship. */
    for (int side = 0; side < 2; side++) {
        int changed = 0;
        for (int k = 0; k < w->officer_count[side]; k++) {
            int i = w->officer_ship[side][k];
            if (i >= 0 && !w->ships[i].alive) { w->officer_ship[side][k] = -2; changed = 1; }
        }
        if (changed) recount(w, side);
    }
}
