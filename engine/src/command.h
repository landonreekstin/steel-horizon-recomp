/* Player commands: every order a player gives goes through here, as a fixed-size,
 * serializable record applied at a tick boundary. Local play, network lockstep and
 * replays all feed the same stream, so the simulation stays deterministic. */
#ifndef COMMAND_H
#define COMMAND_H

#include <stdint.h>

#include "sim.h"

#define MAX_PLAYERS 8
#define SHIP_MASK_WORDS ((MAX_SHIPS + 63) / 64)

typedef enum {
    CMD_NONE, CMD_MOVE, CMD_STOP, CMD_ATTACK, CMD_HOLD_FIRE, CMD_AIM, CMD_EVADE, CMD_DC_POLICY,
    CMD_FLOOD_MAGAZINES, CMD_COUNTER_FLOOD, CMD_STRIKE, CMD_CAP, CMD_RECALL, CMD_BUILD,
    CMD_CAPTAIN_ACT, CMD_CAPTAIN_DISMISS, CMD_FLAGSHIP, CMD_OFFICER, CMD_KINDS
} cmd_kind;

typedef struct {
    uint8_t kind, player, flag, pad;   /* flag: append (move), hold (hold fire), ... */
    int32_t a, b;                      /* kind-specific: target ship, battery mask, policy, ... */
    float x, z;                        /* move point, evade seconds */
    uint64_t ships[SHIP_MASK_WORDS];   /* the ships the order is for */
} command;

/* Who commands what: each player is on a side (team) and owns a fleet. */
typedef struct {
    int count;
    int side[MAX_PLAYERS];
    char name[MAX_PLAYERS][24];
} player_table;

void cmd_set_ship(command *c, int i);
int cmd_has_ship(const command *c, int i);
/* Builds the id list of the command's ships that `player` may order (own, afloat). */
int cmd_ships(const world *w, const command *c, int *ids);
/* Applies one command. Orders for ships the player doesn't own are ignored. */
void world_apply(world *w, const command *c);

/* A small FIFO of commands waiting for their tick. */
#define CMD_QUEUE 256
typedef struct {
    command items[CMD_QUEUE];
    long tick[CMD_QUEUE];
    int head, count;
} command_queue;

int cmdq_push(command_queue *q, const command *c, long tick);
/* Applies every command due at or before the world's current tick, in order. */
void cmdq_apply_due(command_queue *q, world *w);

#endif
