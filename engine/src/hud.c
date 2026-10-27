/* In-battle HUD, immediate mode: each frame lays out and draws its boxes, and a
 * pending click is matched against them as they are drawn. */
#include "hud.h"

#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "math3d.h"

#define PAD 8.0f

static const float PANEL[4] = {0.04f, 0.06f, 0.08f, 0.82f};
static const float TEXT[3] = {0.92f, 0.94f, 0.96f};
static const float DIM[3] = {0.6f, 0.65f, 0.7f};

static void box(hud *h, float x0, float y0, float x1, float y1, const float *c)
{
    ui_rect(x0, y0, x1, y1, c[0], c[1], c[2], c[3]);
    if (h->rect_count < HUD_MAX_RECTS) {
        float *r = h->rects[h->rect_count++];
        r[0] = x0; r[1] = y0; r[2] = x1; r[3] = y1;
    }
}

static int clicked(hud *h, float x0, float y0, float x1, float y1)
{
    if (!h->click || h->click_x < x0 || h->click_x > x1 || h->click_y < y0 || h->click_y > y1) return 0;
    h->click = 0;
    return 1;
}

/* A labelled button; returns 1 when clicked this frame. */
static int button(hud *h, float x, float y, const char *label, float r, float g, float b)
{
    float w = ui_text_width(label) + 2 * PAD, hh = ui_line_height() + 6;
    const float c[4] = {r, g, b, 0.9f};
    box(h, x, y, x + w, y + hh, c);
    ui_text(x + PAD, y + 3, label, 1, 1, 1, 1);
    return clicked(h, x, y, x + w, y + hh);
}

int hud_over(const hud *h, float x, float y)
{
    for (int i = 0; i < h->rect_count; i++) {
        const float *r = h->rects[i];
        if (x >= r[0] && x <= r[2] && y >= r[1] && y <= r[3]) return 1;
    }
    return 0;
}

void ship_label(const world *w, int i, char *out, int n)
{
    const ship_class *c = w->ships[i].cls;
    const char *name = c->name;
    size_t nl = strlen(c->nation);
    if (!strncmp(name, c->nation, nl)) name += nl;
    /* "TorpedoBoat" -> "Torpedo Boat" */
    char spaced[64];
    int k = 0;
    for (const char *p = name; *p && k < 62; p++) {
        if (p != name && *p >= 'A' && *p <= 'Z') spaced[k++] = ' ';
        spaced[k++] = *p;
    }
    spaced[k] = 0;
    snprintf(out, (size_t)n, "%s %d", spaced, i + 1);
}

static void report_text(const world *w, const captain_report *r, char *msg, int n, const char **action)
{
    const ship *s = &w->ships[r->ship];
    int fires = 0;
    for (int k = 0; k < s->cls->comp_count; k++) fires += s->comp[k].fire > 0;
    switch (r->kind) {
    case REP_FLOOD_MAGAZINE: snprintf(msg, (size_t)n, "Fire next to a magazine!"); *action = "Flood magazine"; break;
    case REP_EVADE_TORPEDO: snprintf(msg, (size_t)n, "Torpedoes inbound"); *action = "Evade"; break;
    case REP_COUNTER_FLOOD: snprintf(msg, (size_t)n, "Listing %.0f deg", fabsf(s->list) * 180 / PI_F); *action = "Counter-flood"; break;
    case REP_FIGHT_FLOODING: snprintf(msg, (size_t)n, "Taking on water (%.0f%% of reserve)", 100 * s->water / s->cls->reserve_buoyancy);
        *action = "Pumps first"; break;
    case REP_FIGHT_FIRES: snprintf(msg, (size_t)n, "%d fires burning", fires); *action = "Fight fires"; break;
    case REP_WITHDRAW: snprintf(msg, (size_t)n, "Badly hurt and under fire"); *action = "Withdraw"; break;
    case REP_LAUNCH_STRIKE: snprintf(msg, (size_t)n, "Bombers ready, target in range"); *action = "Launch strike"; break;
    case REP_BRIDGE_LOST: snprintf(msg, (size_t)n, "Bridge destroyed: no reports"); *action = NULL; break;
    case REP_RETURN_TO_YARD: snprintf(msg, (size_t)n, "Needs the drydock (%.0f%%)", 100 * s->hp / s->cls->max_health);
        *action = "Return to yard"; break;
    default: msg[0] = 0; *action = NULL; break;
    }
}

