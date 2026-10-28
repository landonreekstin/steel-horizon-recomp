/* In-battle HUD, immediate mode: each frame lays out and draws its boxes, and a
 * pending click is matched against them as they are drawn. */
#include "hud.h"
#include "tutorial.h"

#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "math3d.h"

#define PAD 8.0f

static void box(hud *h, float x0, float y0, float x1, float y1, const float *c);
static int button(hud *h, float x, float y, const char *label, float r, float g, float b);


static const float PANEL[4] = {0.04f, 0.06f, 0.08f, 0.82f};
static const float TEXT[3] = {0.92f, 0.94f, 0.96f};
static const float DIM[3] = {0.6f, 0.65f, 0.7f};

void hud_box(hud *h, float x0, float y0, float x1, float y1, const float *c)
{
    box(h, x0, y0, x1, y1, c);
}

int hud_button(hud *h, float x, float y, const char *label, float r, float g, float b)
{
    return button(h, x, y, label, r, g, b);
}

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
    case REP_BATTERY_LOW: snprintf(msg, (size_t)n, "Battery at %.0f%%", 100 * s->battery); *action = "Surface"; break;
    default: msg[0] = 0; *action = NULL; break;
    }
}

static int report_icon(int kind)
{
    switch (kind) {
    case REP_FLOOD_MAGAZINE: return ICON_FLOOD_MAGAZINES;
    case REP_EVADE_TORPEDO: return ICON_TORPEDO;
    case REP_COUNTER_FLOOD: return ICON_COUNTER_FLOOD;
    case REP_FIGHT_FLOODING: return ICON_WATER;
    case REP_FIGHT_FIRES: return ICON_FIRE;
    case REP_WITHDRAW: return ICON_WITHDRAW;
    case REP_LAUNCH_STRIKE: return ICON_STRIKE;
    case REP_BRIDGE_LOST: return ICON_BRIDGE_LOST;
    case REP_RETURN_TO_YARD: return ICON_CREW_PRIORITY;
    case REP_BATTERY_LOW: return ICON_BATTERY;
    default: return -1;
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

static void captain_bar(hud *h, renderer *r, world *w, command_queue *q, int sw, float t)
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
        float ty = h->right_top;
        box(h, x, ty, (float)sw - 10, ty + lh + 10, c);
        ui_text(x + PAD, ty + 5, tab, 1, 1, 1, 1);
        if (clicked(h, x, ty, (float)sw - 10, ty + lh + 10)) h->show_captain = 1;
        return;
    }
    float bw = 560, x0 = (float)sw - bw - 10, y = h->right_top;
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
        ui_icon(report_icon(rep[i].kind), x0 + 8, cy + lh + 6 - 16, urgent ? 1 : 0.85f, urgent ? 0.6f : 0.9f, urgent ? 0.4f : 1, 1);
        ui_ship_profile(r, &w->ships[rep[i].ship], x0 + 44, cy + 3, 86, 2 * lh + 6, 0, t);   /* her damage now */
        ui_text(x0 + PAD + 130, cy + 4, who, 0.65f, 0.85f, 1, 1);
        ui_text(x0 + PAD + 130, cy + lh + 6, msg, action ? TEXT[0] : DIM[0], action ? TEXT[1] : DIM[1], action ? TEXT[2] : DIM[2], 1);
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

/* key: an SDL keycode or a HUD_* action; hint: the shortcut shown under the icon
 * (NULL: the key's letter). */
typedef struct { int key; int icon; const char *name, *tip, *hint; } control;

/* ---- icon readouts ------------------------------------------------------------------- */

static const float OK_C[3] = {0.4f, 0.9f, 0.45f}, WARN_C[3] = {0.98f, 0.8f, 0.25f}, BAD_C[3] = {0.98f, 0.32f, 0.25f};

/* Green, amber or red for how bad a value is: 0 fine .. 1 desperate. */
static const float *severity(float bad)
{
    return bad < 0.34f ? OK_C : bad < 0.67f ? WARN_C : BAD_C;
}

static int aim_icon(int kind)
{
    switch (kind) {
    case MOD_ENGINE: return ICON_AIM_ENGINES;
    case MOD_MAGAZINE: return ICON_AIM_MAGAZINES;
    case MOD_STEERING: return ICON_AIM_STEERING;
    case MOD_BRIDGE: return ICON_AIM_BRIDGE;
    case MOD_TURRET: return ICON_AIM_GUNS;
    default: return ICON_AIM_SHIP;
    }
}

int hud_fire_state(const world *w, int i, const float **col, const char **name)
{
    const ship *s = &w->ships[i];
    int main_t = s->order_target[BAT_MAIN], any_t = -1;
    for (int b = 0; b < BATTERIES; b++)
        if (s->order_target[b] >= 0 && s->order_target[b] < w->count && w->ships[s->order_target[b]].alive) any_t = b;
    static const float HOLD[3] = {0.98f, 0.35f, 0.3f}, FOCUS[3] = {1, 0.65f, 0.2f}, FREE[3] = {0.45f, 0.9f, 0.5f};
    if (s->hold_fire) { *col = HOLD; *name = "Holding fire [H]"; return ICON_HOLD_FIRE; }
    if (main_t >= 0 && main_t < w->count && w->ships[main_t].alive) { *col = FOCUS; *name = "Focus fire on her target"; return ICON_FOCUS; }
    if (any_t >= 0) { *col = FOCUS; *name = "Secondaries on an ordered target, mains free"; return ICON_SPLIT_FIRE; }
    *col = FREE;
    *name = "Fire at will [F]";
    return ICON_FIRE_AT_WILL;
}

/* A 16 px icon and its value, in colour c; hovering it leaves `tip` for the tooltip.
 * Returns the width taken, with a gap after it. */
static float stat(hud *h, float x, float y, int icon, const char *val, const float *c, const char *tip)
{
    ui_icon_small(icon, x, y, c[0], c[1], c[2], 1);
    float w = ICON_SMALL_PX + (val && *val ? 3 + ui_text_width(val) : 0);
    if (val && *val) ui_text(x + ICON_SMALL_PX + 3, y, val, c[0], c[1], c[2], 1);
    if (tip && h->mouse_x >= x && h->mouse_x <= x + w && h->mouse_y >= y - 2 && h->mouse_y <= y + ICON_SMALL_PX + 2)
        snprintf(h->tip, sizeof h->tip, "%s", tip);
    return w + 12;
}

static void draw_tip(hud *h, int sw)
{
    if (!h->tip[0]) return;
    float tw = ui_text_width(h->tip), x = fminf(h->mouse_x + 14, (float)sw - tw - 12), y = h->mouse_y - ui_line_height() - 10;
    ui_rect(x - 5, y - 3, x + tw + 5, y + ui_line_height() + 3, 0.03f, 0.04f, 0.05f, 0.95f);
    ui_text(x, y, h->tip, TEXT[0], TEXT[1], TEXT[2], 1);
    h->tip[0] = 0;
}

#define ICON_BTN 40.0f   /* icon button: a 32 px icon and its key letter under it */

/* One row of icon buttons; returns the key of the one clicked, and leaves the one under
 * the mouse in *hover for the tooltip. Buttons whose keys are in lit[4] are drawn as on. */
static int icon_row_lit(hud *h, float x, float y, const control *c, int n, const float *col, const control **hover, const int lit[4])
{
    int key = 0;
    float lh = ui_line_height();
    for (int i = 0; i < n; i++, x += ICON_BTN + 6) {
        float x1 = x + ICON_BTN, y1 = y + ICON_BTN + lh - 2;
        int over = h->mouse_x >= x && h->mouse_x <= x1 && h->mouse_y >= y && h->mouse_y <= y1;
        float k = over ? 1.35f : 1;
        const float on[4] = {0.75f, 0.55f, 0.15f, 0.95f}, bg[4] = {col[0] * k, col[1] * k, col[2] * k, 0.92f};
        int is_on = 0;
        for (int l = 0; l < 4; l++) is_on |= c[i].key == lit[l];
        box(h, x, y, x1, y1, is_on ? on : bg);
        ui_icon(c[i].icon, x + (ICON_BTN - 32) / 2, y + 3, 1, 1, 1, 1);
        char letter[2] = {(char)(c[i].key >= 'a' && c[i].key <= 'z' ? c[i].key - 32 : '?'), 0};
        const char *hint = c[i].hint ? c[i].hint : letter;
        ui_text(x + (ICON_BTN - ui_text_width(hint)) / 2, y + 34, hint, 1, 0.85f, 0.5f, 1);
        if (over) *hover = &c[i];
        if (clicked(h, x, y, x1, y1)) key = c[i].key;
    }
    return key;
}

static int icon_row(hud *h, float x, float y, const control *c, int n, const float *col, const control **hover)
{
    return icon_row_lit(h, x, y, c, n, col, hover, (const int[4]){-1, -1, -1, -1});
}

static int controls_bar(hud *h, const world *w, const unsigned char *selected)
{
    float lh = ui_line_height(), x0 = 10, y = 10;
    int any = 0, carrier = 0, subs = 0;
    for (int i = 0; i < w->count; i++)
        if (selected[i] && w->ships[i].alive) {
            any = 1;
            carrier |= is_carrier(&w->ships[i]);
            subs |= w->ships[i].cls->submerged;
        }
    if (!h->show_controls) {
        if (button(h, x0, y, "Controls [F1]", 0.15f, 0.2f, 0.26f)) h->show_controls = 1;
        h->controls_bottom = y + lh + 6;
        return 0;
    }
    control orders[] = {
        {SDLK_h, ICON_HOLD_FIRE, "Hold fire", "Guns stop firing until ordered", NULL},
        {SDLK_f, ICON_FIRE_AT_WILL, "Fire at will", "Engage the best target in range", NULL},
        {SDLK_t, ICON_AIM, "Aim", "Cycle the part aimed at: ship, engines, magazines, steering, bridge, guns", NULL},
        {SDLK_z, ICON_EVASIVE, "Evasive", "Zig-zag or comb torpedoes; costs own accuracy", NULL},
        {SDLK_n, ICON_STOP, "Stop", "All stop, clear queued moves", NULL},
        {HUD_SPLIT, ICON_SPLIT_FIRE, "Split fire", "Then click an enemy: only the secondaries switch to her", "^RMB"},
        {HUD_QUEUE, ICON_QUEUE, "Queue moves", "While on, right-click moves are added as waypoints", "Sh"},
        {SDLK_l, ICON_FLAGSHIP, "Flagship", "Make the selected ship the fleet's flagship", NULL}};
    static const control dc[] = {
        {SDLK_p, ICON_CREW_PRIORITY, "Crew priority", "Damage-control teams: cycle fires / flooding / modules first", NULL},
        {SDLK_x, ICON_COUNTER_FLOOD, "Counter-flood", "Flood the opposite side to correct the list", NULL},
        {SDLK_m, ICON_FLOOD_MAGAZINES, "Flood magazines", "Saves the ship from a magazine fire; those guns go silent", NULL}};
    static const control air[] = {
        {SDLK_b, ICON_STRIKE, "Air strike", "All bombers: then click the ship to strike (key B: the enemy under the cursor)", NULL},
        {HUD_TORP_STRIKE, ICON_TORPEDO_STRIKE, "Torpedo strike", "Torpedo bombers only, then click the target: low run-in, flood below the waterline", "Sh+B"},
        {HUD_DIVE_STRIKE, ICON_DIVE_STRIKE, "Dive strike", "Dive bombers only, then click the target: accurate, hit decks and magazines", "^B"},
        {SDLK_g, ICON_PATROL, "Fighter patrol", "Fighters circle the carrier against incoming strikes", NULL},
        {SDLK_r, ICON_RECALL, "Recall", "All aircraft return to land and rearm", NULL}};
    static const control depth[] = {
        {SDLK_j, ICON_DIVE, "Dive", "One level deeper: periscope depth (torpedoes only), then deep (silent, no attacks)", NULL},
        {SDLK_k, ICON_SURFACE, "Surface", "One level up: on the surface she is fast, charges and uses her deck gun", NULL}};
    static const control command[] = {
        {HUD_SPECIAL, ICON_SPECIAL, "Special ability", "Her class's special, on her current target", "U"},
        {HUD_RECON, ICON_RECON, "Recon plane", "Then click the sea: a floatplane or scout flies there", "V"}};
    static const char *hints[] = {
        "LMB select / drag box   Shift add", "RMB sea: move   RMB enemy: focus fire",
        "WASD/QE/wheel/MMB camera", "Hover a button for its key and what it does"};
    static const float ORDERS[3] = {0.18f, 0.3f, 0.42f}, DC[3] = {0.4f, 0.25f, 0.15f}, AIR[3] = {0.2f, 0.38f, 0.3f}, DEPTH[3] = {0.1f, 0.3f, 0.4f};
    float bw = 400;
    int mark = ui_mark(), rect = h->rect_count;
    box(h, x0, y, x0 + bw, y + 10, PANEL);          /* bottom edge set once laid out */
    ui_text(x0 + PAD, y + 5, "CONTROLS", 1, 0.85f, 0.5f, 1);
    int key = 0;
    if (button(h, x0 + bw - 70, y + 2, "Hide", 0.25f, 0.28f, 0.32f)) h->show_controls = 0;
    if (h->confirm_quit && h->click) h->confirm_quit = h->click_x > x0 && h->click_x < x0 + bw && h->click_y < y + 3 * lh;
    y += lh + 12;
    /* Game: pause and speed (not online), chat (online), quit. */
    float bx = x0 + PAD;
    if (!h->online) {
        if (button(h, bx, y, h->paused ? "Resume [Space]" : "Pause [Space]", 0.25f, 0.3f, 0.38f)) key = HUD_PAUSE;
        bx += ui_text_width(h->paused ? "Resume [Space]" : "Pause [Space]") + 2 * PAD + 6;
        if (button(h, bx, y, "-", 0.25f, 0.3f, 0.38f)) key = HUD_SLOWER;
        bx += ui_text_width("-") + 2 * PAD + 4;
        char sp[8];
        snprintf(sp, sizeof sp, "x%d", h->time_scale);
        bx += ui_text(bx, y + 3, sp, TEXT[0], TEXT[1], TEXT[2], 1) + 4;
        if (button(h, bx, y, "+", 0.25f, 0.3f, 0.38f)) key = HUD_FASTER;
        bx += ui_text_width("+") + 2 * PAD + 10;
    } else {
        if (button(h, bx, y, "Chat [Y]", 0.25f, 0.3f, 0.38f)) key = HUD_CHAT;
        bx += ui_text_width("Chat [Y]") + 2 * PAD + 10;
    }
    if (!h->confirm_quit) {
        if (button(h, bx, y, "Quit", 0.45f, 0.2f, 0.2f)) h->confirm_quit = 1;
    } else {
        bx += ui_text(bx, y + 3, "Quit?", 1, 0.6f, 0.5f, 1) + 6;
        if (button(h, bx, y, "Yes", 0.6f, 0.2f, 0.2f)) key = HUD_QUIT;
        bx += ui_text_width("Yes") + 2 * PAD + 4;
        if (button(h, bx, y, "No", 0.25f, 0.3f, 0.38f)) h->confirm_quit = 0;
    }
    y += lh + 12;
    /* Control groups: click one to select it; save the selection as a new one. */
    bx = x0 + PAD;
    bx += ui_text(bx, y + 3, "Groups ", DIM[0], DIM[1], DIM[2], 1);
    for (int g = 1; g <= 9; g++) {
        if (!(h->groups >> g & 1)) continue;
        char lab[4];
        snprintf(lab, sizeof lab, "%d", g);
        if (button(h, bx, y, lab, 0.2f, 0.32f, 0.3f)) key = HUD_GROUP + g;
        bx += ui_text_width(lab) + 2 * PAD + 4;
    }
    if (any && button(h, bx + 4, y, "+ Save selection", 0.2f, 0.32f, 0.3f)) key = HUD_GROUP_SAVE;
    else if (!any && !h->groups) ui_text(bx, y + 3, "(none)", DIM[0], DIM[1], DIM[2], 1);
    y += lh + 12;
    for (size_t i = 0; i < sizeof hints / sizeof *hints; i++, y += lh + 3) ui_text(x0 + PAD, y, hints[i], DIM[0], DIM[1], DIM[2], 1);
    y += 6;
    if (!any) {
        ui_text(x0 + PAD, y, "Select ships for their orders", TEXT[0], TEXT[1], TEXT[2], 1);
        ui_set_bottom(mark, y + lh + 10);
        h->rects[rect][3] = y + lh + 10;
        h->controls_bottom = y + lh + 10;
        return key;
    }
    const control *hover = NULL;
    float row = ICON_BTN + lh + 4;
    int k;
    /* Orders and damage control share a row when they fit; aircraft get their own. */
    /* The orders row shows the selection's state: hold fire or fire at will lit, the
     * aim button drawn as the part aimed at, evasive lit while she zig-zags. */
    int holding = 0, evading = 0, first = -1;
    for (int i = 0; i < w->count; i++)
        if (selected[i] && w->ships[i].alive) {
            if (first < 0) first = i;
            holding |= w->ships[i].hold_fire;
            evading |= w->ships[i].evade > 0;
        }
    orders[2].icon = aim_icon(w->ships[first].aim[BAT_MAIN]);
    const int lit[4] = {h->click_mode == CLICK_SPLIT ? HUD_SPLIT : -1, h->queue_on ? HUD_QUEUE : -1,
                        holding ? SDLK_h : SDLK_f, evading ? SDLK_z : -1};
    if ((k = icon_row_lit(h, x0 + PAD, y, orders, 8, ORDERS, &hover, lit))) key = k;
    y += row;
    if ((k = icon_row(h, x0 + PAD, y, dc, 3, DC, &hover))) key = k;
    if (carrier && (k = icon_row(h, x0 + PAD + 3 * (ICON_BTN + 6) + 12, y, air, 5, AIR, &hover))) key = k;
    else if (!carrier && subs && (k = icon_row(h, x0 + PAD + 3 * (ICON_BTN + 6) + 12, y, depth, 2, DEPTH, &hover))) key = k;
    y += row;
    if ((k = icon_row_lit(h, x0 + PAD, y, command, 2, ORDERS, &hover, (const int[4]){h->click_mode == CLICK_RECON ? HUD_RECON : -1, -1, -1, -1})))
        key = k;
    y += row;
    ui_set_bottom(mark, y + 4);
    h->rects[rect][3] = y + 4;
    h->controls_bottom = y + 4;
    if (hover) {                                    /* tooltip under the cursor */
        char title[64];
        if (hover->hint) snprintf(title, sizeof title, "%s  [%s]", hover->name, hover->hint);
        else snprintf(title, sizeof title, "%s  [%c]", hover->name, (char)(hover->key - 32));
        float tw = fmaxf(ui_text_width(title), ui_text_width(hover->tip)), tx = h->mouse_x + 16, ty = h->mouse_y + 20;
        ui_rect(tx - 6, ty - 4, tx + tw + 6, ty + 2 * lh + 6, 0.03f, 0.04f, 0.05f, 0.95f);
        ui_text(tx, ty, title, 1, 0.85f, 0.5f, 1);
        ui_text(tx, ty + lh + 2, hover->tip, TEXT[0], TEXT[1], TEXT[2], 1);
    }
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

static const char *MODULE_NAME[MOD_KINDS] = {"turret", "magazine", "engines", "steering", "bridge", "radar", "AA",
                                              "flight deck", "hangar lift", "drydock", "slipway"};

/* Hovering the cross-section: that compartment's state; clicking: teams there first. */
static void compartment_tip(hud *h, renderer *r, const world *w, command_queue *q, int i, int sw, int sh)
{
    const ship *s = &w->ships[i];
    const ship_class *c = s->cls;
    const ship_profile *p = renderer_profile(r, c);
    float pw = fminf(760, (float)sw - 40), x0 = ((float)sw - pw) / 2 + 20, y0 = (float)sh - 166, bw = pw - 165;
    if (!p || h->mouse_x < x0 || h->mouse_x > x0 + bw || h->mouse_y < y0 || h->mouse_y > y0 + 126) return;
    float z = p->z0 + (h->mouse_x - x0) / bw * (p->z1 - p->z0);
    int k = 0;
    while (k < c->comp_count - 1 && z > c->comp_z[k + 1]) k++;
    const compartment *cp = &s->comp[k];
    char lines[8][96];
    int nl = 0;
    snprintf(lines[nl++], 96, "Compartment %d of %d%s  (click: damage control here first)", k + 1, c->comp_count,
             cp->priority ? "  [PRIORITY]" : "");
    int gun = -1;                                   /* the gun mount nearest the cursor, for aiming */
    float gd = INFINITY;
    for (int m = 0; m < c->module_count; m++) {
        const module_def *d = &c->modules[m];
        if (d->comp != k || d->kind != MOD_TURRET || c->weapons[c->mounts[d->mount].weapon].ammo != AMMO_SHELL) continue;
        float dz = fabsf(c->mounts[d->mount].pos[2] - z);
        if (dz < gd) { gd = dz; gun = d->mount; }
    }
    if (gun >= 0 && s->owner == h->player)
        snprintf(lines[nl++], 96, "  right-click: aim %s at a ship of your choice",
                 c->mounts[gun].name[0] ? c->mounts[gun].name : "this mount");
    int statl = nl;                                 /* drawn as icons below; spaces hold its width */
    snprintf(lines[nl++], 96, "%46s", "");
    for (int m = 0; m < c->module_count && nl < 7; m++) {
        const module_def *d = &c->modules[m];
        if (d->comp != k) continue;
        if (d->kind == MOD_MAGAZINE)
            snprintf(lines[nl++], 96, "  magazine%s", cp->mag_flooded ? " (flooded)" : cp->fire > 0 ? " - FIRE NEARBY" : "");
        else if (d->kind == MOD_TURRET) {
            char own[48] = "";
            int o = s->turrets[d->mount].ordered;
            if (o >= 0 && o < w->count && w->ships[o].alive) {
                char who[40];
                ship_label(w, o, who, sizeof who);
                snprintf(own, sizeof own, "  on %s", who);
            }
            snprintf(lines[nl++], 96, "  %s %s %.0f%%%s  ammo %d%s", c->mounts[d->mount].name[0] ? c->mounts[d->mount].name : "mount",
                     c->weapons[c->mounts[d->mount].weapon].name, 100 * s->module_hp[m], s->module_lost[m] ? " LOST" : "",
                     s->turrets[d->mount].ammo, own);
        }
        else
            snprintf(lines[nl++], 96, "  %s %.0f%%%s", MODULE_NAME[d->kind], 100 * s->module_hp[m], s->module_lost[m] ? " LOST" : "");
    }
    float tw = 0;
    for (int l = 0; l < nl; l++) tw = fmaxf(tw, ui_text_width(lines[l]));
    float lh = ui_line_height(), tx = fminf(h->mouse_x + 14, (float)sw - tw - 14), ty = y0 - (float)nl * (lh + 2) - 60;
    ui_rect(tx - 6, ty - 4, tx + tw + 6, ty + (float)nl * (lh + 2) + 4, 0.03f, 0.04f, 0.05f, 0.92f);
    for (int l = 0; l < nl; l++) ui_text(tx, ty + (float)l * (lh + 2), lines[l], l ? 0.85f : 1, l ? 0.9f : 0.85f, l ? 0.95f : 0.5f, 1);
    {
        static const float FIRE_C[3] = {1, 0.5f, 0.15f}, WATER_C[3] = {0.4f, 0.65f, 1};
        char v[32];
        float sx = tx, sy = ty + (float)statl * (lh + 2);
        snprintf(v, sizeof v, "%.0f%%", 100 * cp->structure);
        sx += stat(h, sx, sy, ICON_CONDITION, v, severity(1 - cp->structure), NULL);
        snprintf(v, sizeof v, "P%.0f%% S%.0f%%%s", 100 * cp->flood[0], 100 * cp->flood[1], cp->leak[0] + cp->leak[1] > 0 ? " leak" : "");
        sx += stat(h, sx, sy, ICON_WATER, v, cp->flood[0] + cp->flood[1] > 0 ? WATER_C : DIM, NULL);
        snprintf(v, sizeof v, "%.0f%%", 100 * cp->fire);
        sx += stat(h, sx, sy, ICON_FIRE, v, cp->fire > 0 ? FIRE_C : DIM, NULL);
        snprintf(v, sizeof v, "%d", cp->crews);
        stat(h, sx, sy, ICON_CREW_PRIORITY, v, cp->crews ? TEXT : DIM, NULL);
    }
    if (h->click && s->owner == h->player) {
        command cmd;
        memset(&cmd, 0, sizeof cmd);
        cmd.kind = CMD_DC_PRIORITY;
        cmd.player = (uint8_t)h->player;
        cmd.a = k;
        cmd_set_ship(&cmd, i);
        hud_issue(q, w, &cmd);
        h->click = 0;
    }
    if (h->rclick && gun >= 0 && s->owner == h->player) {
        h->aim_ship = i;
        h->aim_mount = gun;
        h->rclick = 0;
    }
}

static void ship_readout(hud *h, renderer *r, const world *w, command_queue *q, const unsigned char *selected, int sw, int sh)
{
    int i = -1;
    for (int k = 0; k < w->count && i < 0; k++)
        if (selected[k] && w->ships[k].alive) i = k;
    if (i < 0) return;
    const ship *s = &w->ships[i];
    static const char *policy[] = {"auto", "fires", "flooding", "weapons"};
    char who[64], v[48], tip[160];
    ship_label(w, i, who, sizeof who);
    float lh = ui_line_height(), pw = fminf(760, (float)sw - 40), x0 = ((float)sw - pw) / 2, y0 = (float)sh - 170;
    float row1 = y0 - 2 * lh - 17, row2 = y0 - lh - 11;
    box(h, x0 - 10, y0 - 2 * lh - 22, x0 + pw + 10, y0 - 8, PANEL);
    /* Row 1: who she is, her fire orders, then her state as icons and numbers. */
    float x = x0;
    x += ui_text(x, row1, who, TEXT[0], TEXT[1], TEXT[2], 1) + 10;
    const float *fc;
    const char *fname;
    int fi = hud_fire_state(w, i, &fc, &fname);
    x += stat(h, x, row1, fi, NULL, fc, fname);
    if (s->evade > 0) x += stat(h, x, row1, ICON_EVASIVE, NULL, WARN_C, "Evasive manoeuvres [Z]");
    if (s->flagship) x += stat(h, x, row1, ICON_FLAGSHIP, NULL, WARN_C, "Fleet flagship");
    x += 6;
    float cond = s->hp / s->cls->max_health;
    snprintf(v, sizeof v, "%.0f%%", 100 * cond);
    x += stat(h, x, row1, ICON_CONDITION, v, severity(1 - cond), "Condition: hull structure left");
    if (!is_shore(s)) {
        float flood = s->water / fmaxf(s->cls->reserve_buoyancy, 1e-3f);
        snprintf(v, sizeof v, "%.0f%%", 100 * flood);
        snprintf(tip, sizeof tip, "Water aboard: %.0f%% of her reserve buoyancy (%.0f%% of %.0f%%); at 100%% she sinks",
                 100 * flood, 100 * s->water, 100 * s->cls->reserve_buoyancy);
        x += stat(h, x, row1, ICON_WATER, v, severity(flood * 1.4f), tip);
        float list = fabsf(s->list) * 180 / PI_F;
        snprintf(v, sizeof v, "%.0f deg", list);
        x += stat(h, x, row1, ICON_LIST, v, severity(list / 20), "List: past about 20 degrees she capsizes; counter-flood [X]");
    }
    float crew = s->crew / fmaxf(s->cls->complement, 1);
    snprintf(v, sizeof v, "%.0f/%.0f", s->crew, s->cls->complement);
    x += stat(h, x, row1, ICON_CREW, v, severity((1 - crew) * 1.5f), "Crew: fewer hands load, steer and fight damage slower");
    snprintf(tip, sizeof tip, "Damage-control priority: %s [P]", policy[s->policy]);
    x += stat(h, x, row1, ICON_CREW_PRIORITY, policy[s->policy], TEXT, tip);
    if (module_health(s, MOD_BRIDGE, -1) <= 0 && !s->cls->submerged)
        x += stat(h, x, row1, ICON_BRIDGE_LOST, NULL, BAD_C, is_shore(s) ? "HQ out" : "Bridge out: the helm answers slowly");
    if (s->cls->submerged) {
        static const char *dn[] = {"surfaced", "periscope", "deep"};
        snprintf(v, sizeof v, "%.0f%%", 100 * s->battery);
        snprintf(tip, sizeof tip, "Battery %.0f%%, %s: it charges on the surface", 100 * s->battery, dn[s->depth]);
        x += stat(h, x, row1, s->depth == DEPTH_SURFACE ? ICON_SURFACE : ICON_DIVE, NULL, TEXT, tip);
        x += stat(h, x, row1, ICON_BATTERY, v, severity(1 - s->battery), tip);
    }
    /* Row 2: the main battery against its focus target, else what it's on. */
    int target = s->order_target[BAT_MAIN];
    if (target < 0 || !w->ships[target].alive) {
        target = -1;
        for (int m = 0; m < s->cls->mount_count && target < 0; m++)
            if (s->cls->mounts[m].bat == BAT_MAIN && s->turrets[m].target >= 0) target = s->turrets[m].target;
    }
    int mains = 0, main_ammo = 0, main_cap = 0;
    for (int m = 0; m < s->cls->mount_count; m++)
        if (s->cls->mounts[m].bat == BAT_MAIN) { mains++; main_ammo += s->turrets[m].ammo; main_cap += ammo_capacity(s->cls, m); }
    const float GOLD[3] = {0.95f, 0.85f, 0.55f};
    x = x0;
    snprintf(tip, sizeof tip, "Aim: %s [T]", aim_name(s->aim[BAT_MAIN]));
    x += stat(h, x, row2, aim_icon(s->aim[BAT_MAIN]), aim_name(s->aim[BAT_MAIN]), GOLD, tip);
    if (mains) {
        snprintf(v, sizeof v, "%d", main_ammo);
        snprintf(tip, sizeof tip, "Main battery shells left: %d of %d", main_ammo, main_cap);
        x += stat(h, x, row2, ICON_AMMO, v, severity(main_cap ? 1 - (float)main_ammo / (float)main_cap : 0), tip);
    }
    if (target >= 0) {
        float p = 0;
        int n = 0;
        for (int m = 0; m < s->cls->mount_count; m++)
            if (s->cls->mounts[m].bat == BAT_MAIN && can_engage(w, i, m, target)) { p += hit_probability(w, i, m, target); n++; }
        char tname[64];
        ship_label(w, target, tname, sizeof tname);
        int focus = s->order_target[BAT_MAIN] == target;
        snprintf(tip, sizeof tip, "Main battery %s %s", focus ? "ordered onto" : "engaging", tname);
        x += stat(h, x, row2, focus ? ICON_FOCUS : ICON_FIRE_AT_WILL, tname, GOLD, tip);
        if (n) {
            snprintf(v, sizeof v, "%.0f%%", 100 * p / (float)n);
            snprintf(tip, sizeof tip, "Chance per shell, from range, both ships' speeds, crew and fire control");
            x += stat(h, x, row2, ICON_AIM, v, GOLD, tip);
            /* Guns on bearing: a pip per main mount, filled when it bears. */
            snprintf(tip, sizeof tip, "Main turrets bearing: %d of %d", n, mains);
            float px = x;
            for (int k = 0; k < mains; k++, px += 9)
                ui_rect(px, row2 + 4, px + 6, row2 + 12, k < n ? GOLD[0] : 0.25f, k < n ? GOLD[1] : 0.27f, k < n ? GOLD[2] : 0.3f, 1);
            if (h->mouse_x >= x && h->mouse_x <= px && h->mouse_y >= row2 && h->mouse_y <= row2 + 16) snprintf(h->tip, sizeof h->tip, "%s", tip);
            x = px + 12;
        } else {
            x += stat(h, x, row2, ICON_AIM, "no bearing", DIM, "Out of arc or range of her main battery");
        }
    } else {
        x += stat(h, x, row2, ICON_AIM, s->hold_fire ? "holding" : "no target", DIM,
                  s->hold_fire ? "Holding fire: F to fire at will" : "No enemy in range or arc");
    }
    special_kind sk = special_of(s->cls);
    if (sk != SP_NONE) {
        const float *sc = s->special_left > 0 ? OK_C : s->special_cool > 0 ? DIM : WARN_C;
        if (s->special_left > 0) snprintf(v, sizeof v, "%.0fs", s->special_left);
        else if (s->special_cool > 0) snprintf(v, sizeof v, "%.0fs", s->special_cool);
        else snprintf(v, sizeof v, "ready");
        snprintf(tip, sizeof tip, "%s: %s [U]", special_name(sk),
                 s->special_left > 0 ? "active" : s->special_cool > 0 ? "recharging" : "ready");
        x += stat(h, x, row2, ICON_SPECIAL, v, sc, tip);
    }
    /* Legend along the bottom of the damage panel. */
    float ly = y0 + 122;
    x = x0 + 20;
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
    box(h, x0 + 20, y0 + 4, x0 + pw - 145, y0 + 130, (const float[4]){0, 0, 0, 0});   /* the profile catches clicks */
    compartment_tip(h, r, w, q, i, sw, sh);
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
    if (!h->show_fleet) {
        if (button(h, (float)sw - 120, (float)sh - ui_line_height() - 16, "Fleet [F2]", 0.15f, 0.2f, 0.26f)) h->show_fleet = 1;
        return;
    }
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
    ui_text(x0 + 4, top + 4, "FLEET", 1, 0.85f, 0.5f, 1);
    if (button(h, x0 + cw - 66, top + 1, "Hide", 0.25f, 0.28f, 0.32f)) h->show_fleet = 0;
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
        snprintf(name, sizeof name, "%.10s", who);
        ui_text(x0 + 120, y + 3, name, s->flagship ? 1.0f : TEXT[0], s->flagship ? 0.85f : TEXT[1], s->flagship ? 0.4f : TEXT[2], 1);
        const float *fc;
        const char *fname;
        int fi = hud_fire_state(w, i, &fc, &fname);
        stat(h, x0 + cw - 4 - ICON_SMALL_PX, y + 2, fi, NULL, fc, fname);
        float cf = s->hp / s->cls->max_health;
        ui_rect(x0 + 120, y + lh + 6, x0 + cw - 6, y + lh + 10, 0.08f, 0.08f, 0.1f, 1);
        ui_rect(x0 + 120, y + lh + 6, x0 + 120 + (cw - 126) * cf, y + lh + 10,
                cf > 0.66f ? 0.3f : cf > 0.33f ? 0.95f : 0.95f, cf > 0.66f ? 0.9f : cf > 0.33f ? 0.8f : 0.25f, cf > 0.66f ? 0.35f : 0.2f, 1);
        /* Afflictions: fires, water, list; officers aboard. */
        int fires = 0;
        for (int k = 0; k < s->cls->comp_count; k++) fires += s->comp[k].fire > 0;
        char st[48], tip[96];
        float sx = x0 + 120, sy = y + lh + 11;
        static const float FIRE_C[3] = {1, 0.5f, 0.15f}, WATER_C[3] = {0.4f, 0.65f, 1}, LIST_C[3] = {0.9f, 0.9f, 0.6f};
        if (s->flagship) sx += stat(h, sx, sy, ICON_FLAGSHIP, NULL, WARN_C, "Fleet flagship") - 6;
        if (fires) {
            snprintf(st, sizeof st, "%d", fires);
            snprintf(tip, sizeof tip, "%d compartment%s on fire", fires, fires > 1 ? "s" : "");
            sx += stat(h, sx, sy, ICON_FIRE, st, FIRE_C, tip) - 6;
        }
        if (s->water > 0.02f) {
            snprintf(st, sizeof st, "%.0f%%", 100 * s->water / s->cls->reserve_buoyancy);
            sx += stat(h, sx, sy, ICON_WATER, st, WATER_C, "Water aboard, of her reserve buoyancy") - 6;
        }
        if (fabsf(s->list) > 0.03f) {
            snprintf(st, sizeof st, "%.0f", fabsf(s->list) * 180 / PI_F);
            sx += stat(h, sx, sy, ICON_LIST, st, LIST_C, "List, degrees") - 6;
        }
        for (int k = 0; k < OFFICER_KINDS; k++)
            if (s->officers[k]) { snprintf(st, sizeof st, "%c", OFFICER_LETTER[k]); sx += ui_text(sx, sy, st, 0.95f, 0.8f, 0.4f, 1); }
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

/* ---- minimap (bottom left) ------------------------------------------------------------ */

int hud_minimap_point(const hud *h, float sx, float sy, float *x, float *z)
{
    if (h->map_size <= 0 || sx < h->map_x0 || sy < h->map_y0 || sx > h->map_x0 + h->map_size || sy > h->map_y0 + h->map_size)
        return 0;
    *x = h->map_cx + (sx - h->map_x0 - h->map_size / 2) / h->map_scale;
    *z = h->map_cz - (sy - h->map_y0 - h->map_size / 2) / h->map_scale;   /* +Z up the map */
    return 1;
}

static void minimap(hud *h, const world *w, int sh, float top)
{
    /* Bottom left, shrunk to fit under the panels above it (no smaller than 120). */
    float size = fmaxf(120, fminf(220, (float)sh - 10 - top - 8)), x0 = 10, y0 = (float)sh - size - 10;
    /* Fit everything afloat (and the camera) with a margin, square. */
    float lo_x = h->cam_x, hi_x = h->cam_x, lo_z = h->cam_z, hi_z = h->cam_z;
    for (int i = 0; i < w->count; i++) {
        const ship *s = &w->ships[i];
        if (!s->alive) continue;
        lo_x = fminf(lo_x, s->x); hi_x = fmaxf(hi_x, s->x);
        lo_z = fminf(lo_z, s->z); hi_z = fmaxf(hi_z, s->z);
    }
    float span = fmaxf(fmaxf(hi_x - lo_x, hi_z - lo_z) * 1.15f, 4000);
    h->map_x0 = x0; h->map_y0 = y0; h->map_size = size;
    h->map_cx = (lo_x + hi_x) / 2; h->map_cz = (lo_z + hi_z) / 2;
    h->map_scale = size / span;
    box(h, x0, y0, x0 + size, y0 + size, (const float[4]){0.05f, 0.12f, 0.18f, 0.85f});
    #define MX(x) (x0 + size / 2 + ((x) - h->map_cx) * h->map_scale)
    #define MY(z) (y0 + size / 2 - ((z) - h->map_cz) * h->map_scale)
    int side = -1;
    for (int i = 0; i < w->count && side < 0; i++)
        if (w->ships[i].owner == h->player) side = w->ships[i].side;
    for (int k = 0; k < w->isle_count; k++) {             /* islands */
        const island *is = &w->isles[k];
        float r = is->radius * h->map_scale;
        for (float dy = -r; dy <= r; dy += 1) {         /* a disc, clipped to the map */
            float hw = sqrtf(fmaxf(0, r * r - dy * dy)), yy = MY(is->z) + dy;
            float xa = fmaxf(MX(is->x) - hw, x0), xb = fminf(MX(is->x) + hw, x0 + size);
            if (yy < y0 || yy + 1 > y0 + size || xb <= xa) continue;
            ui_rect(xa, yy, xb, yy + 1, 0.42f, 0.45f, 0.3f, 0.9f);
        }
    }
    for (int i = 0; i < w->count; i++) {
        const ship *s = &w->ships[i];
        if (!s->alive) continue;
        if (side >= 0 && !visible_to(w, side, i)) {           /* fog: where we last saw her */
            if (w->last_seen[side][i] >= 0) {
                float lx = MX(w->last_x[side][i]), lz = MY(w->last_z[side][i]);
                ui_rect(lx - 1.5f, lz - 0.4f, lx + 1.5f, lz + 0.4f, 0.8f, 0.4f, 0.35f, 0.7f);
                ui_rect(lx - 0.4f, lz - 1.5f, lx + 0.4f, lz + 1.5f, 0.8f, 0.4f, 0.35f, 0.7f);
            }
            continue;
        }
        float r = is_shore(s) ? 5 : s->cls->half_length > 80 ? 2.5f : 1.6f;
        float cr = s->side != side ? 0.95f : s->owner == h->player ? 0.35f : 0.4f;
        float cg = s->side != side ? 0.3f : s->owner == h->player ? 0.95f : 0.7f;
        float cb = s->side != side ? 0.25f : s->owner == h->player ? 0.4f : 1.0f;
        ui_rect(MX(s->x) - r, MY(s->z) - r, MX(s->x) + r, MY(s->z) + r, cr, cg, cb, 1);
        if (s->flagship) ui_rect(MX(s->x) - r - 1, MY(s->z) - r - 3, MX(s->x) + r + 1, MY(s->z) - r - 2, 1, 0.85f, 0.3f, 1);
    }
    for (int k = 0; k < w->air_count; k++) {
        const aircraft *a = &w->air[k];
        if (a->state != AIR_OUT && a->state != AIR_ATTACK && a->state != AIR_RETURN) continue;
        float cr = a->side != side ? 1.0f : 0.8f, cg = a->side != side ? 0.6f : 0.95f;
        ui_rect(MX(a->x) - 0.8f, MY(a->z) - 0.8f, MX(a->x) + 0.8f, MY(a->z) + 0.8f, cr, cg, 0.9f, 1);
    }
    /* The camera: a dot and a tick toward where it looks. */
    float cx = MX(h->cam_x), cy = MY(h->cam_z), fx = -sinf(h->cam_yaw), fz = -cosf(h->cam_yaw);
    ui_rect(cx - 2, cy - 2, cx + 2, cy + 2, 1, 1, 1, 1);
    for (int q = 1; q <= 6; q++) ui_rect(cx + fx * (float)q * 2 - 0.7f, cy - fz * (float)q * 2 - 0.7f,
                                         cx + fx * (float)q * 2 + 0.7f, cy - fz * (float)q * 2 + 0.7f, 1, 1, 1, 0.8f);
    #undef MX
    #undef MY
    if (h->click && h->click_x >= x0 && h->click_x <= x0 + size && h->click_y >= y0 && h->click_y <= y0 + size) {
        hud_minimap_point(h, h->click_x, h->click_y, &h->map_click_x, &h->map_click_z);
        h->map_clicked = 1;
        h->click = 0;
    }
}

/* Accuracy tooltip for the enemy under the cursor: what the selected ships' main
 * batteries would get per shell, before you commit to the target. */
static void hover_tooltip(hud *h, const world *w, const unsigned char *selected)
{
    if (h->hover < 0 || h->hover >= w->count || !w->ships[h->hover].alive) return;
    float p = 0;
    int n = 0, ships = 0;
    for (int i = 0; i < w->count; i++) {
        if (!selected[i] || !w->ships[i].alive || w->ships[i].owner != h->player) continue;
        int bearing = 0;
        for (int m = 0; m < w->ships[i].cls->mount_count; m++)
            if (w->ships[i].cls->mounts[m].bat == BAT_MAIN && can_engage(w, i, m, h->hover)) {
                p += hit_probability(w, i, m, h->hover);
                n++;
                bearing = 1;
            }
        ships += bearing;
    }
    char who[64], line[128];
    ship_label(w, h->hover, who, sizeof who);
    const ship *t = &w->ships[h->hover];
    if (n) snprintf(line, sizeof line, "%s  %.0f%%  |  %.0f%% per shell, %d turrets on %d ships", who,
                    100 * t->hp / t->cls->max_health, 100 * p / (float)n, n, ships);
    else snprintf(line, sizeof line, "%s  %.0f%%  |  out of arc or range of the selection", who, 100 * t->hp / t->cls->max_health);
    float tw = ui_text_width(line), x = h->mouse_x + 16, y = h->mouse_y + 12;
    ui_rect(x - 4, y - 2, x + tw + 4, y + ui_line_height() + 2, 0.05f, 0.05f, 0.07f, 0.85f);
    ui_text(x, y, line, 1, 0.85f, 0.6f, 1);
}

static void mode_banner(const world *w, int sw)
{
    char msg[128];
    switch (w->mode) {
    case MODE_ASSAULT: {
        int left = (int)mode_time_left(w);
        snprintf(msg, sizeof msg, "ASSAULT: destroy the enemy yard  %d:%02d  points %.0f-%.0f", left / 60, left % 60,
                 w->score[0], w->score[1]);
        break;
    }
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
    float left_bottom = shipyard_panel(h, w, q, h->controls_bottom + 8);
    h->right_top = 10;
    h->tut_action = 0;
    if (h->tut) {                                   /* the lesson's panel, above the Captain's bar */
        tut_action act;
        h->right_top = tutorial_frame(h->tut, h, w, selected, sw, t, &act);
        h->tut_action = act;
    }
    captain_bar(h, r, w, q, sw, t);
    fleet_bar(h, r, w, q, selected, sw, sh, t);
    minimap(h, w, sh, left_bottom);
    hover_tooltip(h, w, selected);
    ship_readout(h, r, w, q, selected, sw, sh);
    draw_tip(h, sw);
    int result = world_winner(w);
    if (result >= 0) {
        const char *msg = result == 2 ? "DRAW" : result == 0 ? "VICTORY" : "DEFEAT";
        float tw = ui_text_width(msg);
        ui_rect((float)sw / 2 - tw / 2 - 20, 60, (float)sw / 2 + tw / 2 + 20, 90, 0, 0, 0, 0.7f);
        ui_text((float)sw / 2 - tw / 2, 67, msg, 1, 0.85f, 0.4f, 1);
    }
    h->click = 0;              /* a click that hit nothing here belongs to the world */
    h->rclick = 0;
    ui_flush(r, sw, sh);
    return key;
}
