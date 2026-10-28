/* The tutorial's lessons and step checks (design). Each step: what to do (up to three
 * lines), the icon of the control that does it and its key, and how we know it's done. */
#include "tutorial.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "hud.h"
#include "render.h"

typedef enum {
    C_READ,            /* informational: the Next button */
    C_CAMERA,          /* the camera moved */
    C_SELECT,          /* one of her own ships selected */
    C_SELECT_CARRIER, C_SELECT_SUB, C_SELECT_YARD,
    C_CMD,             /* a command of kind arg applied */
    C_MOVE_QUEUED,     /* a move added as a waypoint */
    C_HOLD_ON, C_HOLD_OFF,
    C_HOVER,           /* the cursor rested on an enemy (the accuracy tooltip) */
    C_ENEMIES_SUNK,    /* every enemy ship sunk */
    C_ANY_SUNK,        /* at least one enemy ship sunk */
    C_DEPTH,           /* a selected submarine at depth arg */
    C_NO_FIRES,        /* her ships' fires all out */
    C_GROUP,           /* a control group saved */
    C_AT_YARD,         /* one of her ships moored at her yard */
    C_BUILT            /* her yard launched a ship */
} cond;

typedef struct {
    const char *text;
    int icon;          /* -1 none */
    const char *keys;
    cond c;
    int arg;
} step;