void hud_issue(command_queue *q, const world *w, const command *c)
{
    cmdq_push(q, c, w->tick);
}

static void report_cmd(command_queue *q, const world *w, int player, const captain_report *r, cmd_kind kind)
{
    command c;
    memset(&c, 0, sizeof c);
    c.kind = (uint8_t)kind;
    c.player = (uint8_t)player;
    c.a = r->ship;
    c.b = r->kind;
    hud_issue(q, w, &c);
}

/* This player's reports only: other fleets' captains report to their own admirals. */
static int my_reports(const world *w, int player, captain_report *out, int max)
{
    captain_report all[64];
    int side = -1;
    for (int i = 0; i < w->count && side < 0; i++)
        if (w->ships[i].owner == player) side = w->ships[i].side;
    if (side < 0) return 0;
    int n = captain_reports(w, side, all, 64), k = 0;
    for (int i = 0; i < n && k < max; i++)
        if (w->ships[all[i].ship].owner == player) out[k++] = all[i];
    return k;
}

void hud_act_top(hud *h, command_queue *q, const world *w)
{
    captain_report rep[32];
    int n = my_reports(w, h->player, rep, 32);
    for (int i = 0; i < n; i++)
        if (rep[i].kind != REP_BRIDGE_LOST) { report_cmd(q, w, h->player, &rep[i], CMD_CAPTAIN_ACT); return; }
}

/* ---- Captain's Command bar (top right) ------------------------------------------------ */

static void captain_bar(hud *h, world *w, command_queue *q, int sw, float t)
{
    captain_report rep[32];
    int n = my_reports(w, h->player, rep, 32), pending = 0;
    for (int i = 0; i < n; i++) pending += rep[i].kind != REP_BRIDGE_LOST;
    float lh = ui_line_height();
    if (!h->show_captain) {
        /* Collapsed: a tab that flashes while reports are waiting. */
        char tab[64];
        snprintf(tab, sizeof tab, "Captain's reports: %d  [C]", pending);
        float flash = pending ? 0.5f + 0.5f * sinf(t * 6) : 0;
        float x = (float)sw - ui_text_width(tab) - 3 * PAD;
        const float c[4] = {0.25f + 0.55f * flash, 0.18f + 0.2f * flash, 0.08f, 0.9f};
        box(h, x, 10, (float)sw - 10, 10 + lh + 10, c);
        ui_text(x + PAD, 15, tab, 1, 1, 1, 1);
        if (clicked(h, x, 10, (float)sw - 10, 10 + lh + 10)) h->show_captain = 1;
        return;
    }
    float bw = 430, x0 = (float)sw - bw - 10, y = 10;
    int cap = h->show_fleet ? 4 : 7, shown = n < cap ? n : cap;
    float height = lh + 14 + (float)(shown ? shown : 1) * (2 * lh + 16) + (n > shown ? lh + 6 : 0);
    box(h, x0, y, x0 + bw, y + height, PANEL);
    ui_text(x0 + PAD, y + 5, "CAPTAIN'S COMMAND", 1, 0.85f, 0.5f, 1);
    if (button(h, x0 + bw - 70, y + 2, "Hide", 0.25f, 0.28f, 0.32f)) h->show_captain = 0;
    y += lh + 14;
    if (!n) ui_text(x0 + PAD, y + 8, "All ships report: nothing to act on.", DIM[0], DIM[1], DIM[2], 1);
    for (int i = 0; i < shown; i++) {
        char who[64], msg[96];
        const char *action;
        ship_label(w, rep[i].ship, who, sizeof who);
        report_text(w, &rep[i], msg, sizeof msg, &action);
        float cy = y + (float)i * (2 * lh + 16);
        int urgent = rep[i].urgency >= 90;
        const float card[4] = {urgent ? 0.35f : 0.12f, urgent ? 0.1f : 0.15f, urgent ? 0.08f : 0.18f, 0.9f};
        box(h, x0 + 4, cy, x0 + bw - 4, cy + 2 * lh + 12, card);
        ui_text(x0 + PAD + 4, cy + 4, who, 0.65f, 0.85f, 1, 1);
        ui_text(x0 + PAD + 4, cy + lh + 6, msg, action ? TEXT[0] : DIM[0], action ? TEXT[1] : DIM[1], action ? TEXT[2] : DIM[2], 1);
        if (!action) continue;
        float bx = x0 + bw - 4 - PAD - (ui_text_width(action) + 2 * PAD) - 30;
        if (button(h, bx, cy + lh / 2, action, 0.2f, 0.45f, 0.25f)) report_cmd(q, w, h->player, &rep[i], CMD_CAPTAIN_ACT);
        if (button(h, x0 + bw - 30, cy + lh / 2, "x", 0.3f, 0.3f, 0.32f)) report_cmd(q, w, h->player, &rep[i], CMD_CAPTAIN_DISMISS);
    }
    if (n > shown) {
        char more[48];
        snprintf(more, sizeof more, "+%d more", n - shown);
        ui_text(x0 + PAD, y + (float)shown * (2 * lh + 16), more, DIM[0], DIM[1], DIM[2], 1);
    }
}

