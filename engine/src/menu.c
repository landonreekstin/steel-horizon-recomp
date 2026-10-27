/* Front end: main menu, skirmish list, custom-battle fleet picker, host and join
 * screens, and the online lobby. Immediate-mode UI over a slowly orbiting ocean. */
#include "menu.h"

#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gl.h"

typedef struct {
    int mx, my, click;         /* mouse, and a left click this frame */
    char *field;               /* text field being typed into */
    int field_cap;
    float t;
} ui_in;

static const float BG[4] = {0.04f, 0.06f, 0.08f, 0.85f};

static int hit(const ui_in *in, float x0, float y0, float x1, float y1)
{
    return in->click && in->mx >= x0 && in->mx <= x1 && in->my >= y0 && in->my <= y1;
}

static int btn(ui_in *in, float x, float y, float w, const char *label, int enabled)
{
    float h = ui_line_height() + 12;
    int over = in->mx >= x && in->mx <= x + w && in->my >= y && in->my <= y + h;
    float k = !enabled ? 0.12f : over ? 0.32f : 0.2f;
    ui_rect(x, y, x + w, y + h, k, k + 0.06f, k + 0.12f, 0.95f);
    ui_text(x + (w - ui_text_width(label)) / 2, y + 6, label, enabled ? 1 : 0.5f, enabled ? 1 : 0.5f, enabled ? 1 : 0.5f, 1);
    return enabled && hit(in, x, y, x + w, y + h);
}

static void field(ui_in *in, float x, float y, float w, char *buf, int cap)
{
    float h = ui_line_height() + 10;
    int active = in->field == buf;
    ui_rect(x, y, x + w, y + h, 0.08f, 0.1f, 0.13f, 1);
    ui_rect(x, y + h - 2, x + w, y + h, active ? 0.9f : 0.4f, active ? 0.75f : 0.4f, active ? 0.3f : 0.45f, 1);
    char shown[80];
    snprintf(shown, sizeof shown, "%s%s", buf, active && fmodf(in->t, 1) < 0.5f ? "_" : "");
    ui_text(x + 6, y + 5, shown, 1, 1, 1, 1);
    if (hit(in, x, y, x + w, y + h)) { in->field = buf; in->field_cap = cap; }
}

static void title(float sw, const char *t, const char *sub)
{
    float w = fmaxf(ui_text_width(t), sub ? ui_text_width(sub) : 0);
    ui_rect(sw / 2 - w / 2 - 16, 50, sw / 2 + w / 2 + 16, sub ? 108 : 86, BG[0], BG[1], BG[2], BG[3]);
    ui_text(sw / 2 - ui_text_width(t) / 2, 60, t, 1, 0.85f, 0.5f, 1);
    if (sub) ui_text(sw / 2 - ui_text_width(sub) / 2, 84, sub, 0.75f, 0.8f, 0.85f, 1);
}

/* One frame: events, the ocean, then the caller draws UI. Returns 0 to quit. */
static int frame_begin(SDL_Window *win, renderer *r, ui_in *in, int *sw, int *sh)
{
    static world empty;
    static int init;
    if (!init) { world_init(&empty); init = 1; }
    in->click = 0;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) return 0;
        if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
            in->click = 1;
            in->field = NULL;                    /* a click elsewhere leaves the field */
        }
        if (e.type == SDL_TEXTINPUT && in->field) {
            size_t n = strlen(in->field), add = strlen(e.text.text);
            if (n + add < (size_t)in->field_cap) memcpy(in->field + n, e.text.text, add + 1);
        }
        if (e.type == SDL_KEYDOWN && in->field && e.key.keysym.sym == SDLK_BACKSPACE) {
            size_t n = strlen(in->field);
            if (n) in->field[n - 1] = 0;
        }
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE && !in->field) return 0;
    }
    SDL_GetMouseState(&in->mx, &in->my);
    SDL_GL_GetDrawableSize(win, sw, sh);
    in->t += 1.0f / 60;
    camera cam = {v3(0, 0, 0), in->t * 0.05f, 0.18f, 1600};
    renderer_draw(r, &cam, *sw, *sh, &empty, 1, NULL, NULL, in->t * 4);
    return 1;
}

/* SHRTS_MENU_SHOT=file.ppm [SHRTS_MENU_SCREEN=n]: save the 30th frame and quit (checks). */
static int shot_frames;

