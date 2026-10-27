#include "audio.h"

#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define VOICES 48

typedef struct {
    const int16_t *pcm;
    int len;
    uint32_t pos, step;         /* 16.16 fixed point: the bank's rate over the output's */
    float gl, gr;
} voice;

struct audio {
    SDL_AudioDeviceID dev;
    sound_bank bank;
    voice v[VOICES];
    long last_tick;             /* events up to this tick have been played */
    int played;
    unsigned rng;
};

static void mix(void *ud, Uint8 *stream, int bytes)
{
    audio *a = ud;
    int16_t *out = (int16_t *)stream;
    int frames = bytes / 4;
    for (int f = 0; f < frames; f++) {
        float l = 0, r = 0;
        for (int k = 0; k < VOICES; k++) {
            voice *vc = &a->v[k];
            if (!vc->pcm) continue;
            int i = (int)(vc->pos >> 16);
            float fr = (float)(vc->pos & 0xffff) / 65536.0f;
            float s = vc->pcm[i] + (i + 1 < vc->len ? (vc->pcm[i + 1] - vc->pcm[i]) * fr : 0);   /* linear */
            l += s * vc->gl;
            r += s * vc->gr;
            vc->pos += vc->step;
            if ((int)(vc->pos >> 16) >= vc->len) vc->pcm = NULL;
        }
        /* Soft limit so a broadside doesn't clip harshly. */
        l = 32767 * tanhf(l / 32767);
        r = 32767 * tanhf(r / 32767);
        out[2 * f] = (int16_t)l;
        out[2 * f + 1] = (int16_t)r;
    }
}

audio *audio_open(const char *bank_path)
{
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) { fprintf(stderr, "audio: %s (silent)\n", SDL_GetError()); return NULL; }
    audio *a = calloc(1, sizeof *a);
    if (bnk_load(bank_path, &a->bank)) {
        printf("audio: no sound bank at %s, using synthesised effects\n", bank_path);
        synth_bank(&a->bank);
    }
    SDL_AudioSpec want = {0}, have;
    want.freq = BANK_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = mix;
    want.userdata = a;
    a->dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!a->dev) {
        fprintf(stderr, "audio: %s (silent)\n", SDL_GetError());
        bnk_free(&a->bank);
        free(a);
        return NULL;
    }
    a->rng = 12345;
    a->last_tick = -1;
    SDL_PauseAudioDevice(a->dev, 0);
    printf("audio: %d sounds\n", a->bank.count);
    return a;
}

void audio_close(audio *a)
{
    if (!a) return;
    printf("audio: %d sounds played\n", a->played);
    SDL_CloseAudioDevice(a->dev);
    bnk_free(&a->bank);
    free(a);
}

static void play_index(audio *a, int i, float gain, float pan)
{
    if (!a || i < 0 || gain < 0.01f) return;
    SDL_LockAudioDevice(a->dev);
    int slot = -1, oldest = -1, most = -1;
    for (int k = 0; k < VOICES; k++) {
        if (!a->v[k].pcm) { slot = k; break; }
        if ((int)(a->v[k].pos >> 16) > most) { most = (int)(a->v[k].pos >> 16); oldest = k; }
    }
    if (slot < 0) slot = oldest;                   /* steal the voice nearest its end */
    voice *vc = &a->v[slot];
    a->played++;
    vc->pcm = a->bank.pcm[i];
    vc->len = a->bank.length[i];
    vc->pos = 0;
    vc->step = (uint32_t)((double)a->bank.rate / BANK_RATE * 65536.0 + 0.5);
    pan = clampf(pan, -1, 1);
    vc->gl = gain * sqrtf(0.5f * (1 - pan));
    vc->gr = gain * sqrtf(0.5f * (1 + pan));
    SDL_UnlockAudioDevice(a->dev);
}

void audio_play(audio *a, const char *name, float gain, float pan)
{
    if (a) play_index(a, bnk_find(&a->bank, name), gain, pan);
}

/* "BASE1_WAV" .. "BASEn_WAV" style variants: pick one; names may be cut at 15 chars. */
static int variant(audio *a, const char *base, int n)
{
    a->rng = a->rng * 1103515245u + 12345u;
    int pick = (int)((a->rng >> 16) % (unsigned)n) + 1;
    char name[40];
    for (int tries = 0; tries < n; tries++, pick = pick % n + 1) {
        snprintf(name, sizeof name, base, pick);
        name[15] = 0;                              /* the bank stores 15-character names */
        int i = bnk_find(&a->bank, name);
        if (i >= 0) return i;
    }
    return -1;
}