/* ---- controls toolbar (top left) ------------------------------------------------------ */

typedef struct { SDL_Keycode key; const char *label; } control;

static int controls_bar(hud *h, const world *w, const unsigned char *selected)
{
    float lh = ui_line_height(), x0 = 10, y = 10;
    int any = 0, carrier = 0;
    for (int i = 0; i < w->count; i++)
        if (selected[i] && w->ships[i].alive) { any = 1; carrier |= is_carrier(&w->ships[i]); }
    if (!h->show_controls) {
        if (button(h, x0, y, "Controls [F1]", 0.15f, 0.2f, 0.26f)) h->show_controls = 1;
        h->controls_bottom = y + lh + 6;
        return 0;
    }
    static const control orders[] = {
        {SDLK_h, "[H] Hold fire"}, {SDLK_f, "[F] Fire at will"}, {SDLK_t, "[T] Aim: next part"},
        {SDLK_z, "[Z] Evasive"}, {SDLK_s, "[S] Stop"}};
    static const control dc[] = {{SDLK_p, "[P] Crew priority"}, {SDLK_x, "[X] Counter-flood"}, {SDLK_m, "[M] Flood magazines"}};
    static const control air[] = {{SDLK_b, "[B] Air strike"}, {SDLK_g, "[G] Fighter patrol"}, {SDLK_r, "[R] Recall"}};
    static const char *hints[] = {
        "LMB select / drag box   Shift add", "RMB sea: move   Shift: queue",
        "RMB enemy: focus fire   Ctrl: secondaries", "1-9 groups   Ctrl+1-9 set",
        "WASD/QE/wheel/MMB camera", "Space pause   +/- speed", "Enter: top captain's report", "C captain's bar"};
    float bw = 400;
    int mark = ui_mark(), rect = h->rect_count;
    box(h, x0, y, x0 + bw, y + 10, PANEL);          /* bottom edge set once laid out */
    ui_text(x0 + PAD, y + 5, "CONTROLS", 1, 0.85f, 0.5f, 1);
    int key = 0;
    if (button(h, x0 + bw - 70, y + 2, "Hide", 0.25f, 0.28f, 0.32f)) h->show_controls = 0;
    y += lh + 12;
    for (size_t i = 0; i < sizeof hints / sizeof *hints; i++, y += lh + 3) ui_text(x0 + PAD, y, hints[i], DIM[0], DIM[1], DIM[2], 1);
    y += 6;
    if (!any) {
        ui_text(x0 + PAD, y, "Select ships for their orders", TEXT[0], TEXT[1], TEXT[2], 1);
        ui_set_bottom(mark, y + lh + 10);
        h->rects[rect][3] = y + lh + 10;
        h->controls_bottom = y + lh + 10;
        return 0;
    }
    float bx = x0 + PAD;
    for (size_t i = 0; i < sizeof orders / sizeof *orders; i++) {
        float bwid = ui_text_width(orders[i].label) + 2 * PAD;
        if (bx + bwid > x0 + bw - PAD) { bx = x0 + PAD; y += lh + 10; }
        if (button(h, bx, y, orders[i].label, 0.18f, 0.3f, 0.42f)) key = orders[i].key;
        bx += bwid + 6;
    }
    y += lh + 14;
    bx = x0 + PAD;
    for (size_t i = 0; i < sizeof dc / sizeof *dc; i++) {
        float bwid = ui_text_width(dc[i].label) + 2 * PAD;
        if (bx + bwid > x0 + bw - PAD) { bx = x0 + PAD; y += lh + 10; }
        if (button(h, bx, y, dc[i].label, 0.4f, 0.25f, 0.15f)) key = dc[i].key;
        bx += bwid + 6;
    }
    if (carrier) {
        y += lh + 14;
        bx = x0 + PAD;
        for (size_t i = 0; i < sizeof air / sizeof *air; i++) {
            float bwid = ui_text_width(air[i].label) + 2 * PAD;
            if (bx + bwid > x0 + bw - PAD) { bx = x0 + PAD; y += lh + 10; }
            if (button(h, bx, y, air[i].label, 0.2f, 0.38f, 0.3f)) key = air[i].key;
            bx += bwid + 6;
        }
    }
    ui_set_bottom(mark, y + lh + 16);
    h->rects[rect][3] = y + lh + 16;
    h->controls_bottom = y + lh + 16;
    return key;
}