static const step L1[] = {
    {"Welcome, Admiral. Move the camera: W A S D (or the arrow keys) pan, Q and E turn, the mouse wheel zooms, the middle button drags to orbit.", -1, "WASD", C_CAMERA, 0},
    {"Select a ship: left-click her, or drag a box around several. The fleet bar (bottom right) selects too: click a card, double-click to bring the camera to her.", -1, "LMB", C_SELECT, 0},
    {"Order a move: right-click the sea where she should go. The minimap (bottom left) takes right-clicks as well.", -1, "RMB", C_CMD, CMD_MOVE},
    {"Add a waypoint: hold Shift while you right-click, or turn on Queue in the controls panel and right-click.", ICON_QUEUE, "Sh+RMB", C_MOVE_QUEUED, 0},
    {"All stop: the Stop button (N) halts her and clears the queued moves.", ICON_STOP, "N", C_CMD, CMD_STOP},
    {"Every order has a button in the controls panel (top left); hover one for its key and what it does. Space pauses, - and + change the speed. That's the basics.", -1, NULL, C_READ, 0},
};
static const step L2[] = {
    {"An enemy destroyer is ahead. Rest the cursor on her: the tooltip gives your guns' chance per shell, from range, both ships' speeds, your crew and fire control.", -1, "hover", C_HOVER, 0},
    {"Focus fire: select your battleship and right-click the destroyer. (Ctrl+right-click, or Split fire, sends only the secondaries.)", -1, "RMB", C_CMD, CMD_ATTACK},
    {"Aim for a part of her: T cycles engines, magazines, steering, bridge and guns. Fewer hits, but they land there. The readout under the screen shows the aim point.", ICON_AIM, "T", C_CMD, CMD_AIM},
    {"Hold fire (H): the guns go quiet, e.g. to stay unseen.", ICON_HOLD_FIRE, "H", C_HOLD_ON, 0},
    {"Fire at will (F): the guns pick the best target in range again.", ICON_FIRE_AT_WILL, "F", C_HOLD_OFF, 0},
    {"Now sink her. The cross-section panel at the bottom shows your own ship's compartments, turrets and damage.", -1, NULL, C_ENEMIES_SUNK, 0},
};
static const step L3[] = {
    {"Your cruiser is hit: fires and flooding, and she lists. The Captain's Command bar (top right) reports what to do. Click a report's action button, or press Enter for the top one.", ICON_FIRE, "Enter", C_CMD, CMD_CAPTAIN_ACT},
    {"Send the damage-control teams somewhere first: click a compartment on the cross-section panel at the bottom.", ICON_CREW_PRIORITY, "LMB", C_CMD, CMD_DC_PRIORITY},
    {"Crew priority (P) cycles what the teams fight first: auto, fires, flooding or weapons.", ICON_CREW_PRIORITY, "P", C_CMD, CMD_DC_POLICY},
    {"Counter-flood (X) flooding the other side brings her upright. A ship that lists too far capsizes.", ICON_COUNTER_FLOOD, "X", C_CMD, CMD_COUNTER_FLOOD},
    {"Keep at it until the fires are out. Flood magazines (M) saves a ship from a fire reaching one, but those guns fall silent.", ICON_FLOOD_MAGAZINES, "M", C_NO_FIRES, 0},
};
static const step L4[] = {
    {"Torpedo boats are coming. Their torpedoes run straight; a ship turned end-on to them is a thin target. Evasive (Z) combs the tracks, or zig-zags against gunfire.", ICON_EVASIVE, "Z", C_CMD, CMD_EVADE},
    {"The captain also warns of torpedoes inbound and offers to evade. Evading spoils the enemy's aim most at long range, and costs some of your own. Sink both boats.", ICON_TORPEDO, NULL, C_ENEMIES_SUNK, 0},
};
static const step L5[] = {
    {"Select your carrier: her air group is the strike force.", -1, "LMB", C_SELECT_CARRIER, 0},
    {"Fighter patrol (G): fighters circle her and intercept enemy strikes.", ICON_PATROL, "G", C_CMD, CMD_CAP},
    {"Air strike: click B (or the dive / torpedo strike buttons), then click the enemy cruiser. Dive bombers hit decks and magazines; torpedo bombers flood below the waterline.", ICON_STRIKE, "B", C_CMD, CMD_STRIKE},
    {"The planes fly out, attack and come home to rearm. Recall (R) brings everything back. Sink the cruiser.", ICON_RECALL, "R", C_ENEMIES_SUNK, 0},
};
static const step L6[] = {
    {"Select your submarine. She is at periscope depth: slow, her battery draining, firing torpedoes only, found only by sonar or by aircraft overhead.", -1, "LMB", C_SELECT_SUB, 0},
    {"Surface (K): fast, her battery charges and her deck gun fires, but every gun can hit her. The window title shows her depth and battery.", ICON_SURFACE, "K", C_DEPTH, DEPTH_SURFACE},
    {"Dive (J) back to periscope depth to attack: her bow tubes aim ahead, so point her at the target.", ICON_DIVE, "J", C_DEPTH, DEPTH_SHALLOW},
    {"Torpedo the destroyer ahead: right-click her to attack.", ICON_TORPEDO, "RMB", C_ANY_SUNK, 0},
    {"An escort is coming with depth charges. Go deep (J): silent, no attacks, heard at half the sonar range, and depth charges hurt less. Surface again before the battery runs flat.", ICON_DIVE, "J", C_DEPTH, DEPTH_DEEP},
};
static const step L7[] = {
    {"Select one ship and make her the flagship (L): the fleet fights better around her, and is shaken if she is lost.", ICON_FLAGSHIP, "L", C_CMD, CMD_FLAGSHIP},
    {"Officers: in the fleet bar (bottom right) click an officer's letter, then a ship's card: each improves one thing aboard (gunnery, fire control, damage control...).", -1, "LMB", C_CMD, CMD_OFFICER},
    {"Save a control group: select ships and click + Save selection in the controls panel (or Ctrl+1..9). Click its number (or press it) to select them again.", -1, "Ctrl+1", C_GROUP, 0},
    {"Special ability (U): each class has one, e.g. focused fire or evasive action; it recharges after use.", ICON_SPECIAL, "U", C_CMD, CMD_SPECIAL},
    {"Recon (V): a cruiser or battleship flies a floatplane to the point you click. With fog of war, you see only what your ships, radar and planes see.", ICON_RECON, "V", C_CMD, CMD_RECON},
    {"Now beat the enemy squadron.", -1, NULL, C_ENEMIES_SUNK, 0},
};
static const step L8[] = {
    {"This is your shipyard. Select it: it builds ships, repairs them in its drydock, has coastal guns, AA and an airfield, and a boom keeps torpedoes out of its anchorage.", -1, "LMB", C_SELECT_YARD, 0},
    {"Build a ship: click one in the shipyard panel (left). The budget grows while the yard stands.", -1, "LMB", C_CMD, CMD_BUILD},
    {"Your cruiser is damaged. Bring her alongside the yard (inside the green ring) and stop: the drydock repairs her.", -1, "RMB", C_AT_YARD, 0},
    {"The yard's airfield flies strikes and fighter patrols like a carrier: select the yard and use the air buttons.", ICON_STRIKE, "B", C_CMD, CMD_STRIKE},
    {"Wait for the new ship to come off the slipway.", -1, NULL, C_BUILT, 0},
    {"A raid is coming. Defend the yard and sink it. In Assault and Attack / Defend battles, the yard is what you win or lose.", -1, NULL, C_ENEMIES_SUNK, 0},
};

