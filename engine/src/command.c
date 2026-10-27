#include "command.h"

#include <string.h>

void cmd_set_ship(command *c, int i)
{
    if (i >= 0 && i < MAX_SHIPS) c->ships[i / 64] |= 1ull << (i % 64);
}

int cmd_has_ship(const command *c, int i)
{
    return i >= 0 && i < MAX_SHIPS && (c->ships[i / 64] >> (i % 64)) & 1;
}

int cmd_ships(const world *w, const command *c, int *ids)
{
    int n = 0;
    for (int i = 0; i < w->count; i++)
        if (cmd_has_ship(c, i) && w->ships[i].alive && w->ships[i].owner == c->player) ids[n++] = i;
    return n;
}

static int player_side(const world *w, int player)
{
    /* The side of any ship the player owns (players own at least their yard or a ship). */
    for (int i = 0; i < w->count; i++)
        if (w->ships[i].owner == player) return w->ships[i].side;
    return -1;
}

void world_apply(world *w, const command *c)
{
    int ids[MAX_SHIPS], n = cmd_ships(w, c, ids);
    switch ((cmd_kind)c->kind) {
    case CMD_MOVE: order_move(w, ids, n, c->x, c->z, c->flag); break;
    case CMD_STOP: order_stop(w, ids, n); break;
    case CMD_ATTACK:
        if (c->a >= 0 && (c->a >= w->count || w->ships[c->a].side == player_side(w, c->player))) break;
        order_attack(w, ids, n, c->a, (unsigned)c->b);
        if (c->a >= 0) order_hold_fire(w, ids, n, 0);
        break;
    case CMD_HOLD_FIRE: order_hold_fire(w, ids, n, c->flag); break;
    case CMD_AIM: order_aim(w, ids, n, c->a, (unsigned)c->b); break;
    case CMD_EVADE: order_evade(w, ids, n, c->x); break;
    case CMD_DC_POLICY: if (c->a >= 0 && c->a < DC_POLICIES) order_dc_policy(w, ids, n, (dc_policy)c->a); break;
    case CMD_FLOOD_MAGAZINES: order_flood_magazines(w, ids, n); break;
    case CMD_COUNTER_FLOOD: order_counter_flood(w, ids, n); break;
    case CMD_STRIKE:
        for (int k = 0; k < n; k++)
            if (is_carrier(&w->ships[ids[k]]) && c->a >= 0 && c->a < w->count)   /* b: plane-type mask, 0 = all */
                order_strike_types(w, ids[k], c->a, c->b ? (unsigned)c->b : (1u << PLANE_BOMBER) | (1u << PLANE_TORPEDO));
        break;
    case CMD_CAP: for (int k = 0; k < n; k++) if (is_carrier(&w->ships[ids[k]])) order_cap(w, ids[k]); break;
    case CMD_RECALL: for (int k = 0; k < n; k++) if (is_carrier(&w->ships[ids[k]])) order_recall(w, ids[k]); break;
    case CMD_BUILD: {
        int side = player_side(w, c->player);
        if (side < 0 || c->b < 0 || c->b >= w->sides[side].option_count) break;
        int y = w->sides[side].yard;
        if (y < 0 || w->ships[y].owner != c->player) break;    /* the yard's owner builds */
        order_build(w, side, w->sides[side].options[c->b]);
        break;
    }
    case CMD_CAPTAIN_ACT: case CMD_CAPTAIN_DISMISS: {
        if (c->a < 0 || c->a >= w->count || w->ships[c->a].owner != c->player || c->b < 0 || c->b >= REP_KINDS) break;
        captain_report r = {c->a, (report_kind)c->b, 0};
        if (c->kind == CMD_CAPTAIN_ACT) captain_act(w, &r);
        else captain_dismiss(w, &r);
        break;
    }
    case CMD_FLAGSHIP:
        if (n == 1) order_flagship(w, c->player, ids[0]);
        break;
    case CMD_OFFICER:
        if (n == 1) order_officer(w, c->player, c->a, ids[0]);
        break;
    case CMD_RECON:                                  /* the first ship that can, toward (x, z) */
        for (int k = 0; k < n; k++)
            if (!order_recon(w, ids[k], c->x, c->z)) break;
        break;
    case CMD_DC_PRIORITY:
        if (n == 1) order_dc_priority(w, ids[0], c->a);
        break;
    default: break;
    }
}

int cmdq_push(command_queue *q, const command *c, long tick)
{
    if (q->count == CMD_QUEUE) return -1;
    int i = (q->head + q->count++) % CMD_QUEUE;
    q->items[i] = *c;
    q->tick[i] = tick;
    return 0;
}

void cmdq_apply_due(command_queue *q, world *w)
{
    while (q->count && q->tick[q->head] <= w->tick) {
        world_apply(w, &q->items[q->head]);
        q->head = (q->head + 1) % CMD_QUEUE;
        q->count--;
    }
}