/* ---- selected ship readout, above the damage panel ------------------------------------ */

static const char *aim_name(int kind)
{
    switch (kind) {
    case MOD_ENGINE: return "engines";
    case MOD_MAGAZINE: return "magazines";
    case MOD_STEERING: return "steering";
    case MOD_BRIDGE: return "bridge";
    case MOD_TURRET: return "guns";
    default: return "the ship";
    }
}

static void ship_readout(hud *h, const world *w, const unsigned char *selected, int sw, int sh)
{
    int i = -1;
    for (int k = 0; k < w->count && i < 0; k++)
        if (selected[k] && w->ships[k].alive) i = k;
    if (i < 0) return;
    const ship *s = &w->ships[i];
    static const char *policy[] = {"auto", "fires", "flooding", "weapons"};
    char who[64], l1[192], l2[192];
    ship_label(w, i, who, sizeof who);
    if (is_shore(s))
        snprintf(l1, sizeof l1, "%s   condition %.0f%%  crew %.0f/%.0f  DC: %s%s", who, 100 * s->hp / s->cls->max_health,
                 s->crew, s->cls->complement, policy[s->policy], module_health(s, MOD_BRIDGE, -1) <= 0 ? "  HQ OUT" : "");
    else
        snprintf(l1, sizeof l1, "%s   condition %.0f%%  water %.0f%%/%.0f%%  list %.0f deg  crew %.0f/%.0f  DC: %s%s",
                 who, 100 * s->hp / s->cls->max_health, 100 * s->water, 100 * s->cls->reserve_buoyancy,
                 s->list * 180 / PI_F, s->crew, s->cls->complement, policy[s->policy],
                 module_health(s, MOD_BRIDGE, -1) <= 0 && !s->cls->submerged ? "  BRIDGE OUT" : "");
    /* Accuracy readout: the main battery against its focus target, else what it's on. */
    int target = s->order_target[BAT_MAIN];
    if (target < 0 || !w->ships[target].alive) {
        target = -1;
        for (int m = 0; m < s->cls->mount_count && target < 0; m++)
            if (s->cls->mounts[m].bat == BAT_MAIN && s->turrets[m].target >= 0) target = s->turrets[m].target;
    }
    int mains = 0;
    for (int m = 0; m < s->cls->mount_count; m++) mains += s->cls->mounts[m].bat == BAT_MAIN;
    if (target >= 0) {
        float p = 0;
        int n = 0;
        for (int m = 0; m < s->cls->mount_count; m++)
            if (s->cls->mounts[m].bat == BAT_MAIN && can_engage(w, i, m, target)) { p += hit_probability(w, i, m, target); n++; }
        char tname[64];
        ship_label(w, target, tname, sizeof tname);
        if (n)
            snprintf(l2, sizeof l2, "Main battery on %s%s: %.0f%% per shell, %d of %d turrets bearing   aim: %s [T]",
                     tname, s->order_target[BAT_MAIN] == target ? " (focus)" : "", 100 * p / (float)n, n,
                     mains, aim_name(s->aim[BAT_MAIN]));
        else
            snprintf(l2, sizeof l2, "Main battery: %s out of arc or range   aim: %s [T]", tname, aim_name(s->aim[BAT_MAIN]));
    } else {
        snprintf(l2, sizeof l2, "Main battery: no target%s   aim: %s [T]", s->hold_fire ? " (holding fire)" : "",
                 aim_name(s->aim[BAT_MAIN]));
    }
    float lh = ui_line_height(), pw = fminf(760, (float)sw - 40), x0 = ((float)sw - pw) / 2, y0 = (float)sh - 170;
    float wtext = fmaxf(ui_text_width(l1), ui_text_width(l2)) + 2 * PAD;
    box(h, x0 - 10, y0 - 2 * lh - 22, x0 - 10 + fmaxf(wtext, pw + 20), y0 - 10, PANEL);
    ui_text(x0, y0 - 2 * lh - 17, l1, TEXT[0], TEXT[1], TEXT[2], 1);
    ui_text(x0, y0 - lh - 13, l2, 0.95f, 0.85f, 0.55f, 1);
    /* Legend along the bottom of the damage panel. */
    float ly = y0 + 122;
    float x = x0 + 20;
    x += ui_text(x, ly, "turret ", 0.3f, 0.9f, 0.35f, 1);
    x += ui_text(x, ly, "magazine ", 1, 0.6f, 0.2f, 1);
    x += ui_text(x, ly, "water ", 0.3f, 0.55f, 1, 1);
    x += ui_text(x, ly, "fire ", 1, 0.45f, 0.1f, 1);
    x += ui_text(x, ly, "teams ", 1, 1, 1, 1);
    x += ui_text(x, ly, "| ok ", 0.3f, 0.9f, 0.35f, 1);
    x += ui_text(x, ly, "hit ", 0.95f, 0.8f, 0.2f, 1);
    x += ui_text(x, ly, "bad ", 0.95f, 0.25f, 0.2f, 1);
    ui_text(x, ly, "lost", 0.5f, 0.5f, 0.5f, 1);
    ui_text(x0 + pw - 95, y0 + 2, "list", DIM[0], DIM[1], DIM[2], 1);
    ui_text(x0 + pw - 135, y0 + 76, "water", DIM[0], DIM[1], DIM[2], 1);
    ui_text(x0 + pw - 135, y0 + 90, "cond.", DIM[0], DIM[1], DIM[2], 1);
}

