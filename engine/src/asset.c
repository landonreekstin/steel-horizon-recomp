#include "asset.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t *read_file(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *size = n;
    return buf;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static float rdf(const uint8_t *p) { uint32_t v = rd32(p); float f; memcpy(&f, &v, 4); return f; }

/* GE 24-bit float command argument: the top 24 bits of an IEEE single. */
static float f24(uint32_t arg) { uint32_t v = (arg & 0xffffff) << 8; float f; memcpy(&f, &v, 4); return f; }

/* ---- .pingu ------------------------------------------------------------- */

enum { GE_VADDR = 0x01, GE_PRIM = 0x04, GE_RET = 0x0b, GE_BASE = 0x10, GE_VTYPE = 0x12,
       GE_OFFSET = 0x14, GE_TSU = 0x48, GE_TSV = 0x49, GE_TOU = 0x4a, GE_TOV = 0x4b };
#define MAT_SIZE 0x100
#define RS_SIZE 0x24

typedef struct {
    int strips[2048], strip_count, vertex_count;
    float ts[2], to[2];
} dl_mesh;

int pingu_load(const char *path, pingu *out)
{
    long size;
    uint8_t *d = read_file(path, &size);
    memset(out, 0, sizeof *out);
    if (!d) { fprintf(stderr, "%s: cannot read\n", path); return -1; }
    if (size < 0x80 || memcmp(d, "gniP PSP", 8) || memcmp(d + 0x3c, "MAT ", 4)) goto bad;
    for (int k = 0; k < 3; k++) {
        out->bbox_min[k] = rdf(d + 0x10 + 4 * k);
        out->bbox_max[k] = rdf(d + 0x20 + 4 * k);
    }
    out->radius = rdf(d + 0x30);
    float pos_scale = rdf(d + 0x34) / 128.0f;
    int nmesh = (int)rd32(d + 0x38), nmat = (int)rd32(d + 0x44);
    if (nmesh > 16 || nmat > 16) goto bad;
    out->material_count = nmat;
    for (int i = 0; i < nmat; i++)
        snprintf(out->materials[i], 64, "%.63s", (const char *)d + 0x6c + i * MAT_SIZE);
    long rs_base = 0x70 + (long)nmat * MAT_SIZE, dl = -1;
    for (long o = rs_base; o + 8 <= size; o += 4)
        if (rd32(d + o) == GE_BASE << 24 && rd32(d + o + 4) == GE_OFFSET << 24) { dl = o; break; }
    if (dl < 0) goto bad;

    /* Walk the display lists: one per mesh (BASE ... RET), then a final RET. */
    static dl_mesh m[16];
    int cur = -1, count = 0;
    long o = dl;
    for (;; o += 4) {
        if (o + 4 > size) goto bad;
        uint32_t v = rd32(d + o), cmd = v >> 24, arg = v & 0xffffff;
        if (cmd == GE_BASE) {
            if (count == 16) goto bad;
            cur = count++;
            memset(&m[cur], 0, sizeof m[cur]);
            m[cur].ts[0] = m[cur].ts[1] = 1;
        } else if (cmd == GE_RET) {
            if (cur < 0) { o += 4; break; }
            cur = -1;
        } else if (v == 0 || cur < 0 || cmd == GE_OFFSET || cmd == GE_VADDR) {
            continue;
        } else if (cmd == GE_VTYPE) {
            if (arg != 0xa1) goto bad;                    /* every mesh in the game */
        } else if (cmd == GE_PRIM) {
            if (((arg >> 16) & 7) != 4 || m[cur].strip_count == 2048) goto bad;
            m[cur].strips[m[cur].strip_count++] = (int)(arg & 0xffff);
            m[cur].vertex_count += (int)(arg & 0xffff);
        } else if (cmd == GE_TSU) m[cur].ts[0] = f24(arg);
        else if (cmd == GE_TSV) m[cur].ts[1] = f24(arg);
        else if (cmd == GE_TOU) m[cur].to[0] = f24(arg);
        else if (cmd == GE_TOV) m[cur].to[1] = f24(arg);
        else goto bad;
    }
    if (count != nmesh) goto bad;
    out->mesh_count = count;

    /* Materials: find each mesh record by (nverts, 0, nprims), then its render state. */
    long lo = rs_base, vo = o + 12;
    for (int i = 0; i < count; i++) {
        pingu_mesh *pm = &out->meshes[i];
        pm->material = -1;
        for (long a = lo; a + 7 <= dl; a++) {
            if (rd16(d + a) == m[i].vertex_count && rd16(d + a + 2) == 0 &&
                rd16(d + a + 4) == m[i].strip_count) {
                int rs = d[a + 6];
                int tex = d[rs_base + rs * RS_SIZE + 0x1e];  /* 0xff = untextured */
                if (tex < nmat) pm->material = tex;
                lo = a + 6;
                break;
            }
        }
        pm->vertex_count = m[i].vertex_count;
        pm->vertices = malloc(sizeof(vertex) * (size_t)pm->vertex_count);
        if (vo + 8L * pm->vertex_count > size) goto bad;
        for (int k = 0; k < pm->vertex_count; k++, vo += 8) {
            const uint8_t *q = d + vo;
            vertex *vx = &pm->vertices[k];
            vx->uv[0] = q[0] / 128.0f * m[i].ts[0] + m[i].to[0];
            vx->uv[1] = q[1] / 128.0f * m[i].ts[1] + m[i].to[1];
            for (int c = 0; c < 3; c++) {
                vx->normal[c] = (int8_t)q[2 + c] / 127.0f;
                vx->pos[c] = (int8_t)q[5 + c] * pos_scale;
            }
        }
        /* Strips -> triangle list, flipping every other triangle to keep winding. */
        pm->indices = malloc(sizeof(uint32_t) * 3 * (size_t)pm->vertex_count);
        int base = 0, n = 0;
        for (int s = 0; s < m[i].strip_count; s++) {
            for (int t = 0; t + 2 < m[i].strips[s]; t++) {
                uint32_t a = (uint32_t)(base + t), b = a + 1, c = a + 2;
                if (t & 1) { uint32_t tmp = a; a = b; b = tmp; }
                pm->indices[n++] = a; pm->indices[n++] = b; pm->indices[n++] = c;
            }
            base += m[i].strips[s];
        }
        pm->index_count = n;
    }
    if (vo != size) goto bad;
    free(d);
    return 0;
bad:
    fprintf(stderr, "%s: unexpected .pingu layout\n", path);
    free(d);
    pingu_free(out);
    return -1;
}

