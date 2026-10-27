/* Deterministic battle simulation: fixed 20 Hz tick, no rendering or input.
 * World units are the original game's (models are drawn 1:1, see notes/runtime.md).
 * Heading 0 faces +Z; positive heading turns toward +X. */
#ifndef SIM_H
#define SIM_H

#include "data.h"

#define SIM_HZ 20
#define SIM_DT (1.0f / SIM_HZ)
#define MAX_SHIPS 64
#define MAX_WAYPOINTS 16
#define MAX_PROJECTILES 1024
#define MAX_FX 512
#define MAX_AIRCRAFT 256

/* Tuning (design choices for the RTS, not values from the original). */
#define SPEED_SCALE 0.5f       /* world units per second per point of class speed */
#define RUDDER_RATE 0.4f       /* rudder travel per second (full lock in 2.5 s) */
#define RELOAD_SCALE 2.5f      /* gun reload = gunValues fire_rate x this (16": 17.5 s) */
#define TORP_RELOAD_SCALE 10.0f
#define GRAVITY 60.0f          /* shell arcs: apex about a third of the range */
#define TORP_SPEED 30.0f       /* about 1.8x a battleship's top speed */
#define DISPERSION 0.010f      /* shell aim error (sigma) per unit of range at 85% accuracy */
#define NEAR_MISS 12.0f        /* a shell this close to a hull does a quarter damage */
#define DC_RADIUS 45.0f        /* depth-charge lethal radius around the hull */

/* Aircraft (src/air.c). Plane stats are the original's (CFighterPlane / CBomberPlane
 * constructors, the same for every nation); speeds, AA and the air group are design. */
#define AIR_SPEED_SCALE 0.15f  /* original plane speed (fighter 400, bomber 300) -> u/s */
#define AIR_GROUP_FIGHTERS 6   /* per carrier */
#define AIR_GROUP_BOMBERS 6
#define CRUISE_ALT 250.0f
#define AA_RANGE 1200.0f
#define REARM_S 40.0f

typedef enum { FX_NONE, FX_MUZZLE, FX_SPLASH, FX_HIT, FX_NEAR_MISS, FX_TORP_HIT, FX_DC_BLAST, FX_SINK,
               FX_FLAK, FX_AIR_GUN, FX_PLANE_DOWN, FX_RICOCHET, FX_MAGAZINE } fx_kind;

typedef enum { PLANE_FIGHTER, PLANE_BOMBER } plane_type;
typedef enum { AIR_HANGAR, AIR_OUT, AIR_ATTACK, AIR_RETURN, AIR_DECK, AIR_FALLING, AIR_LOST } air_state;
typedef enum { MISSION_NONE, MISSION_CAP, MISSION_ESCORT, MISSION_STRIKE } air_mission;

typedef struct {
    plane_type type;
    air_state state;
    air_mission mission;
    int carrier, side;
    int target;                /* strike: ship index; fighter: aircraft index */
    float x, y, z, heading, pitch, bank, speed, vy;
    float prev_x, prev_y, prev_z, prev_heading;
    float hp, max_hp, fuel, timer, gun_reload;
    int stage;                 /* 1..3 picks the AS01/02/03 model */
} aircraft;

typedef struct {
    fx_kind kind;
    float x, y, z;
    long tick;
} fx_event;

typedef struct {
    int active;
    ammo_kind ammo;
    int owner, side;
    float damage;
    float x0, y0, z0, vx, vy, vz;   /* launch point and velocity; position is analytic in t */
    long tick0;
    float flight;                  /* shell/DC: seconds to impact; torpedo: seconds to max range */
    float px, pz;                  /* torpedo: position at the previous tick */
    float pen;                     /* shell/bomb penetration (shell_pen) */
    int plunging;                  /* falls on the deck rather than the belt */
} projectile;

typedef enum { DC_AUTO, DC_FIRES, DC_FLOODING, DC_WEAPONS, DC_POLICIES } dc_policy;
typedef enum { HIT_SHELL, HIT_NEAR_MISS, HIT_TORPEDO, HIT_BOMB, HIT_DEPTH_CHARGE } hit_kind;

typedef struct {
    float structure;           /* 1 intact .. 0 wrecked */
    float flood[2];            /* water, fraction of the compartment's half: 0 port, 1 starboard */
    float leak[2];             /* inflow, fraction per second */
    float fire;                /* 0 none .. 1 raging */
    int crews;                 /* damage-control teams working here */
    int mag_flooded;           /* magazine deliberately flooded */
    int counter_flood;         /* side (0/1) being counter-flooded, -1 none */
} compartment;

typedef struct {
    float yaw;                 /* training angle relative to the bow, radians */
    float reload;              /* seconds until loaded */
    float retarget;            /* seconds until the next target search */
    int target;                /* ship index, -1 none */
} turret;

typedef struct {
    float x, z;
} waypoint;

