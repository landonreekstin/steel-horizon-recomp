#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Wire format: u32 length (of type + payload), u8 type, payload. Structs go raw: all
 * peers run the same build on little-endian machines. */
enum { M_HELLO = 1, M_WELCOME, M_START, M_CMD, M_TURN, M_ACK, M_CHAT };
#define CHAT_LINE 96
#define CHAT_KEEP 16

#define RING 1024              /* turns tracked for checksum comparison */
#define LAG_LIMIT 8            /* host waits for a peer more than this many turns behind */
#define BUF 65536

typedef struct {
    int fd;
    unsigned char in[BUF];
    size_t in_len;
    long acked;                /* latest turn this peer finished */
    int catching_up;           /* rejoined: replaying history, not waited for */
} conn;

typedef struct {
    long turn;                 /* which turn's checksums are in this slot, -1 none */
    uint32_t sum[MAX_PLAYERS];
    unsigned acked_mask;
} turn_slot;

struct net {
    int host, listen_fd, port;
    int player, players, joined;
    conn peer[MAX_PLAYERS];    /* host: index = player; client: peer[0] = the host */
    char names[MAX_PLAYERS][24];
    int started;
    net_game game;
    command pending[NET_MAX_TURN_CMDS * 4];
    int pending_count;
    turn_slot ring[RING];
    /* Every turn's commands, from turn 0: a rejoining player replays them all. */
    command *hist;
    int *hist_off, *hist_len;
    long hist_turns, hist_cap;
    int hist_cmds, hist_cmd_cap;
    long desync;
    char chat[CHAT_KEEP][CHAT_LINE];
    int chat_head, chat_count;
};

static void chat_store(net *n, const char *line)
{
    int i = (n->chat_head + n->chat_count) % CHAT_KEEP;
    snprintf(n->chat[i], CHAT_LINE, "%s", line);
    if (n->chat_count < CHAT_KEEP) n->chat_count++;
    else n->chat_head = (n->chat_head + 1) % CHAT_KEEP;
}

static void nonblock(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

static void send_msg(int fd, int type, const void *data, uint32_t len)
{
    if (fd < 0) return;
    unsigned char hdr[5];
    uint32_t l = len + 1;
    memcpy(hdr, &l, 4);
    hdr[4] = (unsigned char)type;
    /* Small messages on a local or game link: a blocking write is fine; retry EAGAIN. */
    const unsigned char *parts[2] = {hdr, data};
    size_t sizes[2] = {5, len};
    for (int p = 0; p < 2; p++) {
        size_t off = 0;
        while (off < sizes[p]) {
            ssize_t r = send(fd, parts[p] + off, sizes[p] - off, MSG_NOSIGNAL);
            if (r > 0) off += (size_t)r;
            else if (r < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            else return;
        }
    }
}

static turn_slot *slot(net *n, long turn)
{
    return &n->ring[((turn % RING) + RING) % RING];
}

net *net_host(int port, int players, const char *name)
{
    if (players < 1 || players > MAX_PLAYERS) return NULL;
    net *n = calloc(1, sizeof *n);
    n->host = 1;
    n->players = players;
    n->player = 0;
    n->joined = 1;
    n->desync = -1;
    snprintf(n->names[0], sizeof n->names[0], "%s", name);
    for (int i = 0; i < MAX_PLAYERS; i++) n->peer[i].fd = -1;
    for (int i = 0; i < RING; i++) n->ring[i].turn = -1;
    n->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(n->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind(n->listen_fd, (struct sockaddr *)&a, sizeof a) || listen(n->listen_fd, MAX_PLAYERS)) {
        perror("net_host");
        close(n->listen_fd);
        free(n);
        return NULL;
    }
    socklen_t al = sizeof a;
    getsockname(n->listen_fd, (struct sockaddr *)&a, &al);
    n->port = ntohs(a.sin_port);
    nonblock(n->listen_fd);
    return n;
}

net *net_join(const char *host, int port, const char *name)
{
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res)) { fprintf(stderr, "net_join: can't resolve %s\n", host); return NULL; }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(fd, res->ai_addr, res->ai_addrlen)) {
        perror("net_join");
        freeaddrinfo(res);
        close(fd);
        return NULL;
    }
    freeaddrinfo(res);
    net *n = calloc(1, sizeof *n);
    n->player = -1;
    n->desync = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) n->peer[i].fd = -1;
    for (int i = 0; i < RING; i++) n->ring[i].turn = -1;
    n->peer[0].fd = fd;
    nonblock(fd);
    char nm[24] = {0};
    snprintf(nm, sizeof nm, "%s", name);
    send_msg(fd, M_HELLO, nm, sizeof nm);
    return n;
}

