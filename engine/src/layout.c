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
#include "json.h"
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

/* Anti-submarine rules (ours, not the original's: notes/battle-logic.md). Depth charges
 * only on destroyers and escorts, rolled off the stern or thrown a short way; capital
 * ships and carriers relied on their screen. Submarines get a deck gun to fight with
 * on the surface. Returns the deck gun's weapon index, or -1. */
static int naval_rules(ship_class *c)
{
    int h = c->hull_type, escort = h == 6 || h == 7 || h == 15;
    int has_gun = 0;
    for (int i = 0; i < c->weapon_count; i++) {
        class_weapon *w = &c->weapons[i];
        if (w->ammo == AMMO_DC) {
            if (!escort) w->ammo = AMMO_NONE;
            w->min_range = 0;
            w->max_range = DC_THROW;
        }
        has_gun |= w->ammo == AMMO_SHELL;
    }
    if (!c->submerged || has_gun || c->weapon_count == MAX_CLASS_WEAPONS) return -1;
    class_weapon *w = &c->weapons[c->weapon_count];
    memset(w, 0, sizeof *w);
    snprintf(w->name, sizeof w->name, "Deck gun");
    w->kind = WK_SURFACE;
    w->ammo = AMMO_SHELL;
    w->mount_group = 1;
    w->damage = 15;                 /* the 3 Inch's figures */
    w->max_range = 1500;
    w->fire_rate = 7;
    return c->weapon_count++;
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
    float L = c->half_length, B = c->half_beam, h = c->submerged ? 2 : 8;
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
            add_mount(c, "", 0, h, g == 2 ? -L * 0.6f : L * (c->submerged ? 0.3f : 0.6f), group_center(g), 150 * DEG, i);
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
        naval_rules(c);
        build_mounts(c, &pts);
        build_damage_layout(c);
    }
    return failed;
}

/* ---- standalone classes ----------------------------------------------------------- */

static void add_weapon(ship_class *c, jnode *guns, const char *name, weapon_kind kind, int group)
{
    if (!name || c->weapon_count == MAX_CLASS_WEAPONS) return;
    class_weapon *w = &c->weapons[c->weapon_count++];
    memset(w, 0, sizeof *w);
    w->kind = kind;
    snprintf(w->name, sizeof w->name, "%s", name);
    w->mount_group = group;
    for (jnode *g = guns ? guns->child : NULL; g; g = g->next) {
        const char *gn = json_str(g, "name");
        if (gn && !strcmp(gn, name)) {
            w->damage = (float)json_num(g, "damage", 0);
            w->max_range = (float)json_num(g, "range", 0);
            w->fire_rate = (float)json_num(g, "rate", 3);
        }
    }
    const char *t = strstr(w->name, " T");
    w->ammo = w->damage <= 0 ? AMMO_NONE : strstr(w->name, " DC") ? AMMO_DC
            : t && (t[2] == 0 || t[2] == ' ') ? AMMO_TORPEDO : AMMO_SHELL;
}

/* "plan": positions as fractions of the length from the bow, heights in metres above
 * the deck. Returns the turret positions (model z) in tz, or 0 if none are given. */
static int load_plan(ship_class *c, const jnode *j, float scale, float *tz, int max_t)
{
    const jnode *p = json_get(j, "plan");
    if (!p) return 0;
    float L = c->half_length;
#define AT(f) (L * (1 - 2 * (float)(f)))
    ship_plan *pl = &c->plan;
    pl->has = 1;
    const jnode *t = json_get(p, "tower");
    if (t && json_len(t) >= 2) {
        pl->tower_z = AT(t->child->num);
        pl->tower_h = (float)t->child->next->num * scale;
        const jnode *st = t->child->next->next;
        pl->tower = st && st->str && !strcmp(st->str, "pagoda") ? TOWER_PAGODA
                  : st && st->str && !strcmp(st->str, "tripod") ? TOWER_TRIPOD : TOWER_BLOCK;
    }
    for (const jnode *f = json_get(p, "funnels") ? json_get(p, "funnels")->child : NULL; f && pl->funnels < PLAN_FUNNELS; f = f->next)
        if (json_len(f) >= 2) {
            pl->funnel_z[pl->funnels] = AT(f->child->num);
            pl->funnel_h[pl->funnels++] = (float)f->child->next->num * scale;
        }
    const jnode *a = json_get(p, "aft");
    if (a && json_len(a) >= 3) {
        pl->aft_z0 = AT(a->child->next->num);
        pl->aft_z1 = AT(a->child->num);
        pl->aft_h = (float)a->child->next->next->num * scale;
    }
    const jnode *m = json_get(p, "mast");
    if (m && json_len(m) >= 2) { pl->mast_z = AT(m->child->num); pl->mast_h = (float)m->child->next->num * scale; }
    const jnode *is = json_get(p, "island");
    if (is && json_len(is) >= 2) { pl->island_z0 = AT(is->child->next->num); pl->island_z1 = AT(is->child->num); }
    int n = 0;
    for (const jnode *e = json_get(p, "turrets") ? json_get(p, "turrets")->child : NULL; e && n < max_t; e = e->next) tz[n++] = AT(e->num);
#undef AT
    return n;
}

