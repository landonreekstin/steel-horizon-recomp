/* Readers for the original PSP assets, ported from tools/pingu.py and tools/bog.py
 * (layouts in notes/formats.md). The engine loads the user's extracted files
 * directly; nothing here ships game data. */
#ifndef ASSET_H
#define ASSET_H

#include <stdint.h>

typedef struct {
    float pos[3], normal[3], uv[2];
} vertex;

typedef struct {
    int material;               /* index into pingu.materials, -1 = untextured */
    int vertex_count;
    vertex *vertices;
    int index_count;
    uint32_t *indices;          /* triangle list built from the GE strips */
} pingu_mesh;

typedef struct {
    float bbox_min[3], bbox_max[3], radius;
    int material_count;
    char materials[16][64];
    int mesh_count;
    pingu_mesh meshes[16];
} pingu;

typedef struct {
    char name[32];
    float pos[3];
    float rot[9];               /* row-major model-space rotation (Q * M) */
} mount_point;

typedef struct {
    int count;
    mount_point points[32];
} mounts;

typedef struct {
    int width, height;
    uint8_t *rgba;              /* width * height * 4 */
} image;

/* Each returns 0 on success, -1 on error (message on stderr). */
int pingu_load(const char *path, pingu *out);
void pingu_free(pingu *p);
int mounts_load(const char *path, mounts *out);   /* missing file: count = 0, returns 0 */
int bog_load(const char *path, image *out);       /* mip level 0 as RGBA */
void image_free(image *img);

/* Sound bank (.bnk "SBlk", tools/bnk.py): named PS-ADPCM samples, decoded to 16-bit
 * mono PCM at 44.1 kHz (see notes/formats.md). */
#define BANK_RATE 44100
typedef struct {
    int count;
    char (*names)[32];
    int16_t **pcm;
    int *length;                /* samples */
} sound_bank;
int bnk_load(const char *path, sound_bank *out);
void bnk_free(sound_bank *b);
int bnk_find(const sound_bank *b, const char *name);   /* -1 if absent */
/* Synthesised stand-ins for the battle sounds (synth.c), for running without the bank. */
void synth_bank(sound_bank *b);

/* Reads a whole file; caller frees. */
uint8_t *read_file(const char *path, long *size);

#endif
