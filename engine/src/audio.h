/* Sound: a small SDL mixer playing the original game's sound bank, and the mapping
 * from battle events to sounds (distance falloff, stereo pan from the camera). */
#ifndef AUDIO_H
#define AUDIO_H

#include "asset.h"
#include "math3d.h"
#include "sim.h"

typedef struct audio audio;

audio *audio_open(const char *bank_path);   /* NULL without a device or bank: silent */
void audio_close(audio *a);
void audio_play(audio *a, const char *name, float gain, float pan);
/* Plays sounds for the world's new events, heard from eye looking along fwd. */
void audio_events(audio *a, const world *w, vec3 eye, vec3 fwd);
/* A ship's acknowledgement when ordered ("BB_USA_03"). */
void audio_ack(audio *a, const ship *s);

#endif
