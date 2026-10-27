/* Engine tests that need no window: asset port + simulation.
 *
 *   test_engine ASSETS_DIR              run the checks
 *   test_engine ASSETS_DIR --hashes OUT write a hash per decoded .bog/.pingu for
 *                                       tests/test_engine_port.py to compare with
 *                                       the Python decoders */
#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asset.h"
#include "data.h"
#include "math3d.h"
#include "sim.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

/* Hash of what the Python tools produce: RGBA bytes; for models, per mesh the
 * material index, then positions/uvs rounded to 1e-4 and the triangle indices. */
static void hash_tree(const char *root, const char *dir, FILE *out)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", root, dir);
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char rel[1024];
        snprintf(rel, sizeof rel, "%s%s%s", dir, dir[0] ? "/" : "", e->d_name);
        snprintf(path, sizeof path, "%s/%s", root, rel);
        size_t n = strlen(e->d_name);
        uint64_t h = 1469598103934665603ull;
        if (n > 4 && !strcmp(e->d_name + n - 4, ".bog")) {
            image img;
            if (bog_load(path, &img)) { fprintf(out, "%s ERROR\n", rel); continue; }
            h = fnv(h, img.rgba, (size_t)img.width * img.height * 4);
            fprintf(out, "%s %d %d %016llx\n", rel, img.width, img.height, (unsigned long long)h);
            image_free(&img);
        } else if (n > 6 && !strcmp(e->d_name + n - 6, ".pingu")) {
            pingu p;
            if (pingu_load(path, &p)) { fprintf(out, "%s ERROR\n", rel); continue; }
            int verts = 0, tris = 0;
            for (int i = 0; i < p.mesh_count; i++) {
                pingu_mesh *m = &p.meshes[i];
                int32_t mat = m->material;
                h = fnv(h, &mat, 4);
                for (int k = 0; k < m->vertex_count; k++) {
                    int32_t q[5];
                    for (int c = 0; c < 3; c++) q[c] = (int32_t)lroundf(m->vertices[k].pos[c] * 10000);
                    for (int c = 0; c < 2; c++) q[3 + c] = (int32_t)lroundf(m->vertices[k].uv[c] * 10000);
                    h = fnv(h, q, sizeof q);
                }
                h = fnv(h, m->indices, sizeof(uint32_t) * (size_t)m->index_count);
                verts += m->vertex_count;
                tris += m->index_count / 3;
            }
            fprintf(out, "%s %d %d %016llx\n", rel, verts, tris, (unsigned long long)h);
            pingu_free(&p);
        } else {
            hash_tree(root, rel, out);
        }
    }
    closedir(d);
}

static float dist(float ax, float az, float bx, float bz) { return hypotf(ax - bx, az - bz); }

