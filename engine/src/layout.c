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

static void add_module(ship_class *c, module_kind kind, float z, int mount)
{
    if (c->module_count == MAX_MODULES) return;
    int k = 0;
    while (k < c->comp_count - 1 && z > c->comp_z[k + 1]) k++;
    module_def *m = &c->modules[c->module_count++];
    m->kind = kind;
    m->comp = k;
    m->mount = mount;
}

/* Compartments and modules from the hull size and the weapon mounts (design: the
 * original has a single HP pool). Armour is in the same units as shell_pen. */
static void build_damage_layout(ship_class *c)
{
    int h = c->hull_type;
    if (h == HULL_SHIPYARD) {
        /* Eight sections of waterfront; modules placed by hand below. */
        float L = c->half_length;
        c->comp_count = 8;
        for (int k = 0; k <= 8; k++) c->comp_z[k] = -L + 2 * L * (float)k / 8;
        c->module_count = 0;
        for (int m = 0; m < c->mount_count; m++) add_module(c, MOD_TURRET, c->mounts[m].pos[2], m);
        add_module(c, MOD_MAGAZINE, -0.55f * L, -1);      /* ammunition dump */
        add_module(c, MOD_DRYDOCK, -0.15f * L, -1);
        add_module(c, MOD_SLIPWAY, 0.25f * L, -1);
        add_module(c, MOD_BRIDGE, 0.05f * L, -1);         /* headquarters */
        add_module(c, MOD_RADAR, 0.05f * L, -1);
        add_module(c, MOD_AA, 0.6f * L, -1);
        add_module(c, MOD_AA, -0.7f * L, -1);
        c->belt = 2.6f;                                   /* concrete and earth */
        c->deck = 2.0f;
        c->reserve_buoyancy = 1;
        c->dc_crews = 6;
        c->complement = 3000;
        return;
    }
    int capital = h == 2 || h == 3 || h == 14 || h == 17 || h == 20 || h == 21;
    int small = h == 6 || h == 7 || h == 13 || h == 15 || c->submerged;
    c->comp_count = capital || h == 1 ? 10 : small ? (h == 13 ? 4 : 6) : 8;
    float L = c->half_length;
    for (int k = 0; k <= c->comp_count; k++) c->comp_z[k] = -L + 2 * L * (float)k / (float)c->comp_count;
    c->module_count = 0;
    /* Turrets, tubes and racks; a magazine under each main-battery compartment. */
    int has_mag[MAX_COMPARTMENTS] = {0};
    for (int m = 0; m < c->mount_count; m++) {
        add_module(c, MOD_TURRET, c->mounts[m].pos[2], m);
        const class_weapon *w = &c->weapons[c->mounts[m].weapon];
        if (c->mounts[m].bat == BAT_MAIN || w->ammo == AMMO_TORPEDO) {
            int k = c->modules[c->module_count - 1].comp;
            if (!has_mag[k]) { has_mag[k] = 1; add_module(c, MOD_MAGAZINE, c->mounts[m].pos[2], -1); }
        }
    }
    /* Machinery amidships, steering gear aft, bridge and radar forward of amidships. */
    add_module(c, MOD_ENGINE, -0.12f * L, -1);
    add_module(c, MOD_ENGINE, 0.08f * L, -1);
    add_module(c, MOD_STEERING, -0.92f * L, -1);
    if (!c->submerged) {
        add_module(c, MOD_BRIDGE, 0.22f * L, -1);
        add_module(c, MOD_RADAR, 0.22f * L, -1);
    }
    if (c->aa_damage > 0) {
        add_module(c, MOD_AA, 0.3f * L, -1);
        add_module(c, MOD_AA, -0.3f * L, -1);
    }
    if (h == 1) {
        add_module(c, MOD_FLIGHT_DECK, 0, -1);
        add_module(c, MOD_HANGAR_LIFT, 0.45f * L, -1);  /* forward and after lifts */
        add_module(c, MOD_HANGAR_LIFT, -0.35f * L, -1);
    }
    /* Armour against shell_pen (16in ~3.6 at point blank, 5in ~1.6): a battleship's belt
     * stops everything below 14in and her own guns beyond ~1,800, where they plunge
     * onto the thinner deck instead. */
    c->belt = capital ? (h == 2 ? 2.2f : 2.9f) : h == 1 ? 1.4f : h == 5 ? 1.8f : small ? 0.3f : 0.8f;
    c->deck = capital ? (h == 2 ? 1.6f : 2.2f) : h == 1 ? 1.2f : h == 5 ? 1.2f : small ? 0.2f : 0.6f;
    c->reserve_buoyancy = capital ? 0.32f : h == 1 ? 0.30f : small ? 0.24f : 0.27f;
    c->dc_crews = capital || h == 1 ? 4 : h == 5 ? 3 : small ? (h == 13 ? 1 : 2) : 2;
    c->complement = h == 1 ? 2600 : capital ? (h == 2 ? 1800 : 2500) : h == 5 ? 1100 : h == 6 || h == 15 ? 330
                  : h == 7 ? 220 : h == 13 ? 17 : c->submerged ? 80 : 300;
}