static const struct { const char *title; const step *steps; int n; } LESSONS[TUT_LESSONS] = {
    {"Command basics", L1, sizeof L1 / sizeof *L1},
    {"Gunnery", L2, sizeof L2 / sizeof *L2},
    {"Damage control", L3, sizeof L3 / sizeof *L3},
    {"Torpedoes and evasion", L4, sizeof L4 / sizeof *L4},
    {"Carriers", L5, sizeof L5 / sizeof *L5},
    {"Submarines", L6, sizeof L6 / sizeof *L6},
    {"Fleet command", L7, sizeof L7 / sizeof *L7},
    {"Shipyards", L8, sizeof L8 / sizeof *L8},
};

const char *tutorial_title(int lesson)
{
    return lesson >= 0 && lesson < TUT_LESSONS ? LESSONS[lesson].title : "";
}

static void next_step(tutorial *t)
{
    t->step++;
    t->step_t = t->hover_t = 0;
    t->seen = 0;
    t->queued_move = t->hold_on = t->hold_off = 0;
}

void tutorial_begin(tutorial *t, int lesson, int player, float cam_x, float cam_z)
{
    memset(t, 0, sizeof *t);
    t->lesson = lesson;
    t->player = player;
    t->cam_x = cam_x;
    t->cam_z = cam_z;
    t->last_t = -1;
}

void tutorial_command(tutorial *t, const command *c)
{
    if (!t || t->lesson < 0 || c->player != t->player || c->kind >= 64) return;
    t->seen |= 1ull << c->kind;
    if (c->kind == CMD_MOVE && c->flag) t->queued_move = 1;
    if (c->kind == CMD_HOLD_FIRE) { if (c->flag) t->hold_on = 1; else t->hold_off = 1; }
}

static int done(tutorial *t, const step *s, hud *h, const world *w, const unsigned char *selected)
{
    int sel_kind = 0, enemies = 0, sunk = 0, fires = 0, moored = 0, built = 0, side = 0;
    for (int i = 0; i < w->count; i++)
        if (w->ships[i].owner == t->player) { side = w->ships[i].side; break; }
    for (int i = 0; i < w->count; i++) {
        const ship *sh = &w->ships[i];
        int mine = sh->owner == t->player;
        if (mine && sh->alive && selected[i]) {
            sel_kind |= 1;
            if (is_carrier(sh) && !is_shore(sh)) sel_kind |= 2;
            if (sh->cls->submerged) sel_kind |= 4;
            if (is_shore(sh)) sel_kind |= 8;
            if (s->c == C_DEPTH && sh->cls->submerged && sh->depth == s->arg) sel_kind |= 16;
        }
        if (sh->side != side && !is_shore(sh)) { enemies += sh->alive; sunk += !sh->alive; }
        if (mine && sh->alive)
            for (int k = 0; k < sh->cls->comp_count; k++) fires += sh->comp[k].fire > 0;
        if (mine && sh->alive && !is_shore(sh) && at_yard(w, i)) moored = 1;
        if (mine && is_shore(sh)) built = w->sides[sh->side & 1].built;
    }
    switch (s->c) {
    case C_READ: return 0;
    case C_CAMERA: return hypotf(h->cam_x - t->cam_x, h->cam_z - t->cam_z) > 300;
    case C_SELECT: return sel_kind & 1;
    case C_SELECT_CARRIER: return sel_kind & 2;
    case C_SELECT_SUB: return sel_kind & 4;
    case C_SELECT_YARD: return sel_kind & 8;
    case C_CMD: return (t->seen >> s->arg) & 1;
    case C_MOVE_QUEUED: return t->queued_move;
    case C_HOLD_ON: return t->hold_on;
    case C_HOLD_OFF: return t->hold_off;
    case C_HOVER: return t->hover_t > 1.0f;
    case C_ENEMIES_SUNK: return enemies == 0;
    case C_ANY_SUNK: return sunk > 0;
    case C_DEPTH: return (sel_kind & 16) != 0;
    case C_NO_FIRES: return fires == 0;
    case C_GROUP: return h->groups != 0;
    case C_AT_YARD: return moored;
    case C_BUILT: return built > 0;
    }
    return 0;
}

/* Word-wraps text to `width` (a '\n' also breaks) and draws it if asked; returns the
 * number of lines. */
