/* Steel Horizon RTS prototype: real-time naval battle using the original PSP assets.
 *
 *   shrts [--assets DIR] [--screenshot OUT.ppm --frames N] [--cam SHIP YAW PITCH DIST]
 *         [--cam-plane AIRCRAFT YAW PITCH DIST]
 *         [--headless SECONDS] [--scenario m01|fleet|carriers|assault|attack|defend] [--seed N] [--ai-both]
 *
 * DIR (default "assets") holds files/ (tools/imp_extract.py) and data/
 * (tools/data2json.py). Controls: LMB select / drag-box, Shift+LMB add,
 * RMB move (Shift queues waypoints), RMB on an enemy: focus fire (Ctrl: secondaries
 * only), H hold fire, F fire at will, S stop, Ctrl+1..9 set group, 1..9 select group,
 * carriers: B air strike on the focus target (or nearest), G fighter patrol, R recall,
 * damage control: P crew priority (auto/fires/flooding/weapons), X counter-flood,
 * M flood magazines,
 * W/A/D + arrows pan, Q/E rotate, wheel zoom, MMB drag orbit, Space pause,
 * +/- time compression (x1..x4), F12 screenshot, Esc quit. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "data.h"
#include "gl.h"
#include "hud.h"
#include "math3d.h"
#include "render.h"
#include "sim.h"

typedef struct {
    world w;
    class_table classes;
    unsigned char selected[MAX_SHIPS];
    unsigned char groups[10][MAX_SHIPS];
    camera cam;
    drag_box box;
    int paused, time_scale;
} game;

static int spawn(game *g, const char *cls, int side, float x, float z, float heading)
{
    const ship_class *c = class_find(&g->classes, cls);
    if (!c) { fprintf(stderr, "ships.json lacks %s\n", cls); return -1; }
    int id = world_spawn(&g->w, c, side, x, z, heading);
    if (id >= 0) g->w.ships[id].ai = side == 1;
    return id;
}

/* A side's shipyard, armed from the nation's battleship, building the listed classes. */
static int yard(game *g, const char *nation, int side, float x, float z, float heading, const char **options, int n)
{
    char donor[64];
    snprintf(donor, sizeof donor, "%sBattleship", nation);
    const ship_class *c = NULL;
    char name[64];
    snprintf(name, sizeof name, "%sShipyard", nation);
    c = class_find(&g->classes, name);
    if (!c) c = classes_add_shipyard(&g->classes, nation, class_find(&g->classes, donor));
    if (!c) { fprintf(stderr, "can't make a %s shipyard\n", nation); return -1; }
    int id = shipyard_spawn(&g->w, c, side, x, z, heading);
    if (id < 0) return -1;
    g->w.ships[id].ai = side == 1;
    side_state *st = &g->w.sides[side];
    for (int k = 0; k < n && st->option_count < MAX_BUILD_OPTIONS; k++) {
        const ship_class *o = class_find(&g->classes, options[k]);
        if (o) st->options[st->option_count++] = o;
    }
    return 0;
}

/* "m01": Mission 01 as measured in the original under PPSSPP (notes/runtime.md):
 * four US battleships in a 2x2 box 600 units apart against four German destroyers.
 * "fleet": a mixed US task force against a Japanese one, with torpedo boats and a
 * submarine on each side (design, not from the original). */
