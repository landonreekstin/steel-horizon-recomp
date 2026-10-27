/* Front end: main menu, skirmish, custom-battle fleet picker, online host/join, lobby. */
#ifndef MENU_H
#define MENU_H

#include <SDL.h>

#include "data.h"
#include "net.h"
#include "render.h"

typedef enum { MENU_QUIT, MENU_PLAY, MENU_HOST, MENU_JOIN } menu_result;

typedef struct {
    char scenario[32];         /* a scenario name, or "custom" for the fleet picker's */
    custom_spec custom;
    char name[24], address[64];
    int port, players, teams[MAX_PLAYERS];
} menu_config;

void menu_defaults(menu_config *c);
menu_result menu_run(SDL_Window *win, renderer *r, const class_table *ct, menu_config *cfg);
/* Online lobby until the game starts (0) or the player leaves / the link drops (-1). */
int menu_lobby(SDL_Window *win, renderer *r, net *n, const menu_config *cfg, uint64_t seed, net_game *out);

#endif
