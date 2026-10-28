#include "audio.h"

#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "json.h"

#define VOICES 48
#define MUSIC_GAIN 0.32f
#define MAX_PLAYLIST 12

/* One decoded music track (tools/at3wav.py): stereo S16 at BANK_RATE, whole in memory. */
typedef struct {
    int16_t *pcm;
    long frames, loop0, loop1;
} track;

typedef struct {
    const int16_t *pcm;
    int len;
    uint32_t pos, step;         /* 16.16 fixed point: the bank's rate over the output's */
    float gl, gr;
    int cut, fade;              /* from sample `cut` fade out over `fade` samples (cut = len: none) */
} voice;

struct audio {
    SDL_AudioDeviceID dev;
    sound_bank bank;
    voice v[VOICES];
    long last_tick;             /* events up to this tick have been played */
    /* Music: the playing track (mixed in the callback) and the playlist the main thread
     * moves through; when a track has looped once the next one is loaded and swapped in. */
    track music;
    long music_pos;
    int music_looped;
    float duck;                 /* smoothed effects level: the music ducks under it */
    char music_dir[512];
    char playlist[MAX_PLAYLIST][32];
    int playlist_len, playlist_at;
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
            if (i >= vc->cut) s *= 1 - (float)(i - vc->cut) / (float)vc->fade;
            l += s * vc->gl;
            r += s * vc->gr;
            vc->pos += vc->step;
            if ((int)(vc->pos >> 16) >= vc->len) vc->pcm = NULL;
        }
        if (a->music.pcm) {
            /* Duck under the effects: fast attack, slow release (about 20 ms / 1 s). */
            float fx = fabsf(l) + fabsf(r);
            a->duck += (fx - a->duck) * (fx > a->duck ? 0.001f : 0.00003f);
            float g = MUSIC_GAIN / (1 + a->duck / 6000);
            l += a->music.pcm[2 * a->music_pos] * g;
            r += a->music.pcm[2 * a->music_pos + 1] * g;
            if (++a->music_pos >= a->music.loop1) { a->music_pos = a->music.loop0; a->music_looped = 1; }
        }
        /* Soft limit so a broadside doesn't clip harshly. */
        l = 32767 * tanhf(l / 32767);
        r = 32767 * tanhf(r / 32767);
        out[2 * f] = (int16_t)l;
        out[2 * f + 1] = (int16_t)r;
    }
}

static audio *open_bank(const char *bank_path)
{
    audio *a = calloc(1, sizeof *a);
    if (bnk_load(bank_path, &a->bank)) {
        printf("audio: no sound bank at %s, using synthesised effects\n", bank_path);
        synth_bank(&a->bank);
    }
    a->rng = 12345;
    a->last_tick = -1;
    return a;
}

audio *audio_open(const char *bank_path)
{
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) { fprintf(stderr, "audio: %s (silent)\n", SDL_GetError()); return NULL; }
    audio *a = open_bank(bank_path);
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
    SDL_PauseAudioDevice(a->dev, 0);
    printf("audio: %d sounds\n", a->bank.count);
    return a;
}

audio *audio_open_offline(const char *bank_path)
{
    audio *a = open_bank(bank_path);
    printf("audio: %d sounds (offline)\n", a->bank.count);
    return a;
}

void audio_render(audio *a, int16_t *out, int frames)
{
    if (a) mix(a, (Uint8 *)out, frames * 4);
    else memset(out, 0, (size_t)frames * 4);
}

/* ---- music --------------------------------------------------------------------- */

static int load_wav(const char *path, track *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char hdr[12];
    int ok = fread(hdr, 1, 12, f) == 12 && !memcmp(hdr, "RIFF", 4) && !memcmp(hdr + 8, "WAVE", 4);
    int channels = 0, bits = 0;
    unsigned rate = 0;
    while (ok) {
        unsigned char ch[8];
        if (fread(ch, 1, 8, f) != 8) { ok = 0; break; }
        unsigned n = ch[4] | ch[5] << 8 | ch[6] << 16 | (unsigned)ch[7] << 24;
        if (!memcmp(ch, "fmt ", 4)) {
            unsigned char fm[16];
            if (n < 16 || fread(fm, 1, 16, f) != 16) { ok = 0; break; }
            channels = fm[2] | fm[3] << 8;
            rate = fm[4] | fm[5] << 8 | fm[6] << 16 | (unsigned)fm[7] << 24;
            bits = fm[14] | fm[15] << 8;
            fseek(f, (long)(n - 16 + (n & 1)), SEEK_CUR);
        } else if (!memcmp(ch, "data", 4)) {
            if (channels != 2 || bits != 16 || rate != BANK_RATE) { ok = 0; break; }
            out->frames = n / 4;
            out->pcm = malloc((size_t)n);
            ok = out->pcm && fread(out->pcm, 4, (size_t)out->frames, f) == (size_t)out->frames;
            break;
        } else {
            fseek(f, (long)(n + (n & 1)), SEEK_CUR);
        }
    }
    fclose(f);
    if (!ok) { free(out->pcm); out->pcm = NULL; return -1; }
    out->loop0 = 0;
    out->loop1 = out->frames;
    return 0;
}