typedef struct {
    const ship_class *cls;
    int side;                  /* 0 = player, 1 = enemy */
    int alive;
    float x, z, heading;       /* position and heading (radians) */
    float speed, rudder;       /* current speed (u/s), rudder -1..1 */
    float max_speed, accel, max_yaw;
    float hp;
    int stage;                 /* damage stage 1..3, picks the 01/02/03 model */
    float sink;                /* seconds since sunk (alive == 0) */
    turret turrets[MAX_MOUNTS];
    int order_target[BATTERIES];   /* ordered target per battery, -1 = choose freely */
    int hold_fire;
    int ai;                    /* steered by ai_step */
    float aa_reload;           /* seconds until the AA battery fires again */
    compartment comp[MAX_COMPARTMENTS];
    float module_hp[MAX_MODULES];  /* 1 intact, 0 destroyed */
    float list, trim;          /* radians: + list to starboard, + trim bow down */
    float water;               /* flooded fraction of the whole hull */
    dc_policy policy;
    float dc_timer;
    int jammed;                /* steering gear out: rudder stuck */
    int cause;                 /* why she went down: 0 afloat, 1 flooding, 2 capsized, 3 magazine, 4 structure */
    float launch_timer;        /* carrier: seconds until the next plane can take off */
    waypoint wp[MAX_WAYPOINTS];
    int wp_count;
    float prev_x, prev_z, prev_heading;  /* state at the previous tick, for interpolation */
} ship;

typedef struct {
    ship ships[MAX_SHIPS];
    int count;
    long tick;
    unsigned long long rng;
    projectile proj[MAX_PROJECTILES];
    fx_event fx[MAX_FX];       /* ring buffer of visual events, newest at fx_head - 1 */
    int fx_head;
    int shots, hits;           /* statistics: shells fired / shells that struck a hull */
    int torps, torp_hits, dc_hits;
    aircraft air[MAX_AIRCRAFT];
    int air_count;
    int bombs, bomb_hits, planes_lost_aa, planes_lost_fighters, planes_ditched;
    int penetrations, ricochets, fires_started, magazine_explosions;
} world;

void world_init(world *w);   /* seed 1 */
void world_seed(world *w, unsigned long long seed);
int world_spawn(world *w, const ship_class *cls, int side, float x, float z, float heading);
void world_step(world *w);
void combat_step(world *w);  /* fire control + projectiles, part of world_step */

/* Move the given ships to (x, z) keeping their formation, rotated to face the move.
 * append = queue after existing waypoints instead of replacing them. */
void order_move(world *w, const int *ids, int n, float x, float z, int append);
void order_stop(world *w, const int *ids, int n);
/* Focus the given batteries (bit mask of 1 << battery) on target; -1 = fire at will. */
void order_attack(world *w, const int *ids, int n, int target, unsigned mask);
void order_hold_fire(world *w, const int *ids, int n, int hold);

/* Original-inspired accuracy (FUN_088e94fc: range bands and hull-vs-hull counters,
 * without the aspect term, which the hull footprint now supplies). Percent. */
float hit_chance(int firer_hull, int target_hull, float dist);
/* Horizontal muzzle speed of a gun, world units per second. */
float shell_speed(const class_weapon *w);
/* 1 if mount m of ship i could engage ship j now: an afloat enemy in range, inside the
 * mount's arc, and of the right kind (only depth charges reach a submerged boat). */
int can_engage(const world *w, int i, int m, int j);
/* Side with ships still afloat if the other has none, else -1. */
int world_winner(const world *w);

/* Damage model (src/damage.c). damage_hit resolves one hit at world point (x, z):
 * pen = penetration (shell calibre and range, see shell_pen), side = 0 port / 1 starboard
 * of the hull that was struck. */
void damage_init(ship *s);
void damage_hit(world *w, int j, hit_kind kind, float x, float z, float dmg, float pen, int plunging);
void damage_step(world *w, int i);
float shell_pen(const class_weapon *wp, float dist);
float module_health(const ship *s, module_kind kind, int mount);   /* mean hp, 1 if none */
float condition(const ship *s);    /* 0..1, drives the health bar and the 01/02/03 models */
void order_dc_policy(world *w, const int *ids, int n, dc_policy p);
void order_flood_magazines(world *w, const int *ids, int n);
void order_counter_flood(world *w, const int *ids, int n);

/* Carrier orders: launch every ready bomber at target ship (fighters escort them
 * when ready), put ready fighters on patrol over the carrier, or recall everything. */
void order_strike(world *w, int carrier, int target);
void order_cap(world *w, int carrier);
void order_recall(world *w, int carrier);
int hangar_count(const world *w, int carrier, plane_type type);
int is_carrier(const ship *s);
void air_step(world *w);     /* part of world_step */
void air_spawn_group(world *w, int carrier);   /* world_spawn calls it for carriers */

/* Simple enemy commander for ships with ai set: close to gun range, turn broadside. */
void ai_step(world *w);

#endif
