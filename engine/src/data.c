#include "data.h"

#include <stdio.h>
#include <string.h>

#include "json.h"

static void copy_str(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src ? src : "");
}

int classes_load(const char *data_dir, class_table *out)
{
    char path[512];
    memset(out, 0, sizeof *out);
    snprintf(path, sizeof path, "%s/weapons.json", data_dir);
    jnode *wdoc = json_load_file(path);
    snprintf(path, sizeof path, "%s/ships.json", data_dir);
    jnode *sdoc = json_load_file(path);
    if (!wdoc || !sdoc) {
        fprintf(stderr, "run tools/data2json.py first (looked in %s)\n", data_dir);
        json_free(wdoc);
        json_free(sdoc);
        return -1;
    }
    jnode *guns = json_get(wdoc, "guns");
    jnode *ships = json_get(sdoc, "ships");
    for (jnode *s = ships ? ships->child : NULL; s && out->count < MAX_CLASSES; s = s->next) {
        ship_class *c = &out->classes[out->count++];
        jnode *st = json_get(s, "stats"), *models = json_get(s, "models");
        copy_str(c->name, sizeof c->name, s->key);
        copy_str(c->nation, sizeof c->nation, json_str(s, "nation"));
        copy_str(c->code, sizeof c->code, json_str(s, "code"));
        c->hull_type = (int)json_num(s, "hull_type", 0);
        c->submerged = (int)json_num(s, "submerged", 0);
        c->max_health = (float)json_num(st, "max_health", 500);
        c->speed = (float)json_num(st, "speed", 30);
        c->turn_rate = (float)json_num(st, "turn_rate", 2);
        c->size_mod = (float)json_num(st, "size_mod", 70);
        c->exposure_mod = (float)json_num(st, "exposure_mod", 50);
        c->aa_damage = (float)json_num(st, "aa_damage", 0);
        copy_str(c->model[0], sizeof c->model[0], json_str(models, "intact"));
        copy_str(c->model[1], sizeof c->model[1], json_str(models, "damaged"));
        copy_str(c->model[2], sizeof c->model[2], json_str(models, "wrecked"));
        copy_str(c->gun_model, sizeof c->gun_model, json_str(models, "gun"));
        for (jnode *w = json_get(s, "weapons") ? json_get(s, "weapons")->child : NULL;
             w && c->weapon_count < MAX_CLASS_WEAPONS; w = w->next) {
            class_weapon *cw = &c->weapons[c->weapon_count++];
            const char *kind = json_str(w, "kind");
            cw->kind = kind && !strcmp(kind, "air") ? WK_AIR
                     : kind && !strcmp(kind, "submerged") ? WK_SUBMERGED : WK_SURFACE;
            copy_str(cw->name, sizeof cw->name, json_str(w, "weapon"));
            cw->mount_group = (int)json_num(w, "mount_group", 0);
            for (jnode *g = guns ? guns->child : NULL; g; g = g->next) {
                const char *gn = json_str(g, "name");
                size_t gl = gn ? strlen(gn) : 0;
                /* exact name, or a note in parentheses: "24 Inch T (variant init)" */
                if (gn && !strncmp(cw->name, gn, gl) &&
                    (cw->name[gl] == 0 || !strncmp(cw->name + gl, " (", 2))) {
                    cw->damage = (float)json_num(g, "base_damage", 0);
                    cw->min_range = (float)json_num(g, "min_range", 0);
                    cw->max_range = (float)json_num(g, "max_range", 0);
                    cw->fire_rate = (float)json_num(g, "fire_rate", 0);
                    break;
                }
            }
            const char *t = strstr(cw->name, " T");    /* "24 Inch T", "24 Inch T (...)" */
            cw->ammo = cw->kind == WK_AIR || cw->damage <= 0 ? AMMO_NONE
                     : strstr(cw->name, " DC") ? AMMO_DC
                     : t && (t[2] == 0 || t[2] == ' ') ? AMMO_TORPEDO : AMMO_SHELL;
        }
    }
    json_free(wdoc);
    json_free(sdoc);
    return out->count ? 0 : -1;
}

const ship_class *class_find(const class_table *t, const char *name)
{
    for (int i = 0; i < t->count; i++)
        if (!strcmp(t->classes[i].name, name)) return &t->classes[i];
    return NULL;
}