static void test_assets(const char *assets, const class_table *ct)
{
    char path[1024];
    pingu p;
    snprintf(path, sizeof path, "%s/files/pingus/ships/allied/PSP_AL_BB01.pingu", assets);
    CHECK(!pingu_load(path, &p), "load BB01");
    CHECK(p.mesh_count == 4, "BB01 meshes %d", p.mesh_count);
    static const int verts[4] = {2581, 592, 56, 99};
    for (int i = 0; i < 4 && i < p.mesh_count; i++)
        CHECK(p.meshes[i].vertex_count == verts[i], "BB01 mesh %d verts %d", i, p.meshes[i].vertex_count);
    CHECK(!strcmp(p.materials[0], "PSP_AL_BB01_4"), "BB01 material 0 = %s", p.materials[0]);
    pingu_free(&p);

    mounts m;
    snprintf(path, sizeof path, "%s/files/pingus/ships/allied/PSP_AL_BB01.pts", assets);
    CHECK(!mounts_load(path, &m) && m.count == 7, "BB01 mounts %d", m.count);
    CHECK(!strncmp(m.points[0].name, "F1_", 3) && fabsf(m.points[0].pos[2] - 50.91f) < 0.01f &&
          fabsf(m.points[0].rot[0] - 1) < 1e-4f && fabsf(m.points[0].rot[4] - 1) < 1e-4f,
          "F1 mount upright at z=50.9");
    CHECK(fabsf(m.points[2].rot[8] + 1) < 1e-4f, "R1 mount faces aft");

    /* Every model the class table points at must load. */
    int loaded = 0;
    for (int i = 0; i < ct->count; i++)
        for (int s = 0; s < 3; s++) {
            if (!ct->classes[i].model[s][0]) continue;
            snprintf(path, sizeof path, "%s/files/%s", assets, ct->classes[i].model[s]);
            CHECK(!pingu_load(path, &p), "load %s", path);
            pingu_free(&p);
            loaded++;
        }
    CHECK(loaded >= 150, "only %d class models", loaded);
    const ship_class *bb = class_find(ct, "AmericanBattleship");
    CHECK(bb && bb->weapon_count >= 3 && !strcmp(bb->weapons[0].name, "16 Inch") &&
          bb->weapons[0].max_range == 3200 && bb->weapons[0].damage == 90, "BB main battery");
    const ship_class *pt = class_find(ct, "AmericanTorpedoBoat");
    int torp = 0;
    for (int i = 0; pt && i < pt->weapon_count; i++)
        if (!strcmp(pt->weapons[i].name, "18 Inch T")) torp = pt->weapons[i].damage == 195;
    CHECK(torp, "PT boat 18in torpedo resolves to the torpedo entry, not the 18in gun");
}

static void run(world *w, float seconds)
{
    for (int t = 0; t < (int)(seconds * SIM_HZ); t++) world_step(w);
}

