/* Helpers shared by the sim's own modules (combat.c, air.c); not part of the API. */
#ifndef SIM_INTERNAL_H
#define SIM_INTERNAL_H

#include "sim.h"

float rand01(world *w);
float gauss(world *w);
void fx(world *w, fx_kind kind, float x, float y, float z);
void apply_damage(world *w, int j, float dmg);
float hull_distance(const ship *s, float x, float z);
float intercept(float mx, float mz, float px, float pz, float vx, float vz, float v);
projectile *new_projectile(world *w);
void launch_ballistic(projectile *p, float mx, float my, float mz, float ax, float az, float T);

#endif