int classes_load_standalone(const char *path, class_table *out)
{
    memset(out, 0, sizeof *out);
    jnode *doc = json_load_file(path);
    if (!doc) { fprintf(stderr, "standalone data not found at %s\n", path); return -1; }
    /* Game size from real size: the original draws small ships big and big ships small
     * (PT boat 56 long, Iowa 195), which keeps small targets hittable. Its models fit
     * length_game = a * length^p; the same factor scales heights, and the beam too for
     * ships it enlarges (a battleship keeps her real beam). */
    jnode *size = json_get(doc, "size");
    float size_a = (float)json_num(size, "a", 10.9), size_p = (float)json_num(size, "p", 0.5155);
    jnode *guns = json_get(doc, "guns"), *classes = json_get(doc, "classes");
    for (jnode *j = classes ? classes->child : NULL; j && out->count < MAX_CLASSES; j = j->next) {
        ship_class *c = &out->classes[out->count++];
        snprintf(c->name, sizeof c->name, "%s", json_str(j, "name") ? json_str(j, "name") : "?");
        snprintf(c->nation, sizeof c->nation, "%s", json_str(j, "nation") ? json_str(j, "nation") : "");
        snprintf(c->code, sizeof c->code, "%s", json_str(j, "code") ? json_str(j, "code") : "");
        c->hull_type = (int)json_num(j, "hull", 6);
        c->submerged = (int)json_num(j, "submerged", 0);
        c->max_health = (float)json_num(j, "hp", 600);
        c->speed = (float)json_num(j, "speed", 30);
        c->turn_rate = (float)json_num(j, "turn", 2);
        c->aa_damage = (float)json_num(j, "aa", 0);
        c->size_mod = 100;
        c->exposure_mod = 70;
        float len = (float)json_num(j, "length", 100), scale = size_a * powf(len, size_p) / len;
        c->half_length = len * scale / 2;
        c->half_beam = (float)json_num(j, "beam", 10) * fmaxf(scale, 1) / 2;
        for (int k = 0; k < 3; k++) snprintf(c->model[k], sizeof c->model[k], "@gen");
        /* Weapons: the main battery fore and aft, secondaries a side, torpedoes, racks, AA. */
        const char *main_gun = json_str(j, "main");
        int nf = (int)json_num(j, "fore", 0), na = (int)json_num(j, "aft", 0);
        if (main_gun) { add_weapon(c, guns, main_gun, WK_SURFACE, 1); add_weapon(c, guns, main_gun, WK_SURFACE, 2); }
        int wmain_f = 0, wmain_a = 1;
        int wsec = -1, wtorp = -1, wdc = -1;
        if (json_str(j, "secondary")) {
            wsec = c->weapon_count;
            add_weapon(c, guns, json_str(j, "secondary"), WK_SURFACE, 3);
        }
        if (json_str(j, "torpedo")) {
            wtorp = c->weapon_count;
            add_weapon(c, guns, json_str(j, "torpedo"), WK_SURFACE, 5);
        }
        if (json_str(j, "dc")) {
            wdc = c->weapon_count;
            add_weapon(c, guns, json_str(j, "dc"), WK_SUBMERGED, 7);
        }
        if (c->aa_damage > 0) add_weapon(c, guns, "AA", WK_AIR, 1);
        float L = c->half_length, B = c->half_beam, deck = B * 0.6f;
        int carrier = c->hull_type == 1;
        /* The real ship's turret positions when the plan gives them (bow to stern: fore
         * first, then aft), else evenly spaced. */
        float tz[16];
        int planned = load_plan(c, j, scale, tz, 16) == nf + na;
        /* Superfiring turrets fore and aft (carriers' guns sit in the gallery, undrawn). */
        for (int k = 0; main_gun && k < nf; k++) {
            char nm[16];
            snprintf(nm, sizeof nm, carrier ? "" : "F%d", k + 1);
            add_mount(c, nm, carrier ? B : 0, deck + (float)k * 3, planned ? tz[k] : L * (0.62f - 0.15f * (float)k), 0,
                      150 * DEG, wmain_f);
        }
        for (int k = 0; main_gun && k < na; k++) {
            char nm[16];
            snprintf(nm, sizeof nm, carrier ? "" : "R%d", k + 1);
            /* R1 is the one nearest amidships: superfiring over the others. */
            add_mount(c, nm, carrier ? -B : 0, deck + (float)(na - 1 - k) * 3,
                      planned ? tz[nf + k] : -L * (0.68f - 0.15f * (float)(na - 1 - k)), PI_F, 150 * DEG, wmain_a);
        }
        if (wsec >= 0) {
            add_mount(c, "", -B, deck, 0, -PI_F / 2, 80 * DEG, wsec);
            add_mount(c, "", B, deck, 0, PI_F / 2, 80 * DEG, wsec);
        }
        if (wtorp >= 0) {
            if (c->submerged) add_mount(c, "", 0, 0, L * 0.8f, 0, 25 * DEG, wtorp);
            else {
                add_mount(c, "", -B * 0.5f, 4, -0.1f * L, -PI_F / 2, 60 * DEG, wtorp);
                add_mount(c, "", B * 0.5f, 4, -0.1f * L, PI_F / 2, 60 * DEG, wtorp);
            }
        }
        int wdeck = naval_rules(c);
        if (wdc >= 0 && c->weapons[wdc].ammo == AMMO_DC) add_mount(c, "", 0, 2, -L * 0.9f, PI_F, PI_F, wdc);
        if (wdeck >= 0) add_mount(c, "", 0, 2, L * 0.3f, 0, 150 * DEG, wdeck);
        build_damage_layout(c);
    }
    json_free(doc);
    return out->count ? 0 : -1;
}