void net_close(net *n)
{
    if (!n) return;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (n->peer[i].fd >= 0) close(n->peer[i].fd);
    if (n->host) close(n->listen_fd);
    free(n->hist);
    free(n->hist_off);
    free(n->hist_len);
    free(n);
}

int net_port(const net *n) { return n->port; }
int net_is_host(const net *n) { return n->host; }
int net_player(const net *n) { return n->player; }
int net_lobby_full(const net *n) { return n->host && n->joined == n->players; }

/* Append turn `turn` to the history (turns arrive in order; the first NET_DELAY are
 * empty by design and filled in here). */
static void store_turn(net *n, long turn, const command *cmds, int count)
{
    if (turn < n->hist_turns) return;                  /* already have it */
    while (n->hist_turns <= turn) {
        if (n->hist_turns == n->hist_cap) {
            n->hist_cap = n->hist_cap ? n->hist_cap * 2 : 4096;
            n->hist_off = realloc(n->hist_off, sizeof(int) * (size_t)n->hist_cap);
            n->hist_len = realloc(n->hist_len, sizeof(int) * (size_t)n->hist_cap);
        }
        int c = n->hist_turns == turn ? (count > NET_MAX_TURN_CMDS ? NET_MAX_TURN_CMDS : count) : 0;
        if (n->hist_cmds + c > n->hist_cmd_cap) {
            n->hist_cmd_cap = (n->hist_cmds + c) * 2 + 256;
            n->hist = realloc(n->hist, sizeof(command) * (size_t)n->hist_cmd_cap);
        }
        n->hist_off[n->hist_turns] = n->hist_cmds;
        n->hist_len[n->hist_turns] = c;
        if (c) memcpy(n->hist + n->hist_cmds, cmds, sizeof(command) * (size_t)c);
        n->hist_cmds += c;
        n->hist_turns++;
    }
}

static void send_turn(net *n, int fd, long turn)
{
    unsigned char buf[10 + NET_MAX_TURN_CMDS * sizeof(command)];
    int64_t t64 = turn;
    uint16_t c16 = (uint16_t)n->hist_len[turn];
    memcpy(buf, &t64, 8);
    memcpy(buf + 8, &c16, 2);
    memcpy(buf + 10, n->hist + n->hist_off[turn], sizeof(command) * (size_t)c16);
    send_msg(fd, M_TURN, buf, 10 + (uint32_t)(c16 * sizeof(command)));
}

static void handle(net *n, int from, int type, const unsigned char *p, uint32_t len)
{
    switch (type) {
    case M_HELLO:                                      /* host: a player arrives */
        if (n->host && len >= 24) {
            memcpy(n->names[from], p, 24);
            n->names[from][23] = 0;
            unsigned char wl[2] = {(unsigned char)from, (unsigned char)n->players};
            send_msg(n->peer[from].fd, M_WELCOME, wl, 2);
            if (n->started) {                          /* rejoining: the game and its whole history */
                send_msg(n->peer[from].fd, M_START, &n->game, sizeof n->game);
                for (long t = 0; t < n->hist_turns; t++) send_turn(n, n->peer[from].fd, t);
                char line[CHAT_LINE];
                snprintf(line, sizeof line, "%.20s rejoined", n->names[from]);
                chat_store(n, line);
                for (int q = 1; q < n->players; q++) send_msg(n->peer[q].fd, M_CHAT, line, (uint32_t)strlen(line));
            }
        }
        break;
    case M_WELCOME:
        if (len >= 2) { n->player = p[0]; n->players = p[1]; }
        break;
    case M_START:
        if (len >= sizeof(net_game)) { memcpy(&n->game, p, sizeof(net_game)); n->started = 1; }
        break;
    case M_CMD:                                        /* host: a player's order */
        if (n->host && len >= sizeof(command) && n->pending_count < (int)(sizeof n->pending / sizeof n->pending[0])) {
            command c;
            memcpy(&c, p, sizeof c);
            c.player = (uint8_t)from;                  /* players speak only for themselves */
            n->pending[n->pending_count++] = c;
        }
        break;
    case M_TURN:
        if (len >= 10) {
            long turn;
            int64_t t64;
            uint16_t cnt;
            memcpy(&t64, p, 8);
            memcpy(&cnt, p + 8, 2);
            turn = (long)t64;
            if (len >= 10 + cnt * sizeof(command)) store_turn(n, turn, (const command *)(p + 10), cnt);
        }
        break;
    case M_ACK:                                        /* host: a peer finished a turn */
        if (n->host && len >= 12) {
            int64_t t64;
            uint32_t sum;
            memcpy(&t64, p, 8);
            memcpy(&sum, p + 8, 4);
            n->peer[from].acked = (long)t64;
            if (n->peer[from].catching_up && (long)t64 >= n->hist_turns - 1 - LAG_LIMIT / 2) n->peer[from].catching_up = 0;
            turn_slot *s = slot(n, (long)t64);
            if (s->turn != (long)t64 && (long)t64 > s->turn) { s->turn = (long)t64; s->acked_mask = 0; }
            if (s->turn == (long)t64) {
                s->sum[from] = sum;
                s->acked_mask |= 1u << from;
                if ((s->acked_mask & 1u) && s->sum[0] != sum && n->desync < 0) n->desync = (long)t64;
            }
        }
        break;
    case M_CHAT: {
        char line[CHAT_LINE];
        uint32_t l = len < CHAT_LINE - 1 ? len : CHAT_LINE - 1;
        memcpy(line, p, l);
        line[l] = 0;
        if (n->host) {                                 /* stamp the speaker, relay to all */
            char full[CHAT_LINE];
            snprintf(full, sizeof full, "%.20s: %.70s", n->names[from], line);
            chat_store(n, full);
            for (int q = 1; q < n->players; q++) send_msg(n->peer[q].fd, M_CHAT, full, (uint32_t)strlen(full));
        } else {
            chat_store(n, line);
        }
        break;
    }
    default: break;
    }
}