static void test_sim(const class_table *ct)
{
    const ship_class *bb = class_find(ct, "AmericanBattleship"), *dd = class_find(ct, "GermanDestroyer");
    world w;

    /* A battleship ordered 3000 units ahead gets there and stops. */
    world_init(&w);
    world_spawn(&w, bb, 0, 0, 0, 0);
    int id = 0;
    order_move(&w, &id, 1, 0, 3000, 0);
    run(&w, 400);
    CHECK(dist(w.ships[0].x, w.ships[0].z, 0, 3000) < 150, "BB arrived (%.0f, %.0f)", w.ships[0].x, w.ships[0].z);
    CHECK(w.ships[0].speed < 0.5f && w.ships[0].wp_count == 0, "BB stopped (speed %.2f)", w.ships[0].speed);

    /* Top speed and acceleration follow the class stats. */
    world_init(&w);
    world_spawn(&w, bb, 0, 0, 0, 0);
    order_move(&w, &id, 1, 0, 100000, 0);
    run(&w, 10);
    float v10 = w.ships[0].speed;
    run(&w, 60);
    CHECK(fabsf(w.ships[0].speed - bb->speed * SPEED_SCALE) < 0.01f, "BB top speed %.2f", w.ships[0].speed);
    CHECK(v10 > 0.2f * w.ships[0].speed && v10 < 0.5f * w.ships[0].speed, "BB still accelerating at 10 s (%.2f)", v10);

    /* A target astern needs a wide turn but is still reached. */
    world_init(&w);
    world_spawn(&w, dd, 0, 0, 0, 0);
    order_move(&w, &id, 1, 0, -2000, 0);
    run(&w, 400);
    CHECK(dist(w.ships[0].x, w.ships[0].z, 0, -2000) < 150, "DD turned and arrived (%.0f, %.0f)", w.ships[0].x, w.ships[0].z);

    /* Four ships keep their 2x2 box when moved as a group. */
    world_init(&w);
    float sx[4] = {0, 600, 0, 600}, sz[4] = {0, 0, 600, 600};
    int ids[4] = {0, 1, 2, 3};
    for (int i = 0; i < 4; i++) world_spawn(&w, bb, 0, sx[i], sz[i], 0);
    order_move(&w, ids, 4, 300, 5300, 0);
    run(&w, 600);
    for (int i = 0; i < 4; i++)
        for (int j = i + 1; j < 4; j++) {
            float before = dist(sx[i], sz[i], sx[j], sz[j]);
            float after = dist(w.ships[i].x, w.ships[i].z, w.ships[j].x, w.ships[j].z);
            CHECK(fabsf(after - before) < 0.15f * before, "formation %d-%d: %.0f -> %.0f", i, j, before, after);
        }

    /* Turning the box 90 degrees must not send ships across each other's paths. */
    world_init(&w);
    for (int i = 0; i < 4; i++) world_spawn(&w, bb, 0, sx[i], sz[i], 0);  /* facing +Z */
    order_move(&w, ids, 4, 5000, 300, 0);                                   /* move east */
    int crossings = 0;
    for (int i = 0; i < 4; i++)
        for (int j = i + 1; j < 4; j++) {
            /* segment (ship -> its slot) intersection test */
            float ax = w.ships[i].x, az = w.ships[i].z, bx = w.ships[i].wp[0].x, bz = w.ships[i].wp[0].z;
            float cx = w.ships[j].x, cz = w.ships[j].z, dx = w.ships[j].wp[0].x, dz = w.ships[j].wp[0].z;
            float d1 = (bx - ax) * (cz - az) - (bz - az) * (cx - ax), d2 = (bx - ax) * (dz - az) - (bz - az) * (dx - ax);
            float d3 = (dx - cx) * (az - cz) - (dz - cz) * (ax - cx), d4 = (dx - cx) * (bz - cz) - (dz - cz) * (bx - cx);
            crossings += d1 * d2 < 0 && d3 * d4 < 0;
        }
    CHECK(crossings == 0, "%d formation paths cross", crossings);

    /* Same check on the Mission 01 layout and order the screenshot mode uses. */
    world_init(&w);
    float mx[4] = {5268, 4668, 5867, 5267}, mz[4] = {6191, 5614, 5558, 5560};
    for (int i = 0; i < 4; i++) world_spawn(&w, bb, 0, mx[i], mz[i], 2.32f);
    order_move(&w, ids, 4, 4450, 3600, 0);
    crossings = 0;
    for (int i = 0; i < 4; i++)
        for (int j = i + 1; j < 4; j++) {
            float ax = w.ships[i].x, az = w.ships[i].z, bx = w.ships[i].wp[0].x, bz = w.ships[i].wp[0].z;
            float cx = w.ships[j].x, cz = w.ships[j].z, dx = w.ships[j].wp[0].x, dz = w.ships[j].wp[0].z;
            float d1 = (bx - ax) * (cz - az) - (bz - az) * (cx - ax), d2 = (bx - ax) * (dz - az) - (bz - az) * (dx - ax);
            float d3 = (dx - cx) * (az - cz) - (dz - cz) * (ax - cx), d4 = (dx - cx) * (bz - cz) - (dz - cz) * (bx - cx);
            crossings += d1 * d2 < 0 && d3 * d4 < 0;
        }
    CHECK(crossings == 0, "%d Mission 01 formation paths cross", crossings);

    /* Deterministic: the same orders give bit-identical state. */
    world a, b;
    for (int pass = 0; pass < 2; pass++) {
        world *x = pass ? &b : &a;
        world_init(x);
        for (int i = 0; i < 4; i++) world_spawn(x, bb, 0, sx[i], sz[i], 0.3f);
        order_move(x, ids, 4, -2000, 3000, 0);
        run(x, 120);
        order_move(x, ids, 2, 4000, 0, 1);
        run(x, 120);
    }
    int same = 1;
    for (int i = 0; i < 4; i++)
        same &= a.ships[i].x == b.ships[i].x && a.ships[i].z == b.ships[i].z && a.ships[i].heading == b.ships[i].heading;
    CHECK(same, "simulation is deterministic");
}

static const turret_mount *find_mount(const ship_class *c, const char *name)
{
    for (int m = 0; m < c->mount_count; m++)
        if (!strcmp(c->mounts[m].name, name)) return &c->mounts[m];
    return NULL;
}

static int count_mounts(const ship_class *c, battery b)
{
    int n = 0;
    for (int m = 0; m < c->mount_count; m++) n += c->mounts[m].bat == b;
    return n;
}