const ship_class *classes_add_shipyard(class_table *t, const char *nation, const ship_class *donor)
{
    if (t->count == MAX_CLASSES || !donor) return NULL;
    ship_class *c = &t->classes[t->count++];
    memset(c, 0, sizeof *c);
    snprintf(c->name, sizeof c->name, "%sShipyard", nation);
    snprintf(c->nation, sizeof c->nation, "%s", nation);
    snprintf(c->code, sizeof c->code, "SY");
    c->hull_type = HULL_SHIPYARD;
    c->max_health = 6000;
    c->size_mod = c->exposure_mod = 100;
    c->aa_damage = 80;
    for (int k = 0; k < 3; k++) snprintf(c->model[k], sizeof c->model[k], "@shipyard");
    snprintf(c->gun_model, sizeof c->gun_model, "%s", donor->gun_model);
    c->half_length = 320;           /* waterfront along its "bow" axis, facing the sea */
    c->half_beam = 180;
    /* Coastal batteries: the donor's main gun twice, its secondary twice. */
    int main_w = -1, sec_w = -1;
    for (int i = 0; i < donor->weapon_count; i++) {
        const class_weapon *w = &donor->weapons[i];
        if (w->ammo != AMMO_SHELL) continue;
        if (main_w < 0 && w->mount_group <= 2) main_w = i;
        if (sec_w < 0 && (w->mount_group == 3 || w->mount_group == 4)) sec_w = i;
    }
    if (main_w < 0) return NULL;
    c->weapons[c->weapon_count++] = donor->weapons[main_w];
    c->weapons[c->weapon_count - 1].mount_group = 1;
    if (sec_w >= 0) {
        c->weapons[c->weapon_count++] = donor->weapons[sec_w];
        c->weapons[c->weapon_count - 1].mount_group = 3;
    }
    struct { const char *name; float x, z, center, half; int w; } m[] = {
        {"F1", -200, 260, 0, 120 * DEG, 0}, {"F2", 200, 260, 0, 120 * DEG, 0},
        {"P1", -160, 120, -PI_F / 2, 100 * DEG, 1}, {"S1", 160, 120, PI_F / 2, 100 * DEG, 1}};
    for (int k = 0; k < 4; k++) {
        if (m[k].w >= c->weapon_count) continue;
        turret_mount *t2 = add_mount(c, m[k].name, m[k].x, 12, m[k].z, m[k].center, m[k].half, m[k].w);
        if (t2 && m[k].w == 1) t2->bat = BAT_SECONDARY;
    }
    build_damage_layout(c);
    return c;
}

float class_cost(const ship_class *c)
{
    switch (c->hull_type) {
    case 1: return 1400;            /* carrier, with her air group */
    case 2: return 1100;
    case 3: case 14: return 1500;
    case 5: return 800;
    case 6: case 15: return 400;
    case 7: return 300;
    case 12: case 16: return 350;
    case 13: return 120;
    default: return 300;
    }
}

float build_time(const ship_class *c)
{
    return class_cost(c) * 0.1f;    /* a battleship in 2.5 minutes, a PT boat in 12 s */
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
        build_damage_layout(c);
    }
    return failed;
}
