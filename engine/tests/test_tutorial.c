/* The tutorial's step machine, without a window: UI calls are stubbed, the world is
 * real. Walks lessons through their steps by doing what each one asks.
 *
 *   test_tutorial ASSETS_DIR     -> "N failures" */
#include <stdio.h>
#include <string.h>

#include "data.h"
#include "hud.h"
#include "render.h"
#include "tutorial.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

/* UI stubs: nothing drawn; "Next lesson" and the other buttons press when asked. */
static const char *press;
void ui_rect(float x0, float y0, float x1, float y1, float r, float g, float b, float a)
{ (void)x0; (void)y0; (void)x1; (void)y1; (void)r; (void)g; (void)b; (void)a; }
float ui_text(float x, float y, const char *s, float r, float g, float b, float a)
{ (void)x; (void)y; (void)r; (void)g; (void)b; (void)a; return 9.0f * (float)strlen(s); }
float ui_text_width(const char *s) { return 9.0f * (float)strlen(s); }
void ui_icon(int id, float x, float y, float r, float g, float b, float a)
{ (void)id; (void)x; (void)y; (void)r; (void)g; (void)b; (void)a; }
float ui_line_height(void) { return 16; }
void hud_box(hud *h, float x0, float y0, float x1, float y1, const float *c) { (void)h; (void)x0; (void)y0; (void)x1; (void)y1; (void)c; }
int hud_button(hud *h, float x, float y, const char *label, float r, float g, float b)
{
    (void)h; (void)x; (void)y; (void)r; (void)g; (void)b;
    return press && !strcmp(press, label);
}

static tutorial tut;
static hud hd;
static unsigned char sel[MAX_SHIPS];

static tut_action frame(const world *w, float t)
{
    tut_action act;
    tutorial_frame(&tut, &hd, w, sel, 1280, t, &act);
    return act;
}

static void apply(world *w, command c)
{
    c.player = 0;
    world_apply(w, &c);
}

static void observe(const world *w, const command *c, void *ud) { (void)w; tutorial_command(ud, c); }

int main(int argc, char **argv)
{
    const char *assets = argc > 1 ? argv[1] : "assets";
    char path[512];
    static class_table ct;
    snprintf(path, sizeof path, "%s/data", assets);
    if (classes_load(path, &ct)) { printf("FAIL no class data\n"); return 1; }
    snprintf(path, sizeof path, "%s/files", assets);
    classes_load_layouts(&ct, path);
    world_apply_observer = observe;
    world_apply_observer_data = &tut;
    static world w;
    float t = 0;

    /* Lesson 1: camera, select, move, queued move, stop, then a reading step. */
    world_init(&w);
    world_spawn(&w, class_find(&ct, "AmericanDestroyer"), 0, 0, 0, 0);
    world_spawn(&w, class_find(&ct, "JapaneseDestroyer"), 1, 0, 40000, 0);
    w.ships[1].owner = 1;
    tutorial_begin(&tut, 0, 0, 0, 0);
    hd.hover = -1;
    frame(&w, t += 1);
    CHECK(tut.step == 0, "waits for the camera (step %d)", tut.step);
    hd.cam_x = 800;
    frame(&w, t += 1);
    CHECK(tut.step == 1, "camera moved: step %d", tut.step);
    sel[0] = 1;
    frame(&w, t += 1);
    CHECK(tut.step == 2, "selected: step %d", tut.step);
    command c = {0};
    cmd_set_ship(&c, 0);
    c.kind = CMD_MOVE; c.x = 0; c.z = 3000;
    apply(&w, c);
    frame(&w, t += 1);
    CHECK(tut.step == 3, "moved: step %d", tut.step);
    apply(&w, c);                                   /* not queued: no progress */
    frame(&w, t += 1);
    CHECK(tut.step == 3, "a plain move doesn't count as queued (step %d)", tut.step);
    c.flag = 1;
    apply(&w, c);
    frame(&w, t += 1);
    CHECK(tut.step == 4, "queued: step %d", tut.step);
    c.kind = CMD_STOP; c.flag = 0;
    apply(&w, c);
    frame(&w, t += 1);
    CHECK(tut.step == 5, "stopped: step %d", tut.step);
    frame(&w, t += 5);
    CHECK(tut.step == 5, "a reading step waits for Next");
    press = "Next";
    frame(&w, t += 1);
    press = NULL;
    CHECK(tut.step == 6, "Next: lesson done (step %d)", tut.step);
    press = "Next lesson";
    CHECK(frame(&w, t += 1) == TUT_NEXT, "Next lesson asked for");
    press = NULL;

    /* Lesson 2's hover step needs a second of resting on an enemy; its last step needs her sunk. */
    tutorial_begin(&tut, 1, 0, 0, 0);
    hd.hover = 1;
    frame(&w, t += 0.6f);
    frame(&w, t += 0.6f);
    CHECK(tut.step == 0, "hover under a second: not yet");
    frame(&w, t += 0.6f);
    CHECK(tut.step == 1, "hovered: step %d", tut.step);
    hd.hover = -1;
    tut.step = 5;                                   /* jump to "sink her" */
    frame(&w, t += 1);
    CHECK(tut.step == 5, "enemy afloat: waits");
    w.ships[1].alive = 0;
    frame(&w, t += 1);
    CHECK(tut.step == 6, "enemy sunk: done (step %d)", tut.step);

    /* Skip step moves on without doing it. */
    tutorial_begin(&tut, 2, 0, 0, 0);
    press = "Skip step";
    frame(&w, t += 1);
    press = NULL;
    CHECK(tut.step == 1, "skipped: step %d", tut.step);

    /* The last lesson offers no next one; Quit quits. */
    tutorial_begin(&tut, TUT_LESSONS - 1, 0, 0, 0);
    tut.step = 99;
    press = "Next lesson";
    CHECK(frame(&w, t += 1) == TUT_NONE, "no lesson after the last");
    press = "Quit";
    CHECK(frame(&w, t += 1) == TUT_QUIT, "Quit");
    press = NULL;
    for (int k = 0; k < TUT_LESSONS; k++) CHECK(tutorial_title(k)[0], "lesson %d has a title", k + 1);

    printf("%d failures\n", failures);
    return failures != 0;
}