void net_chat(net *n, const char *text)
{
    if (n->host) {
        char full[CHAT_LINE];
        snprintf(full, sizeof full, "%.20s: %.70s", n->names[0], text);
        chat_store(n, full);
        for (int q = 1; q < n->players; q++) send_msg(n->peer[q].fd, M_CHAT, full, (uint32_t)strlen(full));
    } else {
        send_msg(n->peer[0].fd, M_CHAT, text, (uint32_t)strlen(text));
    }
}

int net_chat_take(net *n, char *out, int cap)
{
    if (!n->chat_count) return 0;
    snprintf(out, (size_t)cap, "%s", n->chat[n->chat_head]);
    n->chat_head = (n->chat_head + 1) % CHAT_KEEP;
    n->chat_count--;
    return 1;
}

static int read_conn(net *n, int idx)
{
    conn *c = &n->peer[idx];
    for (;;) {
        ssize_t r = recv(c->fd, c->in + c->in_len, BUF - c->in_len, 0);
        if (r > 0) c->in_len += (size_t)r;
        else if (r == 0) return -1;
        else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        else if (errno != EINTR) return -1;
        if (c->in_len == BUF) break;
    }
    size_t off = 0;
    while (c->in_len - off >= 5) {
        uint32_t l;
        memcpy(&l, c->in + off, 4);
        if (l == 0 || l > BUF - 4) return -1;
        if (c->in_len - off < 4 + l) break;
        handle(n, idx, c->in[off + 4], c->in + off + 5, l - 1);
        off += 4 + l;
    }
    memmove(c->in, c->in + off, c->in_len - off);
    c->in_len -= off;
    return 0;
}

int net_poll(net *n)
{
    if (n->host) {
        int fd;
        while ((fd = accept(n->listen_fd, NULL, NULL)) >= 0) {
            int p = -1;
            if (!n->started && n->joined < n->players) p = n->joined++;
            else if (n->started)                       /* a dropped player coming back */
                for (int q = 1; q < n->players && p < 0; q++)
                    if (n->peer[q].fd < 0) p = q;
            if (p < 0) { close(fd); continue; }
            nonblock(fd);
            n->peer[p].fd = fd;
            n->peer[p].in_len = 0;
            n->peer[p].acked = -1;
            n->peer[p].catching_up = n->started;
        }
        for (int p = 1; p < n->joined; p++)
            if (n->peer[p].fd >= 0 && read_conn(n, p)) {
                fprintf(stderr, "player %d (%s) disconnected\n", p, n->names[p]);
                close(n->peer[p].fd);
                n->peer[p].fd = -1;
                char line[CHAT_LINE];
                snprintf(line, sizeof line, "%.20s lost connection (can rejoin)", n->names[p]);
                chat_store(n, line);
                for (int q = 1; q < n->players; q++) send_msg(n->peer[q].fd, M_CHAT, line, (uint32_t)strlen(line));
            }
        return 0;
    }
    return n->peer[0].fd >= 0 && read_conn(n, 0) == 0 ? 0 : -1;
}

