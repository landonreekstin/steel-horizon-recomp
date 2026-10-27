/* Ship classes and weapons from assets/data/{ships,weapons}.json (tools/data2json.py). */
#ifndef DATA_H
#define DATA_H

#define MAX_CLASSES 80
#define MAX_CLASS_WEAPONS 12
#define MAX_MOUNTS 16
#define MAX_COMPARTMENTS 10
#define MAX_MODULES 32

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

/* Damage model (src/damage.c): the hull is cut into compartments stern to bow;
 * modules are the parts that can be knocked out, each sitting in one compartment. */
typedef enum { MOD_TURRET, MOD_MAGAZINE, MOD_ENGINE, MOD_STEERING, MOD_BRIDGE, MOD_RADAR, MOD_AA,
               MOD_FLIGHT_DECK, MOD_HANGAR_LIFT, MOD_DRYDOCK, MOD_SLIPWAY, MOD_KINDS } module_kind;

#define HULL_SHIPYARD 30            /* shore installation (design; not an original hull type) */

typedef struct {
    module_kind kind;
    int comp;               /* compartment index */
    int mount;              /* MOD_TURRET: the mount it is; MOD_MAGAZINE: -1 */
} module_def;

/* Standalone classes: where the real ship's features stand (engine/data/standalone.json
 * "plan"). Positions are model z (+ bow); heights above the deck; world units. */
#define PLAN_FUNNELS 4
typedef enum { TOWER_BLOCK, TOWER_PAGODA, TOWER_TRIPOD } tower_style;
typedef struct {
    int has;
    float tower_z, tower_h;
    tower_style tower;
    int funnels;
    float funnel_z[PLAN_FUNNELS], funnel_h[PLAN_FUNNELS];
    float aft_z0, aft_z1, aft_h;    /* after superstructure; aft_h 0 = none */
    float mast_z, mast_h;           /* mainmast; mast_h 0 = none */
    float island_z0, island_z1;     /* carriers */
} ship_plan;

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
    int comp_count;                 /* compartment k spans comp_z[k]..comp_z[k+1] (model z) */
    float comp_z[MAX_COMPARTMENTS + 1];
    int module_count;
    module_def modules[MAX_MODULES];
    float belt, deck;               /* armour, in shell-penetration units (see damage.c) */
    float reserve_buoyancy;         /* fraction of the hull that can flood before she sinks */
    int dc_crews;                   /* damage-control teams */
    float complement;               /* crew aboard at the start (design: typical WWII figures) */
    ship_plan plan;                 /* standalone only: the real ship's layout */
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
/* Adds "<nation>Shipyard": a shore base armed with copies of the donor class's main
 * and secondary guns. Returns it, or NULL if the table is full. */
/* Standalone mode: classes from our own data (engine/data/standalone.json), with no
 * game files: weapons, mounts and damage layout built from the table; models are
 * generated (genship.c). Returns 0 on success. */
int classes_load_standalone(const char *path, class_table *out);
const ship_class *classes_add_shipyard(class_table *t, const char *nation, const ship_class *gun_donor);
float class_cost(const ship_class *c);       /* build points */
float build_time(const ship_class *c);       /* seconds on the slipway */
const ship_class *class_find(const class_table *t, const char *name);

#endif
