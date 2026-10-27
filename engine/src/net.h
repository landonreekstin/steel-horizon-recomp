/* Online multiplayer: deterministic lockstep over TCP (POSIX sockets).
 *
 * Star layout: players connect to a host. Everyone sends their commands to the host;
 * every NET_TURN ticks the host bundles what it has into a numbered turn, scheduled
 * NET_DELAY turns ahead (hiding latency), and sends it to all. Each peer runs the same
 * simulation and applies turn k's commands at tick k * NET_TURN; a peer that doesn't
 * have the next turn yet waits. Peers report a state checksum per turn; the host
 * compares them and flags a desync. */
#ifndef NET_H
#define NET_H

#include <stdint.h>

#include "command.h"

#define NET_TURN 4             /* ticks per turn (200 ms) */
#define NET_DELAY 2            /* turns of command latency */
#define NET_MAX_TURN_CMDS 64

typedef struct net net;

typedef struct {
    char scenario[32];
    uint64_t seed;
    int players;
    int team[MAX_PLAYERS];
    char name[MAX_PLAYERS][24];
} net_game;

net *net_host(int port, int players, const char *name);      /* NULL on error */
net *net_join(const char *host, int port, const char *name);
void net_close(net *n);
int net_port(const net *n);           /* the host's bound port (useful with port 0) */
int net_is_host(const net *n);
int net_player(const net *n);         /* our player number, -1 until welcomed */

/* Socket I/O; call often. Returns -1 if the connection is lost. */
int net_poll(net *n);
/* Host: 1 once every player has joined. */
int net_lobby_full(const net *n);
/* Host: announce the game (teams assigned 0,1,0,1... unless set) and start. */
void net_start(net *n, const char *scenario, uint64_t seed, const int *teams);
/* 1 once the game has started; fills the settings. */
int net_started(const net *n, net_game *out);

void net_send_command(net *n, const command *c);
/* Host: close turn k with the commands received so far, and send it. */
void net_make_turn(net *n, long turn);
int net_have_turn(const net *n, long turn);
int net_turn_commands(const net *n, long turn, command *out, int max);
/* After applying turn k: report our state checksum (and free the turn's slot). */
void net_done_turn(net *n, long turn, uint32_t checksum);
/* Host: turn where peers' checksums first differed, or -1. */
long net_desync(const net *n);

uint32_t world_checksum(const world *w);

#endif