void net_set_custom(net *n, const custom_spec *c)
{
    n->game.custom = *c;
}

int net_joined(const net *n) { return n->joined; }
const char *net_name(const net *n, int p) { return p >= 0 && p < MAX_PLAYERS ? n->names[p] : ""; }

void net_start(net *n, const char *scenario, uint64_t seed, const int *teams)
{
    if (!n->host) return;
    net_game *g = &n->game;
    custom_spec keep = g->custom;
    memset(g, 0, sizeof *g);
    g->custom = keep;
    snprintf(g->scenario, sizeof g->scenario, "%s", scenario);
    g->seed = seed;
    g->players = n->players;
    for (int p = 0; p < n->players; p++) {
        g->team[p] = teams ? teams[p] : p % 2;
        memcpy(g->name[p], n->names[p], 24);
    }
    for (int p = 1; p < n->players; p++) send_msg(n->peer[p].fd, M_START, g, sizeof *g);
    n->started = 1;
}

int net_started(const net *n, net_game *out)
{
    if (n->started && out) *out = n->game;
    return n->started;
}

void net_send_command(net *n, const command *c)
{
    if (n->host) {
        if (n->pending_count < (int)(sizeof n->pending / sizeof n->pending[0])) {
            n->pending[n->pending_count] = *c;
            n->pending[n->pending_count++].player = (uint8_t)n->player;
        }
    } else {
        send_msg(n->peer[0].fd, M_CMD, c, sizeof *c);
    }
}

void net_make_turn(net *n, long turn)
{
    if (!n->host || turn < n->hist_turns) return;
    int cnt = n->pending_count > NET_MAX_TURN_CMDS ? NET_MAX_TURN_CMDS : n->pending_count;
    store_turn(n, turn, n->pending, cnt);
    memmove(n->pending, n->pending + cnt, sizeof(command) * (size_t)(n->pending_count - cnt));
    n->pending_count -= cnt;
    for (int p = 1; p < n->players; p++) send_turn(n, n->peer[p].fd, turn);
}

long net_latest_turn(const net *n) { return n->hist_turns - 1; }

int net_have_turn(const net *n, long turn)
{
    if (turn < NET_DELAY) return 1;                    /* the first turns are empty by design */
    if (n->host)                                       /* don't run away from slow peers */
        for (int p = 1; p < n->players; p++)
            if (n->peer[p].fd >= 0 && !n->peer[p].catching_up && n->peer[p].acked < turn - LAG_LIMIT) return 0;
    return turn < n->hist_turns;
}

int net_turn_commands(const net *n, long turn, command *out, int max)
{
    if (turn < NET_DELAY || turn >= n->hist_turns) return 0;
    int k = n->hist_len[turn] < max ? n->hist_len[turn] : max;
    memcpy(out, n->hist + n->hist_off[turn], sizeof(command) * (size_t)k);
    return k;
}

void net_done_turn(net *n, long turn, uint32_t checksum)
{
    if (n->host) {
        turn_slot *s = slot(n, turn);
        if (s->turn != turn) {                         /* first report for this turn */
            s->turn = turn;
            s->acked_mask = 0;
        }
        s->sum[0] = checksum;
        s->acked_mask |= 1u;
        n->peer[0].acked = turn;
        /* Compare against peers that have reported this turn. */
        for (int p = 1; p < n->players; p++)
            if ((s->acked_mask >> p) & 1 && s->sum[p] != checksum && n->desync < 0) n->desync = turn;
    } else {
        unsigned char b[12];
        int64_t t64 = turn;
        memcpy(b, &t64, 8);
        memcpy(b + 8, &checksum, 4);
        send_msg(n->peer[0].fd, M_ACK, b, 12);
    }
}

long net_desync(const net *n) { return n->desync; }

uint32_t world_checksum(const world *w)
{
    uint32_t h = 2166136261u;
#define MIX(v) do { uint32_t x_; memcpy(&x_, &(v), 4); h = (h ^ x_) * 16777619u; } while (0)
    for (int i = 0; i < w->count; i++) {
        const ship *s = &w->ships[i];
        MIX(s->x);
        MIX(s->z);
        MIX(s->heading);
        MIX(s->hp);
        MIX(s->speed);
    }
    for (int k = 0; k < w->air_count; k++) { MIX(w->air[k].x); MIX(w->air[k].z); }
    uint32_t c = (uint32_t)w->count, t = (uint32_t)w->tick;
    MIX(c);
    MIX(t);
#undef MIX
    return h;
}