static int lines(const char *text, float x, float y, float width, int draw)
{
    char line[256] = "", word[64];
    int n = 0;
    const char *p = text;
    while (*p) {
        int k = 0;
        while (*p && *p != ' ' && *p != '\n' && k < (int)sizeof word - 1) word[k++] = *p++;
        word[k] = 0;
        char trial[sizeof line + sizeof word + 2];
        snprintf(trial, sizeof trial, "%s%s%s", line, line[0] ? " " : "", word);
        if (line[0] && ui_text_width(trial) > width) {
            if (draw) ui_text(x, y + (float)n * (ui_line_height() + 3), line, 0.92f, 0.94f, 0.96f, 1);
            n++;
            snprintf(line, sizeof line, "%s", word);
        } else {
            memcpy(line, trial, sizeof line - 1);          /* trial fits: the width test kept it short */
            line[sizeof line - 1] = 0;
        }
        if (*p == '\n') {
            if (draw) ui_text(x, y + (float)n * (ui_line_height() + 3), line, 0.92f, 0.94f, 0.96f, 1);
            n++;
            line[0] = 0;
        }
        if (*p) p++;
    }
    if (line[0]) {
        if (draw) ui_text(x, y + (float)n * (ui_line_height() + 3), line, 0.92f, 0.94f, 0.96f, 1);
        n++;
    }
    return n;
}

float tutorial_frame(tutorial *t, hud *h, const world *w, const unsigned char *selected, int sw, float now, tut_action *act)
{
    *act = TUT_NONE;
    if (!t || t->lesson < 0 || t->lesson >= TUT_LESSONS) return 10;
    float dt = t->last_t < 0 ? 0 : fmaxf(0, now - t->last_t);
    t->last_t = now;
    const int nsteps = LESSONS[t->lesson].n;
    int finished = t->step >= nsteps;
    const step *s = finished ? NULL : &LESSONS[t->lesson].steps[t->step];
    if (s) {
        t->step_t += dt;
        t->hover_t = h->hover >= 0 ? t->hover_t + dt : 0;
        if (t->step_t > 0.5f && done(t, s, h, w, selected)) next_step(t);   /* a beat on each step */
        finished = t->step >= nsteps;
        s = finished ? NULL : &LESSONS[t->lesson].steps[t->step];
    }
    float lh = ui_line_height(), bw = 560, x0 = (float)sw - bw - 10, y0 = 10;
    const char *text = s ? s->text
                     : t->lesson + 1 < TUT_LESSONS ? "Lesson complete. On to the next one, or keep playing here."
                                                   : "That's the whole tutorial, Admiral. Skirmishes, custom battles and online games are on the main menu.";
    float textw = bw - 16 - (s && s->icon >= 0 ? 48 : 0);
    int nl = lines(text, 0, 0, textw, 0);
    float height = lh + 14 + (float)nl * (lh + 3) + lh + 20;
    const float panel[4] = {0.06f, 0.09f, 0.14f, 0.9f};
    hud_box(h, x0, y0, x0 + bw, y0 + height, panel);
    char head[96];
    snprintf(head, sizeof head, "TUTORIAL %d/%d: %s", t->lesson + 1, TUT_LESSONS, LESSONS[t->lesson].title);
    ui_text(x0 + 8, y0 + 5, head, 1, 0.85f, 0.5f, 1);
    char prog[32];
    snprintf(prog, sizeof prog, "step %d/%d", finished ? nsteps : t->step + 1, nsteps);
    ui_text(x0 + bw - 8 - ui_text_width(prog), y0 + 5, prog, 0.6f, 0.65f, 0.7f, 1);
    float tx = x0 + 8, ty = y0 + lh + 12;
    if (s && s->icon >= 0) {                        /* the control to use */
        ui_rect(tx, ty, tx + 40, ty + 40, 0.18f, 0.3f, 0.42f, 0.95f);
        ui_icon(s->icon, tx + 4, ty + 2, 1, 1, 1, 1);
        tx += 48;
    }
    lines(text, tx, ty, textw, 1);
    float by = y0 + height - lh - 12;
    if (s && s->keys) {
        char k[48];
        snprintf(k, sizeof k, "[%s]", s->keys);
        ui_text(x0 + 8, by + 3, k, 1, 0.85f, 0.5f, 1);
    }
    float bx = x0 + bw - 8;
    if (s) {
        const char *lab = s->c == C_READ ? "Next" : "Skip step";
        bx -= ui_text_width(lab) + 16;
        if (hud_button(h, bx, by, lab, 0.22f, 0.36f, 0.5f)) next_step(t);
    } else {
        if (t->lesson + 1 < TUT_LESSONS) {
            bx -= ui_text_width("Next lesson") + 16;
            if (hud_button(h, bx, by, "Next lesson", 0.2f, 0.45f, 0.25f)) *act = TUT_NEXT;
        }
        bx -= ui_text_width("Quit") + 16 + 8;
        if (hud_button(h, bx, by, "Quit", 0.45f, 0.2f, 0.2f)) *act = TUT_QUIT;
    }
    return y0 + height + 8;
}