static int setup_scenario(game *g, const char *name)
{
    world_init(&g->w);
    int err = 0;
    if (!strcmp(name, "m01")) {
        const float bbh = 2.32f, ddh = -1.68f;      /* headings read from the original */
        err |= spawn(g, "AmericanBattleship", 0, 5268, 6191, bbh) < 0;
        err |= spawn(g, "AmericanBattleship", 0, 4668, 5614, bbh) < 0;
        err |= spawn(g, "AmericanBattleship", 0, 5867, 5558, bbh) < 0;
        err |= spawn(g, "AmericanBattleship", 0, 5267, 5560, bbh) < 0;
        err |= spawn(g, "GermanDestroyer", 1, 4650, 3759, ddh) < 0;
        err |= spawn(g, "GermanDestroyer", 1, 3856, 3030, ddh) < 0;
        err |= spawn(g, "GermanDestroyer", 1, 5056, 3031, ddh) < 0;
        err |= spawn(g, "GermanDestroyer", 1, 4456, 3029, ddh) < 0;
        g->cam.target = v3(5050, 0, 4900);
    } else if (!strcmp(name, "fleet")) {
        const char *us[] = {"AmericanBattleship", "AmericanBattleship", "AmericanCruiser", "AmericanCruiser",
                            "AmericanDestroyer", "AmericanDestroyer", "AmericanTorpedoBoat", "AmericanSubmarine"};
        const char *jp[] = {"JapaneseBattleship", "JapaneseCruiser", "JapaneseCruiser", "JapaneseDestroyer",
                            "JapaneseDestroyer", "JapaneseDestroyer", "JapaneseTorpedoBoat", "JapaneseSubmarine"};
        for (int k = 0; k < 8; k++) {             /* two lines abreast 5,000 apart */
            float off = ((float)k - 3.5f) * 350;
            err |= spawn(g, us[k], 0, off, k < 4 ? 0 : -500, 0) < 0;
            err |= spawn(g, jp[k], 1, off + 200, k < 4 ? 5000 : 5500, PI_F) < 0;
        }
        g->cam.target = v3(0, 0, 1200);
    } else if (!strcmp(name, "carriers")) {
        /* Two carrier groups 11,000 apart: a carrier behind a BB and two DDs each. */
        const char *us[] = {"AmericanAircraftCarrier", "AmericanBattleship", "AmericanDestroyer", "AmericanDestroyer"};
        const char *jp[] = {"JapaneseAircraftCarrier", "JapaneseBattleship", "JapaneseDestroyer", "JapaneseDestroyer"};
        const float ox[] = {0, 0, -400, 400}, oz[] = {-600, 0, 300, 300};
        for (int k = 0; k < 4; k++) {
            err |= spawn(g, us[k], 0, ox[k], oz[k], 0) < 0;
            err |= spawn(g, jp[k], 1, ox[k] + 300, 11000 - oz[k], PI_F) < 0;
        }
        g->cam.target = v3(0, 0, 0);
    } else if (!strcmp(name, "assault") || !strcmp(name, "attack") || !strcmp(name, "defend")) {
        /* Shipyard modes. assault: a yard each, first to destroy the other's wins.
         * attack: you bring the bigger fleet against a defended Japanese yard, 30 min.
         * defend: the same with sides swapped: you hold the US yard for 30 min. */
        static const char *us_build[] = {"AmericanDestroyer", "AmericanCruiser", "AmericanBattleship",
                                         "AmericanAircraftCarrier", "AmericanTorpedoBoat", "AmericanSubmarine"};
        static const char *jp_build[] = {"JapaneseDestroyer", "JapaneseCruiser", "JapaneseBattleship",
                                         "JapaneseAircraftCarrier", "JapaneseTorpedoBoat", "JapaneseSubmarine"};
        int assault = !strcmp(name, "assault"), player_defends = !strcmp(name, "defend");
        g->w.mode = assault ? MODE_ASSAULT : MODE_DEFEND;
        g->w.defender = player_defends ? 0 : 1;
        g->w.time_limit = 30 * 60;
        if (assault || player_defends) err |= yard(g, "American", 0, 0, -7000, 0, us_build, 6);
        if (assault || !player_defends) err |= yard(g, "Japanese", 1, 0, 7000, PI_F, jp_build, 6);
        /* The defender has the yard, the attacker a bigger fleet. */
        const char *small_us[] = {"AmericanBattleship", "AmericanCruiser", "AmericanDestroyer", "AmericanDestroyer"};
        const char *small_jp[] = {"JapaneseBattleship", "JapaneseCruiser", "JapaneseDestroyer", "JapaneseDestroyer"};
        const char *big_us[] = {"AmericanBattleship", "AmericanBattleship", "AmericanCruiser", "AmericanCruiser",
                                "AmericanAircraftCarrier", "AmericanDestroyer", "AmericanDestroyer", "AmericanDestroyer"};
        const char *big_jp[] = {"JapaneseBattleship", "JapaneseBattleship", "JapaneseCruiser", "JapaneseCruiser",
                                "JapaneseAircraftCarrier", "JapaneseDestroyer", "JapaneseDestroyer", "JapaneseDestroyer"};
        int n_us = assault || player_defends ? 4 : 8, n_jp = assault || !player_defends ? 4 : 8;
        const char **us = n_us == 4 ? small_us : big_us, **jp = n_jp == 4 ? small_jp : big_jp;
        for (int k = 0; k < n_us; k++)
            err |= spawn(g, us[k], 0, ((float)k - (float)(n_us - 1) / 2) * 350, -5600 + (k % 2) * 300.0f, 0) < 0;
        for (int k = 0; k < n_jp; k++)
            err |= spawn(g, jp[k], 1, ((float)k - (float)(n_jp - 1) / 2) * 350, 5600 - (k % 2) * 300.0f, PI_F) < 0;
        if (!assault) {                              /* the attacker's reinforcement waves */
            int a = 1 - g->w.defender;
            side_state *st = &g->w.sides[a];
            const char *wave[] = {a == 0 ? "AmericanCruiser" : "JapaneseCruiser", a == 0 ? "AmericanDestroyer" : "JapaneseDestroyer"};
            for (int k = 0; k < 2; k++) st->options[st->option_count++] = class_find(&g->classes, wave[k]);
            st->reinforce_every = 300;
            st->entry_x = 0;
            st->entry_z = a == 0 ? -6500 : 6500;
            g->w.sides[g->w.defender].income *= 0.7f;   /* tuned: AI vs AI splits about evenly */
        }
        g->cam.target = v3(0, 0, -5000);
    } else {
        fprintf(stderr, "unknown scenario %s (m01, fleet, carriers, assault, attack, defend)\n", name);
        return -1;
    }
    if (err) return -1;
    g->cam.yaw = 0.55f;
    g->cam.pitch = 0.30f;
    g->cam.dist = 2300;
    g->time_scale = 1;
    return 0;
}