/* Loads the playlist's next track and swaps it in. */
static void next_track(audio *a)
{
    if (!a->playlist_len) return;
    char path[600], meta[600];
    const char *name = a->playlist[a->playlist_at++ % a->playlist_len];
    snprintf(path, sizeof path, "%s/%s.wav", a->music_dir, name);
    track t = {0};
    if (load_wav(path, &t)) { fprintf(stderr, "music: can't read %s\n", path); a->playlist_len = 0; return; }
    snprintf(meta, sizeof meta, "%s/music.json", a->music_dir);
    jnode *doc = json_load_file(meta);                   /* the loop the track carries */
    const jnode *lp = json_get(json_get(json_get(doc, "tracks"), name), "loop");
    if (lp && json_len(lp) == 2) {
        long l0 = (long)lp->child->num, l1 = (long)lp->child->next->num + 1;
        if (l0 >= 0 && l0 < l1 && l1 <= t.frames) { t.loop0 = l0; t.loop1 = l1; }
    }
    json_free(doc);
    if (a->dev) SDL_LockAudioDevice(a->dev);
    track old = a->music;
    a->music = t;
    a->music_pos = 0;
    a->music_looped = 0;
    if (a->dev) SDL_UnlockAudioDevice(a->dev);
    free(old.pcm);
    printf("music: %s\n", name);
}

int audio_music(audio *a, const char *music_dir, const char *playlist)
{
    if (!a) return -1;
    char meta[600];
    snprintf(meta, sizeof meta, "%s/music.json", music_dir);
    jnode *doc = json_load_file(meta);
    const jnode *list = json_get(json_get(doc, "playlists"), playlist);
    a->playlist_len = 0;
    for (const jnode *e = list ? list->child : NULL; e && a->playlist_len < MAX_PLAYLIST; e = e->next)
        if (e->type == J_STR) snprintf(a->playlist[a->playlist_len++], sizeof a->playlist[0], "%s", e->str);
    json_free(doc);
    if (!a->playlist_len) return -1;
    snprintf(a->music_dir, sizeof a->music_dir, "%s", music_dir);
    a->rng = a->rng * 1103515245u + 12345u;
    a->playlist_at = (int)((a->rng >> 16) % (unsigned)a->playlist_len);   /* start somewhere in the list */
    next_track(a);
    return a->music.pcm ? 0 : -1;
}

void audio_restart(audio *a)
{
    if (a) a->last_tick = -1;
}

void audio_update(audio *a)
{
    if (a && a->music_looped && a->playlist_len > 1) next_track(a);
}

void audio_close(audio *a)
{
    if (!a) return;
    printf("audio: %d sounds played\n", a->played);
    if (a->dev) SDL_CloseAudioDevice(a->dev);
    free(a->music.pcm);
    bnk_free(&a->bank);
    free(a);
}

/* keep > 0: play only that many seconds, then fade out over a third of it. */
static void play_cut(audio *a, int i, float gain, float pan, float keep)
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
    vc->cut = vc->len;
    vc->fade = 1;
    if (keep > 0 && keep * 4 / 3 * a->bank.rate < vc->len) {
        vc->cut = (int)(keep * a->bank.rate);
        vc->fade = (int)(keep / 3 * a->bank.rate);
        vc->len = vc->cut + vc->fade;
    }
    SDL_UnlockAudioDevice(a->dev);
}

static void play_index(audio *a, int i, float gain, float pan)
{
    play_cut(a, i, gain, pan, 0);
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
        float keep = 0;
        /* The bank's gun, hit and splash sounds carry 4-7 s rumbling tails: whole, one
         * shot and its splash sound like a gun still firing for ~9 s. Cut them short. */
        switch (e->kind) {
        case FX_MUZZLE: snprintf(base, sizeof base, "%sSHT%%d_WAV", calibre(e->mag)); i = variant(a, base, 3); gain *= 0.8f; keep = 1.2f; break;
        case FX_HIT: snprintf(base, sizeof base, "%sHT%%d_WAV", calibre(e->mag)); i = variant(a, base, 3); keep = 1.8f; break;
        case FX_SPLASH: case FX_NEAR_MISS:
            snprintf(base, sizeof base, "%sMISS%%d_WAV", calibre(e->mag)); i = variant(a, base, 3); gain *= 0.7f; keep = 1.5f; break;
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
        if (dist < 9000) play_cut(a, i, gain, pan, keep);
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
