/* Deterministic battle simulation: fixed 20 Hz tick, no rendering or input.
 * World units are the original game's (models are drawn 1:1, see notes/runtime.md).
 * Heading 0 faces +Z; positive heading turns toward +X. */
#ifndef SIM_H
#define SIM_H

#include <stdint.h>

#include "data.h"

#define SIM_HZ 20
#define SIM_DT (1.0f / SIM_HZ)
#define MAX_SHIPS 160       /* shipyards build mid-battle; wrecks keep their slots */
#define MAX_WAYPOINTS 16
#define MAX_PROJECTILES 1024
#define MAX_FX 512
#define MAX_AIRCRAFT 256
#define REP_KINDS_MAX 12       /* >= REP_KINDS (report_kind is declared after ship) */
#define MAX_BUILD_OPTIONS 8
#define MAX_OFFICERS 8         /* per team */
#define FLEET_PLAYERS 8        /* = MAX_PLAYERS in command.h */

/* Distinguished officers: each boosts one aspect of the ship he serves on. */
typedef enum { OFF_GUNNERY, OFF_FIRE_CONTROL, OFF_DAMAGE_CONTROL, OFF_ENGINEERING, OFF_AA,
               OFF_NAVIGATION, OFF_AVIATION, OFFICER_KINDS } officer_kind;
#define MAX_QUEUE 6

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
#define AIR_GROUP_DIVE 4       /* dive bombers */
#define AIR_GROUP_TORPEDO 4    /* torpedo bombers */
#define AIR_GROUP_BOMBERS (AIR_GROUP_DIVE + AIR_GROUP_TORPEDO)
#define CRUISE_ALT 250.0f
#define AA_RANGE 1200.0f
#define REARM_S 40.0f

typedef enum { FX_NONE, FX_MUZZLE, FX_SPLASH, FX_HIT, FX_NEAR_MISS, FX_TORP_HIT, FX_DC_BLAST, FX_SINK,
               FX_FLAK, FX_AIR_GUN, FX_PLANE_DOWN, FX_RICOCHET, FX_MAGAZINE, FX_TORP_LAUNCH, FX_DC_LAUNCH,
               FX_TAKEOFF, FX_LANDING } fx_kind;

typedef enum { PLANE_FIGHTER, PLANE_BOMBER /* dive bomber */, PLANE_TORPEDO, PLANE_RECON } plane_type;
typedef enum { AIR_HANGAR, AIR_OUT, AIR_ATTACK, AIR_RETURN, AIR_DECK, AIR_FALLING, AIR_LOST } air_state;
typedef enum { MISSION_NONE, MISSION_CAP, MISSION_ESCORT, MISSION_STRIKE, MISSION_RECON } air_mission;

/* Fog of war (src/vision.c, design). */
#define VIS_RANGE 4500.0f      /* lookouts: masthead sighting of a ship */
#define RADAR_RANGE 6500.0f    /* with working radar */
#define SUB_DETECT 900.0f      /* a submerged boat: sonar range */
#define AIR_VIS 3500.0f        /* what a plane can see around it */
#define FLASH_RANGE 9000.0f    /* gun flashes seen this far */
#define REVEAL_S 10.0f
#define RECON_COOLDOWN 180.0f
#define SHIP_WORDS ((MAX_SHIPS + 63) / 64)
#define MAX_ISLANDS 24

/* Terrain: round islands (src/islands.c). */
typedef struct {
    float x, z, radius, height;
    unsigned seed;             /* shapes the generated hill */
} island;

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
    float recon_x, recon_z;    /* MISSION_RECON: the point to circle */
} aircraft;

typedef struct {
    fx_kind kind;
    float x, y, z;
    float mag;                 /* weapon damage for shots and impacts (picks the sound); 0 else */
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
    int priority;              /* the player sent damage control here first */
} compartment;

typedef struct {
    float yaw;                 /* training angle relative to the bow, radians */
    float reload;              /* seconds until loaded */
    float retarget;            /* seconds until the next target search */
    int target;                /* ship index, -1 none */
    int ammo;                  /* rounds (torpedo tubes: spreads) left in the magazine */
} turret;

typedef struct {
    float x, z;
} waypoint;