/* ---- shipyard (left, below the controls) and the game-mode banner ---------------------- */

static float shipyard_panel(hud *h, world *w, command_queue *q, float y)
{
    int side = -1;
    for (int i = 0; i < w->count && side < 0; i++)
        if (w->ships[i].owner == h->player) side = w->ships[i].side;
    if (side < 0) return y;
    const side_state *st = &w->sides[side];
    if (st->yard < 0 || w->ships[st->yard].owner != h->player) return y;
    const ship *yard = &w->ships[st->yard];
    float lh = ui_line_height(), x0 = 10, bw = 400;
    int mark = ui_mark(), rect = h->rect_count;
    box(h, x0, y, x0 + bw, y + 10, PANEL);
    char line[128];
    snprintf(line, sizeof line, "SHIPYARD  %s", yard->alive ? "" : "(destroyed)");
    ui_text(x0 + PAD, y + 5, line, 1, 0.85f, 0.5f, 1);
    y += lh + 10;
    float slip = module_health(yard, MOD_SLIPWAY, -1), dock = module_health(yard, MOD_DRYDOCK, -1);
    snprintf(line, sizeof line, "budget %.0f  +%.0f/s   slipway %.0f%%  drydock %.0f%%", st->budget, st->income,
             100 * slip, 100 * dock);
    ui_text(x0 + PAD, y, line, DIM[0], DIM[1], DIM[2], 1);
    y += lh + 4;
    for (int q = 0; q < st->queue_count; q++, y += lh + 2) {
        char who[64];
        const ship_class *c = st->queue[q];
        snprintf(who, sizeof who, "%s", c->name + strlen(c->nation));
        if (q == 0) snprintf(line, sizeof line, "building %-20s %3.0f%%", who, 100 * st->progress / build_time(c));
        else snprintf(line, sizeof line, "queued   %s", who);
        ui_text(x0 + PAD, y, line, q ? DIM[0] : TEXT[0], q ? DIM[1] : TEXT[1], q ? DIM[2] : TEXT[2], 1);
    }
    y += 4;
    float bx = x0 + PAD;
    for (int o = 0; o < st->option_count && yard->alive; o++) {
        const ship_class *c = st->options[o];
        char label[64];
        snprintf(label, sizeof label, "%s %.0f", c->name + strlen(c->nation), class_cost(c));
        float bwid = ui_text_width(label) + 2 * PAD;
        if (bx + bwid > x0 + bw - PAD) { bx = x0 + PAD; y += lh + 10; }
        int ok = st->budget >= class_cost(c) && st->queue_count < MAX_QUEUE;
        if (button(h, bx, y, label, ok ? 0.22f : 0.2f, ok ? 0.36f : 0.2f, ok ? 0.5f : 0.22f) && ok) {
            command bc;
            memset(&bc, 0, sizeof bc);
            bc.kind = CMD_BUILD;
            bc.player = (uint8_t)h->player;
            bc.b = o;
            hud_issue(q, w, &bc);
        }
        bx += bwid + 6;
    }
    y += lh + 16;
    ui_set_bottom(mark, y);
    h->rects[rect][3] = y;
    return y + 8;
}

