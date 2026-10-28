/* Special abilities: the original's eight (BOOT.BIN's ability list, paired with hulls
 * by its training text), one per hull type. The effects are this game's own, and the
 * recharge is six times the original's 10-20 s to suit long battles.
 *   Focus Fire (battleship): 25 s, every main turret on one target at 0.6x spread.
 *   Rapid Fire (destroyer): guns reloaded now, reloads x0.4 for 15 s.
 *   Targeting Torpedo (submarine): tubes reloaded; the next spread homes on a target.
 *   Accurate Fire (battlecruiser): 20 s at half the spread.
 *   Hedgehog (destroyer escort): a ring of contact-fused bombs over a submarine ahead.
 *   Evasive Action (torpedo boat): 20 s at 1.5x the spread against her, sharper turns.
 *   AA Blast (cruiser): 15 s of AA at 3x the rate, 1.5x damage, +30% range.
 *   Ace Pilot (carrier): 60 s of her fighters hitting twice as often. */
#include <math.h>
#include <string.h>

#include "math3d.h"
#include "sim_internal.h"

special_kind special_of(const ship_class *c)
{
    switch (c->hull_type) {
    case 3: case 14: case 20: case 21: return SP_FOCUS_FIRE;
    case 6: case 15: return SP_RAPID_FIRE;
    case 12: case 16: case 19: return SP_TARGETING_TORPEDO;
    case 2: return SP_ACCURATE_FIRE;
    case 7: return SP_HEDGEHOG;
    case 13: return SP_EVASIVE_ACTION;
    case 5: return SP_AA_BLAST;
    case 1: return SP_ACE_PILOT;
    default: return SP_NONE;
    }
}

const char *special_name(special_kind k)
{
    static const char *n[SP_KINDS] = {"none", "Focus Fire", "Rapid Fire", "Targeting Torpedo", "Accurate Fire",
                                      "Hedgehog", "Evasive Action", "AA Blast", "Ace Pilot"};
    return k < SP_KINDS ? n[k] : "none";
}

float special_recharge(special_kind k)
{
    /* The original's recharge (CV/BB/DE/PT 15 s, CA/DD 10, CB/SS 20) x 6. */
    switch (k) {
    case SP_RAPID_FIRE: case SP_AA_BLAST: return 60;
    case SP_ACCURATE_FIRE: case SP_TARGETING_TORPEDO: return 120;
    default: return 90;
    }
}

static float duration(special_kind k)
{
    switch (k) {
    case SP_FOCUS_FIRE: return 25;
    case SP_RAPID_FIRE: case SP_AA_BLAST: return 15;
    case SP_ACCURATE_FIRE: case SP_EVASIVE_ACTION: return 20;
    case SP_ACE_PILOT: return 60;
    case SP_TARGETING_TORPEDO: return 120;             /* until the homing spread is fired */
    default: return 0;
    }
}

int order_special(world *w, int i, int target)
{
    ship *s = &w->ships[i];
    special_kind k = special_of(s->cls);
    if (!s->alive || k == SP_NONE || s->special_cool > 0) return -1;
    if ((k == SP_FOCUS_FIRE || k == SP_TARGETING_TORPEDO) &&
        (target < 0 || target >= w->count || !w->ships[target].alive || w->ships[target].side == s->side)) return -1;
    if (k == SP_HEDGEHOG) {
        /* A submarine within 900 off the bow quarters, or nothing to throw at. */
        int sub = -1;
        float bd = 900;
        for (int j = 0; j < w->count; j++) {
            const ship *t = &w->ships[j];
            if (!t->alive || t->side == s->side || !is_submerged(t) || !visible_to(w, s->side, j)) continue;
            float d = hypotf(t->x - s->x, t->z - s->z);
            if (d < bd) { bd = d; sub = j; }
        }
        if (sub < 0) return -1;
        const ship *t = &w->ships[sub];
        for (int q = 0; q < 12; q++) {                 /* 24 spigots in a ring, here 12 bombs */
            projectile *p = new_projectile(w);
            if (!p) break;
            float a = 6.2831853f * (float)q / 12;
            p->ammo = AMMO_DC;
            p->owner = i;
            p->side = s->side;
            p->damage = 45;
            launch_ballistic(p, s->x, 6, s->z, t->x + cosf(a) * 35, t->z + sinf(a) * 35, fmaxf(1.5f, bd / 200));
            p->homing = -2;                            /* contact fuze: see the DC branch in combat.c */
        }
    }
    if (k == SP_RAPID_FIRE || k == SP_TARGETING_TORPEDO)
        for (int m = 0; m < s->cls->mount_count; m++) {
            const class_weapon *wp = &s->cls->weapons[s->cls->mounts[m].weapon];
            if ((k == SP_RAPID_FIRE && wp->ammo == AMMO_SHELL) || (k == SP_TARGETING_TORPEDO && wp->ammo == AMMO_TORPEDO))
                s->turrets[m].reload = 0;
        }
    if (k == SP_FOCUS_FIRE) {
        for (int b = 0; b < BATTERIES; b++) s->order_target[b] = target;
        s->hold_fire = 0;
    }
    s->special_target = target;
    s->special_left = duration(k);
    s->special_cool = special_recharge(k);
    return 0;
}

float special_spread(const world *w, int i, int j)
{
    const ship *s = &w->ships[i], *t = &w->ships[j];
    float f = 1;
    special_kind k = special_of(s->cls);
    if (s->special_left > 0 && k == SP_FOCUS_FIRE && s->special_target == j) f *= 0.6f;
    if (s->special_left > 0 && k == SP_ACCURATE_FIRE) f *= 0.5f;
    if (t->special_left > 0 && special_of(t->cls) == SP_EVASIVE_ACTION) f *= 1.5f;
    return f;
}

void specials_step(world *w)
{
    for (int i = 0; i < w->count; i++) {
        ship *s = &w->ships[i];
        s->special_cool = fmaxf(0, s->special_cool - SIM_DT);
        s->special_left = fmaxf(0, s->special_left - SIM_DT);
        if (!s->alive) s->special_left = 0;
    }
}
