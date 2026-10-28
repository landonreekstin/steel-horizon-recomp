/* In-game tutorial: eight short lessons, each a scenario (main.c, "tutorial1".."8")
 * and a list of steps that complete when the player actually does the thing (an
 * order applied, a ship selected, a target sunk). The panel sits top right, above
 * the Captain's Command bar. */
#ifndef TUTORIAL_H
#define TUTORIAL_H

#include "command.h"
#include "sim.h"

#define TUT_LESSONS 8

typedef enum { TUT_NONE, TUT_NEXT, TUT_QUIT } tut_action;

typedef struct tutorial_s {
    int lesson, step;          /* lesson -1: no tutorial */
    float step_t, hover_t, last_t;
    unsigned long long seen;   /* command kinds applied during this step */
    int queued_move, hold_on, hold_off;
    float cam_x, cam_z;
    int player;
} tutorial;

const char *tutorial_title(int lesson);
void tutorial_begin(tutorial *t, int lesson, int player, float cam_x, float cam_z);
/* Feed it every applied command (world_apply_observer). */
void tutorial_command(tutorial *t, const command *c);

struct hud_s;
/* Checks and draws the current step (called from hud_draw); returns its panel's bottom
 * edge, and sets *act when the player asks for the next lesson or to quit. */
float tutorial_frame(tutorial *t, struct hud_s *h, const world *w, const unsigned char *selected, int sw, float now,
                     tut_action *act);

#endif