static int frame_end(SDL_Window *win, renderer *r, int sw, int sh)
{
    ui_flush(r, sw, sh);
    const char *shot = getenv("SHRTS_MENU_SHOT");
    if (shot && ++shot_frames == 30) {
        unsigned char *px = malloc((size_t)sw * sh * 3);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, sw, sh, GL_RGB, GL_UNSIGNED_BYTE, px);
        FILE *f = fopen(shot, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", sw, sh);
            for (int y = sh - 1; y >= 0; y--) fwrite(px + (size_t)y * sw * 3, 1, (size_t)sw * 3, f);
            fclose(f);
        }
        free(px);
        return 0;
    }
    SDL_GL_SwapWindow(win);
    return 1;
}

static const char *SCEN[][2] = {
    {"m01", "Mission 01: four US battleships against four German destroyers"},
    {"fleet", "Fleet action: US and Japanese task forces, 8 ships each"},
    {"archipelago", "Archipelago: the fleet action among islands"},
    {"carriers", "Carrier battle: two carrier groups 11,000 apart"},
    {"assault", "Shipyard assault: a yard each, first to destroy the other's"},
    {"attack", "Attack: destroy the Japanese shipyard within 30 minutes"},
    {"defend", "Defend: hold the US shipyard for 30 minutes"},
};
#define NSCEN ((int)(sizeof SCEN / sizeof *SCEN))

static const char *NATIONS[] = {"American", "British", "German", "Japanese", "Shadow"};
static const char *HULLS[CUSTOM_HULLS] = {"Battleship", "Battlecruiser", "Cruiser", "Destroyer",
                                          "DestroyerEscort", "AircraftCarrier", "TorpedoBoat", "Submarine"};
static const char *HULL_SHORT[CUSTOM_HULLS] = {"Battleships", "Battlecruisers", "Cruisers", "Destroyers",
                                               "Escorts", "Carriers", "Torpedo boats", "Submarines"};

static int nation_index(const char *n)
{
    for (int k = 0; k < 5; k++)
        if (!strcmp(NATIONS[k], n)) return k;
    return 0;
}

static int has_class(const class_table *ct, const char *nation, int h)
{
    char name[64];
    snprintf(name, sizeof name, "%s%s", nation, HULLS[h]);
    return class_find(ct, name) != NULL;
}

void menu_defaults(menu_config *c)
{
    memset(c, 0, sizeof *c);
    snprintf(c->scenario, sizeof c->scenario, "fleet");
    snprintf(c->name, sizeof c->name, "Admiral");
    snprintf(c->address, sizeof c->address, "127.0.0.1:47800");
    c->port = 47800;
    c->players = 2;
    for (int p = 0; p < MAX_PLAYERS; p++) c->teams[p] = p % 2;
    snprintf(c->custom.nation[0], sizeof c->custom.nation[0], "American");
    snprintf(c->custom.nation[1], sizeof c->custom.nation[1], "Japanese");
    const uint8_t start[CUSTOM_HULLS] = {1, 0, 2, 3, 0, 1, 0, 0};
    memcpy(c->custom.count[0], start, sizeof start);
    memcpy(c->custom.count[1], start, sizeof start);
}

