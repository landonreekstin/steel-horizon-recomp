/* Synthesised sound effects (design), for when the game's sound bank isn't there:
 * filtered noise and decaying tones shaped into reports, impacts, splashes, engines
 * and a sinking groan. Same names as the bank entries the battle maps events to. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asset.h"

static unsigned rng_state = 0x12345678u;
static float noise(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return (float)(rng_state >> 8) / 8388608.0f - 1.0f;
}

typedef struct {
    float seconds;
    float lowpass;      /* one-pole cutoff, Hz */
    float decay;        /* amplitude e-folding time, s */
    float attack;       /* s */
    float tone, tone_amp, tone_decay;   /* a low thump / ring */
    float gain;
    float crackle;      /* random spikes per second */
    float am;           /* engine: amplitude-modulation rate, Hz (0 none) */
    float saw;          /* engine: sawtooth frequency, Hz (0 none) */
    float sweep;        /* sinking: tone falls by this ratio over the sound */
} recipe;

static void render(const recipe *r, int16_t **out, int *len)
{
    int n = (int)(r->seconds * BANK_RATE);
    int16_t *pcm = malloc(sizeof(int16_t) * (size_t)n);
    float lp = 0, k = 1 - expf(-2 * 3.14159265f * r->lowpass / BANK_RATE), ph = 0, sp = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / BANK_RATE;
        float env = (t < r->attack ? t / r->attack : 1) * expf(-t / r->decay);
        lp += k * (noise() - lp);
        float s = lp * 2.5f;
        if (r->crackle > 0 && noise() > 1 - 2 * r->crackle / BANK_RATE) s += noise() * 2;
        float f = r->tone * (r->sweep > 0 ? powf(r->sweep, t / r->seconds) : 1);
        ph += 2 * 3.14159265f * f / BANK_RATE;
        s += r->tone_amp * sinf(ph) * expf(-t / r->tone_decay);
        if (r->saw > 0) {
            sp += r->saw / BANK_RATE;
            sp -= floorf(sp);
            s += (sp * 2 - 1) * 0.6f * (r->am > 0 ? 0.7f + 0.3f * sinf(2 * 3.14159265f * r->am * t) : 1);
        }
        float v = s * env * r->gain * 32767;
        pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    *out = pcm;
    *len = n;
}

static void add(sound_bank *b, const char *name, const recipe *r)
{
    int i = b->count++;
    b->names = realloc(b->names, sizeof *b->names * (size_t)b->count);
    b->pcm = realloc(b->pcm, sizeof *b->pcm * (size_t)b->count);
    b->length = realloc(b->length, sizeof *b->length * (size_t)b->count);
    memset(b->names[i], 0, sizeof b->names[i]);
    strncpy(b->names[i], name, 15);                  /* the bank's 15-character names */
    render(r, &b->pcm[i], &b->length[i]);
}

void synth_bank(sound_bank *b)
{
    memset(b, 0, sizeof *b);
    b->rate = BANK_RATE;
    static const char *cal[4] = {"ENORMCANN", "LRGCANN", "MEDCANN", "SMLCANN"};
    for (int c = 0; c < 4; c++) {
        float size = 1 - 0.25f * (float)c;           /* 1 = 16in .. 0.25 = small guns */
        for (int v = 1; v <= 3; v++) {
            char nm[32];
            recipe shot = {0.6f + 1.4f * size, 500 + 2500 * (1 - size) + 100 * (float)v, 0.12f + 0.45f * size, 0.002f,
                           45 + 40 * (1 - size), 0.9f * size, 0.15f + 0.3f * size, 0.9f, 0, 0, 0, 0};
            snprintf(nm, sizeof nm, "%sSHT%d_WAV", cal[c], v);
            add(b, nm, &shot);
            recipe hit = {0.8f + size, 900 + 600 * (float)v, 0.2f + 0.4f * size, 0.004f, 180 + 60 * (float)v, 0.35f, 0.25f, 0.8f,
                          40 * size, 0, 0, 0};
            snprintf(nm, sizeof nm, "%sHT%d_WAV", cal[c], v);
            add(b, nm, &hit);
            recipe miss = {1.0f + 0.6f * size, 1800 + 400 * (float)v, 0.35f + 0.3f * size, 0.08f, 0, 0, 1, 0.45f + 0.2f * size, 0, 0, 0, 0};
            snprintf(nm, sizeof nm, "%sMISS%d_WAV", cal[c], v);
            add(b, nm, &miss);
        }
    }
    for (int v = 1; v <= 3; v++) {
        char nm[32];
        recipe tshot = {0.5f, 4000, 0.12f, 0.01f, 0, 0, 1, 0.5f, 0, 0, 0, 0};
        snprintf(nm, sizeof nm, "TORPSHT%d_WAV", v);
        add(b, nm, &tshot);
    }
    for (int v = 1; v <= 2; v++) {
        char nm[32];
        recipe thit = {2.5f, 300, 0.9f, 0.01f, 35, 1.0f, 0.8f, 1.0f, 20, 0, 0, 0};
        snprintf(nm, sizeof nm, "TORPHT%d_WAV", v);
        add(b, nm, &thit);
        recipe dshot = {0.4f, 2500, 0.1f, 0.002f, 90, 0.5f, 0.1f, 0.6f, 0, 0, 0, 0};
        snprintf(nm, sizeof nm, "DPTHCHRGSHT%d_WAV", v);
        add(b, nm, &dshot);
        recipe dexp = {2.0f, 200, 0.7f, 0.05f, 30, 0.9f, 0.6f, 0.9f, 0, 0, 0, 0};
        snprintf(nm, sizeof nm, "DPTHCHRGEXPL%d_WAV", v);
        add(b, nm, &dexp);
        recipe flak = {0.35f, 3000, 0.08f, 0.001f, 300, 0.2f, 0.05f, 0.5f, 0, 0, 0, 0};
        snprintf(nm, sizeof nm, "AAGUNEXPL%d_WAV", v);
        add(b, nm, &flak);
        recipe mg = {0.5f, 6000, 0.4f, 0.001f, 0, 0, 1, 0.35f, 0, 0, 0, 0};
        mg.am = 18;
        mg.saw = 0;
        snprintf(nm, sizeof nm, "_50CALSHT%d_WAV", v);
        add(b, nm, &mg);
        recipe crash = {1.6f, 1500, 0.5f, 0.01f, 60, 0.5f, 0.3f, 0.7f, 30, 0, 0, 0};
        snprintf(nm, sizeof nm, "CRSHWTR%d_WAV", v);
        add(b, nm, &crash);
    }
    recipe sink = {5.0f, 150, 3.0f, 0.5f, 55, 0.7f, 4.0f, 0.8f, 6, 0, 0, 0.4f};
    add(b, "SINKINGLRG2_WAV", &sink);
    recipe takeoff = {2.5f, 800, 1.6f, 0.4f, 0, 0, 1, 0.35f, 0, 3, 95, 0};
    add(b, "TAKEOFFALS2_WAV", &takeoff);
    recipe land = {1.8f, 700, 1.0f, 0.1f, 0, 0, 1, 0.3f, 0, 2, 70, 0};
    add(b, "LANDALS_WAV", &land);
}