typedef struct {
    const ship_class *cls;
    int side;                  /* team: 0 or 1 */
    int owner;                 /* player commanding her (command.h); -1 none (AI) */
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
    unsigned char module_lost[MAX_MODULES];   /* knocked past the critical point: no repairs */
    float crew;                /* aboard and fit for duty */
    int aim[BATTERIES];        /* macro target per battery: a module_kind, or -1 the ship */
    float evade;               /* seconds of evasive manoeuvring left */
    float evade_heading;       /* course being steered while evading */
    float evade_base;          /* the course she was on when told to evade */
    float report_cool[REP_KINDS_MAX];
    float reveal;              /* seconds her gun flashes still give her away */
    float recon_cool;          /* seconds until her floatplane / scout can go again */  /* per report_kind: seconds before the captain raises it again */
    float list, trim;          /* radians: + list to starboard, + trim bow down */
    float water;               /* flooded fraction of the whole hull */
    dc_policy policy;
    float dc_timer;
    int jammed;                /* steering gear out: rudder stuck */
    int returning;             /* AI: heading home to the shipyard for repairs */
    int committed;             /* AI: part of an attack wave (else mustering at the rally point) */
    int flagship;              /* her owner's flagship */
    unsigned char officers[OFFICER_KINDS];   /* distinguished officers aboard, by speciality */
    int cause;                 /* why she went down: 0 afloat, 1 flooding, 2 capsized, 3 magazine, 4 structure, 5 abandoned */
    float launch_timer;        /* carrier: seconds until the next plane can take off */
    waypoint wp[MAX_WAYPOINTS];
    int wp_count;
    float prev_x, prev_z, prev_heading;  /* state at the previous tick, for interpolation */
} ship;

typedef enum { MODE_SKIRMISH, MODE_ASSAULT, MODE_DEFEND } game_mode;

/* Per side: its shipyard, build budget and production queue (src/base.c). */
typedef struct {
    int yard;                  /* ship index of the side's shipyard, -1 none */
    float budget, income;      /* build points, and points per second while the yard stands */
    const ship_class *options[MAX_BUILD_OPTIONS];
    int option_count;
    const ship_class *queue[MAX_QUEUE];
    int queue_count;
    float progress;            /* seconds of work done on queue[0] */
    int built;
    /* A side without a yard (the attacker in MODE_DEFEND) gets reinforcement waves of
     * its build options every reinforce_every seconds, arriving at (entry_x, entry_z). */
    float reinforce_every, reinforce_timer, entry_x, entry_z;
    int waves;                 /* AI attack waves launched; each one musters bigger */
} side_state;

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
    int penetrations, ricochets, fires_started, magazine_explosions, upperworks;
    int air_torps;             /* aerial torpedoes dropped */
    game_mode mode;
    side_state sides[2];
    uint64_t seen[2][SHIP_WORDS];        /* which ships each side has in sight */
    float last_x[2][MAX_SHIPS], last_z[2][MAX_SHIPS];   /* where each side last saw them */
    long last_seen[2][MAX_SHIPS];        /* tick, -1 never */
    int fog;                             /* 1: fog of war on */
    island isles[MAX_ISLANDS];
    int isle_count;
    /* Each team's officers: speciality and the ship they serve on (-1 ashore, -2 lost). */
    int officer_count[2];
    officer_kind officer_kind_of[2][MAX_OFFICERS];
    int officer_ship[2][MAX_OFFICERS];
    int flagship_of[FLEET_PLAYERS];      /* ship index, -1 none */
    float shaken[FLEET_PLAYERS];         /* seconds the fleet is rattled after losing her */
    int defender;              /* MODE_DEFEND: the side holding the shipyard */
    float time_limit;          /* MODE_DEFEND: seconds the defender must hold; MODE_ASSAULT:
                                * after this, the side with more points wins (0: no limit) */
    float score[2];            /* points: the build cost of enemy ships sunk */
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
void order_dc_priority(world *w, int ship, int comp);   /* toggle "teams here first" */
int ammo_capacity(const ship_class *c, int mount);      /* full magazine for a mount */
/* Fire at a part of the target (MOD_ENGINE, MOD_MAGAZINE, MOD_STEERING, MOD_BRIDGE,
 * MOD_TURRET) instead of the ship as a whole; -1 for the whole ship. */
void order_aim(world *w, const int *ids, int n, int kind, unsigned mask);
/* Evasive manoeuvres for `seconds`: comb torpedoes heading her way, else zig-zag. */
void order_evade(world *w, const int *ids, int n, float seconds);
/* Expected chance that one shell from mount m of ship i hits ship j now (0..1):
 * gun, range, both ships' speed and turning, fire control and crew. */