void pingu_free(pingu *p)
{
    for (int i = 0; i < 16; i++) {
        free(p->meshes[i].vertices);
        free(p->meshes[i].indices);
    }
    memset(p, 0, sizeof *p);
}

/* ---- .pts --------------------------------------------------------------- */

int mounts_load(const char *path, mounts *out)
{
    long size;
    memset(out, 0, sizeof *out);
    uint8_t *d = read_file(path, &size);
    if (!d) return 0;
    if (size < 0x14 || memcmp(d, "POINT 1.00", 10)) { free(d); return -1; }
    static const float Q[9] = {-1, 0, 0, 0, 0, 1, 0, 1, 0};  /* authoring axes -> model */
    int n = (int)rd32(d + 0x10);
    for (int i = 0; i < n && i < 32 && 0x14 + (i + 1) * 0x50 <= size; i++) {
        const uint8_t *r = d + 0x14 + i * 0x50;
        mount_point *mp = &out->points[out->count++];
        snprintf(mp->name, sizeof mp->name, "%.31s", (const char *)r);
        float mm[9];
        for (int k = 0; k < 3; k++) mp->pos[k] = rdf(r + 32 + 4 * k);
        for (int k = 0; k < 9; k++) mm[k] = rdf(r + 44 + 4 * k);
        for (int row = 0; row < 3; row++)
            for (int col = 0; col < 3; col++) {
                float s = 0;
                for (int k = 0; k < 3; k++) s += Q[row * 3 + k] * mm[k * 3 + col];
                mp->rot[row * 3 + col] = s;
            }
    }
    free(d);
    return 0;
}