menu_result menu_run(SDL_Window *win, renderer *r, const class_table *ct, menu_config *cfg)
{
    enum { MAIN, SKIRMISH, CUSTOM, HOST, JOIN } screen = MAIN;
    if (getenv("SHRTS_MENU_SCREEN")) screen = atoi(getenv("SHRTS_MENU_SCREEN"));
    ui_in in = {0};
    SDL_StartTextInput();
    for (;;) {
        int sw, sh;
        if (!frame_begin(win, r, &in, &sw, &sh)) return MENU_QUIT;
        float cx = (float)sw / 2, bw = 360;
        switch (screen) {
        case MAIN: {
            title((float)sw, "STEEL HORIZON  -  NAVAL COMMAND", "a real-time naval battle game on the 1940s seas");
            ui_rect(cx - bw / 2 - 20, 130, cx + bw / 2 + 20, 485, BG[0], BG[1], BG[2], BG[3]);
            ui_text(cx - bw / 2, 145, "Admiral's name", 0.7f, 0.75f, 0.8f, 1);
            field(&in, cx - bw / 2, 165, bw, cfg->name, sizeof cfg->name);
            if (btn(&in, cx - bw / 2, 215, bw, "Skirmish", 1)) screen = SKIRMISH;
            if (btn(&in, cx - bw / 2, 260, bw, "Custom battle", 1)) screen = CUSTOM;
            if (btn(&in, cx - bw / 2, 305, bw, "Host an online game", 1)) screen = HOST;
            if (btn(&in, cx - bw / 2, 350, bw, "Join an online game", 1)) screen = JOIN;
            if (btn(&in, cx - bw / 2, 395, bw, cfg->generated ? "Ship models: generated" : "Ship models: original", 1))
                cfg->generated = !cfg->generated;
            if (btn(&in, cx - bw / 2, 425, bw, "Quit", 1)) return MENU_QUIT;
            break;
        }
        case SKIRMISH: {
            title((float)sw, "SKIRMISH", "against the computer");
            float w2 = 640;
            ui_rect(cx - w2 / 2 - 20, 120, cx + w2 / 2 + 20, 140 + NSCEN * 46.0f + 70, BG[0], BG[1], BG[2], BG[3]);
            for (int k = 0; k < NSCEN; k++)
                if (btn(&in, cx - w2 / 2, 135 + (float)k * 46, w2, SCEN[k][1], 1)) {
                    snprintf(cfg->scenario, sizeof cfg->scenario, "%s", SCEN[k][0]);
                    return MENU_PLAY;
                }
            if (btn(&in, cx - 90, 140 + NSCEN * 46.0f + 10, 180, "Back", 1)) screen = MAIN;
            break;
        }
        case CUSTOM: {
            title((float)sw, "CUSTOM BATTLE", "choose each side's navy and ships");
            float colw = 380, x0 = cx - colw - 20;
            ui_rect(x0 - 20, 120, cx + colw + 40, 640, BG[0], BG[1], BG[2], BG[3]);
            for (int sd = 0; sd < 2; sd++) {
                float x = sd ? cx + 20 : x0, y = 135;
                ui_text(x, y + 6, sd ? "Enemy" : "Your fleet", 1, 0.85f, 0.5f, 1);
                char lab[48];
                snprintf(lab, sizeof lab, "%s (change)", cfg->custom.nation[sd]);
                if (btn(&in, x + 120, y, colw - 120, lab, 1)) {
                    int k = (nation_index(cfg->custom.nation[sd]) + 1) % 5;
                    snprintf(cfg->custom.nation[sd], sizeof cfg->custom.nation[sd], "%s", NATIONS[k]);
                }
                y += 50;
                for (int h = 0; h < CUSTOM_HULLS; h++, y += 42) {
                    int ok = has_class(ct, cfg->custom.nation[sd], h);
                    if (!ok) cfg->custom.count[sd][h] = 0;
                    ui_text(x, y + 6, HULL_SHORT[h], ok ? 0.9f : 0.4f, ok ? 0.92f : 0.4f, ok ? 0.95f : 0.4f, 1);
                    if (btn(&in, x + 200, y, 40, "-", ok && cfg->custom.count[sd][h] > 0)) cfg->custom.count[sd][h]--;
                    snprintf(lab, sizeof lab, "%d", cfg->custom.count[sd][h]);
                    ui_text(x + 262, y + 6, lab, 1, 1, 1, 1);
                    if (btn(&in, x + 290, y, 40, "+", ok && cfg->custom.count[sd][h] < 8)) cfg->custom.count[sd][h]++;
                }
            }
            int total[2] = {0, 0};
            for (int sd = 0; sd < 2; sd++)
                for (int h = 0; h < CUSTOM_HULLS; h++) total[sd] += cfg->custom.count[sd][h];
            char yl[48];
            snprintf(yl, sizeof yl, "Shipyards: %s", cfg->custom.yards ? "yes (assault)" : "no");
            if (btn(&in, x0, 545, 300, yl, 1)) cfg->custom.yards = !cfg->custom.yards;
            snprintf(yl, sizeof yl, "Islands: %s", cfg->custom.islands ? "yes" : "no");
            if (btn(&in, x0, 590, 300, yl, 1)) cfg->custom.islands = !cfg->custom.islands;
            if (btn(&in, cx + 20, 545, 180, "Start", total[0] && total[1] && total[0] + total[1] < 120)) {
                snprintf(cfg->scenario, sizeof cfg->scenario, "custom");
                return MENU_PLAY;
            }
            if (btn(&in, cx + 220, 545, 180, "Back", 1)) screen = MAIN;
            break;
        }
        case HOST: {
            title((float)sw, "HOST AN ONLINE GAME", "players join you by address and port (LAN, VPN or a forwarded port)");
            float w2 = 560, x = cx - w2 / 2, y = 130;
            ui_rect(x - 20, 120, x + w2 + 20, 600, BG[0], BG[1], BG[2], BG[3]);
            char lab[64];
            snprintf(lab, sizeof lab, "Players: %d", cfg->players);
            ui_text(x, y + 6, lab, 1, 1, 1, 1);
            if (btn(&in, x + 200, y, 40, "-", cfg->players > 2)) cfg->players--;
            if (btn(&in, x + 250, y, 40, "+", cfg->players < MAX_PLAYERS)) cfg->players++;
            y += 50;
            ui_text(x, y + 6, "Teams (click to switch):", 0.7f, 0.75f, 0.8f, 1);
            y += 34;
            for (int p = 0; p < cfg->players; p++) {
                snprintf(lab, sizeof lab, "P%d: team %d", p + 1, cfg->teams[p] + 1);
                if (btn(&in, x + (float)(p % 4) * 140, y + (float)(p / 4) * 44, 130, lab, 1)) cfg->teams[p] ^= 1;
            }
            y += 100;
            ui_text(x, y + 6, "Battle:", 0.7f, 0.75f, 0.8f, 1);
            int cur = -1;
            for (int k = 0; k < NSCEN; k++) if (!strcmp(SCEN[k][0], cfg->scenario)) cur = k;
            snprintf(lab, sizeof lab, "%s (change)", cur >= 0 ? SCEN[cur][0] : "custom fleets");
            if (btn(&in, x + 100, y, 300, lab, 1)) {
                int next = cur + 1;
                if (next >= NSCEN) snprintf(cfg->scenario, sizeof cfg->scenario, "custom");
                else snprintf(cfg->scenario, sizeof cfg->scenario, "%s", SCEN[next < 0 ? 0 : next][0]);
            }
            y += 50;
            ui_text(x, y + 6, "Port:", 0.7f, 0.75f, 0.8f, 1);
            static char port[8];
            if (!port[0]) snprintf(port, sizeof port, "%d", cfg->port);
            field(&in, x + 100, y, 120, port, sizeof port);
            y += 60;
            if (btn(&in, x, y, 260, "Host", 1)) {
                cfg->port = atoi(port);
                return MENU_HOST;
            }
            if (btn(&in, x + 280, y, 260, "Back", 1)) screen = MAIN;
            break;
        }
        case JOIN: {
            title((float)sw, "JOIN AN ONLINE GAME", "the host's address and port");
            float w2 = 520, x = cx - w2 / 2, y = 140;
            ui_rect(x - 20, 120, x + w2 + 20, 300, BG[0], BG[1], BG[2], BG[3]);
            field(&in, x, y, w2, cfg->address, sizeof cfg->address);
            y += 60;
            if (btn(&in, x, y, 250, "Join", cfg->address[0] != 0)) return MENU_JOIN;
            if (btn(&in, x + 270, y, 250, "Back", 1)) screen = MAIN;
            break;
        }
        }
        if (!frame_end(win, r, sw, sh)) return MENU_QUIT;
    }
}