/* ---- fleet toolbar (bottom right): a card per ship with a live damage thumbnail ---- */

static const char OFFICER_LETTER[OFFICER_KINDS] = {'G', 'F', 'D', 'E', 'A', 'N', 'V'};
static const char *OFFICER_NAME[OFFICER_KINDS] = {"gunnery", "fire control", "damage control", "engineering",
                                                  "anti-aircraft", "navigation", "aviation"};

static void fleet_bar(hud *h, renderer *r, world *w, command_queue *q, unsigned char *selected, int sw, int sh, float t)
{
    if (!h->show_fleet) return;
    int ids[MAX_SHIPS], n = 0, side = -1;
    for (int i = 0; i < w->count; i++)
        if (w->ships[i].owner == h->player) {
            if (side < 0) side = w->ships[i].side;
            if (w->ships[i].alive && !is_shore(&w->ships[i])) ids[n++] = i;
        }
    if (side < 0) return;
    float lh = ui_line_height(), cw = 250, ch = 46, x0 = (float)sw - cw - 10;
    int shown = n < 9 ? n : 9;
    int officers = w->officer_count[side & 1];
    float top = (float)sh - 10 - (float)shown * (ch + 4) - (officers ? 2 * lh + 18 : 0) - lh - 10;
    box(h, x0 - 4, top, x0 + cw + 4, (float)sh - 6, PANEL);
    ui_text(x0 + 4, top + 4, "FLEET [F2]   L: flagship", 1, 0.85f, 0.5f, 1);
    float y = top + lh + 8;
    if (officers) {
        /* Officers: click one, then a ship card, to post him there. */
        float bx = x0;
        for (int k = 0; k < officers; k++) {
            int at = w->officer_ship[side & 1][k];
            char lab[16];
            if (at == -2) snprintf(lab, sizeof lab, "%c:x", OFFICER_LETTER[w->officer_kind_of[side & 1][k]]);
            else if (at >= 0) snprintf(lab, sizeof lab, "%c:%d", OFFICER_LETTER[w->officer_kind_of[side & 1][k]], at + 1);
            else snprintf(lab, sizeof lab, "%c:-", OFFICER_LETTER[w->officer_kind_of[side & 1][k]]);
            float bwid = ui_text_width(lab) + 2 * PAD;
            if (bx + bwid > x0 + cw) { bx = x0; y += lh + 8; }
            int picked = h->pending_officer == k;
            if (button(h, bx, y, lab, picked ? 0.7f : 0.3f, picked ? 0.55f : 0.3f, picked ? 0.15f : 0.35f) && at != -2)
                h->pending_officer = picked ? -1 : k;
            bx += bwid + 4;
        }
        y += lh + 10;
        if (h->pending_officer >= 0 && h->pending_officer < officers)
            ui_text(x0, y - 2, OFFICER_NAME[w->officer_kind_of[side & 1][h->pending_officer]], DIM[0], DIM[1], DIM[2], 1);
        y += 4;
    }
    for (int c = 0; c < shown; c++, y += ch + 4) {
        int i = ids[c];
        const ship *s = &w->ships[i];
        const float bg[4] = {selected[i] ? 0.16f : 0.1f, selected[i] ? 0.3f : 0.12f, selected[i] ? 0.2f : 0.15f, 0.95f};
        box(h, x0, y, x0 + cw, y + ch, bg);
        ui_ship_profile(r, s, x0 + 4, y + 4, 110, ch - 8, 0, t);
        char name[32], who[64];
        ship_label(w, i, who, sizeof who);
        snprintf(name, sizeof name, "%.13s", who);
        ui_text(x0 + 120, y + 3, name, s->flagship ? 1.0f : TEXT[0], s->flagship ? 0.85f : TEXT[1], s->flagship ? 0.4f : TEXT[2], 1);
        if (s->flagship) ui_text(x0 + cw - 14, y + 3, "*", 1, 0.85f, 0.3f, 1);
        float cf = s->hp / s->cls->max_health;
        ui_rect(x0 + 120, y + lh + 6, x0 + cw - 6, y + lh + 10, 0.08f, 0.08f, 0.1f, 1);
        ui_rect(x0 + 120, y + lh + 6, x0 + 120 + (cw - 126) * cf, y + lh + 10,
                cf > 0.66f ? 0.3f : cf > 0.33f ? 0.95f : 0.95f, cf > 0.66f ? 0.9f : cf > 0.33f ? 0.8f : 0.25f, cf > 0.66f ? 0.35f : 0.2f, 1);
        /* Afflictions: fires, water, list; officers aboard. */
        int fires = 0;
        for (int k = 0; k < s->cls->comp_count; k++) fires += s->comp[k].fire > 0;
        char st[48];
        float sx = x0 + 120;
        if (fires) { snprintf(st, sizeof st, "^%d ", fires); sx += ui_text(sx, y + lh + 12, st, 1, 0.5f, 0.15f, 1); }
        if (s->water > 0.02f) { snprintf(st, sizeof st, "~%.0f%% ", 100 * s->water / s->cls->reserve_buoyancy); sx += ui_text(sx, y + lh + 12, st, 0.4f, 0.65f, 1, 1); }
        if (fabsf(s->list) > 0.03f) { snprintf(st, sizeof st, "/%.0f ", fabsf(s->list) * 180 / PI_F); sx += ui_text(sx, y + lh + 12, st, 0.9f, 0.9f, 0.6f, 1); }
        for (int k = 0; k < OFFICER_KINDS; k++)
            if (s->officers[k]) { snprintf(st, sizeof st, "%c", OFFICER_LETTER[k]); sx += ui_text(sx, y + lh + 12, st, 0.95f, 0.8f, 0.4f, 1); }
        if (clicked(h, x0, y, x0 + cw, y + ch)) {
            if (h->pending_officer >= 0) {                   /* post the picked officer here */
                command oc;
                memset(&oc, 0, sizeof oc);
                oc.kind = CMD_OFFICER;
                oc.player = (uint8_t)h->player;
                oc.a = h->pending_officer;
                cmd_set_ship(&oc, i);
                hud_issue(q, w, &oc);
                h->pending_officer = -1;
            } else {
                if (!(SDL_GetModState() & KMOD_SHIFT)) memset(selected, 0, MAX_SHIPS);
                selected[i] = 1;
                if (h->last_card == i && t - h->last_click_t < 0.5f) h->focus = i;
                h->last_card = i;
                h->last_click_t = t;
            }
        }
    }
    if (n > shown) {
        char more[32];
        snprintf(more, sizeof more, "+%d more", n - shown);
        ui_text(x0 + 4, y - 2, more, DIM[0], DIM[1], DIM[2], 1);
    }
}

