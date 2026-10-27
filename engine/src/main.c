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
#include <unistd.h>

#include "data.h"
#include "gl.h"
#include "command.h"
#include "audio.h"
#include "hud.h"
#include "menu.h"
#include "net.h"
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
    int player, side;          /* the local player (command.h) and his team */
    command_queue q;
    net *net;                  /* online game, or NULL */
    audio *snd;                /* NULL: silent */
    custom_spec custom;        /* the "custom" scenario's fleets */
    /* Online chat: the line being typed (Y opens it), and recent lines on screen. */
    int typing;
    char chat_in[80];
    char chat_log[6][96];
    float chat_time[6];
} game;

static const char *HULL_NAME[CUSTOM_HULLS] = {"Battleship", "Battlecruiser", "Cruiser", "Destroyer",
                                              "DestroyerEscort", "AircraftCarrier", "TorpedoBoat", "Submarine"};

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
    } else if (!strcmp(name, "fleet") || !strcmp(name, "archipelago")) {
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
        if (!strcmp(name, "archipelago")) islands_generate(&g->w, 7, 10, -4500, 1200, 4500, 4300);
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
        g->w.time_limit = assault ? 60 * 60 : 30 * 60;   /* assault: then decided on points */
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
    } else if (!strcmp(name, "custom")) {
        /* Fleets from the picker: capital ships in the first line, the rest behind. */
        const custom_spec *cs = &g->custom;
        for (int sd = 0; sd < 2; sd++) {
            int n = 0;
            for (int h = 0; h < CUSTOM_HULLS; h++) n += cs->count[sd][h];
            int k = 0;
            for (int h = 0; h < CUSTOM_HULLS; h++)
                for (int c = 0; c < cs->count[sd][h]; c++, k++) {
                    char cls[64];
                    snprintf(cls, sizeof cls, "%s%s", cs->nation[sd], HULL_NAME[h]);
                    int row = h < 3 || h == 5 ? 0 : 1, per = n > 8 ? 8 : n;
                    float x = ((float)(k % per) - (float)(per - 1) / 2) * 380;
                    float z = sd == 0 ? -500.0f * (float)row - 400.0f * (float)(k / 8) : 6000 + 500.0f * (float)row + 400.0f * (float)(k / 8);
                    err |= spawn(g, cls, sd, x, z, sd ? PI_F : 0) < 0;
                }
        }
        if (cs->islands) islands_generate(&g->w, 11, 12, -5000, 1000, 5000, 5000);
        if (cs->yards) {
            g->w.mode = MODE_ASSAULT;
            g->w.time_limit = 60 * 60;
            for (int sd = 0; sd < 2; sd++) {
                const char *opts[CUSTOM_HULLS];
                char names[CUSTOM_HULLS][64];
                int no = 0;
                for (int h = 0; h < CUSTOM_HULLS; h++) {
                    snprintf(names[h], sizeof names[h], "%s%s", cs->nation[sd], HULL_NAME[h]);
                    if (class_find(&g->classes, names[h])) opts[no++] = names[h];
                }
                err |= yard(g, cs->nation[sd], sd, 0, sd ? 13000 : -7000, sd ? PI_F : 0, opts, no);
            }
        }
        g->cam.target = v3(0, 0, 1200);
    } else {
        fprintf(stderr, "unknown scenario %s (m01, fleet, archipelago, carriers, assault, attack, defend, custom)\n", name);
        return -1;
    }
    if (err) return -1;
    /* Every team gets four distinguished officers; each player's biggest ship starts as
     * flagship (the player can move both). */
    static const officer_kind staff[] = {OFF_GUNNERY, OFF_FIRE_CONTROL, OFF_DAMAGE_CONTROL, OFF_AA};
    for (int sd = 0; sd < 2; sd++)
        for (int k = 0; k < 4; k++) officer_add(&g->w, sd, staff[k]);
    for (int p = 0; p < 2; p++) {
        int best = -1;
        float bc = -1;
        for (int i = 0; i < g->w.count; i++)
            if (g->w.ships[i].owner == p && !is_shore(&g->w.ships[i]) && class_cost(g->w.ships[i].cls) > bc) {
                bc = class_cost(g->w.ships[i].cls);
                best = i;
            }
        if (best >= 0) order_flagship(&g->w, p, best);
    }
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
        if (s->side == g->side || !s->alive || !visible_to(&g->w, g->side, i) ||
            !project(vp, v3(s->x, 10, s->z), w, h, &sx, &sy)) continue;
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
        if (s->owner != g->player || !s->alive || !project(vp, v3(s->x, 10, s->z), w, h, &sx, &sy)) continue;
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
/* A command for the selected ships, from the local player. */
static command sel_cmd(const game *g, cmd_kind kind)
{
    command c;
    memset(&c, 0, sizeof c);
    c.kind = (uint8_t)kind;
    c.player = (uint8_t)g->player;
    for (int i = 0; i < g->w.count; i++)
        if (g->selected[i]) cmd_set_ship(&c, i);
    return c;
}

