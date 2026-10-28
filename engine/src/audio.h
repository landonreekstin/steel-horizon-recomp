/* Sound: a small SDL mixer playing the original game's sound bank, and the mapping
 * from battle events to sounds (distance falloff, stereo pan from the camera). */
#ifndef AUDIO_H
#define AUDIO_H

#include "asset.h"
#include "math3d.h"
#include "sim.h"

typedef struct audio audio;

audio *audio_open(const char *bank_path);   /* NULL without a device or bank: silent */
/* No device: the caller pulls the mix with audio_render (recording to a file). */
audio *audio_open_offline(const char *bank_path);
/* Mixes the next `frames` stereo S16 frames at BANK_RATE into out. */
void audio_render(audio *a, int16_t *out, int frames);
void audio_close(audio *a);
/* Music: plays the named playlist from music_dir/music.json (tools/at3wav.py), moving to
 * the next track each time one has played through. -1 without the decoded music. */
int audio_music(audio *a, const char *music_dir, const char *playlist);
/* Main thread, once a frame: loads the next track when it's due. */
void audio_update(audio *a);
/* The world started again (a new tutorial lesson): its events count from tick 0. */
void audio_restart(audio *a);
void audio_play(audio *a, const char *name, float gain, float pan);
/* Plays sounds for the world's new events, heard from eye looking along fwd. */
void audio_events(audio *a, const world *w, vec3 eye, vec3 fwd);
/* A ship's acknowledgement when ordered ("BB_USA_03"). */
void audio_ack(audio *a, const ship *s);

#endif