static int mount_index(const ship_class *c, const char *name)
{
    const turret_mount *m = find_mount(c, name);
    return m ? (int)(m - c->mounts) : -1;
}

/* Shells a BB fires at an enemy BB 2100 off its starboard quarter (beyond its 5in
 * secondaries, so main battery only), held afloat and silent;
 * returns the hit fraction. speed > 0: the target steams across the line of fire. */
static float hit_rate(const class_table *ct, float speed, int *shots)
{
    const ship_class *bb = class_find(ct, "AmericanBattleship");
    world w;
    world_init(&w);
    world_spawn(&w, bb, 0, 0, 0, 0);
    world_spawn(&w, bb, 1, 1500, -1500, 0);
    w.ships[1].hold_fire = 1;
    if (speed > 0) {
        w.ships[1].speed = speed;
        w.ships[1].wp[0] = (waypoint){1500, 60000};
        w.ships[1].wp_count = 1;
    }
    for (int t = 0; t < 600 * SIM_HZ; t++) {
        world_step(&w);
        w.ships[1].hp = bb->max_health;
    }
    *shots = w.shots;
    return w.shots ? (float)w.hits / (float)w.shots : 0;
}

static void test_combat(const class_table *ct)
{
    const ship_class *bb = class_find(ct, "AmericanBattleship"), *dd = class_find(ct, "GermanDestroyer");
    const ship_class *pt = class_find(ct, "AmericanTorpedoBoat"), *ss = class_find(ct, "GermanSubmarine");
    CHECK(bb && dd && pt && ss, "combat classes present");
    if (!bb || !dd || !pt || !ss) return;

    /* Layout: four 16" turrets on the model's mounts, virtual side batteries and racks. */
    CHECK(fabsf(bb->half_length - 97) < 2 && fabsf(bb->half_beam - 17.5f) < 1.5f,
          "BB footprint %.1f x %.1f", bb->half_length, bb->half_beam);
    const char *turrets[] = {"F1", "F2", "R1", "R2"};
    for (int k = 0; k < 4; k++) {
        const turret_mount *m = find_mount(bb, turrets[k]);
        CHECK(m && !strcmp(bb->weapons[m->weapon].name, "16 Inch") && m->bat == BAT_MAIN,
              "BB %s carries a 16in main-battery gun", turrets[k]);
    }
    CHECK(fabsf(find_mount(bb, "R1")->arc_center - PI_F) < 1e-5f, "BB R1 trains around the stern");
    CHECK(count_mounts(bb, BAT_SECONDARY) == 2 && count_mounts(bb, BAT_ASW) == 1,
          "BB secondaries %d, depth-charge racks %d", count_mounts(bb, BAT_SECONDARY), count_mounts(bb, BAT_ASW));
    CHECK(count_mounts(pt, BAT_TORPEDO) == 2, "PT boat torpedo tubes %d", count_mounts(pt, BAT_TORPEDO));
    CHECK(count_mounts(ss, BAT_TORPEDO) == 1 && count_mounts(ss, BAT_MAIN) == 0, "submarine: bow tubes only");
    CHECK(hit_chance(6, 3, 1000) == 35 && hit_chance(3, 6, 1000) == 85 && hit_chance(3, 3, 3000) == 65,
          "hit chance DD->BB %.0f, BB->DD %.0f, BB->BB at 3000 %.0f",
          hit_chance(6, 3, 1000), hit_chance(3, 6, 1000), hit_chance(3, 3, 3000));

    /* Arcs: fore turrets can't bear dead astern, aft ones can't bear dead ahead. */
    world w;
    world_init(&w);
    world_spawn(&w, bb, 0, 0, 0, 0);
    world_spawn(&w, bb, 1, 0, -2000, 0);
    world_spawn(&w, bb, 1, 0, 2000, 0);
    CHECK(!can_engage(&w, 0, mount_index(bb, "F1"), 1) && can_engage(&w, 0, mount_index(bb, "R1"), 1),
          "F1 can't bear astern, R1 can");
    CHECK(can_engage(&w, 0, mount_index(bb, "F1"), 2) && !can_engage(&w, 0, mount_index(bb, "R2"), 2),
          "F1 bears ahead, R2 can't");

    /* Traverse: a target abeam takes F1 about 18 s to reach (5 deg/s); nothing fires first. */
    world_init(&w);
    world_spawn(&w, bb, 0, 0, 0, 0);
    world_spawn(&w, bb, 1, 2400, 0, 0);
    w.ships[1].hold_fire = 1;
    for (int b = 0; b < BATTERIES; b++) w.ships[0].order_target[b] = 1;
    int f1 = mount_index(bb, "F1");
    run(&w, 5);
    float yaw5 = w.ships[0].turrets[f1].yaw * 180 / PI_F;
    CHECK(fabsf(yaw5 - 25) < 1.5f, "F1 trained %.1f deg in 5 s", yaw5);
    CHECK(w.shots == 0, "no main-battery shell before on bearing (%d)", w.shots);
    run(&w, 15);
    CHECK(w.shots >= 4, "all four turrets fired once on bearing (%d shells)", w.shots);

    /* Range: 3500 away is beyond every gun aboard. */
    world_init(&w);
    world_spawn(&w, bb, 0, 0, 0, 0);
    world_spawn(&w, bb, 1, 3500, 0, 0);
    run(&w, 60);
    CHECK(w.shots == 0, "%d shells fired out of range", w.shots);

    /* Accuracy at 2100 against a broadside battleship, and the lead on a crossing one. */
    int n0, n1;
    float still = hit_rate(ct, 0, &n0), moving = hit_rate(ct, bb->speed * SPEED_SCALE, &n1);
    printf("hit rate at 2100: stationary %.0f%% of %d, crossing at full speed %.0f%% of %d\n",
           still * 100, n0, moving * 100, n1);
    CHECK(still > 0.30f && still < 0.85f, "stationary hit rate %.2f", still);
    CHECK(moving > 0.6f * still, "lead works: crossing hit rate %.2f vs %.2f", moving, still);

    /* Damage stages follow HP, a sunk ship stops shooting. */
    world_init(&w);
    world_spawn(&w, bb, 0, 0, 0, 0);
    world_spawn(&w, dd, 1, 1500, 0, PI_F / 2);
    w.ships[1].hold_fire = 1;
    int bad_stage = 0, saw[4] = {0};
    long sunk_at = -1;
    for (int t = 0; t < 900 * SIM_HZ && sunk_at < 0; t++) {
        world_step(&w);
        const ship *s = &w.ships[1];
        float f = s->hp / dd->max_health;
        int want = s->hp <= 0 ? 3 : f < 1.0f / 3 ? 3 : f < 2.0f / 3 ? 2 : 1;
        bad_stage += s->stage != want;
        saw[s->stage] = 1;
        if (!s->alive) sunk_at = w.tick;
    }
    CHECK(sunk_at > 0 && saw[1] && saw[2] && saw[3] && !bad_stage,
          "DD went 1->2->3 and sank (sunk at tick %ld, %d bad stages)", sunk_at, bad_stage);
    int shots = w.shots;
    world_spawn(&w, dd, 1, -1500, 0, 0);       /* the BB keeps fighting; the wreck stays quiet */
    w.ships[1].hold_fire = 0;
    run(&w, 30);
    CHECK(w.ships[1].sink > 25 && w.shots > shots, "wreck sinking %.0f s, BB engaged the next DD", w.ships[1].sink);

    /* Submarines: only depth charges reach them. */
    world_init(&w);
    world_spawn(&w, dd, 0, 0, 0, 0);
    world_spawn(&w, ss, 1, 700, 0, 0);
    w.ships[1].hold_fire = 1;
    run(&w, 120);
    CHECK(w.shots == 0, "%d shells fired at a submerged boat", w.shots);
    CHECK(w.dc_hits > 0 && w.ships[1].hp < ss->max_health, "depth charges hit the sub (%d, hp %.0f)", w.dc_hits, w.ships[1].hp);

    /* Torpedoes: a PT boat's spread finds a stationary broadside battleship. */
    world_init(&w);
    world_spawn(&w, pt, 0, 0, 0, 0);
    world_spawn(&w, bb, 1, 1200, 0, 0);
    w.ships[1].hold_fire = 1;
    run(&w, 60);
    CHECK(w.torps >= 3 && w.torp_hits >= 1, "torpedoes %d fired, %d hit", w.torps, w.torp_hits);

    /* Deterministic battles: same seed, same result. */
    float hp[2][8];
    for (int pass = 0; pass < 2; pass++) {
        world_init(&w);
        world_seed(&w, 7);
        for (int k = 0; k < 4; k++) {
            world_spawn(&w, bb, 0, (float)k * 400, 0, 0);
            world_spawn(&w, dd, 1, (float)k * 400, 3000, PI_F);
        }
        for (int i = 0; i < w.count; i++) w.ships[i].ai = 1;
        run(&w, 120);
        for (int i = 0; i < 8; i++) hp[pass][i] = w.ships[i].hp;
    }
    CHECK(!memcmp(hp[0], hp[1], sizeof hp[0]), "combat is deterministic");
}