static int selected_ids(const game *g, int *ids)
{
    int n = 0;
    for (int i = 0; i < g->w.count; i++)
        if (g->selected[i] && g->w.ships[i].alive) ids[n++] = i;
    return n;
}

/* Project a world point to window pixels; returns 0 if behind the camera. */
static int project(mat4 vp, vec3 p, int w, int h, float *sx, float *sy)
{
    float cx = vp.m[0] * p.x + vp.m[4] * p.y + vp.m[8] * p.z + vp.m[12];
    float cy = vp.m[1] * p.x + vp.m[5] * p.y + vp.m[9] * p.z + vp.m[13];
    float cw = vp.m[3] * p.x + vp.m[7] * p.y + vp.m[11] * p.z + vp.m[15];
    if (cw <= 0) return 0;
    *sx = (cx / cw * 0.5f + 0.5f) * (float)w;
    *sy = (1 - (cy / cw * 0.5f + 0.5f)) * (float)h;
    return 1;
}

/* Intersect the mouse ray with the sea plane y = 0. */
static int mouse_to_sea(const game *g, int mx, int my, int w, int h, float *x, float *z)
{
    mat4 vp = camera_viewproj(&g->cam, w, h, NULL), inv;
    if (!m4invert(vp, &inv)) return 0;
    float nx = (float)mx / (float)w * 2 - 1, ny = 1 - (float)my / (float)h * 2;
    vec3 a = m4point(inv, v3(nx, ny, -1)), b = m4point(inv, v3(nx, ny, 1));
    float wa = inv.m[3] * nx + inv.m[7] * ny - inv.m[11] + inv.m[15];
    float wb = inv.m[3] * nx + inv.m[7] * ny + inv.m[11] + inv.m[15];
    a = v3scale(a, 1 / wa);
    b = v3scale(b, 1 / wb);
    vec3 d = v3sub(b, a);
    if (fabsf(d.y) < 1e-6f) return 0;
    float t = -a.y / d.y;
    if (t < 0) return 0;
    *x = a.x + d.x * t;
    *z = a.z + d.z * t;
    return 1;
}