int menu_lobby(SDL_Window *win, renderer *r, net *n, const menu_config *cfg, uint64_t seed, net_game *out)
{
    ui_in in = {0};
    for (;;) {
        int sw, sh;
        if (!frame_begin(win, r, &in, &sw, &sh)) return -1;
        if (net_poll(n)) { fprintf(stderr, "lost the connection\n"); return -1; }
        if (net_started(n, out) && net_player(n) >= 0) return 0;
        float cx = (float)sw / 2, w2 = 520, x = cx - w2 / 2, y = 140;
        title((float)sw, "LOBBY", net_is_host(n) ? "waiting for players to join" : "waiting for the host to start");
        ui_rect(x - 20, 120, x + w2 + 20, 520, BG[0], BG[1], BG[2], BG[3]);
        char lab[96];
        if (net_is_host(n)) {
            snprintf(lab, sizeof lab, "Hosting on port %d: %d of %d players", net_port(n), net_joined(n), cfg->players);
            ui_text(x, y, lab, 1, 1, 1, 1);
            y += 34;
            for (int p = 0; p < net_joined(n); p++, y += 26) {
                snprintf(lab, sizeof lab, "P%d  %-20s  team %d", p + 1, net_name(n, p), cfg->teams[p] + 1);
                ui_text(x, y, lab, 0.8f, 0.85f, 0.9f, 1);
            }
            if (btn(&in, x, 460, 250, "Start", net_lobby_full(n))) {
                if (!strcmp(cfg->scenario, "custom")) net_set_custom(n, &cfg->custom);
                net_start(n, cfg->scenario, seed, cfg->teams);
            }
        } else {
            snprintf(lab, sizeof lab, "Connected as player %d", net_player(n) + 1);
            ui_text(x, y, net_player(n) >= 0 ? lab : "Connecting...", 1, 1, 1, 1);
        }
        if (btn(&in, x + 270, 460, 250, "Leave", 1)) return -1;
        if (!frame_end(win, r, sw, sh)) return -1;
        usleep(4000);
    }
}