/* ---- .bog --------------------------------------------------------------- */

static void unswizzle(const uint8_t *in, uint8_t *out, int rb, int h)
{
    int i = 0;
    for (int by = 0; by < h; by += 8)
        for (int bx = 0; bx < rb; bx += 16)
            for (int y = by; y < by + 8; y++, i += 16)
                if (y < h) memcpy(out + y * rb + bx, in + i, 16);
}

int bog_load(const char *path, image *out)
{
    long size;
    uint8_t *d = read_file(path, &size);
    memset(out, 0, sizeof *out);
    if (!d) { fprintf(stderr, "%s: cannot read\n", path); return -1; }
    if (size < 0x34 || memcmp(d, " GOB PSP", 8)) goto bad;
    int fmt = (int)rd32(d + 0x10), w = (int)rd32(d + 0x14), h = (int)rd32(d + 0x18);
    uint32_t flags = rd32(d + 0x28);
    int bpp = fmt == 3 ? 16 : fmt == 5 ? 32 : fmt == 6 ? 8 : fmt == 7 ? 4 : 0;
    if (!bpp || w <= 0 || h <= 0 || w > 4096 || h > 4096) goto bad;
    int pal_n = fmt == 6 ? 256 : fmt == 7 ? 16 : 0;
    int rb = ((w + 15) & ~15) * bpp / 8;              /* width padded to 16 pixels */
    long pix = 0x34 + pal_n * 4;
    if (pix + (long)rb * h > size) goto bad;
    uint8_t *raw = malloc((size_t)rb * h);
    /* Direct-colour formats are always swizzled; CLUT ones when flag 0x4 is set. */
    if (fmt == 3 || fmt == 5 || (flags & 4)) unswizzle(d + pix, raw, rb, h);
    else memcpy(raw, d + pix, (size_t)rb * h);
    const uint8_t *pal = d + 0x34;
    out->width = w;
    out->height = h;
    out->rgba = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *row = raw + y * rb;
            uint8_t *px = out->rgba + (y * w + x) * 4;
            if (fmt == 5) memcpy(px, row + x * 4, 4);
            else if (fmt == 6) memcpy(px, pal + row[x] * 4, 4);
            else if (fmt == 7) memcpy(px, pal + ((row[x >> 1] >> ((x & 1) * 4)) & 15) * 4, 4);
            else {                                   /* ABGR1555 */
                unsigned v = row[x * 2] | row[x * 2 + 1] << 8;
                unsigned r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
                px[0] = (uint8_t)(r << 3 | r >> 2);
                px[1] = (uint8_t)(g << 3 | g >> 2);
                px[2] = (uint8_t)(b << 3 | b >> 2);
                px[3] = v & 0x8000 ? 255 : 0;
            }
        }
    free(raw);
    free(d);
    return 0;
bad:
    fprintf(stderr, "%s: unexpected .bog layout\n", path);
    free(d);
    return -1;
}

void image_free(image *img)
{
    free(img->rgba);
    img->rgba = NULL;
}

/* ---- sound banks ---------------------------------------------------------------------- */

