/* In-battle HUD: Captain's Command bar, controls toolbar, selected-ship readout. */
#ifndef HUD_H
#define HUD_H

#include "render.h"
#include "sim.h"

#define HUD_MAX_RECTS 64

typedef struct {
    int show_captain, show_controls;
    int click;                 /* a left click waiting to be handled this frame */
    float click_x, click_y;
    int rect_count;            /* last frame's UI boxes, for hud_over */
    float rects[HUD_MAX_RECTS][4];
} hud;

/* Draws the HUD for side 0. Handles a pending click: a captain's report is carried out
 * (or dismissed) directly; a toolbar button returns its key (SDL keycode) for the
 * caller to act on as if pressed. Returns 0 otherwise. */
int hud_draw(hud *h, renderer *r, world *w, const unsigned char *selected, int sw, int sh, float t);
/* 1 if (x, y) is over a HUD box (so a click there isn't a world click). */
int hud_over(const hud *h, float x, float y);
/* Carries out the most urgent report (Enter key). */
void hud_act_top(world *w);
/* "Battleship 3": the class without its nation, and the ship's number. */
void ship_label(const world *w, int i, char *out, int n);

#endif