static const char *calibre(float dmg)
{
    return dmg >= 85 ? "ENORMCANN" : dmg >= 55 ? "LRGCANN" : dmg >= 30 ? "MEDCANN" : "SMLCANN";
}

void audio_events(audio *a, const world *w, vec3 eye, vec3 fwd)
{
    if (!a) return;
    vec3 right = v3norm(v3cross(fwd, v3(0, 1, 0)));
    long newest = a->last_tick;
    for (int k = 0; k < MAX_FX; k++) {
        const fx_event *e = &w->fx[k];
        if (e->kind == FX_NONE || e->tick <= a->last_tick) continue;
        if (e->tick > newest) newest = e->tick;
        vec3 d = v3sub(v3(e->x, e->y, e->z), eye);
        float dist = v3len(d);
        float gain = 1.6f / (1 + dist / 1200);
        float pan = dist > 1 ? v3dot(d, right) / dist : 0;
        char base[40];
        int i = -1;
        switch (e->kind) {
        case FX_MUZZLE: snprintf(base, sizeof base, "%sSHT%%d_WAV", calibre(e->mag)); i = variant(a, base, 3); gain *= 0.8f; break;
        case FX_HIT: snprintf(base, sizeof base, "%sHT%%d_WAV", calibre(e->mag)); i = variant(a, base, 3); break;
        case FX_SPLASH: case FX_NEAR_MISS:
            snprintf(base, sizeof base, "%sMISS%%d_WAV", calibre(e->mag)); i = variant(a, base, 3); gain *= 0.7f; break;
        case FX_RICOCHET: i = variant(a, "SMLCANNHT%d_WAV", 3); gain *= 0.5f; break;
        case FX_TORP_LAUNCH: i = variant(a, "TORPSHT%d_WAV", 3); break;
        case FX_TORP_HIT: i = variant(a, "TORPHT%d_WAV", 2); gain *= 1.3f; break;
        case FX_DC_LAUNCH: i = variant(a, "DPTHCHRGSHT%d_WAV", 2); break;
        case FX_DC_BLAST: i = variant(a, "DPTHCHRGEXPL%d_WAV", 2); break;
        case FX_FLAK: i = variant(a, "AAGUNEXPL%d_WAV", 2); gain *= 0.5f; break;
        case FX_AIR_GUN: i = variant(a, "_50CALSHT%d_WAV", 2); gain *= 0.4f; break;
        case FX_PLANE_DOWN: i = variant(a, "CRSHWTR%d_WAV", 2); break;
        case FX_SINK: i = bnk_find(&a->bank, "SINKINGLRG2_WAV"); gain *= 1.4f; break;
        case FX_MAGAZINE: i = variant(a, "ENORMCANNHT%d_WA", 3); gain *= 2.0f; break;
        case FX_TAKEOFF: i = bnk_find(&a->bank, "TAKEOFFALS2_WAV"); gain *= 0.6f; break;
        case FX_LANDING: i = bnk_find(&a->bank, "LANDALS_WAV"); gain *= 0.5f; break;
        default: break;
        }
        if (dist < 9000) play_index(a, i, gain, pan);
        if (e->kind == FX_MAGAZINE) play_index(a, bnk_find(&a->bank, "SINKINGLRG2_WAV"), gain, pan);
    }
    a->last_tick = newest;
}

void audio_ack(audio *a, const ship *s)
{
    if (!a) return;
    const char *nat = !strcmp(s->cls->nation, "American") ? "USA" : !strcmp(s->cls->nation, "British") ? "BRIT"
                    : !strcmp(s->cls->nation, "German") ? "GERM" : !strcmp(s->cls->nation, "Japanese") ? "JAPAN" : "SHAD";
    char code[8];
    snprintf(code, sizeof code, "%s", s->cls->code);
    if (!strcmp(code, "PT")) snprintf(code, sizeof code, "MPT");
    if (code[0] && code[2] == 'F') code[2] = 0;                 /* flagship classes: BBF -> BB */
    char base[40];
    snprintf(base, sizeof base, "%s_%s_%%02d", code, nat);
    play_index(a, variant(a, base, 6), 1.0f, 0);
}
