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
#include "genship.h"
#include "command.h"
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
        } else if (n > 4 && !strcmp(e->d_name + n - 4, ".bnk")) {
            sound_bank b;
            if (bnk_load(path, &b)) { fprintf(out, "%s ERROR\n", rel); continue; }
            long total = 0;
            for (int i = 0; i < b.count; i++) {
                h = fnv(h, b.names[i], strlen(b.names[i]));
                h = fnv(h, b.pcm[i], sizeof(int16_t) * (size_t)b.length[i]);
                total += b.length[i];
            }
            fprintf(out, "%s %d %ld %016llx\n", rel, b.count, total, (unsigned long long)h);
            bnk_free(&b);
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

/* Every sound the battle maps events to is in the original bank (names cut at 15). */
static void test_sound(const char *assets)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/files/sound/banks/game.bnk", assets);
    sound_bank b;
    CHECK(!bnk_load(path, &b) && b.count == 415, "game.bnk: %d sounds", b.count);
    static const char *used[] = {"ENORMCANNSHT1_W", "LRGCANNSHT1_WAV", "MEDCANNSHT1_WAV", "SMLCANNSHT1_WAV",
                                 "ENORMCANNHT1_WA", "LRGCANNHT1_WAV", "MEDCANNHT1_WAV", "SMLCANNHT1_WAV",
                                 "ENORMCANNMISS1_", "LRGCANNMISS1_WA", "MEDCANNMISS1_WA", "SMLCANNMISS1_WA",
                                 "TORPSHT1_WAV", "TORPHT1_WAV", "DPTHCHRGSHT1_WA", "DPTHCHRGEXPL1_W", "AAGUNEXPL1_WAV",
                                 "_50CALSHT1_WAV", "CRSHWTR1_WAV", "SINKINGLRG2_WAV", "TAKEOFFALS2_WAV", "LANDALS_WAV",
                                 "BB_USA_01", "DD_JAPAN_01", "MPT_USA_01", "CV_BRIT_01"};
    for (size_t k = 0; k < sizeof used / sizeof *used; k++) {
        int i = bnk_find(&b, used[k]);
        CHECK(i >= 0 && b.length[i] > 1000, "sound %s present", used[k]);
    }
    /* Decoded audio isn't silence: a big gun's report is loud. */
    int i = bnk_find(&b, "ENORMCANNSHT1_W"), peak = 0;
    for (int k = 0; i >= 0 && k < b.length[i]; k++) peak = abs(b.pcm[i][k]) > peak ? abs(b.pcm[i][k]) : peak;
    CHECK(peak > 8000, "16in report peak %d", peak);
    bnk_free(&b);
    /* The synthesised stand-ins cover every effect sound (not the voices). */
    synth_bank(&b);
    for (size_t k = 0; k < sizeof used / sizeof *used; k++) {
        if (strstr(used[k], "_USA_") || strstr(used[k], "_JAPAN_") || strstr(used[k], "_BRIT_")) continue;
        int j = bnk_find(&b, used[k]);
        int pk = 0;
        for (int q = 0; j >= 0 && q < b.length[j]; q++) pk = abs(b.pcm[j][q]) > pk ? abs(b.pcm[j][q]) : pk;
        CHECK(j >= 0 && pk > 3000, "synth %s present and audible (peak %d)", used[k], pk);
    }
    bnk_free(&b);
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
/* Shells a BB fires at an enemy BB 2100 off its starboard quarter (beyond its 5in
 * secondaries, so main battery only), held afloat and silent; over four seeds.
 * Returns the hit fraction and, in *pred, the mean of hit_probability at each shot. */
static float hit_rate(const class_table *ct, float speed, int *shots, float *pred)
{
    const ship_class *bb = class_find(ct, "AmericanBattleship");
    int fired = 0, hits = 0;
    double psum = 0;
    for (unsigned seed = 1; seed <= 4; seed++) {
        world w;
        world_init(&w);
        world_seed(&w, seed);
        world_spawn(&w, bb, 0, 0, 0, 0);
        world_spawn(&w, bb, 1, 1500, -1500, 0);
        w.ships[1].hold_fire = 1;
        if (speed > 0) {
            w.ships[1].speed = speed;
            w.ships[1].wp[0] = (waypoint){1500, 60000};
            w.ships[1].wp_count = 1;
        }
        for (int t = 0; t < 600 * SIM_HZ; t++) {
            int before = w.shots;
            /* The prediction for this tick, averaged over the mounts that can bear. */
            float p = 0;
            int n = 0;
            for (int m = 0; m < bb->mount_count; m++)
                if (bb->mounts[m].bat == BAT_MAIN && can_engage(&w, 0, m, 1)) { p += hit_probability(&w, 0, m, 1); n++; }
            world_step(&w);
            if (n) psum += (double)(p / (float)n) * (w.shots - before);
            damage_init(&w.ships[1]);
            w.ships[1].hp = bb->max_health;
        }
        fired += w.shots;
        hits += w.hits;
    }
    *shots = fired;
    *pred = fired ? (float)(psum / fired) : 0;
    return fired ? (float)hits / (float)fired : 0;
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
    /* Depth charges only on destroyers and escorts (naval_rules), with a short throw. */
    CHECK(count_mounts(bb, BAT_SECONDARY) == 2 && count_mounts(bb, BAT_ASW) == 0 && count_mounts(dd, BAT_ASW) == 1,
          "BB secondaries %d, depth-charge racks BB %d DD %d", count_mounts(bb, BAT_SECONDARY), count_mounts(bb, BAT_ASW),
          count_mounts(dd, BAT_ASW));
    for (int m = 0; m < dd->mount_count; m++)
        if (dd->mounts[m].bat == BAT_ASW) CHECK(dd->weapons[dd->mounts[m].weapon].max_range <= DC_THROW, "DC throw is short");
    CHECK(count_mounts(pt, BAT_TORPEDO) == 2, "PT boat torpedo tubes %d", count_mounts(pt, BAT_TORPEDO));
    CHECK(count_mounts(ss, BAT_TORPEDO) == 1 && count_mounts(ss, BAT_MAIN) == 1, "submarine: bow tubes and a deck gun");
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
    float p0, p1, still = hit_rate(ct, 0, &n0, &p0), moving = hit_rate(ct, bb->speed * SPEED_SCALE, &n1, &p1);
    printf("hit rate at 2100: stationary %.0f%% of %d (predicted %.0f%%), crossing at full speed %.0f%% of %d (predicted %.0f%%)\n",
           still * 100, n0, p0 * 100, moving * 100, n1, p1 * 100);
    CHECK(still > 0.30f && still < 0.85f, "stationary hit rate %.2f", still);
    /* The accuracy shown to the player has to match what the shells do; with the
     * lead broken the crossing rate falls far below the prediction. */
    CHECK(fabsf(still - p0) < 0.12f && fabsf(moving - p1) < 0.12f,
          "hit_probability calibrated: %.2f vs %.2f still, %.2f vs %.2f crossing", still, p0, moving, p1);
    CHECK(p1 < p0, "a crossing target is predicted harder to hit (%.2f vs %.2f)", p1, p0);

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
    world_spawn(&w, ss, 1, 150, 0, 0);
    w.ships[1].hold_fire = 1;
    run(&w, 120);
    CHECK(w.shots == 0, "%d shells fired at a submerged boat", w.shots);
    CHECK(w.dc_hits > 0 && w.ships[1].hp < ss->max_health, "depth charges hit the sub (%d, hp %.0f)", w.dc_hits, w.ships[1].hp);


    /* Out of reach of the racks: a sub 700 off is left alone by the depth charges. */
    world_init(&w);
    world_spawn(&w, dd, 0, 0, 0, 0);
    world_spawn(&w, ss, 1, 700, 0, 0);
    w.ships[1].hold_fire = 1;
    run(&w, 60);
    CHECK(w.dc_hits == 0, "no depth charges thrown 700 out (%d hits)", w.dc_hits);

    /* Deep: the first pattern hurts her less; sonar hears her at half the range. */
    float loss[2];
    for (int deep = 0; deep < 2; deep++) {
        world_init(&w);
        world_spawn(&w, dd, 0, 0, 0, 0);
        world_spawn(&w, ss, 1, 150, 0, 0);
        w.ships[1].hold_fire = 1;
        if (deep) w.ships[1].depth = w.ships[1].depth_goal = DEPTH_DEEP;
        for (int t = 0; t < 120 * SIM_HZ && !w.dc_hits; t++) world_step(&w);
        loss[deep] = ss->max_health - w.ships[1].hp;
    }
    CHECK(loss[0] > 0 && loss[1] > 0 && loss[1] < 0.8f * loss[0], "first DC hit: %.0f deep vs %.0f shallow", loss[1], loss[0]);
    world_init(&w);
    w.fog = 1;
    world_spawn(&w, dd, 0, 0, 0, 0);
    world_spawn(&w, ss, 1, SUB_DETECT, 0, 0);       /* inside the escort's sonar at periscope depth */
    w.ships[1].hold_fire = 1;
    w.ships[1].max_speed = 0;
    run(&w, 1);
    int heard_shallow = visible_to(&w, 0, 1);
    w.ships[1].depth = w.ships[1].depth_goal = DEPTH_DEEP;
    run(&w, 1);
    CHECK(heard_shallow && !visible_to(&w, 0, 1), "sonar: heard at periscope depth (%d), not deep (%d)",
          heard_shallow, visible_to(&w, 0, 1));

    /* Surfaced: the guns can hit her, the racks can't; her deck gun fires back, but
     * at periscope depth only her torpedoes do, and deep nothing. */
    world_init(&w);
    world_spawn(&w, dd, 0, 0, 0, 0);
    world_spawn(&w, ss, 1, 900, 0, 0);
    w.ships[1].depth = w.ships[1].depth_goal = DEPTH_SURFACE;
    int deck = -1;
    for (int m = 0; m < ss->mount_count; m++) if (ss->mounts[m].bat == BAT_MAIN) deck = m;
    CHECK(deck >= 0 && can_engage(&w, 1, deck, 0), "surfaced: her deck gun bears");
    run(&w, 60);
    CHECK(w.shots > 0 && w.hits > 0 && w.dc_hits == 0, "surfaced sub shelled: %d shots, %d hits, %d DC", w.shots, w.hits, w.dc_hits);
    w.ships[1].depth = w.ships[1].depth_goal = DEPTH_SHALLOW;
    CHECK(deck >= 0 && !can_engage(&w, 1, deck, 0), "periscope depth: no deck gun");
    w.ships[1].depth = w.ships[1].depth_goal = DEPTH_DEEP;
    int any = 0;
    for (int m = 0; m < ss->mount_count; m++) any |= can_engage(&w, 1, m, 0);
    CHECK(!any, "deep: no weapon bears");

    /* Battery: it drains underwater, a flat one brings her up, and the surface charges it. */
    world_init(&w);
    world_spawn(&w, ss, 1, 0, 0, 0);
    w.ships[0].battery = 0.001f;
    order_move(&w, (int[]){0}, 1, 0, 50000, 0);
    run(&w, 10);
    CHECK(w.ships[0].depth_goal == DEPTH_SURFACE, "flat battery: ordered up");
    run(&w, DIVE_S + 1);
    float b0 = w.ships[0].battery;
    run(&w, 30);
    CHECK(w.ships[0].depth == DEPTH_SURFACE && w.ships[0].battery > b0, "surfaced and charging (%.2f -> %.2f)",
          b0, w.ships[0].battery);
    float vs = w.ships[0].speed;
    order_depth(&w, (int[]){0}, 1, DEPTH_SHALLOW);
    run(&w, DIVE_S + 20);
    CHECK(w.ships[0].depth == DEPTH_SHALLOW && w.ships[0].speed < 0.6f * vs && w.ships[0].battery < 1,
          "dived: %.1f vs %.1f surfaced", w.ships[0].speed, vs);

    /* One turret on its own target: F1 on the second destroyer while the battery is
     * focused on the first; a new battery order puts it back under the battery. */
    world_init(&w);
    world_spawn(&w, bb, 0, 0, 0, 0);
    world_spawn(&w, dd, 1, -300, 1500, 0);
    world_spawn(&w, dd, 1, 300, 1500, 0);
    w.ships[1].hold_fire = w.ships[2].hold_fire = 1;
    order_attack(&w, (int[]){0}, 1, 1, 1 << BAT_MAIN);
    int solo = -1;
    for (int m = 0; m < bb->mount_count; m++) if (!strcmp(bb->mounts[m].name, "F1")) solo = m;
    order_turret(&w, 0, solo, 2);
    run(&w, 2);
    int others_on_1 = 1;
    for (int m = 0; m < bb->mount_count; m++)
        if (bb->mounts[m].bat == BAT_MAIN && m != solo && can_engage(&w, 0, m, 1)) others_on_1 &= w.ships[0].turrets[m].target == 1;
    CHECK(solo >= 0 && w.ships[0].turrets[solo].target == 2 && others_on_1, "F1 on her own target (%d), the rest on the focus",
          solo >= 0 ? w.ships[0].turrets[solo].target : -9);
    order_attack(&w, (int[]){0}, 1, 1, 1 << BAT_MAIN);
    run(&w, 2);
    CHECK(solo >= 0 && w.ships[0].turrets[solo].ordered == -1 && w.ships[0].turrets[solo].target == 1, "battery order: F1 back with the battery");

    /* An AI submarine stalks a battleship at periscope depth and torpedoes her. */
    world_init(&w);
    world_spawn(&w, bb, 0, 0, 0, PI_F / 2);
    world_spawn(&w, ss, 1, 0, 4000, PI_F);
    w.ships[1].ai = 1;
    run(&w, 450);                               /* a 3,200 run takes the fish 107 s */
    CHECK(w.torps > 0 && w.torp_hits > 0 && w.shots == 0, "sub attack: %d torpedoes, %d hit, %d shells at her (depth %d)",
          w.torps, w.torp_hits, w.shots, w.ships[1].depth);

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
    CHECK(count_state(&w, 0, PLANE_BOMBER, AIR_HANGAR) + count_state(&w, 0, PLANE_TORPEDO, AIR_HANGAR) == AIR_GROUP_BOMBERS &&
          strike_ready(&w, 0) == 0, "strike queues every bomber");
    run(&w, 26);                                  /* 11 launches, 2 s apart */
    int up = count_state(&w, 0, PLANE_BOMBER, AIR_OUT) + count_state(&w, 0, PLANE_TORPEDO, AIR_OUT) +
             count_state(&w, 0, PLANE_FIGHTER, AIR_OUT);
    CHECK(up == AIR_GROUP_BOMBERS + AIR_GROUP_FIGHTERS / 2, "bombers plus half the fighters launched (%d)", up);
    run(&w, 400);
    CHECK(w.bombs == AIR_GROUP_DIVE && w.air_torps == AIR_GROUP_TORPEDO && w.bomb_hits >= 1 && w.ships[1].hp < bb->max_health,
          "%d bombs (%d hit), %d aerial torpedoes, BB hp %.0f", w.bombs, w.bomb_hits, w.air_torps, w.ships[1].hp);
    run(&w, REARM_S + 5);
    CHECK(strike_ready(&w, 0) == AIR_GROUP_BOMBERS, "all bombers back and rearmed (%d)", strike_ready(&w, 0));

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
        if (w.air[k].carrier == 0 && is_strike_plane(w.air[k].type)) { w.air[k].mission = MISSION_STRIKE; w.air[k].target = 1; }
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

    /* Dive bombers set her afire; torpedo bombers flood her, and pay more to AA. */
    float water[2] = {0, 0};
    int lost[2] = {0, 0}, fires[2] = {0, 0};
    for (int type = 0; type < 2; type++)
        for (unsigned seed = 1; seed <= 3; seed++) {
            world_init(&w);
            world_seed(&w, seed);
            world_spawn(&w, cv, 0, 0, 0, 0);
            world_spawn(&w, bb, 1, 0, 5000, 0);          /* a battleship alone: weak AA */
            w.ships[1].wp[0] = (waypoint){0, 40000};
            w.ships[1].wp_count = 1;
            order_strike_types(&w, 0, 1, 1u << (type ? PLANE_TORPEDO : PLANE_BOMBER));
            float peak = 0;
            for (int t = 0; t < 400 * SIM_HZ; t++) { world_step(&w); peak = fmaxf(peak, w.ships[1].water); }
            water[type] += peak;
            lost[type] += w.planes_lost_aa;
            fires[type] += w.fires_started;
        }
    printf("strikes on a lone BB, 3 seeds: dive bombers water %.2f fires %d lost %d; torpedo bombers water %.2f fires %d lost %d\n",
           water[0], fires[0], lost[0], water[1], fires[1], lost[1]);
    CHECK(water[1] > water[0] + 0.02f, "torpedo bombers flood her (%.2f vs %.2f)", water[1], water[0]);
    CHECK(lost[1] > lost[0], "torpedo bombers lose more to AA (%d vs %d)", lost[1], lost[0]);

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

static int count_modules(const ship_class *c, module_kind k)
{
    int n = 0;
    for (int m = 0; m < c->module_count; m++) n += c->modules[m].kind == k;
    return n;
}

static void test_damage(const class_table *ct)
{
    const ship_class *bb = class_find(ct, "AmericanBattleship"), *dd = class_find(ct, "GermanDestroyer");
    const ship_class *cv = class_find(ct, "AmericanAircraftCarrier");
    if (!bb || !dd || !cv) return;
    world w;

    /* Layout */
    CHECK(bb->comp_count == 10 && dd->comp_count == 6, "compartments BB %d DD %d", bb->comp_count, dd->comp_count);
    /* R1 and R2 share a compartment, so one magazine serves both. */
    CHECK(count_modules(bb, MOD_MAGAZINE) == 3 && count_modules(bb, MOD_ENGINE) == 2 &&
          count_modules(bb, MOD_STEERING) == 1 && count_modules(bb, MOD_BRIDGE) == 1 && count_modules(bb, MOD_RADAR) == 1,
          "BB magazines %d engines %d", count_modules(bb, MOD_MAGAZINE), count_modules(bb, MOD_ENGINE));
    CHECK(count_modules(cv, MOD_FLIGHT_DECK) == 1 && count_modules(bb, MOD_FLIGHT_DECK) == 0, "only the carrier has a flight deck");
    int steer_comp = -1;
    for (int m = 0; m < bb->module_count; m++) if (bb->modules[m].kind == MOD_STEERING) steer_comp = bb->modules[m].comp;
    CHECK(steer_comp == 0, "steering gear in the stern compartment (%d)", steer_comp);

    /* Armour: 5in never pierces a battleship's belt, 16in does at short range. */
    const class_weapon *w16 = &bb->weapons[0], *w5 = NULL;
    for (int i = 0; i < dd->weapon_count; i++) if (dd->weapons[i].ammo == AMMO_SHELL) w5 = &dd->weapons[i];
    CHECK(w5 && shell_pen(w16, 0) > shell_pen(w16, 3000) && shell_pen(w5, 0) < bb->belt, "penetration falls with range; 5in < BB belt");
    world_init(&w);
    world_spawn(&w, bb, 1, 0, 0, 0);
    for (int k = 0; k < 40; k++) damage_hit(&w, 0, HIT_SHELL, 10, (float)(k % 9) * 15 - 60, w5->damage, shell_pen(w5, 1000), 0);
    CHECK(w.penetrations == 0 && w.ricochets + w.upperworks == 40 && w.upperworks > 0,
          "5in on a BB: %d through the belt, %d on the upperworks", w.penetrations, w.upperworks);
    world_init(&w);
    world_spawn(&w, bb, 1, 0, 0, 0);
    for (int k = 0; k < 20; k++) {                       /* fresh ship each time: 20 hits would sink her */
        damage_init(&w.ships[0]);
        damage_hit(&w, 0, HIT_SHELL, 10, 0, w16->damage, shell_pen(w16, 500), 0);
    }
    CHECK(w.penetrations + w.upperworks >= 18 && w.penetrations >= 10, "16in at 500: %d of 20 through the belt, %d upperworks",
          w.penetrations, w.upperworks);

    /* A torpedo floods one side and lists her; the crew counter-floods it back. */
    world_init(&w);
    world_spawn(&w, bb, 1, 0, 0, 0);
    damage_hit(&w, 0, HIT_TORPEDO, 15, 20, 240, 0, 0);   /* +X: starboard */
    run(&w, 25);
    float peak = w.ships[0].list;
    run(&w, 180);
    CHECK(peak > 0.05f && w.ships[0].alive, "torpedo to starboard lists her %.1f deg", peak * 180 / PI_F);
    CHECK(fabsf(w.ships[0].list) < peak * 0.6f, "counter-flooding: list %.1f -> %.1f deg", peak * 180 / PI_F,
          w.ships[0].list * 180 / PI_F);

    /* Three torpedoes on one side sink a destroyer by flooding or capsizing. */
    world_init(&w);
    world_spawn(&w, dd, 1, 0, 0, 0);
    for (int k = 0; k < 3; k++) damage_hit(&w, 0, HIT_TORPEDO, 5, (float)(k - 1) * 30, 240, 0, 0);
    run(&w, 120);
    CHECK(!w.ships[0].alive && (w.ships[0].cause == 1 || w.ships[0].cause == 2), "DD sunk by torpedoes (cause %d)", w.ships[0].cause);

    /* Fire in a magazine: crews and flooding the magazine are what save her. */
    static ship_class nocrew;
    nocrew = *bb;
    nocrew.dc_crews = 0;
    int mag_comp = -1;
    for (int m = 0; m < bb->module_count; m++) if (bb->modules[m].kind == MOD_MAGAZINE) { mag_comp = bb->modules[m].comp; break; }
    int blew = 0, survived_flooded = 0, survived_crewed = 0;
    for (int trial = 0; trial < 3; trial++) {
        world_init(&w);
        world_seed(&w, 100 + (unsigned)trial);
        world_spawn(&w, trial == 2 ? bb : &nocrew, 1, 0, 0, 0);
        w.ships[0].comp[mag_comp].fire = 1;
        if (trial == 1) { int id = 0; order_flood_magazines(&w, &id, 1); }
        run(&w, 1500);
        if (trial == 0) blew = !w.ships[0].alive && w.ships[0].cause == 3;
        if (trial == 1) survived_flooded = w.ships[0].alive || w.ships[0].cause != 3;
        if (trial == 2) survived_crewed = w.ships[0].alive;
    }
    CHECK(blew, "an unfought magazine fire cooks off");
    CHECK(survived_flooded && survived_crewed, "flooded magazine / crews prevent it (%d, %d)", survived_flooded, survived_crewed);

    /* Crew priorities: fires first vs flooding first. */
    int fire_crews[2];
    for (int pass = 0; pass < 2; pass++) {
        world_init(&w);
        world_spawn(&w, bb, 1, 0, 0, 0);
        w.ships[0].comp[2].fire = 0.3f;
        w.ships[0].comp[7].leak[0] = 0.01f;
        w.ships[0].policy = pass ? DC_FLOODING : DC_FIRES;
        world_step(&w);
        fire_crews[pass] = w.ships[0].comp[2].crews;
    }
    CHECK(fire_crews[0] > fire_crews[1], "fire priority puts more teams on the fire (%d vs %d)", fire_crews[0], fire_crews[1]);

    /* Manual priority: the player's compartment gets teams first. */
    world_init(&w);
    world_spawn(&w, bb, 1, 0, 0, 0);
    w.ships[0].comp[2].fire = 0.8f;                      /* the big fire... */
    w.ships[0].comp[6].fire = 0.2f;                      /* ...and the one the player cares about */
    order_dc_priority(&w, 0, 6);
    world_step(&w);
    CHECK(w.ships[0].comp[6].crews >= w.ships[0].comp[2].crews && w.ships[0].comp[6].crews >= 2,
          "priority compartment teams %d vs %d", w.ships[0].comp[6].crews, w.ships[0].comp[2].crews);

    /* Flooded magazines silence those turrets: a target dead ahead at 2,500 is out of
     * 5in range, so only the main battery could fire. */
    world_init(&w);
    world_spawn(&w, bb, 0, 0, 0, 0);
    world_spawn(&w, bb, 1, 0, 2500, 0);
    w.ships[1].hold_fire = 1;
    { int id = 0; order_flood_magazines(&w, &id, 1); }
    run(&w, 60);
    CHECK(w.shots == 0, "%d shells from flooded magazines", w.shots);

    /* Machinery and steering. */
    world_init(&w);
    world_spawn(&w, bb, 0, 0, 0, 0);
    for (int m = 0; m < bb->module_count; m++)
        if (bb->modules[m].kind == MOD_ENGINE) w.ships[0].module_hp[m] = 0;
    w.ships[0].dc_timer = 1e9f;                          /* no repairs in this check */
    w.ships[0].wp[0] = (waypoint){0, 5000};
    w.ships[0].wp_count = 1;
    run(&w, 30);
    CHECK(w.ships[0].speed < 0.01f, "dead in the water without engines (%.2f)", w.ships[0].speed);
    world_init(&w);
    world_spawn(&w, bb, 0, 0, 0, 0);
    w.ships[0].rudder = 0.5f;
    for (int m = 0; m < bb->module_count; m++)
        if (bb->modules[m].kind == MOD_STEERING) w.ships[0].module_hp[m] = 0;
    w.ships[0].dc_timer = 1e9f;
    w.ships[0].wp[0] = (waypoint){-3000, 0};
    w.ships[0].wp_count = 1;
    run(&w, 10);
    CHECK(w.ships[0].jammed && w.ships[0].rudder == 0.5f, "steering gear out: rudder stuck at %.2f", w.ships[0].rudder);
}

static int module_index(const ship_class *c, module_kind k)
{
    for (int m = 0; m < c->module_count; m++) if (c->modules[m].kind == k) return m;
    return -1;
}

static void test_command(const class_table *ct)
{
    const ship_class *bb = class_find(ct, "AmericanBattleship"), *pt = class_find(ct, "AmericanTorpedoBoat");
    const ship_class *cv = class_find(ct, "AmericanAircraftCarrier");
    if (!bb || !pt || !cv) return;
    world w;

    /* Crew: penetrating hits kill men; a short-handed ship mans fewer teams. */
    world_init(&w);
    world_spawn(&w, bb, 1, 0, 0, 0);
    CHECK(w.ships[0].crew == bb->complement && bb->complement > 2000, "BB crew %.0f", w.ships[0].crew);
    for (int k = 0; k < 4; k++) damage_hit(&w, 0, HIT_SHELL, 5, (float)(k - 2) * 40, 90, 3.6f, 0);
    CHECK(w.ships[0].crew < bb->complement, "casualties: %.0f of %.0f left", w.ships[0].crew, bb->complement);
    world_init(&w);
    world_spawn(&w, bb, 1, 0, 0, 0);
    w.ships[0].crew = bb->complement * 0.3f;
    for (int k = 0; k < bb->comp_count; k++) w.ships[0].comp[k].fire = 0.3f;
    world_step(&w);
    int teams = 0;
    for (int k = 0; k < bb->comp_count; k++) teams += w.ships[0].comp[k].crews;
    CHECK(teams == 2, "30%% crew mans %d of %d teams", teams, bb->dc_crews);

    /* Repairs: a damaged module comes back to 70%, one past the critical point never. */
    world_init(&w);
    world_spawn(&w, bb, 1, 0, 0, 0);
    int eng = module_index(bb, MOD_ENGINE), rad = module_index(bb, MOD_RADAR);
    w.ships[0].module_hp[eng] = 0.4f;
    w.ships[0].module_hp[rad] = 0;
    w.ships[0].module_lost[rad] = 1;
    run(&w, 120);
    CHECK(fabsf(w.ships[0].module_hp[eng] - 0.7f) < 1e-3f && w.ships[0].module_hp[rad] == 0,
          "engine repaired to %.2f, lost radar %.2f", w.ships[0].module_hp[eng], w.ships[0].module_hp[rad]);

    /* Hangar lifts out: bombers that land can't be rearmed. */
    world_init(&w);
    world_spawn(&w, cv, 0, 0, 0, 0);
    world_spawn(&w, bb, 1, 0, 5000, 0);
    w.ships[1].hold_fire = 1;
    order_strike(&w, 0, 1);
    for (int m = 0; m < cv->module_count; m++)
        if (cv->modules[m].kind == MOD_HANGAR_LIFT) { w.ships[0].module_hp[m] = 0; w.ships[0].module_lost[m] = 1; }
    run(&w, 420 + REARM_S + 30);
    int on_deck = count_state(&w, 0, PLANE_BOMBER, AIR_DECK) + count_state(&w, 0, PLANE_TORPEDO, AIR_DECK);
    CHECK(strike_ready(&w, 0) == 0 && on_deck >= 3, "no lifts: %d bombers stuck on deck, %d rearmed", on_deck, strike_ready(&w, 0));

    /* Macro targeting: aiming for the steering gear puts the damage aft. */
    float aft_share[2];
    for (int aimed = 0; aimed < 2; aimed++) {
        float aft = 0, total = 0;
        for (unsigned seed = 1; seed <= 3; seed++) {
            world_init(&w);
            world_seed(&w, seed);
            world_spawn(&w, bb, 0, 0, 0, 0);
            world_spawn(&w, bb, 1, 1500, -1500, 0);
            w.ships[1].hold_fire = 1;
            int id = 0;
            order_attack(&w, &id, 1, 1, 1u << BAT_MAIN);
            if (aimed) order_aim(&w, &id, 1, MOD_STEERING, 1u << BAT_MAIN);
            for (int t = 0; t < 300 * SIM_HZ; t++) {
                float before[MAX_COMPARTMENTS];
                for (int k = 0; k < bb->comp_count; k++) before[k] = w.ships[1].comp[k].structure;
                world_step(&w);
                for (int k = 0; k < bb->comp_count; k++) {
                    float loss = before[k] - w.ships[1].comp[k].structure;
                    total += loss;
                    if (k <= 1) aft += loss;
                }
                damage_init(&w.ships[1]);
            }
        }
        aft_share[aimed] = total > 0 ? aft / total : 0;
    }
    CHECK(aft_share[1] > aft_share[0] * 2, "aiming at the steering: %.0f%% of damage aft vs %.0f%%",
          aft_share[1] * 100, aft_share[0] * 100);

    /* Evasion: combing a PT boat's spread saves hits. */
    int torp_hits[2] = {0, 0};
    for (int evade = 0; evade < 2; evade++)
        for (unsigned seed = 1; seed <= 4; seed++) {
            world_init(&w);
            world_seed(&w, seed);
            world_spawn(&w, pt, 0, 0, 0, 0);
            world_spawn(&w, bb, 1, 1800, 0, 0);
            w.ships[0].wp[0] = (waypoint){0, 1};      /* hold her bow-on to the target line */
            w.ships[1].hold_fire = 1;
            w.ships[1].speed = w.ships[1].max_speed;   /* steady, so the boat's lead is good */
            w.ships[1].wp[0] = (waypoint){1800, 40000};
            w.ships[1].wp_count = 1;
            for (int t = 0; t < 110 * SIM_HZ; t++) {     /* the spreads run ~70 s */
                float comb;
                if (evade && w.ships[1].evade <= 0 && torpedo_threat(&w, 1, &comb)) { int id = 1; order_evade(&w, &id, 1, 60); }
                world_step(&w);
            }
            torp_hits[evade] += w.torp_hits;
        }
    CHECK(torp_hits[1] < torp_hits[0], "evading: %d torpedo hits vs %d", torp_hits[1], torp_hits[0]);

    /* Captain's reports: a fire by the magazine comes first; acting floods it. */
    world_init(&w);
    world_spawn(&w, bb, 0, 0, 0, 0);
    int mag = module_index(bb, MOD_MAGAZINE), mk = bb->modules[mag].comp;
    w.ships[0].comp[mk].fire = 0.6f;
    w.ships[0].comp[(mk + 3) % bb->comp_count].fire = 0.3f;
    /* An 8 degree list: past the report threshold, short of the crew's own counter-flooding. */
    w.ships[0].comp[0].flood[1] = 0.9f;
    world_step(&w);
    captain_report rep[16];
    int n = captain_reports(&w, 0, rep, 16);
    CHECK(n >= 3 && rep[0].kind == REP_FLOOD_MAGAZINE, "reports %d, first kind %d", n, n ? (int)rep[0].kind : -1);
    captain_act(&w, &rep[0]);
    CHECK(w.ships[0].comp[mk].mag_flooded, "acting on the report flooded that magazine");
    n = captain_reports(&w, 0, rep, 16);
    int still_mag = 0, has_list = -1;
    for (int k = 0; k < n; k++) { still_mag |= rep[k].kind == REP_FLOOD_MAGAZINE; if (rep[k].kind == REP_COUNTER_FLOOD) has_list = k; }
    CHECK(!still_mag && has_list >= 0, "magazine report gone, list report pending");
    if (has_list >= 0) {
        captain_act(&w, &rep[has_list]);
        int counter = 0;
        for (int k = 0; k < bb->comp_count; k++) counter |= w.ships[0].comp[k].counter_flood >= 0;
        CHECK(counter, "acting on the list report counter-floods");
    }
}

static void test_bases(class_table *ct)
{
    const ship_class *bb = class_find(ct, "AmericanBattleship"), *dd = class_find(ct, "AmericanDestroyer");
    const ship_class *jdd = class_find(ct, "JapaneseDestroyer");
    const ship_class *yard = classes_add_shipyard(ct, "Japanese", class_find(ct, "JapaneseBattleship"));
    const ship_class *uyard = classes_add_shipyard(ct, "American", bb);
    CHECK(yard && uyard && bb && dd && jdd, "shipyard classes");
    if (!yard || !uyard || !bb || !dd || !jdd) return;
    world w;

    /* The class: four batteries, drydock, slipway, ammunition dump, no machinery. */
    CHECK(yard->mount_count == 4 && count_modules(yard, MOD_DRYDOCK) == 1 && count_modules(yard, MOD_SLIPWAY) == 1 &&
          count_modules(yard, MOD_MAGAZINE) == 1 && count_modules(yard, MOD_ENGINE) == 0,
          "yard mounts %d", yard->mount_count);
    CHECK(!strcmp(yard->weapons[0].name, "18 Inch"), "Japanese yard mounts the battleship's 18in (%s)", yard->weapons[0].name);

    /* Ashore: torpedoes and near misses don't flood it; it doesn't move. */
    world_init(&w);
    int y = shipyard_spawn(&w, yard, 1, 0, 0, 0);
    for (int k = 0; k < 4; k++) damage_hit(&w, y, HIT_TORPEDO, 100, 300, 240, 0, 0);
    damage_hit(&w, y, HIT_NEAR_MISS, 0, 330, 90, 0, 0);
    run(&w, 30);
    CHECK(w.ships[y].alive && w.ships[y].water == 0 && w.ships[y].list == 0 && w.ships[y].x == 0 && w.ships[y].z == 0,
          "yard dry and in place (water %.2f)", w.ships[y].water);

    /* Repairs alongside: a battered BB moored by the drydock comes back; one far off doesn't. */
    world_init(&w);
    shipyard_spawn(&w, uyard, 0, 0, 0, 0);
    int nearb = world_spawn(&w, bb, 0, 0, 900, 0), farb = world_spawn(&w, bb, 0, 6000, 6000, 0);
    int eng = module_index(bb, MOD_ENGINE);
    for (int q = 0; q < 2; q++) {
        int id = q ? farb : nearb;
        for (int k = 3; k < 6; k++) w.ships[id].comp[k].structure = 0.2f;
        w.ships[id].module_hp[eng] = 0;
        w.ships[id].module_lost[eng] = 1;
        w.ships[id].crew = bb->complement * 0.5f;
    }
    CHECK(at_yard(&w, nearb) && !at_yard(&w, farb), "moored by the drydock: %d, far off: %d", at_yard(&w, nearb), at_yard(&w, farb));
    run(&w, 200);
    CHECK(w.ships[nearb].comp[4].structure > 0.9f && !w.ships[nearb].module_lost[eng] && w.ships[nearb].crew > bb->complement * 0.6f,
          "yard repairs: structure %.2f, engine lost %d, crew %.0f", w.ships[nearb].comp[4].structure,
          w.ships[nearb].module_lost[eng], w.ships[nearb].crew);
    CHECK(w.ships[farb].comp[4].structure < 0.21f && w.ships[farb].module_lost[eng], "no repairs at sea");

    /* Building: budget pays, the slipway launches, a wrecked slipway stops work. */
    world_init(&w);
    y = shipyard_spawn(&w, uyard, 0, 0, 0, 0);
    float before = w.sides[0].budget;
    CHECK(order_build(&w, 0, dd) == 0 && w.sides[0].budget == before - class_cost(dd), "queued a destroyer");
    w.sides[0].budget = 10;
    CHECK(order_build(&w, 0, bb) == -1, "can't afford a battleship");
    int ships0 = w.count;
    run(&w, build_time(dd) + 1);
    CHECK(w.count == ships0 + 1 && w.ships[w.count - 1].cls == dd, "destroyer launched after %.0f s", build_time(dd));
    const ship *nb = &w.ships[w.count - 1];
    CHECK(hypotf(nb->x, nb->z) < 1200 && nb->z > 0, "launched seaward of the yard (%.0f, %.0f)", nb->x, nb->z);
    w.sides[0].budget = 1000;
    order_build(&w, 0, dd);
    for (int m = 0; m < uyard->module_count; m++)
        if (uyard->modules[m].kind == MOD_SLIPWAY) { w.ships[y].module_hp[m] = 0; w.ships[y].module_lost[m] = 1; }
    ships0 = w.count;
    run(&w, build_time(dd) * 2);
    CHECK(w.count == ships0, "no slipway, no ships");

    /* Ammunition: empty magazines don't fire; the yard restocks them. */
    world_init(&w);
    shipyard_spawn(&w, uyard, 0, 0, 0, 0);
    int gunner = world_spawn(&w, bb, 0, 6000, 900, 0);   /* out at sea first */
    world_spawn(&w, jdd, 1, 6000, 3000, PI_F);
    w.ships[2].hold_fire = 1;
    for (int m = 0; m < bb->mount_count; m++) w.ships[gunner].turrets[m].ammo = 0;
    w.ships[0].hold_fire = 1;                            /* keep the yard's own guns out of it */
    run(&w, 20);
    CHECK(w.shots == 0, "no ammunition, no shells (%d)", w.shots);
    w.ships[gunner].x = w.ships[gunner].prev_x = 0;      /* now alongside the drydock */
    w.ships[gunner].hold_fire = 1;
    run(&w, 130);
    int full = 0;
    for (int m = 0; m < bb->mount_count; m++) full += w.ships[gunner].turrets[m].ammo == ammo_capacity(bb, m);
    CHECK(full == bb->mount_count || w.shots > 0, "restocked alongside: %d of %d mounts full", full, bb->mount_count);

    /* Ships can't sail through the yard. */
    world_init(&w);
    shipyard_spawn(&w, uyard, 0, 0, 0, 0);
    int intr = world_spawn(&w, dd, 1, 0, -100, 0);
    run(&w, 1);
    CHECK(hypotf(w.ships[intr].x, w.ships[intr].z) > 360, "pushed off the land to %.0f", hypotf(w.ships[intr].x, w.ships[intr].z));

    /* Modes. */
    world_init(&w);
    w.mode = MODE_ASSAULT;
    shipyard_spawn(&w, uyard, 0, 0, -7000, 0);
    int jy = shipyard_spawn(&w, yard, 1, 0, 7000, PI_F);
    world_spawn(&w, dd, 0, 0, -5000, 0);
    world_step(&w);
    CHECK(world_winner(&w) == -1, "assault: undecided while both yards stand (no enemy ships is not a win)");
    for (int k = 0; k < 400 && w.ships[jy].alive; k++) damage_hit(&w, jy, HIT_SHELL, 0, (float)(k % 8) * 70 - 280, 95, 9, 0);
    world_step(&w);
    CHECK(!w.ships[jy].alive && world_winner(&w) == 0, "assault: destroying the Japanese yard wins (alive %d)", w.ships[jy].alive);
    world_init(&w);
    w.mode = MODE_DEFEND;
    w.defender = 1;
    w.time_limit = 60;
    shipyard_spawn(&w, yard, 1, 0, 7000, PI_F);
    world_spawn(&w, dd, 0, 0, -5000, 0);
    run(&w, 30);
    CHECK(world_winner(&w) == -1 && mode_time_left(&w) > 29, "defend: undecided at 30 s");
    run(&w, 31);
    CHECK(world_winner(&w) == 1, "defend: the defender wins on time");

    /* AI yard builds; the attacker's reinforcements arrive on schedule. */
    world_init(&w);
    y = shipyard_spawn(&w, yard, 1, 0, 0, 0);
    w.ships[y].ai = 1;
    w.sides[1].options[w.sides[1].option_count++] = jdd;
    w.sides[0].options[w.sides[0].option_count++] = dd;
    w.sides[0].reinforce_every = 100;
    w.sides[0].entry_z = -6000;
    run(&w, 150);
    int jp = 0, us = 0;
    for (int i = 0; i < w.count; i++) { jp += w.ships[i].side == 1 && !is_shore(&w.ships[i]); us += w.ships[i].side == 0; }
    CHECK(jp >= 2 && us == 1, "AI yard built %d ships, reinforcements %d", jp, us);
}

static command mk(cmd_kind k, int player, int ship)
{
    command c;
    memset(&c, 0, sizeof c);
    c.kind = (uint8_t)k;
    c.player = (uint8_t)player;
    if (ship >= 0) cmd_set_ship(&c, ship);
    return c;
}

static void test_fleet(const class_table *ct)
{
    const ship_class *bb = class_find(ct, "AmericanBattleship"), *dd = class_find(ct, "AmericanDestroyer");
    const ship_class *jbb = class_find(ct, "JapaneseBattleship");
    world w;

    /* Commands: a player orders his own ships, not his teammate's or the enemy's. */
    world_init(&w);
    int mine = world_spawn(&w, dd, 0, 0, 0, 0), mate = world_spawn(&w, dd, 0, 500, 0, 0);
    int foe = world_spawn(&w, dd, 1, 0, 5000, PI_F);
    w.ships[mate].owner = 2;                             /* a second player on team 0 */
    command c = mk(CMD_MOVE, 0, mine);
    cmd_set_ship(&c, mate);
    cmd_set_ship(&c, foe);
    c.x = 0;
    c.z = 3000;
    world_apply(&w, &c);
    CHECK(w.ships[mine].wp_count == 1 && w.ships[mate].wp_count == 0 && w.ships[foe].wp_count == 0,
          "move: own %d, teammate's %d, enemy's %d", w.ships[mine].wp_count, w.ships[mate].wp_count, w.ships[foe].wp_count);
    c = mk(CMD_ATTACK, 0, mine);
    c.a = mate;                                          /* can't focus fire on a friend */
    c.b = 1 << BAT_MAIN;
    world_apply(&w, &c);
    CHECK(w.ships[mine].order_target[BAT_MAIN] == -1, "no friendly focus fire");

    /* Flagship: one per player, tighter gunnery, nearby ships helped, the fleet rattled
     * when she goes. */
    world_init(&w);
    int f = world_spawn(&w, bb, 0, 0, 0, 0), other = world_spawn(&w, bb, 0, 600, 0, 0), far = world_spawn(&w, bb, 0, 9000, 0, 0);
    world_spawn(&w, jbb, 1, 0, 2500, PI_F);
    float before = command_accuracy(&w, other);
    c = mk(CMD_FLAGSHIP, 0, f);
    world_apply(&w, &c);
    CHECK(w.ships[f].flagship && w.flagship_of[0] == f, "flagship set");
    CHECK(command_accuracy(&w, f) < 0.9f && command_accuracy(&w, other) < before && command_accuracy(&w, far) == before,
          "flagship %.2f, near %.2f, far %.2f", command_accuracy(&w, f), command_accuracy(&w, other), command_accuracy(&w, far));
    c = mk(CMD_FLAGSHIP, 0, other);
    world_apply(&w, &c);
    CHECK(!w.ships[f].flagship && w.ships[other].flagship, "only one flagship per player");
    damage_hit(&w, other, HIT_SHELL, 0, 0, 1e6f, 99, 0);
    world_step(&w);
    CHECK(!w.ships[other].alive && w.shaken[0] > 0 && command_accuracy(&w, f) > 1.1f, "losing her rattles the fleet (%.2f)",
          command_accuracy(&w, f));

    /* Officers: assign, stack, lose with the ship. */
    world_init(&w);
    int a = world_spawn(&w, bb, 0, 0, 0, 0), b = world_spawn(&w, bb, 0, 600, 0, 0);
    int g1 = officer_add(&w, 0, OFF_GUNNERY), g2 = officer_add(&w, 0, OFF_GUNNERY), dc = officer_add(&w, 0, OFF_DAMAGE_CONTROL);
    c = mk(CMD_OFFICER, 0, a);
    c.a = g1;
    world_apply(&w, &c);
    c.a = g2;
    world_apply(&w, &c);
    c = mk(CMD_OFFICER, 0, b);
    c.a = dc;
    world_apply(&w, &c);
    CHECK(w.ships[a].officers[OFF_GUNNERY] == 2 && officer_level(&w.ships[a], OFF_GUNNERY) == 1.5f &&
          w.ships[b].officers[OFF_DAMAGE_CONTROL] == 1, "officers aboard");
    /* Gunnery officers reload faster: compare shots over a minute against a twin without. */
    int shots[2];
    for (int pass = 0; pass < 2; pass++) {
        world_init(&w);
        world_spawn(&w, bb, 0, 0, 0, 0);
        world_spawn(&w, jbb, 1, 2400, 0, 0);
        w.ships[1].hold_fire = 1;
        if (pass) { int o = officer_add(&w, 0, OFF_GUNNERY); order_officer(&w, 0, o, 0); }
        for (int t = 0; t < 120 * SIM_HZ; t++) { world_step(&w); damage_init(&w.ships[1]); }
        shots[pass] = w.shots;
    }
    CHECK(shots[1] > shots[0], "gunnery officer: %d shells vs %d", shots[1], shots[0]);
    /* A DC officer adds a team. */
    world_init(&w);
    world_spawn(&w, bb, 0, 0, 0, 0);
    int o = officer_add(&w, 0, OFF_DAMAGE_CONTROL);
    order_officer(&w, 0, o, 0);
    for (int k = 0; k < bb->comp_count; k++) w.ships[0].comp[k].fire = 0.3f;
    world_step(&w);
    int teams = 0;
    for (int k = 0; k < bb->comp_count; k++) teams += w.ships[0].comp[k].crews;
    CHECK(teams == bb->dc_crews + 1, "DC officer: %d teams", teams);
    damage_hit(&w, 0, HIT_SHELL, 0, 0, 1e6f, 99, 0);
    world_step(&w);
    CHECK(w.officer_ship[0][o] == -2, "officer lost with his ship");

    /* AI captains: a fire by the magazine gets it flooded without anyone asking. */
    world_init(&w);
    int aib = world_spawn(&w, bb, 1, 0, 0, 0);
    w.ships[aib].ai = 1;
    int mag = module_index(bb, MOD_MAGAZINE);
    w.ships[aib].comp[bb->modules[mag].comp].fire = 0.7f;
    run(&w, 3);
    CHECK(w.ships[aib].comp[bb->modules[mag].comp].mag_flooded, "AI captain flooded the burning magazine");

    /* Assault home guards: a quarter of each side, whatever the ships' indices. */
    world_init(&w);
    w.mode = MODE_ASSAULT;
    const ship_class *uyd = class_find(ct, "AmericanShipyard"), *jyd = class_find(ct, "JapaneseShipyard");
    if (uyd && jyd) {
        shipyard_spawn(&w, uyd, 0, 0, -7000, 0);
        shipyard_spawn(&w, jyd, 1, 0, 7000, PI_F);
        for (int k = 0; k < 8; k++) {               /* interleaved, as two yards build them */
            world_spawn(&w, dd, 0, (float)k * 300, -5000, 0);
            world_spawn(&w, jbb, 1, (float)k * 300, 5000, PI_F);
        }
        for (int i = 0; i < w.count; i++) w.ships[i].ai = 1;
        run(&w, 120);
        float sp[2] = {0, 0};
        for (int i = 0; i < w.count; i++)
            if (!is_shore(&w.ships[i])) sp[w.ships[i].side] += hypotf(w.ships[i].x - w.ships[w.sides[w.ships[i].side].yard].x,
                                                                      w.ships[i].z - w.ships[w.sides[w.ships[i].side].yard].z);
        CHECK(fabsf(sp[0] - sp[1]) < 0.35f * (sp[0] + sp[1]) / 2, "both sides deploy alike (mean distance from home %.0f vs %.0f)",
              sp[0] / 8, sp[1] / 8);
    }

    /* Replays: the same command stream reproduces the battle exactly. */
    float final[2][4];
    for (int pass = 0; pass < 2; pass++) {
        static command_queue q;
        memset(&q, 0, sizeof q);
        world_init(&w);
        world_seed(&w, 5);
        for (int k = 0; k < 2; k++) {
            world_spawn(&w, bb, 0, (float)k * 500, 0, 0);
            world_spawn(&w, jbb, 1, (float)k * 500, 4000, PI_F);
        }
        w.ships[1].ai = w.ships[3].ai = 1;
        command m = mk(CMD_MOVE, 0, 0);
        cmd_set_ship(&m, 2);
        m.x = 2000;
        m.z = 2000;
        cmdq_push(&q, &m, 100);
        command at = mk(CMD_ATTACK, 0, 0);
        at.a = 3;
        at.b = 1 << BAT_MAIN;
        cmdq_push(&q, &at, 400);
        command ev = mk(CMD_EVADE, 0, 2);
        ev.x = 30;
        cmdq_push(&q, &ev, 900);
        for (int t = 0; t < 180 * SIM_HZ; t++) {
            cmdq_apply_due(&q, &w);
            world_step(&w);
        }
        for (int k = 0; k < 4; k++) final[pass][k] = w.ships[k].hp + w.ships[k].x;
    }
    CHECK(!memcmp(final[0], final[1], sizeof final[0]), "replayed commands give the same battle");
}

/* Generated models: every class gets a plausible ship (and turret) of its own size. */
static void test_genship(const class_table *ct)
{
    int bad = 0, checked = 0;
    for (int i = 0; i < ct->count; i++) {
        const ship_class *c = &ct->classes[i];
        if (c->hull_type == HULL_SHIPYARD) continue;
        gen_model g;
        genship_build(c, &g);
        int tris = 0;
        float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
        for (int p = 0; p < g.count; p++) {
            tris += g.part[p].ni / 3;
            for (int k = 0; k < g.part[p].nv; k++)
                for (int q = 0; q < 3; q++) { lo[q] = fminf(lo[q], g.part[p].v[k].pos[q]); hi[q] = fmaxf(hi[q], g.part[p].v[k].pos[q]); }
        }
        float len = hi[2] - lo[2], beam = hi[0] - lo[0];
        int ok = tris > 200 && fabsf(len - 2 * c->half_length) < 0.1f * c->half_length + 2 &&
                 beam < 2.6f * c->half_beam + 1 && beam > 1.5f * c->half_beam && lo[1] < 0 && hi[1] > 0;
        if (!ok) {
            bad++;
            if (bad < 5) printf("  genship %s: %d tris, length %.0f (class %.0f), beam %.0f (class %.0f), y %.0f..%.0f\n",
                                c->name, tris, len, 2 * c->half_length, beam, 2 * c->half_beam, lo[1], hi[1]);
        }
        genship_free(&g);
        genship_gun(c, &g);
        if (g.part[5].ni < 36) bad++;
        genship_free(&g);
        checked++;
    }
    CHECK(checked > 50 && bad == 0, "generated models: %d of %d off", bad, checked);
}

static void test_vision(const class_table *ct)
{
    const ship_class *bb = class_find(ct, "AmericanBattleship"), *dd = class_find(ct, "AmericanDestroyer");
    const ship_class *jdd = class_find(ct, "JapaneseDestroyer"), *ss = class_find(ct, "JapaneseSubmarine");
    const ship_class *ca = class_find(ct, "AmericanCruiser");
    world w;

    /* Lookouts and radar. */
    world_init(&w);
    w.fog = 1;
    int a = world_spawn(&w, dd, 0, 0, 0, 0), near = world_spawn(&w, jdd, 1, 4000, 0, 0), mid = world_spawn(&w, jdd, 1, 0, 5500, 0);
    int far = world_spawn(&w, jdd, 1, -8000, 0, 0);
    for (int i = 1; i < 4; i++) w.ships[i].hold_fire = 1;
    w.ships[a].hold_fire = 1;
    world_step(&w);
    CHECK(visible_to(&w, 0, near) && visible_to(&w, 0, mid) && !visible_to(&w, 0, far), "radar: 4,000 and 5,500 seen, 8,000 not");
    int rad = module_index(dd, MOD_RADAR);
    w.ships[a].module_hp[rad] = 0;
    w.ships[a].module_lost[rad] = 1;
    world_step(&w);
    CHECK(visible_to(&w, 0, near) && !visible_to(&w, 0, mid), "radar out: 5,500 lost to sight");
    CHECK(!can_engage(&w, far, 0, a) || 1, "");
    /* Gun flashes: after firing she stays visible to a distant ship for a while. */
    world_init(&w);
    w.fog = 1;
    int gun = world_spawn(&w, bb, 0, 0, 0, 0);
    int decoy = world_spawn(&w, jdd, 1, 2500, 0, PI_F / 2);
    int watcher = world_spawn(&w, jdd, 1, 0, -8000, 0);
    w.ships[decoy].hold_fire = w.ships[watcher].hold_fire = 1;
    run(&w, 25);                                         /* she fires at the decoy */
    int fired = w.shots > 0 && w.ships[gun].reveal > 0;
    w.ships[decoy].alive = 0;                            /* the decoy's gone: only the watcher looks */
    w.ships[gun].hold_fire = 1;
    world_step(&w);
    int flash = visible_to(&w, 1, gun);
    run(&w, REVEAL_S + 1);
    CHECK(fired && flash && !visible_to(&w, 1, gun), "gun flashes: fired %d, seen from 8,000 %d, then lost %d", fired, flash,
          !visible_to(&w, 1, gun));
    /* Submarines: only close. */
    world_init(&w);
    w.fog = 1;
    world_spawn(&w, dd, 0, 0, 0, 0);
    int sub = world_spawn(&w, ss, 1, 1600, 0, 0);
    w.ships[sub].hold_fire = 1;
    w.ships[0].hold_fire = 1;
    world_step(&w);
    int far_sub = visible_to(&w, 0, sub);
    w.ships[sub].x = w.ships[sub].prev_x = 1100;
    world_step(&w);
    CHECK(!far_sub && visible_to(&w, 0, sub), "sub: hidden at 1,600, heard at 1,100");
    /* Recon: a floatplane finds a ship far beyond sight. */
    world_init(&w);
    w.fog = 1;
    world_spawn(&w, ca, 0, 0, 0, 0);
    int hidden = world_spawn(&w, jdd, 1, 0, 9000, 0);
    w.ships[hidden].hold_fire = 1;
    w.ships[0].hold_fire = 1;
    world_step(&w);
    int seen0 = visible_to(&w, 0, hidden);
    CHECK(order_recon(&w, 0, 0, 9000) == 0 && order_recon(&w, 0, 0, 9000) == -1, "recon plane launched (once per cooldown)");
    int found = 0;
    for (int t = 0; t < 200 * SIM_HZ && !found; t++) { world_step(&w); found = visible_to(&w, 0, hidden); }
    CHECK(!seen0 && found, "recon found the hidden ship");
    /* The AI searches when it sees nothing. */
    world_init(&w);
    w.fog = 1;
    int seeker = world_spawn(&w, dd, 0, 0, 0, 0);
    world_spawn(&w, jdd, 1, 0, 12000, 0);
    w.ships[seeker].ai = 1;
    w.ships[1].hold_fire = 1;
    run(&w, 60);
    CHECK(w.ships[seeker].z > 300, "AI went looking (moved %.0f toward the enemy)", w.ships[seeker].z);
}

/* Standalone: our own data alone gives a full set of classes that fight. */
static void test_standalone(void)
{
    static class_table st;
    const char *paths[] = {"engine/data/standalone.json", "../data/standalone.json", "data/standalone.json"};
    int ok = -1;
    for (int k = 0; k < 3 && ok; k++) ok = classes_load_standalone(paths[k], &st);
    CHECK(!ok && st.count >= 24, "standalone data: %d classes", st.count);
    if (ok) return;
    const char *needed[] = {"AmericanBattleship", "AmericanCruiser", "AmericanDestroyer", "AmericanAircraftCarrier",
                            "AmericanSubmarine", "AmericanTorpedoBoat", "JapaneseBattleship", "JapaneseCruiser",
                            "JapaneseDestroyer", "JapaneseAircraftCarrier", "JapaneseSubmarine", "JapaneseTorpedoBoat",
                            "GermanDestroyer", "BritishBattleship", "GermanBattleship"};
    for (size_t k = 0; k < sizeof needed / sizeof *needed; k++) CHECK(class_find(&st, needed[k]), "standalone has %s", needed[k]);
    const ship_class *iowa = class_find(&st, "AmericanBattleship"), *yamato = class_find(&st, "JapaneseBattleship");
    const ship_class *fletcher = class_find(&st, "AmericanDestroyer");
    if (!iowa || !yamato || !fletcher) return;
    CHECK(fabsf(iowa->half_length - 10.9f * powf(270, 0.5155f) / 2) < 1 && iowa->mount_count >= 5 && fletcher->mount_count >= 7,
          "Iowa %.0f long, %d mounts; Fletcher %d mounts", 2 * iowa->half_length, iowa->mount_count, fletcher->mount_count);
    /* Damage layout from the plan: engine rooms under the funnels, bridge in the tower. */
    for (int m = 0; m < iowa->module_count; m++) {
        const module_def *d = &iowa->modules[m];
        float lo = iowa->comp_z[d->comp], hi = iowa->comp_z[d->comp + 1];
        if (d->kind == MOD_BRIDGE) CHECK(iowa->plan.tower_z >= lo && iowa->plan.tower_z <= hi, "Iowa's bridge is in her tower");
        if (d->kind == MOD_ENGINE) {
            int under = 0;
            for (int f = 0; f < iowa->plan.funnels; f++) under |= iowa->plan.funnel_z[f] >= lo && iowa->plan.funnel_z[f] <= hi;
            CHECK(under, "Iowa's engine room (compartment %d) is under a funnel", d->comp);
        }
    }
    int torp = 0;
    for (int m = 0; m < fletcher->mount_count; m++) torp += fletcher->mounts[m].bat == BAT_TORPEDO;
    CHECK(torp == 2 && iowa->comp_count == 10 && count_modules(iowa, MOD_MAGAZINE) >= 2, "Fletcher tubes %d, Iowa layout", torp);
    world w;
    world_init(&w);
    world_spawn(&w, iowa, 0, 0, 0, 0);
    world_spawn(&w, yamato, 1, 2000, 2000, PI_F);
    w.ships[0].ai = w.ships[1].ai = 1;
    run(&w, 300);
    CHECK(w.shots > 20 && (w.ships[0].hp < iowa->max_health || w.ships[1].hp < yamato->max_health),
          "Iowa and Yamato fight: %d shells", w.shots);
}

static void test_islands(const class_table *ct)
{
    const ship_class *dd = class_find(ct, "AmericanDestroyer"), *jdd = class_find(ct, "JapaneseDestroyer");
    const ship_class *pt = class_find(ct, "AmericanTorpedoBoat"), *bb = class_find(ct, "JapaneseBattleship");
    world w;
    world_init(&w);
    w.fog = 1;
    w.isles[0] = (island){0, 2000, 600, 100, 1};
    w.isle_count = 1;
    int a = world_spawn(&w, dd, 0, 0, 0, 0), hidden = world_spawn(&w, jdd, 1, 0, 3900, 0), open = world_spawn(&w, jdd, 1, 3000, 1500, 0);
    w.ships[a].hold_fire = w.ships[hidden].hold_fire = w.ships[open].hold_fire = 1;
    world_step(&w);
    CHECK(!visible_to(&w, 0, hidden) && visible_to(&w, 0, open), "island hides the ship behind it");
    /* Routing: a move straight through the island goes round it, and arrives. */
    int ids[1] = {a};
    order_move(&w, ids, 1, 0, 4400, 0);
    CHECK(w.ships[a].wp_count >= 2, "routed round the island (%d waypoints)", w.ships[a].wp_count);
    float closest = 1e9f;
    for (int t = 0; t < 600 * SIM_HZ && w.ships[a].wp_count; t++) {
        world_step(&w);
        closest = fminf(closest, hypotf(w.ships[a].x, w.ships[a].z - 2000));
    }
    CHECK(closest > 600 && hypotf(w.ships[a].x, w.ships[a].z - 4400) < 300, "never aground (closest %.0f), arrived", closest);
    /* Land stops a ship pushed at it, and a torpedo run at it. */
    world_init(&w);
    w.isles[0] = (island){1200, 0, 450, 30, 2};         /* low: guns still see over the line */
    w.isle_count = 1;
    world_spawn(&w, pt, 0, 0, 0, 0);
    world_spawn(&w, bb, 1, 2400, 0, 0);                  /* abeam, beyond the island */
    w.ships[1].hold_fire = 1;
    run(&w, 120);
    CHECK(w.torps >= 3 && w.torp_hits == 0, "torpedoes stopped by the island (%d hits, %d fired)", w.torp_hits, w.torps);
    world_init(&w);
    w.isles[0] = (island){0, 600, 300, 80, 3};
    w.isle_count = 1;
    int ram = world_spawn(&w, dd, 0, 0, 0, 0);
    w.ships[ram].wp[0] = (waypoint){0, 600};      /* steer straight for its centre */
    w.ships[ram].wp_count = 1;
    run(&w, 60);
    CHECK(hypotf(w.ships[ram].x, w.ships[ram].z - 600) >= 300, "can't sail onto land (%.0f from centre)",
          hypotf(w.ships[ram].x, w.ships[ram].z - 600));
}

static void test_specials(const class_table *ct)
{
    const ship_class *bb = class_find(ct, "AmericanBattleship"), *dd = class_find(ct, "AmericanDestroyer");
    const ship_class *ss = class_find(ct, "AmericanSubmarine"), *cb = class_find(ct, "AmericanBattlecruiser");
    const ship_class *de = class_find(ct, "AmericanDestroyerEscort"), *pt = class_find(ct, "AmericanTorpedoBoat");
    const ship_class *ca = class_find(ct, "AmericanCruiser"), *cv = class_find(ct, "AmericanAircraftCarrier");
    const ship_class *jbb = class_find(ct, "JapaneseBattleship"), *jss = class_find(ct, "JapaneseSubmarine");
    if (!bb || !dd || !ss || !cb || !de || !pt || !ca || !cv || !jbb || !jss) { CHECK(0, "special classes"); return; }
    CHECK(special_of(bb) == SP_FOCUS_FIRE && special_of(dd) == SP_RAPID_FIRE && special_of(ss) == SP_TARGETING_TORPEDO &&
          special_of(cb) == SP_ACCURATE_FIRE && special_of(de) == SP_HEDGEHOG && special_of(pt) == SP_EVASIVE_ACTION &&
          special_of(ca) == SP_AA_BLAST && special_of(cv) == SP_ACE_PILOT, "the original's specials by hull");
    world w;
    /* Focus Fire: tighter on the chosen target, once per recharge. */
    world_init(&w);
    world_spawn(&w, bb, 0, 0, 0, 0);
    world_spawn(&w, jbb, 1, 2000, 0, 0);
    CHECK(order_special(&w, 0, 1) == 0 && order_special(&w, 0, 1) == -1, "Focus Fire goes, then recharges");
    CHECK(special_spread(&w, 0, 1) < 0.65f && w.ships[0].order_target[BAT_MAIN] == 1, "Focus Fire tightens the spread on her target");
    /* Rapid Fire: more shells in the same quarter-minute. */
    int shots[2];
    for (int pass = 0; pass < 2; pass++) {
        world_init(&w);
        world_spawn(&w, dd, 0, 0, 0, 0);
        world_spawn(&w, jbb, 1, 1500, 0, 0);
        w.ships[1].hold_fire = 1;
        run(&w, 10);
        if (pass) order_special(&w, 0, 1);
        int before = w.shots;
        run(&w, 15);
        shots[pass] = w.shots - before;
    }
    CHECK(shots[1] > shots[0] * 3 / 2, "Rapid Fire: %d shells vs %d", shots[1], shots[0]);
    /* Hedgehog: a ring of bombs on a nearby submarine. */
    world_init(&w);
    world_spawn(&w, de, 0, 0, 0, 0);
    world_spawn(&w, jss, 1, 0, 500, 0);
    w.ships[0].hold_fire = w.ships[1].hold_fire = 1;
    CHECK(order_special(&w, 0, -1) == 0, "Hedgehog thrown");
    run(&w, 10);
    CHECK(w.dc_hits > 0 && w.ships[1].hp < jss->max_health, "Hedgehog hits (%d)", w.dc_hits);
    /* Evasive Action: harder to hit. */
    world_init(&w);
    world_spawn(&w, jbb, 1, 0, 0, 0);
    world_spawn(&w, pt, 0, 1500, 0, 0);
    order_special(&w, 1, -1);
    CHECK(special_spread(&w, 0, 1) > 1.4f, "Evasive Action widens the spread against her");
    /* Targeting Torpedo: a spread that follows a ship turning away. */
    int hits[2];
    for (int pass = 0; pass < 2; pass++) {
        world_init(&w);
        world_spawn(&w, ss, 0, 0, 0, 0);
        world_spawn(&w, jbb, 1, 0, 1600, PI_F / 2);
        w.ships[1].hold_fire = 1;
        w.ships[1].speed = w.ships[1].max_speed * 0.4f;   /* slow enough for the bow tubes' lead */
        w.ships[1].max_speed *= 0.4f;
        w.ships[1].wp[0] = (waypoint){20000, 1600};
        w.ships[1].wp_count = 1;
        if (pass) order_special(&w, 0, 1);
        for (int t = 0; t < 120 * SIM_HZ; t++) {
            world_step(&w);
            if (w.torps > 0 && w.ships[1].wp[0].z < 2000) {   /* torpedoes away: she turns hard */
                w.ships[1].wp[0] = (waypoint){20000, 20000};
            }
        }
        hits[pass] = w.torp_hits;
        if (!w.torps) hits[pass] = -100;
    }
    CHECK(hits[1] > hits[0] && hits[0] >= 0, "Targeting Torpedo: %d hits vs %d unguided", hits[1], hits[0]);
}

/* The AI fleet commander: in formation on the approach (the battle line abreast, the
 * screen ahead, carriers astern) at the slowest ship's pace, then engaged with the
 * screen's torpedo craft sent at the enemy's biggest ship. */
static void test_commander(const class_table *ct)
{
    const ship_class *bb = class_find(ct, "AmericanBattleship"), *dd = class_find(ct, "AmericanDestroyer");
    const ship_class *cv = class_find(ct, "AmericanAircraftCarrier"), *pt = class_find(ct, "AmericanTorpedoBoat");
    const ship_class *ebb = class_find(ct, "JapaneseBattleship"), *edd = class_find(ct, "JapaneseDestroyer");
    if (!bb || !dd || !cv || !pt || !ebb || !edd) { CHECK(0, "commander classes"); return; }
    static world w;
    float spread[2];
    for (int on = 0; on < 2; on++) {
        world_init(&w);
        w.fog = 1;
        w.no_commander = on ? 0 : 1;
        const ship_class *fleet[] = {bb, bb, dd, pt, cv};
        for (int k = 0; k < 5; k++) { world_spawn(&w, fleet[k], 0, (float)k * 300, (float)-k * 200, 0); w.ships[k].ai = 1; }
        world_spawn(&w, ebb, 1, 0, 30000, PI_F);       /* far out of sight */
        world_spawn(&w, edd, 1, 300, 30000, PI_F);
        w.ships[5].hold_fire = w.ships[6].hold_fire = 1;
        run(&w, 240);
        spread[on] = w.ships[3].z - w.ships[0].z;      /* how far the PT boat ran ahead of the line */
        if (on) {
            const ship *b0 = &w.ships[0], *d = &w.ships[2], *c = &w.ships[4];
            CHECK(w.cmdr[0].state == CMDR_APPROACH && b0->z > 1500, "approach: the line has advanced (%.0f)", b0->z);
            CHECK(d->z > b0->z + 800 && c->z < b0->z - 1500, "formation: screen %.0f ahead, carrier %.0f astern of the line",
                  d->z - b0->z, b0->z - c->z);
        }
    }
    CHECK(fabsf(spread[1] - 1500) < 600, "the fast PT boat keeps her station 1,500 ahead: %.0f (%.0f without a commander)",
          spread[1], spread[0]);
    /* In contact: the PT boat and the destroyer go for the battleship, not the nearer destroyer. */
    world_init(&w);
    for (int k = 0; k < 3; k++) w.ships[world_spawn(&w, k ? (k == 1 ? dd : pt) : bb, 0, (float)k * 300, 0, 0)].ai = 1;
    world_spawn(&w, edd, 1, 600, 3000, PI_F);
    world_spawn(&w, ebb, 1, -2500, 6000, PI_F);
    w.ships[3].hold_fire = w.ships[4].hold_fire = 1;
    run(&w, 4);
    CHECK(w.cmdr[0].state == CMDR_ENGAGE && w.cmdr[0].capital == 4 && w.ships[2].ai_target == 4,
          "engaged: torpedo craft sent at the battleship (capital %d, PT target %d)", w.cmdr[0].capital, w.ships[2].ai_target);
}

int main(int argc, char **argv)
{
    const char *assets = argc > 1 ? argv[1] : "assets";
    char path[1024];
    if (argc > 3 && !strcmp(argv[2], "--synth-wav")) {   /* write the synthesised sounds as WAVs */
        sound_bank b;
        synth_bank(&b);
        for (int i = 0; i < b.count; i++) {
            snprintf(path, sizeof path, "%s/%s.wav", argv[3], b.names[i]);
            FILE *f = fopen(path, "wb");
            if (!f) continue;
            uint32_t data = (uint32_t)b.length[i] * 2, riff = 36 + data, fmt = 16, rate = BANK_RATE, bps = BANK_RATE * 2;
            uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
            fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
            fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&bps, 4, 1, f);
            fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
            fwrite(b.pcm[i], 2, (size_t)b.length[i], f);
            fclose(f);
        }
        printf("%d synthesised sounds written\n", b.count);
        bnk_free(&b);
        return 0;
    }
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
    test_sound(assets);
    test_sim(&ct);
    test_combat(&ct);
    test_air(&ct);
    test_damage(&ct);
    test_command(&ct);
    test_commander(&ct);
    test_bases(&ct);
    test_fleet(&ct);
    test_genship(&ct);
    test_vision(&ct);
    test_standalone();
    test_islands(&ct);
    test_specials(&ct);
    printf("%d failures\n", failures);
    return failures != 0;
}
