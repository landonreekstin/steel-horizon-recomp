/* In-battle HUD: Captain's Command bar, controls toolbar, selected-ship readout. */
#ifndef HUD_H
#define HUD_H

#include "command.h"
#include "render.h"
#include "sim.h"

#define HUD_MAX_RECTS 64

/* Icons in engine/src/icon_atlas.h, in tools/bakeicons.py order. */
enum {
    ICON_HOLD_FIRE, ICON_FIRE_AT_WILL, ICON_AIM, ICON_EVASIVE, ICON_STOP, ICON_CREW_PRIORITY, ICON_COUNTER_FLOOD,
    ICON_FLOOD_MAGAZINES, ICON_STRIKE, ICON_PATROL, ICON_RECALL, ICON_FIRE, ICON_TORPEDO, ICON_WATER, ICON_WITHDRAW,
    ICON_BRIDGE_LOST, ICON_DIVE, ICON_SURFACE, ICON_BATTERY, ICON_SPLIT_FIRE, ICON_QUEUE, ICON_FLAGSHIP, ICON_SPECIAL,
    ICON_RECON, ICON_TORPEDO_STRIKE, ICON_DIVE_STRIKE
};

/* What a HUD button can return besides an SDL keycode: actions with no single key. */
enum {
    HUD_PAUSE = 0x7000, HUD_SLOWER, HUD_FASTER, HUD_QUEUE, HUD_SPLIT, HUD_RECON, HUD_SPECIAL,
    HUD_TORP_STRIKE, HUD_DIVE_STRIKE, HUD_GROUP_SAVE, HUD_CHAT, HUD_QUIT,
    HUD_GROUP = 0x7100           /* + n: select control group n (1..9) */
};
/* Click modes: the next left click on the world picks the point or ship for an order. */
typedef enum { CLICK_NONE, CLICK_RECON, CLICK_SPLIT, CLICK_TURRET, CLICK_STRIKE } click_mode;

typedef struct hud_s {
    int show_captain, show_controls, show_fleet;
    int player;                /* local player: whose reports and fleet are shown */
    int focus;                 /* set to a ship index when a fleet card is double-clicked */
    int pending_officer;       /* officer picked in the fleet bar, waiting for a ship; -1 */
    int last_card;
    float last_click_t;
    /* Minimap: last frame's mapping, a click on it (-> camera), the camera position. */
    float map_x0, map_y0, map_size, map_cx, map_cz, map_scale;
    int map_clicked;
    float map_click_x, map_click_z, cam_x, cam_z, cam_yaw;
    /* Enemy under the cursor, for the accuracy tooltip; -1 none. */
    int hover;
    float mouse_x, mouse_y;
    int click;                 /* a left click waiting to be handled this frame */
    float click_x, click_y;
    int rclick;                /* a right click on the HUD, waiting likewise */
    float rclick_x, rclick_y;
    int aim_ship, aim_mount;   /* set (aim_mount >= 0) when a turret was right-clicked on the panel */
    float controls_bottom;     /* where the controls toolbar ended, for the panel below it */
    /* Game state the buttons show, set by the caller each frame. */
    int paused, time_scale, online, queue_on, click_mode;
    unsigned groups;           /* bit n: control group n holds ships */
    int confirm_quit;
    struct tutorial_s *tut;    /* a running tutorial lesson, or NULL */
    int tut_action;            /* tut_action the tutorial panel asked for this frame */
    float right_top;           /* where the Captain's bar starts (below the tutorial panel) */
    int rect_count;            /* last frame's UI boxes, for hud_over */
    float rects[HUD_MAX_RECTS][4];
} hud;

/* Draws the HUD for side 0. Handles a pending click: a captain's report is carried out
 * (or dismissed) directly; a toolbar button returns its key (SDL keycode) for the
 * caller to act on as if pressed. Returns 0 otherwise. */
int hud_draw(hud *h, renderer *r, world *w, command_queue *q, int player, unsigned char *selected, int sw, int sh, float t);
/* A panel box / labelled button that counts as HUD for clicks (for the tutorial panel). */
void hud_box(hud *h, float x0, float y0, float x1, float y1, const float *c);
int hud_button(hud *h, float x, float y, const char *label, float r, float g, float b);
/* Queues a command for the next tick. */
void hud_issue(command_queue *q, const world *w, const command *c);
/* Screen point on the minimap -> world (x, z); 0 if not on it. */
int hud_minimap_point(const hud *h, float sx, float sy, float *x, float *z);
/* 1 if (x, y) is over a HUD box (so a click there isn't a world click). */
int hud_over(const hud *h, float x, float y);
/* Carries out the most urgent report (Enter key). */
void hud_act_top(hud *h, command_queue *q, const world *w);
/* "Battleship 3": the class without its nation, and the ship's number. */
void ship_label(const world *w, int i, char *out, int n);

#endif