/* Everything the player orders goes through the command stream (command.h). */
static void issue(game *g, const command *c)
{
    /* The first ship ordered answers (move and attack orders). */
    if (c->kind == CMD_MOVE || c->kind == CMD_ATTACK)
        for (int i = 0; i < g->w.count; i++)
            if (cmd_has_ship(c, i) && g->w.ships[i].owner == g->player && g->w.ships[i].alive) {
                audio_ack(g->snd, &g->w.ships[i]);
                break;
            }
    if (g->net) net_send_command(g->net, c);
    else hud_issue(&g->q, &g->w, c);
}

/* Online: share each team's ships among its human players (round robin; the yard to
 * the first); a team without humans is left to the AI. */
static void assign_players(game *g, const net_game *ng)
{
    for (int side = 0; side < 2; side++) {
        int humans[MAX_PLAYERS], nh = 0;
        for (int p = 0; p < ng->players; p++)
            if (ng->team[p] == side) humans[nh++] = p;
        int k = 0;
        for (int i = 0; i < g->w.count; i++) {
            ship *s = &g->w.ships[i];
            if (s->side != side) continue;
            if (!nh) { s->owner = 8 + side; s->ai = 1; continue; }   /* AI admiral */
            s->owner = is_shore(s) ? humans[0] : humans[k++ % nh];
            s->ai = 0;
        }
    }
    for (int p = 0; p < FLEET_PLAYERS; p++) g->w.flagship_of[p] = -1;
    for (int i = 0; i < g->w.count; i++) g->w.ships[i].flagship = 0;
    for (int p = 0; p < ng->players; p++) {
        int best = -1;
        float bc = -1;
        for (int i = 0; i < g->w.count; i++)
            if (g->w.ships[i].owner == p && !is_shore(&g->w.ships[i]) && class_cost(g->w.ships[i].cls) > bc) {
                bc = class_cost(g->w.ships[i].cls);
                best = i;
            }
        if (best >= 0) order_flagship(&g->w, p, best);
    }
}

/* One tick, lockstep: at a turn boundary apply that turn's commands (the host first
 * closes the turn NET_DELAY ahead); without the turn yet, don't step. 1 if stepped. */
static int mp_step(game *g)
{
    if (!g->net) {
        cmdq_apply_due(&g->q, &g->w);
        world_step(&g->w);
        return 1;
    }
    long t = g->w.tick;
    if (t % NET_TURN == 0) {
        long k = t / NET_TURN;
        if (net_is_host(g->net)) net_make_turn(g->net, k + NET_DELAY);
        if (!net_have_turn(g->net, k)) return 0;
        command cmds[NET_MAX_TURN_CMDS];
        int n = net_turn_commands(g->net, k, cmds, NET_MAX_TURN_CMDS);
        for (int i = 0; i < n; i++) world_apply(&g->w, &cmds[i]);
    }
    world_step(&g->w);
    if (g->w.tick % NET_TURN == 0) net_done_turn(g->net, g->w.tick / NET_TURN - 1, world_checksum(&g->w));
    return 1;
}