/* The enemy ship drawn nearest the cursor (within 40 px), or -1. */
static int enemy_at(const game *g, int mx, int my, int w, int h)
{
    mat4 vp = camera_viewproj(&g->cam, w, h, NULL);
    int best = -1;
    float best_d = 40 * 40;
    for (int i = 0; i < g->w.count; i++) {
        const ship *s = &g->w.ships[i];
        float sx, sy;
        if (s->side == 0 || !s->alive || !project(vp, v3(s->x, 10, s->z), w, h, &sx, &sy)) continue;
        float d = (sx - (float)mx) * (sx - (float)mx) + (sy - (float)my) * (sy - (float)my);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

static void select_at(game *g, int w, int h, int add)
{
    mat4 vp = camera_viewproj(&g->cam, w, h, NULL);
    int x0 = g->box.x0 < g->box.x1 ? g->box.x0 : g->box.x1, x1 = g->box.x0 < g->box.x1 ? g->box.x1 : g->box.x0;
    int y0 = g->box.y0 < g->box.y1 ? g->box.y0 : g->box.y1, y1 = g->box.y0 < g->box.y1 ? g->box.y1 : g->box.y0;
    int click = (x1 - x0) < 6 && (y1 - y0) < 6;
    if (!add) memset(g->selected, 0, sizeof g->selected);
    int best = -1;
    float best_d = 40 * 40;
    for (int i = 0; i < g->w.count; i++) {
        const ship *s = &g->w.ships[i];
        float sx, sy;
        if (s->side != 0 || !s->alive || !project(vp, v3(s->x, 10, s->z), w, h, &sx, &sy)) continue;
        if (click) {
            float d = (sx - (float)x0) * (sx - (float)x0) + (sy - (float)y0) * (sy - (float)y0);
            if (d < best_d) { best_d = d; best = i; }
        } else if (sx >= (float)x0 && sx <= (float)x1 && sy >= (float)y0 && sy <= (float)y1) {
            g->selected[i] = 1;
        }
    }
    if (click && best >= 0) g->selected[best] = 1;
}

static void save_ppm(const char *path, int w, int h)
{
    unsigned char *px = malloc((size_t)w * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; y--) fwrite(px + (size_t)y * w * 3, 1, (size_t)w * 3, f);
        fclose(f);
        printf("saved %s\n", path);
    }
    free(px);
}

static void print_state(const world *w)
{
    for (int i = 0; i < w->count; i++) {
        const ship *s = &w->ships[i];
        printf("ship %2d %-26s side %d pos %7.0f %7.0f hdg %6.1f spd %5.2f hp %5.0f/%-5.0f %s\n", i,
               s->cls->name, s->side, s->x, s->z, s->heading * 180 / PI_F, s->speed,
               s->hp, s->cls->max_health, s->alive ? "" : "SUNK");
        if (s->alive) {
            float st = 0;
            int fires = 0, dead_turrets = 0, turrets = 0;
            for (int k = 0; k < s->cls->comp_count; k++) { st += s->comp[k].structure; fires += s->comp[k].fire > 0; }
            for (int m = 0; m < s->cls->module_count; m++)
                if (s->cls->modules[m].kind == MOD_TURRET) { turrets++; dead_turrets += s->module_hp[m] <= 0; }
            printf("      structure %.2f water %.2f list %.0f deg fires %d turrets out %d/%d engines %.2f%s\n",
                   st / (float)s->cls->comp_count, s->water, s->list * 180 / PI_F, fires, dead_turrets, turrets,
                   module_health(s, MOD_ENGINE, -1), s->jammed ? " rudder jammed" : "");
        }
        if (is_carrier(s)) {
            static const char *names[] = {"hangar", "out", "attack", "return", "deck", "falling", "lost"};
            for (int type = 0; type < 2; type++) {
                int n[7] = {0};
                for (int k = 0; k < w->air_count; k++)
                    if (w->air[k].carrier == i && (int)w->air[k].type == type) n[w->air[k].state]++;
                printf("      %s:", type ? "bombers " : "fighters");
                for (int q = 0; q < 7; q++) if (n[q]) printf(" %s %d", names[q], n[q]);
                printf("\n");
            }
        }
    }
}

/* Orders from a key (or the controls toolbar, which sends the same keys). */
static void order_key(game *g, SDL_Keycode k, int ctrl, hud *hd)
{
    if (k == SDLK_z) {                              /* evasive manoeuvres for a minute */
        int ids[MAX_SHIPS], n = selected_ids(g, ids);
        order_evade(&g->w, ids, n, 60);
    } else if (k == SDLK_t) {                       /* cycle the main battery's aim point */
        static const int cycle[] = {-1, MOD_ENGINE, MOD_MAGAZINE, MOD_STEERING, MOD_BRIDGE, MOD_TURRET};
        int ids[MAX_SHIPS], n = selected_ids(g, ids);
        if (n) {
            int cur = g->w.ships[ids[0]].aim[BAT_MAIN], q = 0;
            while (q < 6 && cycle[q] != cur) q++;
            order_aim(&g->w, ids, n, cycle[(q + 1) % 6], (1u << BAT_MAIN) | (1u << BAT_SECONDARY));
        }
    } else if (k == SDLK_c) hd->show_captain = !hd->show_captain;
    else if (k == SDLK_F1) hd->show_controls = !hd->show_controls;
    else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) hud_act_top(&g->w);
    else if (k == SDLK_s && !ctrl) {
        int ids[MAX_SHIPS];
        order_stop(&g->w, ids, selected_ids(g, ids));
    } else if (k == SDLK_b || k == SDLK_g || k == SDLK_r) {
        /* Carriers: B strike the focus target (else the nearest enemy ship),
         * G fighter patrol overhead, R recall the air group. */
        int ids[MAX_SHIPS], n = selected_ids(g, ids);
        for (int q = 0; q < n; q++) {
            const ship *c = &g->w.ships[ids[q]];
            if (!is_carrier(c)) continue;
            int t = c->order_target[BAT_MAIN];
            if (k == SDLK_b && (t < 0 || !g->w.ships[t].alive)) {
                float bd = INFINITY;
                t = -1;
                for (int j = 0; j < g->w.count; j++) {
                    const ship *e2 = &g->w.ships[j];
                    if (!e2->alive || e2->side == c->side || e2->cls->submerged) continue;
                    float d = hypotf(e2->x - c->x, e2->z - c->z);
                    if (d < bd) { bd = d; t = j; }
                }
            }
            if (k == SDLK_b) order_strike(&g->w, ids[q], t);
            else if (k == SDLK_g) order_cap(&g->w, ids[q]);
            else order_recall(&g->w, ids[q]);
        }
    } else if (k == SDLK_p || k == SDLK_x || k == SDLK_m) {
        /* Damage control: P cycles crew priority, X counter-floods, M floods
         * the magazines (no more cook-offs, but those turrets fall silent). */
        int ids[MAX_SHIPS], n = selected_ids(g, ids);
        if (k == SDLK_p && n)
            order_dc_policy(&g->w, ids, n, (dc_policy)((g->w.ships[ids[0]].policy + 1) % DC_POLICIES));
        else if (k == SDLK_x) order_counter_flood(&g->w, ids, n);
        else if (k == SDLK_m) order_flood_magazines(&g->w, ids, n);
    } else if (k == SDLK_h || k == SDLK_f) {
        /* H toggles hold fire; F: fire at will (drop focus targets). */
        int ids[MAX_SHIPS], n = selected_ids(g, ids);
        if (k == SDLK_f) order_attack(&g->w, ids, n, -1, ~0u);
        order_hold_fire(&g->w, ids, n, k == SDLK_h && n && !g->w.ships[ids[0]].hold_fire);
    } else if (k >= SDLK_1 && k <= SDLK_9) {
        int gi = (int)(k - SDLK_0);
        if (ctrl) memcpy(g->groups[gi], g->selected, sizeof g->selected);
        else memcpy(g->selected, g->groups[gi], sizeof g->selected);
    }
}

