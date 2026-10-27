/* Hull footprints and weapon mounts per class, from the intact model's bbox and .pts.
 *
 * The original lists surface weapons per *mount group* (1 fore, 2 aft, 3/4 port and
 * starboard, 5/6 torpedoes, 7 depth charges; notes/battle-logic.md), while the models
 * place only the main turrets (F1.., R1.., sometimes P1../S1..). Each model turret takes the
 * next weapon entry of its group (cycling, so the US battleship's single group-2 entry
 * arms both R1 and R2); entries no model turret took get a virtual mount. Arcs and
 * virtual positions are design choices. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "asset.h"
#include "data.h"
#include "math3d.h"

#define DEG (PI_F / 180.0f)

static int mount_group(char c)
{
    return c == 'F' ? 1 : c == 'R' ? 2 : c == 'P' ? 3 : c == 'S' ? 4 : 0;
}

static float group_center(int g)
{
    return g == 2 ? PI_F : g == 3 ? -PI_F / 2 : g == 4 ? PI_F / 2 : 0;
}

static turret_mount *add_mount(ship_class *c, const char *name, float x, float y, float z,
                               float center, float half, int weapon)
{
    if (c->mount_count == MAX_MOUNTS) return NULL;
    turret_mount *m = &c->mounts[c->mount_count++];
    memset(m, 0, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", name);
    m->pos[0] = x;
    m->pos[1] = y;
    m->pos[2] = z;
    m->arc_center = center;
    m->arc_half = half;
    m->weapon = weapon;
    const class_weapon *w = &c->weapons[weapon];
    m->bat = w->ammo == AMMO_TORPEDO ? BAT_TORPEDO
           : w->ammo == AMMO_DC ? BAT_ASW
           : w->mount_group == 3 || w->mount_group == 4 ? BAT_SECONDARY : BAT_MAIN;
    return m;
}

static void build_mounts(ship_class *c, const mounts *pts)
{
    int used[MAX_CLASS_WEAPONS] = {0}, next[8] = {0};
    c->mount_count = 0;
    /* Model turrets, in .pts order. */
    for (int k = 0; k < pts->count; k++) {
        const mount_point *p = &pts->points[k];
        int g = mount_group(p->name[0]);
        if (!g || !isdigit((unsigned char)p->name[1])) continue;
        int cand[MAX_CLASS_WEAPONS], n = 0;
        for (int i = 0; i < c->weapon_count; i++)
            if (c->weapons[i].ammo == AMMO_SHELL && c->weapons[i].mount_group == g) cand[n++] = i;
        /* No entry for this group (a model turret the loadout doesn't list): main gun. */
        for (int i = 0; !n && i < c->weapon_count; i++)
            if (c->weapons[i].ammo == AMMO_SHELL && c->weapons[i].mount_group <= 2) cand[n++] = i;
        if (!n) continue;
        int wi = cand[next[g]++ % n];
        used[wi] = 1;
        char name[3] = {p->name[0], p->name[1], 0};
        add_mount(c, name, p->pos[0], p->pos[1], p->pos[2], group_center(g),
                  g <= 2 ? 150 * DEG : 80 * DEG, wi);
    }
    /* Everything else gets a virtual mount. */
    float L = c->half_length, B = c->half_beam, h = 8;
    for (int i = 0; i < c->weapon_count; i++) {
        const class_weapon *w = &c->weapons[i];
        if (used[i] || w->ammo == AMMO_NONE) continue;
        int g = w->mount_group;
        if (w->ammo == AMMO_DC) {
            add_mount(c, "", 0, 2, -L * 0.9f, PI_F, PI_F, i);        /* stern racks, all round */
        } else if (w->ammo == AMMO_TORPEDO) {
            if (c->submerged) add_mount(c, "", 0, 0, L * 0.8f, 0, 25 * DEG, i);   /* bow tubes */
            else {                     /* trainable deck tubes, one per beam for either group */
                add_mount(c, "", -B * 0.5f, 4, 0, -PI_F / 2, 60 * DEG, i);
                add_mount(c, "", B * 0.5f, 4, 0, PI_F / 2, 60 * DEG, i);
            }
        } else if (g == 3 || g == 4) {
            add_mount(c, "", g == 3 ? -B : B, h, 0, group_center(g), 80 * DEG, i);
        } else {
            add_mount(c, "", 0, h, g == 2 ? -L * 0.6f : L * 0.6f, group_center(g), 150 * DEG, i);
        }
    }
}

int classes_load_layouts(class_table *t, const char *files_dir)
{
    int failed = 0;
    for (int i = 0; i < t->count; i++) {
        ship_class *c = &t->classes[i];
        char path[512];
        snprintf(path, sizeof path, "%s/%s", files_dir, c->model[0]);
        pingu p;
        mounts pts = {0};
        if (!c->model[0][0] || pingu_load(path, &p)) {
            c->half_length = 50;
            c->half_beam = 8;
            failed++;
        } else {
            c->half_length = fmaxf(fabsf(p.bbox_min[2]), fabsf(p.bbox_max[2]));
            c->half_beam = fmaxf(fabsf(p.bbox_min[0]), fabsf(p.bbox_max[0]));
            pingu_free(&p);
            size_t n = strlen(path);
            if (n > 6) {
                strcpy(path + n - 6, ".pts");
                mounts_load(path, &pts);
            }
        }
        build_mounts(c, &pts);
    }
    return failed;
}