static int count_state(const world *w, int carrier, plane_type type, air_state st)
{
    int n = 0;
    for (int k = 0; k < w->air_count; k++)
        n += w->air[k].carrier == carrier && w->air[k].type == type && w->air[k].state == st;
    return n;
}

static void test_air(const class_table *ct)
{
    const ship_class *cv = class_find(ct, "AmericanAircraftCarrier"), *bb = class_find(ct, "JapaneseBattleship");
    const ship_class *ca = class_find(ct, "JapaneseCruiser"), *jcv = class_find(ct, "JapaneseAircraftCarrier");
    CHECK(cv && bb && ca && jcv, "air classes present");
    if (!cv || !bb || !ca || !jcv) return;
    world w;

    /* Air group: the original's plane stats, only carriers get one. */
    world_init(&w);
    world_spawn(&w, bb, 1, 0, 0, 0);
    world_spawn(&w, cv, 0, 0, 0, 0);
    CHECK(w.air_count == AIR_GROUP_FIGHTERS + AIR_GROUP_BOMBERS && w.air[0].carrier == 1,
          "only the carrier has an air group (%d planes)", w.air_count);
    CHECK(w.air[0].max_hp == 600 && w.air[AIR_GROUP_FIGHTERS].max_hp == 800, "fighter 600 HP, bomber 800 HP");

    /* Strike on a silent battleship 5,000 away (hold fire also silences its AA):
     * bombs hit, every bomber comes home, lands and is rearmed. */
    world_init(&w);
    world_spawn(&w, cv, 0, 0, 0, 0);
    world_spawn(&w, bb, 1, 0, 5000, 0);
    w.ships[1].hold_fire = 1;
    order_strike(&w, 0, 1);
    CHECK(count_state(&w, 0, PLANE_BOMBER, AIR_HANGAR) == AIR_GROUP_BOMBERS &&
          hangar_count(&w, 0, PLANE_BOMBER) == 0, "strike queues every bomber");
    run(&w, 20);
    int up = count_state(&w, 0, PLANE_BOMBER, AIR_OUT) + count_state(&w, 0, PLANE_FIGHTER, AIR_OUT);
    CHECK(up == AIR_GROUP_BOMBERS + AIR_GROUP_FIGHTERS / 2, "bombers plus half the fighters launched (%d)", up);
    run(&w, 400);
    CHECK(w.bombs == AIR_GROUP_BOMBERS && w.bomb_hits >= 2 && w.ships[1].hp < bb->max_health,
          "%d bombs, %d hit, BB hp %.0f", w.bombs, w.bomb_hits, w.ships[1].hp);
    run(&w, REARM_S + 5);
    CHECK(hangar_count(&w, 0, PLANE_BOMBER) == AIR_GROUP_BOMBERS, "all bombers back and rearmed (%d)",
          hangar_count(&w, 0, PLANE_BOMBER));

    /* AA: the same strike into three cruisers costs bombers. */
    world_init(&w);
    world_spawn(&w, cv, 0, 0, 0, 0);
    world_spawn(&w, bb, 1, 0, 5000, 0);
    world_spawn(&w, ca, 1, -300, 4800, 0);
    world_spawn(&w, ca, 1, 300, 4800, 0);
    world_spawn(&w, ca, 1, 0, 5300, 0);
    for (int i = 1; i < 5; i++) w.ships[i].hold_fire = 0;
    order_strike(&w, 0, 1);
    run(&w, 300);
    CHECK(w.planes_lost_aa >= 1, "AA shot down %d planes", w.planes_lost_aa);

    /* Fighters on patrol over a carrier intercept an unescorted strike. */
    world_init(&w);
    world_spawn(&w, cv, 0, 0, 0, 0);
    world_spawn(&w, jcv, 1, 0, 7000, PI_F);
    w.ships[0].hold_fire = w.ships[1].hold_fire = 1;
    order_cap(&w, 1);
    run(&w, 30);
    for (int k = 0; k < w.air_count; k++)       /* strike with bombers only */
        if (w.air[k].carrier == 0 && w.air[k].type == PLANE_BOMBER) { w.air[k].mission = MISSION_STRIKE; w.air[k].target = 1; }
    run(&w, 300);
    CHECK(w.planes_lost_fighters >= 2, "fighters shot down %d bombers", w.planes_lost_fighters);

    /* Recall: everything airborne turns for home and lands. */
    world_init(&w);
    world_spawn(&w, cv, 0, 0, 0, 0);
    world_spawn(&w, bb, 1, 0, 8000, 0);
    w.ships[1].hold_fire = 1;
    order_strike(&w, 0, 1);
    run(&w, 40);
    order_recall(&w, 0);
    run(&w, 120);
    int airborne_left = 0;
    for (int k = 0; k < w.air_count; k++)
        airborne_left += w.air[k].state == AIR_OUT || w.air[k].state == AIR_ATTACK || w.air[k].state == AIR_RETURN;
    CHECK(airborne_left == 0 && w.bombs == 0, "recalled strike landed without bombing (%d still up)", airborne_left);

    /* Deterministic with aircraft. */
    float hp[2][8];
    for (int pass = 0; pass < 2; pass++) {
        world_init(&w);
        world_seed(&w, 11);
        world_spawn(&w, cv, 0, 0, 0, 0);
        world_spawn(&w, jcv, 1, 0, 8000, PI_F);
        world_spawn(&w, bb, 0, 400, 500, 0);
        world_spawn(&w, bb, 1, 400, 7500, PI_F);
        for (int i = 0; i < w.count; i++) w.ships[i].ai = 1;
        run(&w, 400);
        for (int i = 0; i < 4; i++) { hp[pass][i] = w.ships[i].hp; hp[pass][4 + i] = w.air[i * 3].hp; }
    }
    CHECK(!memcmp(hp[0], hp[1], sizeof hp[0]), "air combat is deterministic");
}

int main(int argc, char **argv)
{
    const char *assets = argc > 1 ? argv[1] : "assets";
    char path[1024];
    if (argc > 3 && !strcmp(argv[2], "--hashes")) {
        FILE *out = fopen(argv[3], "w");
        snprintf(path, sizeof path, "%s/files", assets);
        hash_tree(path, "", out);
        fclose(out);
        return 0;
    }
    static class_table ct;
    snprintf(path, sizeof path, "%s/data", assets);
    if (classes_load(path, &ct)) { printf("FAIL cannot load class data\n"); return 1; }
    snprintf(path, sizeof path, "%s/files", assets);
    CHECK(classes_load_layouts(&ct, path) == 0, "every class model loads for its layout");
    test_assets(assets, &ct);
    test_sim(&ct);
    test_combat(&ct);
    test_air(&ct);
    printf("%d failures\n", failures);
    return failures != 0;
}