int main(int argc, char **argv)
{
    const char *assets = "assets", *shot = NULL, *scenario = "m01";
    unsigned long long seed = 1;
    int ai_both = 0;
    float trace = 0;
    int frames = 60;
    float headless = -1, cam_yaw = 0, cam_pitch = 0, cam_dist = 0;
    int cam_ship = -1, cam_plane = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--assets") && i + 1 < argc) assets = argv[++i];
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scenario") && i + 1 < argc) scenario = argv[++i];
        else if (!strcmp(argv[i], "--ai-both")) ai_both = 1;
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--headless") && i + 1 < argc) headless = (float)atof(argv[++i]);
        else if ((!strcmp(argv[i], "--cam") || !strcmp(argv[i], "--cam-plane")) && i + 4 < argc) {
            if (!strcmp(argv[i], "--cam")) cam_ship = atoi(argv[++i]);
            else cam_plane = atoi(argv[++i]);
            cam_yaw = (float)atof(argv[++i]);
            cam_pitch = (float)atof(argv[++i]);
            cam_dist = (float)atof(argv[++i]);
        }
        else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }
    static game g;
    char path[512];
    snprintf(path, sizeof path, "%s/data", assets);
    if (classes_load(path, &g.classes)) return 1;
    snprintf(path, sizeof path, "%s/files", assets);
    classes_load_layouts(&g.classes, path);
    if (setup_scenario(&g, scenario)) return 1;
    world_seed(&g.w, seed);
    for (int i = 0; ai_both && i < g.w.count; i++) g.w.ships[i].ai = 1;

    if (headless >= 0) {
        /* Sim only: the player side holds its course, both sides fire at will;
         * runs until one side is sunk or the time is up. */
        int ticks = (int)(headless * SIM_HZ), t;
        int sunk_seen[MAX_SHIPS] = {0};
        for (t = 0; t < ticks && world_winner(&g.w) < 0; t++) {
            world_step(&g.w);
            if (trace > 0 && t % (int)(trace * SIM_HZ) == 0) {
                /* Ships afloat per side, yard condition, budget, closest approach to the enemy yard. */
                printf("t=%6.0f", (double)t / SIM_HZ);
                for (int sd = 0; sd < 2; sd++) {
                    int n = 0;
                    float closest = INFINITY;
                    int ey = g.w.sides[1 - sd].yard;
                    for (int i = 0; i < g.w.count; i++) {
                        const ship *s = &g.w.ships[i];
                        if (!s->alive || s->side != sd || is_shore(s)) continue;
                        n++;
                        if (ey >= 0) closest = fminf(closest, hypotf(s->x - g.w.ships[ey].x, s->z - g.w.ships[ey].z));
                    }
                    int y = g.w.sides[sd].yard;
                    printf("  | side %d: %2d ships", sd, n);
                    if (y >= 0) printf(", yard %3.0f%%, budget %5.0f", 100 * g.w.ships[y].hp / g.w.ships[y].cls->max_health, g.w.sides[sd].budget);
                    if (ey >= 0 && n) printf(", %5.0f from enemy yard", closest);
                }
                printf("\n");
            }
            for (int i = 0; i < g.w.count; i++)
                if (!g.w.ships[i].alive && !sunk_seen[i]) {
                    sunk_seen[i] = 1;
                    static const char *why[] = {"?", "flooded", "capsized", "magazine exploded", "structure failed", "abandoned"};
                    const ship *sk = &g.w.ships[i];
                    printf("t=%6.1f s  sunk: ship %d %s (%s, water %.2f, list %.0f deg)\n", (double)t / SIM_HZ, i,
                           sk->cls->name, why[sk->cause], sk->water, sk->list * 180 / PI_F);
                }
        }
        printf("after %.1f s (%d ticks): winner %d, %d shells fired, %d hit (%.1f%%), "
               "%d torpedoes %d hit, %d depth-charge hits\n"
               "  air: %d bombs %d hit, planes lost to AA %d, to fighters %d, ditched %d\n"
               "  damage: %d penetrations, %d bounced, %d fires, %d magazine explosions\n",
               (double)t / SIM_HZ, t, world_winner(&g.w), g.w.shots, g.w.hits,
               g.w.shots ? 100.0 * g.w.hits / g.w.shots : 0.0, g.w.torps, g.w.torp_hits, g.w.dc_hits,
               g.w.bombs, g.w.bomb_hits, g.w.planes_lost_aa, g.w.planes_lost_fighters, g.w.planes_ditched,
               g.w.penetrations, g.w.ricochets, g.w.fires_started, g.w.magazine_explosions);
        print_state(&g.w);
        return 0;
    }

    if (SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
    SDL_Window *win = SDL_CreateWindow("Steel Horizon RTS", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!win) {                                     /* retry without MSAA */
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
        win = SDL_CreateWindow("Steel Horizon RTS", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    }
    SDL_GLContext ctx = win ? SDL_GL_CreateContext(win) : NULL;
    if (!ctx || !gl_load()) { fprintf(stderr, "OpenGL 3.3: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(1);
    printf("GL %s\n", glGetString(GL_VERSION));
    glEnable(GL_MULTISAMPLE);

    renderer *r = renderer_create(path);
    for (int i = 0; i < g.w.count; i++)
        if (renderer_load_class(r, g.w.ships[i].cls)) return 1;
    for (int s = 0; s < 2; s++)                     /* ships the yards can build */
        for (int o = 0; o < g.w.sides[s].option_count; o++) renderer_load_class(r, g.w.sides[s].options[o]);

    Uint64 freq = SDL_GetPerformanceFrequency(), last = SDL_GetPerformanceCounter();
    double acc = 0, sim_time = 0;
    int running = 1, frame = 0, orbit = 0;
    static hud hd = {.show_captain = 1, .show_controls = 1};
    if (shot) {                                     /* deterministic picture for checking */
        /* Select the player's ships and steam them halfway to the enemy. */
        int ids[MAX_SHIPS], n = 0, ne = 0;
        float cx = 0, cz = 0, ex = 0, ez = 0;
        for (int i = 0; i < g.w.count; i++) {
            const ship *s = &g.w.ships[i];
            if (s->side == 0) { ids[n++] = i; g.selected[i] = 1; cx += s->x; cz += s->z; }
            else { ne++; ex += s->x; ez += s->z; }
        }
        if (n && ne)
            order_move(&g.w, ids, n, (cx / (float)n + ex / (float)ne) / 2, (cz / (float)n + ez / (float)ne) / 2, 0);
        for (int q = 0; q < n; q++)                 /* carriers strike the nearest enemy ship */
            if (is_carrier(&g.w.ships[ids[q]])) {
                int t = -1;
                float bd = INFINITY;
                for (int j = 0; j < g.w.count; j++) {
                    const ship *e2 = &g.w.ships[j];
                    float d = hypotf(e2->x - g.w.ships[ids[q]].x, e2->z - g.w.ships[ids[q]].z);
                    if (e2->side != 0 && !e2->cls->submerged && d < bd) { bd = d; t = j; }
                }
                order_strike(&g.w, ids[q], t);
            }
    }
    while (running) {
        SDL_Event e;
        int w, h;
        SDL_GL_GetDrawableSize(win, &w, &h);
        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            else if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                int ctrl = (e.key.keysym.mod & KMOD_CTRL) != 0;
                if (k == SDLK_ESCAPE) running = 0;
                else if (k == SDLK_SPACE) g.paused = !g.paused;
                else if (k == SDLK_EQUALS || k == SDLK_PLUS || k == SDLK_KP_PLUS) g.time_scale = g.time_scale < 4 ? g.time_scale * 2 : 4;
                else if (k == SDLK_MINUS || k == SDLK_KP_MINUS) g.time_scale = g.time_scale > 1 ? g.time_scale / 2 : 1;
                else if (k == SDLK_F12) save_ppm("shrts_screenshot.ppm", w, h);
                else order_key(&g, k, ctrl, &hd);
            } else if (e.type == SDL_MOUSEBUTTONDOWN) {
                if (e.button.button == SDL_BUTTON_LEFT && hud_over(&hd, (float)e.button.x, (float)e.button.y)) {
                    hd.click = 1;
                    hd.click_x = (float)e.button.x;
                    hd.click_y = (float)e.button.y;
                } else if (e.button.button == SDL_BUTTON_LEFT) {
                    g.box.x0 = g.box.x1 = e.button.x;
                    g.box.y0 = g.box.y1 = e.button.y;
                    g.box.active = 1;
                } else if (e.button.button == SDL_BUTTON_RIGHT) {
                    float x, z;
                    int ids[MAX_SHIPS], n = selected_ids(&g, ids);
                    int enemy = n ? enemy_at(&g, e.button.x, e.button.y, w, h) : -1;
                    if (enemy >= 0) {
                        /* Focus fire; Ctrl splits it: only the secondaries switch target. */
                        unsigned mask = (SDL_GetModState() & KMOD_CTRL) ? 1u << BAT_SECONDARY
                                      : (1u << BAT_MAIN) | (1u << BAT_SECONDARY) | (1u << BAT_TORPEDO);
                        order_attack(&g.w, ids, n, enemy, mask);
                        order_hold_fire(&g.w, ids, n, 0);
                    } else if (n && mouse_to_sea(&g, e.button.x, e.button.y, w, h, &x, &z)) {
                        order_move(&g.w, ids, n, x, z, (SDL_GetModState() & KMOD_SHIFT) != 0);
                    }
                } else if (e.button.button == SDL_BUTTON_MIDDLE) {
                    orbit = 1;
                }
            } else if (e.type == SDL_MOUSEBUTTONUP) {
                if (e.button.button == SDL_BUTTON_LEFT && g.box.active) {
                    g.box.active = 0;
                    select_at(&g, w, h, (SDL_GetModState() & KMOD_SHIFT) != 0);
                } else if (e.button.button == SDL_BUTTON_MIDDLE) {
                    orbit = 0;
                }
            } else if (e.type == SDL_MOUSEMOTION) {
                if (g.box.active) { g.box.x1 = e.motion.x; g.box.y1 = e.motion.y; }
                if (orbit) {
                    g.cam.yaw -= (float)e.motion.xrel * 0.005f;
                    g.cam.pitch = clampf(g.cam.pitch + (float)e.motion.yrel * 0.004f, 0.08f, 1.45f);
                }
            } else if (e.type == SDL_MOUSEWHEEL) {
                g.cam.dist = clampf(g.cam.dist * powf(0.88f, (float)e.wheel.y), 250, 20000);
            }
        }
        Uint64 now = SDL_GetPerformanceCounter();
        double dt = (double)(now - last) / (double)freq;
        last = now;
        if (shot) dt = 1.0 / 30;                     /* fixed steps for repeatable shots */
        if (dt > 0.25) dt = 0.25;

        /* Camera: pan along the ground relative to its yaw, speed scales with height. */
        float pan = g.cam.dist * 0.9f * (float)dt;
        float fx = -sinf(g.cam.yaw), fz = -cosf(g.cam.yaw), rx = -fz, rz = fx;
        if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) { g.cam.target.x += fx * pan; g.cam.target.z += fz * pan; }
        if (keys[SDL_SCANCODE_DOWN]) { g.cam.target.x -= fx * pan; g.cam.target.z -= fz * pan; }
        if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]) { g.cam.target.x -= rx * pan; g.cam.target.z -= rz * pan; }
        if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) { g.cam.target.x += rx * pan; g.cam.target.z += rz * pan; }
        if (keys[SDL_SCANCODE_Q]) g.cam.yaw += 1.2f * (float)dt;
        if (keys[SDL_SCANCODE_E]) g.cam.yaw -= 1.2f * (float)dt;

        if (!g.paused) {
            acc += dt * g.time_scale;
            while (acc >= SIM_DT) {
                world_step(&g.w);
                acc -= SIM_DT;
                sim_time += SIM_DT;
            }
        }
        if (cam_ship >= 0 && cam_ship < g.w.count) {   /* follow one ship (screenshots) */
            g.cam.target = v3(g.w.ships[cam_ship].x, 0, g.w.ships[cam_ship].z);
            g.cam.yaw = g.w.ships[cam_ship].heading + cam_yaw;
            g.cam.pitch = cam_pitch;
            g.cam.dist = cam_dist;
        }
        if (cam_plane >= 0 && cam_plane < g.w.air_count) {   /* follow one aircraft */
            const aircraft *a = &g.w.air[cam_plane];
            g.cam.target = v3(a->x, a->y, a->z);
            g.cam.yaw = a->heading + cam_yaw;
            g.cam.pitch = cam_pitch;
            g.cam.dist = cam_dist;
        }
        renderer_draw(r, &g.cam, w, h, &g.w, g.paused ? 1.0f : (float)(acc / SIM_DT),
                      g.selected, &g.box, (float)sim_time);
        int key = hud_draw(&hd, r, &g.w, g.selected, w, h, (float)sim_time);
        if (key) order_key(&g, key, 0, &hd);
        if (shot && ++frame >= frames) {
            save_ppm(shot, w, h);
            running = 0;
        }
        SDL_GL_SwapWindow(win);
        char title[256];
        int ids[MAX_SHIPS];
        int win_side = world_winner(&g.w), nsel = selected_ids(&g, ids);
        char air_info[64] = "", dc_info[64] = "";
        static const char *policy_names[] = {"auto", "fires", "flooding", "weapons"};
        if (nsel) {
            const ship *s0 = &g.w.ships[ids[0]];
            snprintf(dc_info, sizeof dc_info, "  |  DC: %s, list %.0f deg", policy_names[s0->policy], s0->list * 180 / PI_F);
        }
        for (int q = 0; q < nsel; q++)
            if (is_carrier(&g.w.ships[ids[q]])) {
                snprintf(air_info, sizeof air_info, "  |  hangar: %d fighters %d bombers (B/G/R)",
                         hangar_count(&g.w, ids[q], PLANE_FIGHTER), hangar_count(&g.w, ids[q], PLANE_BOMBER));
                break;
            }
        snprintf(title, sizeof title, "Steel Horizon RTS  |  %s x%d  |  %d selected%s%s%s  |  t=%.0fs%s",
                 g.paused ? "PAUSED" : "running", g.time_scale, nsel,
                 nsel && g.w.ships[ids[0]].hold_fire ? " (holding fire)" : "", dc_info, air_info, sim_time,
                 win_side == 0 ? "  |  VICTORY" : win_side == 1 ? "  |  DEFEAT" : "");
        SDL_SetWindowTitle(win, title);
    }
    renderer_destroy(r);
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
