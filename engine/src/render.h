/* OpenGL 3.3 renderer: sky, Gerstner-wave ocean, ships from the original models. */
#ifndef RENDER_H
#define RENDER_H

#include "math3d.h"
#include "sim.h"

typedef struct {
    vec3 target;               /* point on the sea the camera orbits */
    float yaw, pitch, dist;
} camera;

typedef struct {
    int x0, y0, x1, y1, active;
} drag_box;

typedef struct renderer renderer;

/* A model's side profile (from its vertices): per slice along z, the top and bottom. */
#define PROFILE_BINS 64
typedef struct {
    int ok;
    float z0, z1, ymin, ymax, deck;
    float top[PROFILE_BINS], bot[PROFILE_BINS];
} ship_profile;

renderer *renderer_create(const char *files_dir);
/* 1: draw ships from genship.c's generated models instead of the game's (--models generated). */
void renderer_set_generated(renderer *r, int on);
/* Fog of war: draw only what this side can see (-1: everything). */
void renderer_set_view_side(renderer *r, int side);
void renderer_destroy(renderer *r);

/* Preload the models a class uses (all damage stages + gun). Returns 0 on success. */
int renderer_load_class(renderer *r, const ship_class *cls);

mat4 camera_viewproj(const camera *c, int w, int h, vec3 *eye);

/* alpha: interpolation between the previous and current sim tick (0..1).
 * selected: per-ship flags (MAX_SHIPS). t: seconds, animates the sea. */
void renderer_draw(renderer *r, const camera *cam, int w, int h, const world *wd,
                   float alpha, const unsigned char *selected, const drag_box *box, float t);

/* Screen-space UI, in pixels from the top left; batched until ui_flush. */
void ui_rect(float x0, float y0, float x1, float y1, float r, float g, float b, float a);
float ui_text(float x, float y, const char *str, float r, float g, float b, float a);  /* returns width */
float ui_text_width(const char *str);
int ui_mark(void);                          /* vertex index of the next ui_rect */
void ui_set_bottom(int mark, float y1);     /* resize that rect once its contents are laid out */
float ui_line_height(void);
void ui_flush(renderer *r, int w, int h);
const ship_profile *renderer_profile(renderer *r, const ship_class *c);
/* Draws ship s's profile into the box, showing her damage (detail: the full panel). */
void ui_ship_profile(renderer *r, const ship *s, float x, float y, float w, float h, int detail, float t);

/* Sea surface height at (x, z) and time t, matching the ocean shader's waves. */
float sea_height(float x, float z, float t);

#endif