static void mode_banner(const world *w, int sw)
{
    char msg[128];
    switch (w->mode) {
    case MODE_ASSAULT: snprintf(msg, sizeof msg, "ASSAULT: destroy the enemy yard"); break;
    case MODE_DEFEND: {
        int left = (int)mode_time_left(w);
        snprintf(msg, sizeof msg, w->defender == 0 ? "DEFEND the yard: %d:%02d left" : "ATTACK the yard: %d:%02d left",
                 left / 60, left % 60);
        break;
    }
    default: return;
    }
    float tw = ui_text_width(msg);
    ui_rect((float)sw / 2 - tw / 2 - 12, 10, (float)sw / 2 + tw / 2 + 12, 36, 0.04f, 0.06f, 0.08f, 0.8f);
    ui_text((float)sw / 2 - tw / 2, 15, msg, 1, 0.85f, 0.5f, 1);
}

int hud_draw(hud *h, renderer *r, world *w, command_queue *q, int player, unsigned char *selected, int sw, int sh, float t)
{
    h->rect_count = 0;
    h->player = player;
    mode_banner(w, sw);
    int key = controls_bar(h, w, selected);
    shipyard_panel(h, w, q, h->controls_bottom + 8);
    captain_bar(h, w, q, sw, t);
    fleet_bar(h, r, w, q, selected, sw, sh, t);
    ship_readout(h, w, selected, sw, sh);
    int result = world_winner(w);
    if (result >= 0) {
        const char *msg = result == 0 ? "VICTORY" : "DEFEAT";
        float tw = ui_text_width(msg);
        ui_rect((float)sw / 2 - tw / 2 - 20, 60, (float)sw / 2 + tw / 2 + 20, 90, 0, 0, 0, 0.7f);
        ui_text((float)sw / 2 - tw / 2, 67, msg, 1, 0.85f, 0.4f, 1);
    }
    h->click = 0;              /* a click that hit nothing here belongs to the world */
    ui_flush(r, sw, sh);
    return key;
}