/* --bot: scripted orders for testing online play: every 30 s each of the player's
 * ships is sent at the nearest enemy and told to engage it. */
static void bot_orders(game *g)
{
    static long last = -1;
    if (g->w.tick % (30 * SIM_HZ) != 1 || g->w.tick == last) return;
    last = g->w.tick;
    for (int i = 0; i < g->w.count; i++) {
        const ship *s = &g->w.ships[i];
        if (s->owner != g->player || !s->alive || is_shore(s)) continue;
        int best = -1;
        float bd = INFINITY;
        for (int j = 0; j < g->w.count; j++) {
            const ship *t = &g->w.ships[j];
            if (!t->alive || t->side == s->side || t->cls->submerged) continue;
            float d = hypotf(t->x - s->x, t->z - s->z);
            if (d < bd) { bd = d; best = j; }
        }
        if (best < 0) continue;
        command c;
        memset(&c, 0, sizeof c);
        c.player = (uint8_t)g->player;
        cmd_set_ship(&c, i);
        c.kind = CMD_ATTACK;
        c.a = best;
        c.b = (1 << BAT_MAIN) | (1 << BAT_SECONDARY) | (1 << BAT_TORPEDO);
        issue(g, &c);
        c.kind = CMD_MOVE;
        c.x = (s->x + g->w.ships[best].x) / 2;
        c.z = (s->z + g->w.ships[best].z) / 2;
        c.flag = 0;
        issue(g, &c);
    }
}

/* Orders from a key (or the controls toolbar, which sends the same keys). */
static void order_key(game *g, SDL_Keycode k, int ctrl, hud *hd)
{
    int ids[MAX_SHIPS], n = selected_ids(g, ids);
    command c = sel_cmd(g, CMD_NONE);
    if (k == SDLK_z) {                              /* evasive manoeuvres for a minute */
        c.kind = CMD_EVADE;
        c.x = 60;
    } else if (k == SDLK_t && n) {                  /* cycle the main battery's aim point */
        static const int cycle[] = {-1, MOD_ENGINE, MOD_MAGAZINE, MOD_STEERING, MOD_BRIDGE, MOD_TURRET};
        int cur = g->w.ships[ids[0]].aim[BAT_MAIN], q = 0;
        while (q < 6 && cycle[q] != cur) q++;
        c.kind = CMD_AIM;
        c.a = cycle[(q + 1) % 6];
        c.b = (1 << BAT_MAIN) | (1 << BAT_SECONDARY);
    } else if (k == SDLK_c) hd->show_captain = !hd->show_captain;
    else if (k == SDLK_F1) hd->show_controls = !hd->show_controls;
    else if (k == SDLK_F2) hd->show_fleet = !hd->show_fleet;
    else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) hud_act_top(hd, &g->q, &g->w);
    else if (k == SDLK_s && !ctrl) c.kind = CMD_STOP;
    else if (k == SDLK_b || k == SDLK_g || k == SDLK_r) {
        /* Carriers: B strike the focus target (else the nearest enemy ship),
         * G fighter patrol overhead, R recall the air group. */
        for (int q = 0; q < n; q++) {
            const ship *cv = &g->w.ships[ids[q]];
            if (!is_carrier(cv)) continue;
            command one = c;
            memset(one.ships, 0, sizeof one.ships);
            cmd_set_ship(&one, ids[q]);
            one.kind = k == SDLK_b ? CMD_STRIKE : k == SDLK_g ? CMD_CAP : CMD_RECALL;
            int t = cv->order_target[BAT_MAIN];
            if (k == SDLK_b && (t < 0 || !g->w.ships[t].alive)) {
                float bd = INFINITY;
                t = -1;
                for (int j = 0; j < g->w.count; j++) {
                    const ship *e2 = &g->w.ships[j];
                    if (!e2->alive || e2->side == cv->side || e2->cls->submerged) continue;
                    float d = hypotf(e2->x - cv->x, e2->z - cv->z);
                    if (d < bd) { bd = d; t = j; }
                }
            }
            one.a = t;
            /* Shift+B: torpedo bombers only; Ctrl+B: dive bombers only. */
            if (k == SDLK_b && (SDL_GetModState() & KMOD_SHIFT)) one.b = 1 << PLANE_TORPEDO;
            else if (k == SDLK_b && ctrl) one.b = 1 << PLANE_BOMBER;
            issue(g, &one);
        }
    } else if (k == SDLK_p && n) {
        /* Damage control: P cycles crew priority, X counter-floods, M floods
         * the magazines (no more cook-offs, but those turrets fall silent). */
        c.kind = CMD_DC_POLICY;
        c.a = (g->w.ships[ids[0]].policy + 1) % DC_POLICIES;
    } else if (k == SDLK_x) c.kind = CMD_COUNTER_FLOOD;
    else if (k == SDLK_m) c.kind = CMD_FLOOD_MAGAZINES;
    else if (k == SDLK_l && n == 1) c.kind = CMD_FLAGSHIP;     /* L: make her the flagship */
    else if (k == SDLK_h) {                         /* H toggles hold fire */
        c.kind = CMD_HOLD_FIRE;
        c.flag = n && !g->w.ships[ids[0]].hold_fire;
    } else if (k == SDLK_f) {                       /* F: fire at will (drop focus targets) */
        command free = c;
        free.kind = CMD_ATTACK;
        free.a = -1;
        free.b = -1;
        issue(g, &free);
        c.kind = CMD_HOLD_FIRE;
        c.flag = 0;
    } else if (k >= SDLK_1 && k <= SDLK_9) {
        int gi = (int)(k - SDLK_0);
        if (ctrl) memcpy(g->groups[gi], g->selected, sizeof g->selected);
        else memcpy(g->selected, g->groups[gi], sizeof g->selected);
    }
    if (c.kind != CMD_NONE) issue(g, &c);
}