float hit_probability(const world *w, int i, int m, int j);
float crew_factor(const ship *s);  /* crew aboard / complement */
/* 1 if an enemy torpedo will pass close to ship i soon; sets the combing course. */
int torpedo_threat(const world *w, int i, float *comb_heading);

/* Captain's reports: the recommended immediate action for each ship of a side. */
typedef enum { REP_FLOOD_MAGAZINE, REP_COUNTER_FLOOD, REP_EVADE_TORPEDO, REP_FIGHT_FLOODING,
               REP_FIGHT_FIRES, REP_WITHDRAW, REP_LAUNCH_STRIKE,
               REP_BRIDGE_LOST,  /* not an action: her captain can't report any more */
               REP_RETURN_TO_YARD,
               REP_KINDS } report_kind;
typedef struct {
    int ship;
    report_kind kind;
    float urgency;             /* higher first */
} captain_report;
int captain_reports(const world *w, int side, captain_report *out, int max);
void captain_act(world *w, const captain_report *r);
void captain_dismiss(world *w, const captain_report *r);   /* quiet for a minute */
/* Flood one compartment's magazine (the captain's choice), or all of them. */
void order_flood_magazine(world *w, int ship, int comp);

/* Carrier orders: launch every ready bomber at target ship (fighters escort them
 * when ready), put ready fighters on patrol over the carrier, or recall everything. */
void order_strike(world *w, int carrier, int target);
void order_cap(world *w, int carrier);
void order_recall(world *w, int carrier);
int hangar_count(const world *w, int carrier, plane_type type);
int strike_ready(const world *w, int carrier);   /* dive + torpedo bombers ready */
/* A strike with only some types: mask of 1 << PLANE_BOMBER / 1 << PLANE_TORPEDO. */
void order_strike_types(world *w, int carrier, int target, unsigned types);
int is_strike_plane(plane_type t);
int is_carrier(const ship *s);
void air_step(world *w);     /* part of world_step */
void air_spawn_group(world *w, int carrier);   /* world_spawn calls it for carriers */

/* Shipyards and game modes (src/base.c). */
int is_shore(const ship *s);
int shipyard_spawn(world *w, const ship_class *yard, int side, float x, float z, float heading);
int order_build(world *w, int side, const ship_class *c);   /* 0 queued, -1 can't */
void base_step(world *w);    /* part of world_step: income, production, repairs, land */
float repair_radius(const ship *yard);
int at_yard(const world *w, int i);          /* moored in reach of her side's working drydock */
float mode_time_left(const world *w);        /* MODE_DEFEND, seconds; else 0 */

/* Fog of war (src/vision.c). Ships of one's own side, shore installations, and
 * everything with fog off are always visible. */
int visible_to(const world *w, int side, int j);
void vision_step(world *w);  /* part of world_step */
/* Launch a recon plane (a floatplane off a cruiser or battleship, a scout off a
 * carrier) to circle (x, z). Returns 0 if one went. */
int order_recon(world *w, int ship, float x, float z);
int can_recon(const ship *s);

/* Islands (src/islands.c): a seeded archipelago between two lines, collisions,
 * line of sight, and detours for move orders. */
void islands_generate(world *w, unsigned seed, int count, float x0, float z0, float x1, float z1);
int island_blocks(const world *w, float ax, float az, float bx, float bz);   /* sight / torpedo path */
int island_at(const world *w, float x, float z, float margin);              /* index or -1 */
void islands_step(world *w);         /* part of world_step: keep ships off the land */
/* Waypoints from (x0, z0) to (x1, z1) that go round islands; returns count (<= max). */
int island_route(const world *w, float x0, float z0, float x1, float z1, waypoint *out, int max);

/* Fleet command (src/fleet.c). */
void order_flagship(world *w, int player, int ship);
void order_officer(world *w, int player, int officer, int ship);   /* ship -1: ashore */
int officer_add(world *w, int side, officer_kind kind);            /* returns its index */
/* Strength of a speciality aboard: 0 none, 1 one officer, 1.5 two, 1.75 three ... */
float officer_level(const ship *s, officer_kind k);
/* Multiplier on gunnery spread from flagship status, her nearby presence and a lost
 * flagship (< 1 is better). */
float command_accuracy(const world *w, int i);
void fleet_step(world *w);   /* part of world_step */

/* Simple enemy commander for ships with ai set: close to gun range, turn broadside. */
void ai_step(world *w);

#endif
