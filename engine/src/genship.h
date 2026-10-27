/* Ship models generated from proportions (no game assets): a lofted hull with sheer,
 * flare and a keel, superstructure by hull type, and turrets sized by calibre. Plain
 * geometry; the renderer uploads it. Used with --models generated. */
#ifndef GENSHIP_H
#define GENSHIP_H

#include "asset.h"
#include "data.h"

#define GEN_PARTS 8

typedef struct {
    float color[3];
    vertex *v;
    uint32_t *idx;
    int nv, ni, cap_v, cap_i;
} gen_part;

typedef struct {
    int count;
    gen_part part[GEN_PARTS];   /* one per colour: boot-topping, hull, deck, upperworks, ... */
} gen_model;

/* The ship (turrets excluded: they're the gun model, drawn at the mounts). */
void genship_build(const ship_class *c, gen_model *out);
/* The main battery's turret with its barrels, +Z forward, origin at the turret ring. */
void genship_gun(const ship_class *c, gen_model *out);
void genship_free(gen_model *m);

#endif