static int video_init(SDL_Window **win, SDL_GLContext *ctx)
{
    if (SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return -1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
    *win = SDL_CreateWindow("Steel Horizon RTS", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                            1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!*win) {                                    /* retry without MSAA */
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
        *win = SDL_CreateWindow("Steel Horizon RTS", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    }
    *ctx = *win ? SDL_GL_CreateContext(*win) : NULL;
    if (!*ctx || !gl_load()) { fprintf(stderr, "OpenGL 3.3: %s\n", SDL_GetError()); return -1; }
    SDL_GL_SetSwapInterval(1);
    printf("GL %s\n", glGetString(GL_VERSION));
    glEnable(GL_MULTISAMPLE);
    return 0;
}

int main(int argc, char **argv)
{
    const char *assets = "assets", *shot = NULL, *scenario = "m01";
    unsigned long long seed = 1;
    int mute = 0, sound = 0, scenario_given = 0, generated = 0, no_fog = 0, standalone = 0;
    int ai_both = 0, host_port = -1, mp_players = 2, teams[MAX_PLAYERS] = {0}, teams_given = 0, bot = 0;
    long desync_at = -1;
    const char *join_addr = NULL, *player_name = "Admiral";
    float trace = 0;
    int frames = 60;
    float headless = -1, cam_yaw = 0, cam_pitch = 0, cam_dist = 0;
    int cam_ship = -1, cam_plane = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--assets") && i + 1 < argc) assets = argv[++i];
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scenario") && i + 1 < argc) { scenario = argv[++i]; scenario_given = 1; }
        else if (!strcmp(argv[i], "--ai-both")) ai_both = 1;
        else if (!strcmp(argv[i], "--host") && i + 1 < argc) host_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--players") && i + 1 < argc) mp_players = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--join") && i + 1 < argc) join_addr = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) player_name = argv[++i];
        else if (!strcmp(argv[i], "--bot")) bot = 1;
        else if (!strcmp(argv[i], "--mute")) mute = 1;
        else if (!strcmp(argv[i], "--no-fog")) no_fog = 1;
        else if (!strcmp(argv[i], "--standalone")) standalone = 1;   /* no game files at all */
        else if (!strcmp(argv[i], "--sound")) sound = 1;   /* sound in --screenshot runs too */
        else if (!strcmp(argv[i], "--models") && i + 1 < argc) generated = !strcmp(argv[++i], "generated");
        else if (!strcmp(argv[i], "--desync-test") && i + 1 < argc) desync_at = atol(argv[++i]);
        else if (!strcmp(argv[i], "--teams") && i + 1 < argc) {
            const char *p = argv[++i];
            for (int k = 0; k < MAX_PLAYERS && *p; k++) {
                teams[k] = atoi(p) & 1;
                while (*p && *p != ',') p++;
                if (*p == ',') p++;
            }
            teams_given = 1;
        }
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
    g.player = 0;
    g.side = 0;
    char path[512];
    /* Ship data: the game's (extracted from the user's ISO), or our own standalone
     * table when asked or when the game data isn't there. */
    snprintf(path, sizeof path, "%s/data/ships.json", assets);
    FILE *probe = fopen(path, "rb");
    if (probe) fclose(probe);
    else if (!standalone) { printf("no game data under %s: standalone mode\n", assets); standalone = 1; }
    if (standalone) {
        char data[1024];
        char *base = SDL_GetBasePath();                  /* engine/build/ -> engine/data/ */
        snprintf(data, sizeof data, "%s../data/standalone.json", base ? base : "");
        SDL_free(base);
        FILE *f = fopen(data, "rb");
        if (f) fclose(f);
        else snprintf(data, sizeof data, "engine/data/standalone.json");
        if (classes_load_standalone(data, &g.classes)) return 1;
        generated = 1;
    } else {
        snprintf(path, sizeof path, "%s/data", assets);
        if (classes_load(path, &g.classes)) return 1;
        snprintf(path, sizeof path, "%s/files", assets);
        classes_load_layouts(&g.classes, path);
    }
    snprintf(path, sizeof path, "%s/files", assets);
    /* No mode on the command line: the front end chooses (and opens the window). */
    SDL_Window *win = NULL;
    SDL_GLContext ctx = NULL;
    renderer *r = NULL;
    menu_config mc;
    menu_defaults(&mc);
    g.custom = mc.custom;
    int menu_mode = !scenario_given && headless < 0 && !shot && host_port < 0 && !join_addr;
    snprintf(path, sizeof path, "%s/files", assets);
    if (menu_mode) {
        if (video_init(&win, &ctx)) return 1;
        r = renderer_create(path);
        menu_result mr = menu_run(win, r, &g.classes, &mc);
        if (mr == MENU_QUIT) return 0;
        scenario = mc.scenario;
        g.custom = mc.custom;
        player_name = mc.name;
        generated = mc.generated || standalone;
        if (mr == MENU_HOST) {
            host_port = mc.port;
            mp_players = mc.players;
            memcpy(teams, mc.teams, sizeof teams);
            teams_given = 1;
        } else if (mr == MENU_JOIN) {
            join_addr = mc.address;
        }
    }
    /* Online: host or join, wait in the lobby, take the host's scenario and seed. */
    net_game ng;
    if (host_port >= 0 || join_addr) {
        g.net = host_port >= 0 ? net_host(host_port, mp_players, player_name) : NULL;
        if (join_addr) {
            char hostname[128];
            int port = 0;
            const char *colon = strrchr(join_addr, ':');
            snprintf(hostname, sizeof hostname, "%.*s", colon ? (int)(colon - join_addr) : (int)strlen(join_addr), join_addr);
            port = colon ? atoi(colon + 1) : 47800;
            g.net = net_join(hostname, port, player_name);
        }
        if (!g.net) return 1;
        if (net_is_host(g.net)) printf("hosting on port %d, waiting for %d players\n", net_port(g.net), mp_players);
        fflush(stdout);
        if (menu_mode && menu_lobby(win, r, g.net, &mc, seed, &ng)) return 0;
        for (; !menu_mode;) {
            if (net_poll(g.net)) { fprintf(stderr, "lost the connection\n"); return 1; }
            if (net_is_host(g.net) && net_lobby_full(g.net) && !net_started(g.net, NULL)) net_start(g.net, scenario, seed, teams_given ? teams : NULL);
            if (net_started(g.net, &ng) && net_player(g.net) >= 0) break;
            usleep(2000);
        }
        scenario = ng.scenario;
        seed = ng.seed;
        g.custom = ng.custom;
        g.player = net_player(g.net);
        g.side = ng.team[g.player];
        printf("player %d (%s) on team %d, %d players, scenario %s\n", g.player, ng.name[g.player], g.side, ng.players, scenario);
    }
    if (setup_scenario(&g, scenario)) return 1;
    world_seed(&g.w, seed);
    g.w.fog = !no_fog || g.net;                          /* online games always have fog */
    for (int i = 0; ai_both && i < g.w.count; i++) g.w.ships[i].ai = 1;
    if (g.net) assign_players(&g, &ng);

    if (headless >= 0) {
        /* Sim only: the player side holds its course, both sides fire at will;
         * runs until one side is sunk or the time is up. */
        int ticks = (int)(headless * SIM_HZ), t, chat_lines = 0;
        int sunk_seen[MAX_SHIPS] = {0};
        for (t = 0; t < ticks && world_winner(&g.w) < 0;) {
            if (g.net && net_poll(g.net)) { fprintf(stderr, "lost the connection\n"); return 1; }
            if (bot) bot_orders(&g);
            if (!mp_step(&g)) { usleep(500); continue; }   /* waiting for the next turn */
            t++;
            if (desync_at >= 0 && g.w.tick == desync_at) g.w.ships[0].x += 1;   /* --desync-test */
            if (g.net && bot && g.w.tick == 100) {      /* the bot says hello, for the chat test */
                char hello[64];
                snprintf(hello, sizeof hello, "hello from player %d", g.player);
                net_chat(g.net, hello);
            }
            for (char line[96]; g.net && net_chat_take(g.net, line, sizeof line);) {
                printf("chat: %s\n", line);
                chat_lines++;
            }
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
                    int committed = 0, returning = 0;
                    for (int i = 0; i < g.w.count; i++)
                        if (g.w.ships[i].alive && g.w.ships[i].side == sd && !is_shore(&g.w.ships[i])) {
                            committed += g.w.ships[i].committed;
                            returning += g.w.ships[i].returning;
                        }
                    printf("  | side %d: %2d ships (%d committed, %d repairing)", sd, n, committed, returning);
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
        if (g.net) {
            /* Let the last acknowledgements arrive, then report for the test harness. */
            for (int k = 0; k < 200; k++) { net_poll(g.net); usleep(2000); }
            for (char line[96]; net_chat_take(g.net, line, sizeof line);) chat_lines++;
            printf("chat lines %d\n", chat_lines);
            printf("final tick %ld checksum %08x desync %ld\n", g.w.tick, world_checksum(&g.w),
                   net_is_host(g.net) ? net_desync(g.net) : -1L);
            net_close(g.net);
        }
        return 0;
    }

    if (!win) {
        if (video_init(&win, &ctx)) return 1;
        r = renderer_create(path);
    }

    char bank_path[600];
    snprintf(bank_path, sizeof bank_path, "%s/sound/banks/game.bnk", path);
    g.snd = (shot && !sound) || mute ? NULL : audio_open(bank_path);
    renderer_set_generated(r, generated);
    renderer_set_view_side(r, g.w.fog && !shot ? g.side : -1);
    for (int i = 0; i < g.w.count; i++)
        if (renderer_load_class(r, g.w.ships[i].cls)) return 1;
    for (int s = 0; s < 2; s++)                     /* ships the yards can build */
        for (int o = 0; o < g.w.sides[s].option_count; o++) renderer_load_class(r, g.w.sides[s].options[o]);

    Uint64 freq = SDL_GetPerformanceFrequency(), last = SDL_GetPerformanceCounter();
    double acc = 0, sim_time = 0;
    int running = 1, frame = 0, orbit = 0;
    float mx_w, mz_w;
    static hud hd = {.show_captain = 1, .show_controls = 1, .show_fleet = 1, .focus = -1, .pending_officer = -1,
                     .last_card = -1, .hover = -1};
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
            else if (g.typing && e.type == SDL_TEXTINPUT) {
                size_t n = strlen(g.chat_in), add = strlen(e.text.text);
                if (n + add < sizeof g.chat_in) memcpy(g.chat_in + n, e.text.text, add + 1);
            } else if (g.typing && e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                size_t n = strlen(g.chat_in);
                if (k == SDLK_BACKSPACE && n) g.chat_in[n - 1] = 0;
                else if (k == SDLK_ESCAPE) { g.typing = 0; SDL_StopTextInput(); }
                else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    if (n) net_chat(g.net, g.chat_in);
                    g.chat_in[0] = 0;
                    g.typing = 0;
                    SDL_StopTextInput();
                }
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_v) {
                float vx, vz;                            /* V: recon plane to the cursor */
                int mx, my;
                SDL_GetMouseState(&mx, &my);
                if (mouse_to_sea(&g, mx, my, w, h, &vx, &vz)) {
                    command c = sel_cmd(&g, CMD_RECON);
                    c.x = vx;
                    c.z = vz;
                    issue(&g, &c);
                }
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_y && g.net) {
                g.typing = 1;                            /* Y: say something */
                g.chat_in[0] = 0;
                SDL_StartTextInput();
            }
            else if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                int ctrl = (e.key.keysym.mod & KMOD_CTRL) != 0;
                if (k == SDLK_ESCAPE) running = 0;
                else if (k == SDLK_SPACE && !g.net) g.paused = !g.paused;   /* online: no pause, real time */
                else if ((k == SDLK_EQUALS || k == SDLK_PLUS || k == SDLK_KP_PLUS) && !g.net) g.time_scale = g.time_scale < 4 ? g.time_scale * 2 : 4;
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
                } else if (e.button.button == SDL_BUTTON_RIGHT &&
                           hud_minimap_point(&hd, (float)e.button.x, (float)e.button.y, &mx_w, &mz_w)) {
                    command c = sel_cmd(&g, CMD_MOVE);          /* RMB on the minimap: go there */
                    c.x = mx_w;
                    c.z = mz_w;
                    c.flag = (SDL_GetModState() & KMOD_SHIFT) != 0;
                    issue(&g, &c);
                } else if (e.button.button == SDL_BUTTON_RIGHT) {
                    float x, z;
                    int ids[MAX_SHIPS], n = selected_ids(&g, ids);
                    int enemy = n ? enemy_at(&g, e.button.x, e.button.y, w, h) : -1;
                    if (enemy >= 0) {
                        /* Focus fire; Ctrl splits it: only the secondaries switch target. */
                        command c = sel_cmd(&g, CMD_ATTACK);
                        c.a = enemy;
                        c.b = (SDL_GetModState() & KMOD_CTRL) ? 1 << BAT_SECONDARY
                            : (1 << BAT_MAIN) | (1 << BAT_SECONDARY) | (1 << BAT_TORPEDO);
                        issue(&g, &c);
                    } else if (n && mouse_to_sea(&g, e.button.x, e.button.y, w, h, &x, &z)) {
                        command c = sel_cmd(&g, CMD_MOVE);
                        c.x = x;
                        c.z = z;
                        c.flag = (SDL_GetModState() & KMOD_SHIFT) != 0;
                        issue(&g, &c);
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
            if (g.net && net_poll(g.net)) { fprintf(stderr, "lost the connection\n"); running = 0; }
            /* Rejoined (or far behind): replay the history as fast as it goes. */
            for (int k = 0; g.net && k < 6000 && net_latest_turn(g.net) * NET_TURN > g.w.tick + 60; k++)
                if (!mp_step(&g)) break;
            while (acc >= SIM_DT) {
                if (!mp_step(&g)) { acc = SIM_DT; break; }     /* waiting for the next turn */
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
        {
            vec3 eye;
            camera_viewproj(&g.cam, w, h, &eye);
            audio_events(g.snd, &g.w, eye, v3norm(v3sub(g.cam.target, eye)));
        }
        renderer_draw(r, &g.cam, w, h, &g.w, g.paused ? 1.0f : (float)(acc / SIM_DT),
                      g.selected, &g.box, (float)sim_time);
        {
            int mx, my;
            SDL_GetMouseState(&mx, &my);
            hd.mouse_x = (float)mx;
            hd.mouse_y = (float)my;
            hd.hover = hud_over(&hd, (float)mx, (float)my) ? -1 : enemy_at(&g, mx, my, w, h);
            hd.cam_x = g.cam.target.x;
            hd.cam_z = g.cam.target.z;
            hd.cam_yaw = g.cam.yaw;
        }
        int key = hud_draw(&hd, r, &g.w, &g.q, g.player, g.selected, w, h, (float)sim_time);
        if (g.net) {                                     /* chat: new lines in, recent lines shown */
            char line[96];
            while (net_chat_take(g.net, line, sizeof line)) {
                memmove(g.chat_log[1], g.chat_log[0], sizeof g.chat_log[0] * 5);
                memmove(&g.chat_time[1], &g.chat_time[0], sizeof g.chat_time[0] * 5);
                snprintf(g.chat_log[0], sizeof g.chat_log[0], "%s", line);
                g.chat_time[0] = (float)sim_time;
            }
            float cy = (float)h - 290;
            for (int k = 0; k < 6; k++)
                if (g.chat_log[k][0] && sim_time - g.chat_time[k] < 12) {
                    ui_rect(250, cy - 2, 250 + ui_text_width(g.chat_log[k]) + 8, cy + ui_line_height() + 2, 0, 0, 0, 0.55f);
                    ui_text(254, cy, g.chat_log[k], 0.9f, 0.95f, 1, 1);
                    cy -= ui_line_height() + 6;
                }
            if (g.typing) {
                char shown[100];
                snprintf(shown, sizeof shown, "Say: %s_", g.chat_in);
                ui_rect(250, (float)h - 266, 250 + ui_text_width(shown) + 8, (float)h - 246, 0.1f, 0.12f, 0.18f, 0.9f);
                ui_text(254, (float)h - 264, shown, 1, 0.9f, 0.6f, 1);
            }
            ui_flush(r, w, h);
        }
        if (hd.map_clicked) {                            /* LMB on the minimap: look there */
            g.cam.target = v3(hd.map_click_x, 0, hd.map_click_z);
            hd.map_clicked = 0;
        }
        while (g.net && g.q.count) {                     /* online: HUD orders go out too */
            net_send_command(g.net, &g.q.items[g.q.head]);
            g.q.head = (g.q.head + 1) % CMD_QUEUE;
            g.q.count--;
        }
        if (hd.focus >= 0 && hd.focus < g.w.count) {     /* double-clicked a fleet card */
            g.cam.target = v3(g.w.ships[hd.focus].x, 0, g.w.ships[hd.focus].z);
            hd.focus = -1;
        }
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
                         hangar_count(&g.w, ids[q], PLANE_FIGHTER), strike_ready(&g.w, ids[q]));
                break;
            }
        snprintf(title, sizeof title, "Steel Horizon RTS  |  %s x%d  |  %d selected%s%s%s  |  t=%.0fs%s",
                 g.paused ? "PAUSED" : "running", g.time_scale, nsel,
                 nsel && g.w.ships[ids[0]].hold_fire ? " (holding fire)" : "", dc_info, air_info, sim_time,
                 win_side == 0 ? "  |  VICTORY" : win_side == 1 ? "  |  DEFEAT" : "");
        SDL_SetWindowTitle(win, title);
    }
    audio_close(g.snd);
    renderer_destroy(r);
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