static const int ADPCM_COEF[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

static int adpcm_decode(const uint8_t *f, size_t size, int16_t *out)
{
    int n = 0, s1 = 0, s2 = 0;
    for (size_t o = 0; o + 16 <= size; o += 16) {
        int pred = f[o] >> 4, shift = f[o] & 15;
        int k1 = pred < 5 ? ADPCM_COEF[pred][0] : 0, k2 = pred < 5 ? ADPCM_COEF[pred][1] : 0;
        for (int b = 2; b < 16; b++)
            for (int h = 0; h < 2; h++) {
                int nib = h ? f[o + b] >> 4 : f[o + b] & 15;
                int v = nib >= 8 ? nib - 16 : nib;
                v = (v * 4096) >> shift;             /* arithmetic shift, as Python's >> */
                v += (s1 * k1 + s2 * k2) >> 6;
                v = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
                out[n++] = (int16_t)v;
                s2 = s1;
                s1 = v;
            }
        if (f[o + 1] & 1) break;
    }
    return n;
}

static int is_name_char(uint8_t c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

int bnk_load(const char *path, sound_bank *out)
{
    memset(out, 0, sizeof *out);
    long size;
    uint8_t *d = read_file(path, &size);
    if (!d) return -1;
    if (size < 0x40 || memcmp(d + 0x18, "SBlk", 4)) { fprintf(stderr, "%s: not an SBlk bank\n", path); free(d); return -1; }
    uint32_t data_off;
    memcpy(&data_off, d + 16, 4);
    if (data_off >= (uint32_t)size) { free(d); return -1; }
    /* Entries: 24-byte records starting at the first f32 85.21 (00 6e aa 42). */
    static const uint8_t mark[4] = {0x00, 0x6e, 0xaa, 0x42};
    long e0 = -1;
    for (long o = 0; o + 4 <= (long)data_off; o++)
        if (!memcmp(d + o, mark, 4)) { e0 = o; break; }
    int ents = 0;
    for (long o = e0; e0 >= 0 && o + 24 <= (long)data_off; o += 24) {
        float f;
        uint32_t off, sz;
        memcpy(&f, d + o, 4);
        memcpy(&off, d + o + 16, 4);
        memcpy(&sz, d + o + 20, 4);
        if (!(f > 1 && f < 200) || (long)data_off + (long)off + (long)sz > size) break;
        ents++;
    }
    /* Names: the [A-Z0-9_]{4,} runs before the data; the last `ents` of them. */
    int nn = 0, cap = 1024;
    long *name_at = malloc(sizeof(long) * (size_t)cap);
    int *name_len = malloc(sizeof(int) * (size_t)cap);
    for (long o = 0; o < (long)data_off;) {
        long r = o;
        while (r < (long)data_off && is_name_char(d[r])) r++;
        if (r - o >= 4 && nn < cap) { name_at[nn] = o; name_len[nn++] = (int)(r - o); }
        o = r > o ? r : o + 1;
    }
    out->count = ents < nn ? ents : nn;
    out->names = calloc((size_t)out->count, sizeof *out->names);
    out->pcm = calloc((size_t)out->count, sizeof *out->pcm);
    out->length = calloc((size_t)out->count, sizeof *out->length);
    for (int i = 0; i < out->count; i++) {
        long o = e0 + 24L * i;
        uint32_t off, sz;
        memcpy(&off, d + o + 16, 4);
        memcpy(&sz, d + o + 20, 4);
        int k = nn - out->count + i;
        int l = name_len[k] < 31 ? name_len[k] : 31;
        memcpy(out->names[i], d + name_at[k], (size_t)l);
        out->pcm[i] = malloc(sizeof(int16_t) * (sz / 16 * 28 + 28));
        out->length[i] = adpcm_decode(d + data_off + off, sz, out->pcm[i]);
    }
    free(name_at);
    free(name_len);
    free(d);
    return out->count ? 0 : -1;
}

void bnk_free(sound_bank *b)
{
    for (int i = 0; i < b->count; i++) free(b->pcm[i]);
    free(b->pcm);
    free(b->names);
    free(b->length);
    memset(b, 0, sizeof *b);
}

int bnk_find(const sound_bank *b, const char *name)
{
    for (int i = 0; i < b->count; i++)
        if (!strcmp(b->names[i], name)) return i;
    return -1;
}
