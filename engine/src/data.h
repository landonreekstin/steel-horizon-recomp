/* Ship classes and weapons from assets/data/{ships,weapons}.json (tools/data2json.py). */
#ifndef DATA_H
#define DATA_H

#define MAX_CLASSES 80
#define MAX_CLASS_WEAPONS 12
#define MAX_MOUNTS 16

typedef enum { WK_SURFACE, WK_AIR, WK_SUBMERGED } weapon_kind;
typedef enum { AMMO_NONE, AMMO_SHELL, AMMO_TORPEDO, AMMO_DC, AMMO_BOMB } ammo_kind;
typedef enum { BAT_MAIN, BAT_SECONDARY, BAT_TORPEDO, BAT_ASW, BATTERIES } battery;

typedef struct {
    weapon_kind kind;
    char name[32];          /* gunValues name, e.g. "16 Inch" */
    int mount_group;        /* original mount group: 1 fore, 2 aft, 3/4 sides, 5/6 torp, 7 DC */
    float damage, min_range, max_range, fire_rate;  /* from gunValues.xml; 0 if not a gun */
    ammo_kind ammo;         /* from the name: "... T" torpedo, "... DC" depth charge */
} class_weapon;

/* A weapon mount: a turret point from the model's .pts, or a virtual one for
 * batteries the models don't place (side guns, torpedo tubes, depth-charge racks). */
typedef struct {
    char name[8];           /* .pts prefix, e.g. "F1" (the gun model is drawn there); "" if virtual */
    float pos[3];           /* model space, +Z bow, +Y up */
    float arc_center, arc_half;  /* training arc, radians relative to the bow (+ = toward +X) */
    int weapon;             /* index into ship_class.weapons */
    battery bat;
} turret_mount;

typedef struct {
    char name[48];          /* e.g. "AmericanBattleship" */
    char nation[16];
    char code[8];           /* hull code in model names, e.g. "BB" */
    int hull_type, submerged;
    float max_health, speed, turn_rate, size_mod, exposure_mod, aa_damage;
    char model[3][128];     /* intact, damaged, wrecked (.pingu paths, relative to files/) */
    char gun_model[128];
    int weapon_count;
    class_weapon weapons[MAX_CLASS_WEAPONS];
    /* Filled by classes_load_layouts (src/layout.c), from the intact model: */
    float half_length, half_beam;  /* hull footprint (model bbox), world units */
    int mount_count;
    turret_mount mounts[MAX_MOUNTS];
} ship_class;

typedef struct {
    int count;
    ship_class classes[MAX_CLASSES];
} class_table;

/* data_dir holds ships.json and weapons.json. Returns 0 on success. */
int classes_load(const char *data_dir, class_table *out);
/* Hull footprints and weapon mounts from each class's intact .pingu/.pts under
 * files_dir. Returns the number of classes whose model failed to load. */
int classes_load_layouts(class_table *t, const char *files_dir);
const ship_class *class_find(const class_table *t, const char *name);

#endif
