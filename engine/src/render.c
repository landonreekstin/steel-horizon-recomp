#include "render.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asset.h"
#include "font_atlas.h"
#include "gl.h"

/* ---- waves (shared by the ocean shader and sea_height) ---------------------- */

typedef struct { float dx, dz, length, amp, speed; } wave;
static const wave WAVES[4] = {
    {0.80f, 0.60f, 310.0f, 2.6f, 1.0f},
    {-0.35f, 0.94f, 170.0f, 1.4f, 1.0f},
    {0.97f, -0.24f, 95.0f, 0.7f, 1.0f},
    {-0.70f, -0.71f, 55.0f, 0.35f, 1.0f},
};

float sea_height(float x, float z, float t)
{
    float h = 0;
    for (int i = 0; i < 4; i++) {
        const wave *w = &WAVES[i];
        float k = 2 * PI_F / w->length, c = sqrtf(9.81f * 20.0f / k);  /* deep-water speed */
        h += w->amp * sinf(k * (w->dx * x + w->dz * z) - k * c * w->speed * t * 0.05f);
    }
    return h;
}

/* ---- shaders ---------------------------------------------------------------- */

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "shader compile failed:\n%s\n", log);
    }
    return s;
}

static GLuint program(const char *vs, const char *fs)
{
    GLuint p = glCreateProgram(), v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "program link failed:\n%s\n", log);
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

#define GLSL "#version 330 core\n"
#define SKY_GLSL \
    "const vec3 SUN = normalize(vec3(0.45, 0.55, -0.70));\n" \
    "vec3 sky_dome(vec3 d) {\n" \
    "  float h = clamp(d.y, -0.2, 1.0);\n" \
    "  return mix(vec3(0.72, 0.80, 0.86), vec3(0.26, 0.45, 0.70), pow(max(h, 0.0), 0.55));\n" \
    "}\n" \
    "vec3 sky(vec3 d) {\n" \
    "  vec3 c = sky_dome(d);\n" \
    "  c += vec3(1.0, 0.85, 0.6) * pow(max(dot(d, SUN), 0.0), 400.0) * 4.0;\n" \
    "  c += vec3(1.0, 0.8, 0.5) * pow(max(dot(d, SUN), 0.0), 8.0) * 0.25;\n" \
    "  return c;\n" \
    "}\n" \
    "vec3 fog(vec3 c, vec3 wpos, vec3 eye) {\n" \
    "  float f = 1.0 - exp(-length(wpos - eye) / 26000.0);\n" \
    "  return mix(c, sky(normalize(vec3((wpos - eye).x, 0.02, (wpos - eye).z))), f);\n" \
    "}\n"

static const char *SHIP_VS = GLSL
    "layout(location=0) in vec3 a_pos; layout(location=1) in vec3 a_nrm; layout(location=2) in vec2 a_uv;\n"
    "uniform mat4 u_vp, u_model;\n"
    "out vec3 v_nrm; out vec2 v_uv; out vec3 v_wpos;\n"
    "void main() {\n"
    "  vec4 w = u_model * vec4(a_pos, 1.0);\n"
    "  v_wpos = w.xyz; v_uv = a_uv; v_nrm = mat3(u_model) * a_nrm;\n"
    "  gl_Position = u_vp * w;\n"
    "}\n";

static const char *SHIP_FS = GLSL SKY_GLSL
    "in vec3 v_nrm; in vec2 v_uv; in vec3 v_wpos;\n"
    "uniform sampler2D u_tex; uniform int u_textured; uniform vec3 u_eye; uniform vec4 u_tint; uniform vec3 u_flat;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  vec4 c = u_textured != 0 ? texture(u_tex, v_uv) : vec4(u_flat, 1.0);\n"
    "  if (c.a < 0.5) discard;\n"
    "  vec3 n = normalize(v_nrm);\n"
    "  float l = 0.45 + 0.75 * abs(dot(n, SUN));\n"     /* two-sided: GE strips mix windings */
    "  vec3 col = c.rgb * l * u_tint.rgb;\n"
    "  o = vec4(fog(col, v_wpos, u_eye), 1.0);\n"
    "}\n";

static const char *SEA_VS = GLSL
    "layout(location=0) in vec2 a_xz;\n"
    "uniform mat4 u_vp; uniform vec2 u_origin; uniform float u_t;\n"
    "uniform vec4 u_w[4]; uniform vec2 u_w2[4];\n"      /* (dx, dz, length, amp), (speed, -) */
    "out vec3 v_wpos; out vec3 v_nrm;\n"
    "void main() {\n"
    "  vec2 p = a_xz + u_origin; vec3 d = vec3(p.x, 0.0, p.y);\n"
    "  vec3 tx = vec3(1, 0, 0), tz = vec3(0, 0, 1);\n"
    "  for (int i = 0; i < 4; i++) {\n"
    "    float k = 6.2831853 / u_w[i].z, c = sqrt(9.81 * 20.0 / k), a = u_w[i].w;\n"
    "    vec2 dir = u_w[i].xy; float ph = k * dot(dir, p) - k * c * u_w2[i].x * u_t * 0.05;\n"
    "    float q = 0.6 / (k * a * 4.0);\n"               /* Gerstner steepness, no loops */
    "    d.x += q * a * dir.x * cos(ph); d.z += q * a * dir.y * cos(ph); d.y += a * sin(ph);\n"
    "    tx.y += k * a * dir.x * cos(ph); tz.y += k * a * dir.y * cos(ph);\n"
    "  }\n"
    "  v_nrm = normalize(cross(tz, tx)); v_wpos = d;\n"
    "  gl_Position = u_vp * vec4(d, 1.0);\n"
    "}\n";

static const char *SEA_FS = GLSL SKY_GLSL
    "in vec3 v_wpos; in vec3 v_nrm; uniform vec3 u_eye; out vec4 o;\n"
    "void main() {\n"
    "  vec3 n = normalize(v_nrm), v = normalize(u_eye - v_wpos), r = reflect(-v, n);\n"
    "  float fres = 0.04 + 0.96 * pow(1.0 - max(dot(n, v), 0.0), 5.0);\n"
    "  vec3 deep = vec3(0.02, 0.09, 0.15), body = deep + vec3(0.03, 0.10, 0.11) * max(n.y - 0.9, 0.0) * 8.0;\n"
    /* Reflect the sky without its sun disk: on this coarse mesh the reflected disk
     * shows as bright flat patches; the capped glint below stands in for it. */
    "  vec3 c = mix(body, sky_dome(r) * 0.9, fres);\n"
    /* Sun glitter: a soft sheen plus sparkles from a position hash, since the smooth
     * mesh can't make the glitter path's small facets itself. */
    "  float g = pow(max(dot(r, SUN), 0.0), 250.0);\n"
    "  float hsh = fract(sin(dot(floor(v_wpos.xz * 0.8), vec2(12.9898, 78.233))) * 43758.5453);\n"
    "  c += vec3(1.0, 0.9, 0.7) * g * (0.06 + 1.5 * step(0.94, hsh));\n"
    "  o = vec4(fog(c, v_wpos, u_eye), 1.0);\n"
    "}\n";

static const char *SKY_VS = GLSL
    "out vec2 v_ndc;\n"
    "void main() { vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2) * 2.0 - 1.0;\n"
    "  v_ndc = p; gl_Position = vec4(p, 0.9999, 1.0); }\n";

static const char *SKY_FS = GLSL SKY_GLSL
    "in vec2 v_ndc; uniform mat4 u_inv_vp; uniform vec3 u_eye; out vec4 o;\n"
    "void main() { vec4 w = u_inv_vp * vec4(v_ndc, 1.0, 1.0);\n"
    "  o = vec4(sky(normalize(w.xyz / w.w - u_eye)), 1.0); }\n";

static const char *LINE_VS = GLSL
    "layout(location=0) in vec3 a_pos; layout(location=1) in vec4 a_col;\n"
    "uniform mat4 u_vp; out vec4 v_col;\n"
    "void main() { v_col = a_col; gl_Position = u_vp * vec4(a_pos, 1.0); }\n";

/* Screen-space UI: solid quads (uv.x < 0) and glyphs from the font atlas. */
static const char *UI_VS = GLSL
    "layout(location=0) in vec2 a_pos; layout(location=1) in vec2 a_uv; layout(location=2) in vec4 a_col;\n"
    "uniform vec2 u_screen; out vec2 v_uv; out vec4 v_col;\n"
    "void main() { v_uv = a_uv; v_col = a_col;\n"
    "  gl_Position = vec4(a_pos.x / u_screen.x * 2.0 - 1.0, 1.0 - a_pos.y / u_screen.y * 2.0, 0.0, 1.0); }\n";
static const char *UI_FS = GLSL
    "in vec2 v_uv; in vec4 v_col; uniform sampler2D u_font; out vec4 o;\n"
    "void main() { o = v_uv.x < 0.0 ? v_col : vec4(v_col.rgb, v_col.a * texture(u_font, v_uv).r); }\n";

static const char *LINE_FS = GLSL
    "in vec4 v_col; out vec4 o; void main() { o = v_col; }\n";

/* ---- GPU models --------------------------------------------------------------- */

typedef struct {
    GLuint vao, vbo, ibo;
    int index_count, material;
} gpu_mesh;

typedef struct {
    char path[128];
    int ok;
    float radius, bbox_min[3], bbox_max[3];
    int mesh_count;
    gpu_mesh meshes[16];
    GLuint textures[16];
    int textured[16];
    float flat[16][3];          /* colour of untextured materials */
    mounts mts;
    ship_profile prof;
} gpu_model;

#define MAX_MODELS 160
#define MAX_TEX_INDEX 2048

struct renderer {
    char files_dir[256];
    GLuint ship_prog, sea_prog, sky_prog, line_prog, ui_prog;
    GLuint sea_vao, sea_ibo, sky_vao, line_vao, line_vbo, ui_vao, ui_vbo, font_tex;
    int sea_index_count;
    gpu_model models[MAX_MODELS];
    int model_count;
    int tex_count;
    char tex_name[MAX_TEX_INDEX][64];     /* lower-case basename without .bog */
    char tex_path[MAX_TEX_INDEX][512];
};

static void index_textures(renderer *r, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        size_t n = strlen(e->d_name);
        if (n > 4 && !strcmp(e->d_name + n - 4, ".bog")) {
            if (r->tex_count == MAX_TEX_INDEX) continue;
            int i = r->tex_count++;
            snprintf(r->tex_path[i], sizeof r->tex_path[i], "%s", path);
            size_t k;
            for (k = 0; k < n - 4 && k < 63; k++) r->tex_name[i][k] = (char)tolower((unsigned char)e->d_name[k]);
            r->tex_name[i][k] = 0;
        } else {
            index_textures(r, path);              /* a directory (or junk: opendir fails) */
        }
    }
    closedir(d);
}

static const char *find_texture(const renderer *r, const char *name)
{
    char low[64];
    size_t k;
    for (k = 0; name[k] && k < 63; k++) low[k] = (char)tolower((unsigned char)name[k]);
    low[k] = 0;
    for (int i = 0; i < r->tex_count; i++)
        if (!strcmp(r->tex_name[i], low)) return r->tex_path[i];
    return NULL;
}

static GLuint upload_texture(const image *img)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img->width, img->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, img->rgba);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    return t;
}

/* Side profile: highest and lowest vertex per slice along the length. */
static void profile_begin(ship_profile *p, float z0, float z1)
{
    memset(p, 0, sizeof *p);
    p->z0 = z0;
    p->z1 = z1 > z0 ? z1 : z0 + 1;
    for (int b = 0; b < PROFILE_BINS; b++) { p->top[b] = -1e9f; p->bot[b] = 1e9f; }
}

static void profile_add(ship_profile *p, const float *pos)
{
    int b = (int)((pos[2] - p->z0) / (p->z1 - p->z0) * PROFILE_BINS);
    if (b < 0 || b >= PROFILE_BINS) return;
    p->top[b] = fmaxf(p->top[b], pos[1]);
    p->bot[b] = fminf(p->bot[b], pos[1]);
}

static void profile_end(ship_profile *p)
{
    /* Empty slices take their neighbours' extents; the deck line is the lower quartile
     * of the tops (bow and stern decks sit low, superstructure stands above). */
    for (int pass = 0; pass < 2; pass++)
        for (int b = 0; b < PROFILE_BINS; b++)
            if (p->top[b] < -1e8f) {
                int n = b > 0 ? b - 1 : b + 1;
                if (n < PROFILE_BINS && p->top[n] > -1e8f) { p->top[b] = p->top[n]; p->bot[b] = p->bot[n]; }
            }
    float tops[PROFILE_BINS];
    int n = 0;
    p->ymin = 1e9f;
    p->ymax = -1e9f;
    for (int b = 0; b < PROFILE_BINS; b++)
        if (p->top[b] > -1e8f) {
            tops[n++] = p->top[b];
            p->ymin = fminf(p->ymin, p->bot[b]);
            p->ymax = fmaxf(p->ymax, p->top[b]);
        } else {
            p->top[b] = p->bot[b] = 0;
        }
    for (int a = 1; a < n; a++)
        for (int b = a; b > 0 && tops[b] < tops[b - 1]; b--) { float t = tops[b]; tops[b] = tops[b - 1]; tops[b - 1] = t; }
    p->deck = n ? tops[n / 4] : 0;
    p->ok = n > 0;
}

static gpu_model *get_model(renderer *r, const char *rel)
{
    if (!rel || !rel[0]) return NULL;
    if (rel[0] == '@') {                       /* generated: built by renderer_load_class */
        for (int i = 0; i < r->model_count; i++)
            if (!strcmp(r->models[i].path, rel)) return &r->models[i];
        return NULL;
    }
    for (int i = 0; i < r->model_count; i++)
        if (!strcmp(r->models[i].path, rel)) return r->models[i].ok ? &r->models[i] : NULL;
    if (r->model_count == MAX_MODELS) return NULL;
    gpu_model *m = &r->models[r->model_count++];
    memset(m, 0, sizeof *m);
    snprintf(m->path, sizeof m->path, "%s", rel);
    char path[512];
    snprintf(path, sizeof path, "%s/%s", r->files_dir, rel);
    pingu p;
    if (pingu_load(path, &p)) return NULL;
    m->ok = 1;
    for (int i = 0; i < 16; i++) { m->flat[i][0] = 0.45f; m->flat[i][1] = 0.47f; m->flat[i][2] = 0.5f; }
    m->radius = p.radius;
    memcpy(m->bbox_min, p.bbox_min, sizeof m->bbox_min);
    memcpy(m->bbox_max, p.bbox_max, sizeof m->bbox_max);
    for (int i = 0; i < p.material_count; i++) {
        const char *tp = find_texture(r, p.materials[i]);
        image img;
        if (tp && !bog_load(tp, &img)) {
            m->textures[i] = upload_texture(&img);
            m->textured[i] = 1;
            image_free(&img);
        } else {
            fprintf(stderr, "%s: missing texture %s\n", rel, p.materials[i]);
        }
    }
    for (int i = 0; i < p.mesh_count; i++) {
        pingu_mesh *pm = &p.meshes[i];
        if (pm->material < 0) continue;   /* untextured mesh (shadow/blob?), not drawn */
        gpu_mesh *g = &m->meshes[m->mesh_count++];
        g->material = pm->material;
        g->index_count = pm->index_count;
        glGenVertexArrays(1, &g->vao);
        glBindVertexArray(g->vao);
        glGenBuffers(1, &g->vbo);
        glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(vertex) * (size_t)pm->vertex_count), pm->vertices, GL_STATIC_DRAW);
        glGenBuffers(1, &g->ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(sizeof(uint32_t) * (size_t)pm->index_count), pm->indices, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vertex), (void *)offsetof(vertex, pos));
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(vertex), (void *)offsetof(vertex, normal));
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(vertex), (void *)offsetof(vertex, uv));
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
    }
    profile_begin(&m->prof, p.bbox_min[2], p.bbox_max[2]);
    for (int i = 0; i < p.mesh_count; i++)
        for (int k = 0; k < p.meshes[i].vertex_count; k++) profile_add(&m->prof, p.meshes[i].vertices[k].pos);
    profile_end(&m->prof);
    pingu_free(&p);
    /* Mount points live next to the model: same name, .pts. */
    size_t n = strlen(path);
    if (n > 6) {
        strcpy(path + n - 6, ".pts");
        mounts_load(path, &m->mts);
    }
    return m;
}

/* Carrier planes: US/British use AL_AS, German/Japanese AX_AS, Shadow SH_AS
 * (the original's CPlane constructors); 01 intact, 02/03 damaged where present. */
static const gpu_model *plane_model(renderer *r, const char *nation, int stage)
{
    const char *f = !strcmp(nation, "American") || !strcmp(nation, "British") ? "AL"
                  : !strcmp(nation, "Shadow") ? "SH" : "AX";
    for (int st = f[1] == 'L' && stage > 2 ? 2 : stage; st >= 1; st--) {   /* AL has 01/02 only */
        char rel[64];
        snprintf(rel, sizeof rel, "pingus/props/PSP_%s_AS%02d.pingu", f, st);
        const gpu_model *m = get_model(r, rel);
        if (m) return m;
    }
    return NULL;
}

/* ---- generated models (no original asset exists) ---------------------------------- */

#define GEN_MAX_VERTS 4096
typedef struct {
    vertex v[GEN_MAX_VERTS];
    uint32_t idx[GEN_MAX_VERTS * 2];
    int nv, ni;
} gen_mesh;

static void gen_box(gen_mesh *g, float x0, float y0, float z0, float x1, float y1, float z1)
{
    static const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    if (g->nv + 24 > GEN_MAX_VERTS) return;
    for (int f = 0; f < 6; f++) {
        int a = f / 2, sgn = f % 2 == 0;           /* axis and side of this face */
        int u = (a + 1) % 3, w = (a + 2) % 3;
        float lo[3] = {x0, y0, z0}, hi[3] = {x1, y1, z1};
        int base = g->nv;
        for (int c = 0; c < 4; c++) {
            float p[3];
            p[a] = sgn ? hi[a] : lo[a];
            p[u] = (c == 1 || c == 2) ? hi[u] : lo[u];
            p[w] = (c >= 2) ? hi[w] : lo[w];
            vertex *vx = &g->v[g->nv++];
            memcpy(vx->pos, p, sizeof p);
            memcpy(vx->normal, n[f], sizeof vx->normal);
            vx->uv[0] = vx->uv[1] = 0;
        }
        uint32_t q[6] = {0, 1, 2, 0, 2, 3};
        for (int k = 0; k < 6; k++) g->idx[g->ni++] = (uint32_t)base + q[k];
    }
}

static void upload_gen(gpu_model *m, const gen_mesh *g, float r, float gr, float b)
{
    if (!g->ni || m->mesh_count == 16) return;
    int k = m->mesh_count;
    gpu_mesh *gm = &m->meshes[m->mesh_count++];
    gm->material = k;
    gm->index_count = g->ni;
    m->textured[k] = 0;
    m->flat[k][0] = r; m->flat[k][1] = gr; m->flat[k][2] = b;
    glGenVertexArrays(1, &gm->vao);
    glBindVertexArray(gm->vao);
    glGenBuffers(1, &gm->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, gm->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(vertex) * (size_t)g->nv), g->v, GL_STATIC_DRAW);
    glGenBuffers(1, &gm->ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gm->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(sizeof(uint32_t) * (size_t)g->ni), g->idx, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vertex), (void *)offsetof(vertex, pos));
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(vertex), (void *)offsetof(vertex, normal));
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(vertex), (void *)offsetof(vertex, uv));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
}

/* Placeholder shipyard (+Z faces the sea): ground, quay, drydock, slipway with a hull
 * on it, cranes, workshops, HQ with a radar mast, ammunition bunkers; gun mounts from
 * the class so the coastal batteries are drawn and trained like turrets. */
static gpu_model *shipyard_model(renderer *r, const ship_class *cls)
{
    for (int i = 0; i < r->model_count; i++)
        if (!strcmp(r->models[i].path, cls->model[0])) return &r->models[i];
    if (r->model_count == MAX_MODELS) return NULL;
    gpu_model *m = &r->models[r->model_count++];
    memset(m, 0, sizeof *m);
    snprintf(m->path, sizeof m->path, "%s", cls->model[0]);
    m->ok = 1;
    static gen_mesh ground, concrete, dark, steel, brick, bunker;
    memset(&ground, 0, sizeof ground); memset(&concrete, 0, sizeof concrete); memset(&dark, 0, sizeof dark);
    memset(&steel, 0, sizeof steel); memset(&brick, 0, sizeof brick); memset(&bunker, 0, sizeof bunker);
    gen_box(&ground, -240, -6, -360, 240, 5, 260);
    gen_box(&concrete, -240, -6, 260, 240, 7, 320);                  /* quay */
    gen_box(&dark, -130, -6, -170, -20, 2, 262);                     /* drydock basin */
    gen_box(&steel, -105, 2, -120, -45, 16, 200);                    /* a hull in the dock */
    gen_box(&concrete, 30, 5, -60, 150, 8, 262);                     /* slipway */
    gen_box(&steel, 60, 8, -30, 120, 24, 210);                       /* a hull building */
    for (int k = 0; k < 4; k++) {                                    /* cranes */
        float x = k < 2 ? -150 + (float)k * 140 : 20 + (float)(k - 2) * 140, z = k % 2 ? 40 : 180;
        gen_box(&steel, x - 3, 5, z - 3, x + 3, 70, z + 3);
        gen_box(&steel, x - 3, 64, z - 40, x + 3, 70, z + 30);
    }
    for (int k = 0; k < 5; k++)                                      /* workshops */
        gen_box(&brick, -220 + (float)k * 90, 5, -330, -150 + (float)k * 90, 22 + (float)(k % 3) * 8, -230);
    gen_box(&brick, -20, 5, -10, 20, 40, 40);                        /* headquarters */
    gen_box(&steel, -2, 40, 10, 2, 70, 14);                          /* radar mast */
    gen_box(&steel, -10, 66, 8, 10, 70, 16);
    for (int k = 0; k < 4; k++)                                      /* ammunition bunkers */
        gen_box(&bunker, -210 + (float)(k % 2) * 60, 5, -200 + (float)(k / 2) * 50, -170 + (float)(k % 2) * 60, 14,
                -170 + (float)(k / 2) * 50);
    for (int q = 0; q < cls->mount_count; q++)                       /* battery emplacements */
        gen_box(&concrete, cls->mounts[q].pos[0] - 25, 5, cls->mounts[q].pos[2] - 25, cls->mounts[q].pos[0] + 25, 12,
                cls->mounts[q].pos[2] + 25);
    profile_begin(&m->prof, -360, 320);
    const gen_mesh *all[] = {&ground, &concrete, &dark, &steel, &brick, &bunker};
    for (int q = 0; q < 6; q++)
        for (int k = 0; k < all[q]->nv; k++) profile_add(&m->prof, all[q]->v[k].pos);
    profile_end(&m->prof);
    upload_gen(m, &ground, 0.36f, 0.42f, 0.27f);
    upload_gen(m, &concrete, 0.62f, 0.62f, 0.6f);
    upload_gen(m, &dark, 0.12f, 0.16f, 0.2f);
    upload_gen(m, &steel, 0.38f, 0.4f, 0.43f);
    upload_gen(m, &brick, 0.52f, 0.36f, 0.28f);
    upload_gen(m, &bunker, 0.3f, 0.34f, 0.26f);
    m->bbox_min[0] = -240; m->bbox_max[0] = 240;
    m->bbox_min[2] = -360; m->bbox_max[2] = 320;
    m->bbox_max[1] = 70;
    m->radius = 420;
    for (int q = 0; q < cls->mount_count && m->mts.count < 32; q++) {
        mount_point *p = &m->mts.points[m->mts.count++];
        snprintf(p->name, sizeof p->name, "%s", cls->mounts[q].name);
        memcpy(p->pos, cls->mounts[q].pos, sizeof p->pos);
        p->pos[1] = 12;
        float a = cls->mounts[q].arc_center, c = cosf(a), sn = sinf(a);
        float rot[9] = {c, 0, sn, 0, 1, 0, -sn, 0, c};
        memcpy(p->rot, rot, sizeof rot);
    }
    return m;
}

int renderer_load_class(renderer *r, const ship_class *cls)
{
    if (cls->model[0][0] == '@') {
        get_model(r, cls->gun_model);
        return shipyard_model(r, cls) ? 0 : -1;
    }
    if (cls->hull_type == 1)
        for (int st = 1; st <= 3; st++) plane_model(r, cls->nation, st);
    int ok = get_model(r, cls->model[0]) != NULL;
    get_model(r, cls->model[1]);
    get_model(r, cls->model[2]);
    get_model(r, cls->gun_model);
    return ok ? 0 : -1;
}

renderer *renderer_create(const char *files_dir)
{
    renderer *r = calloc(1, sizeof *r);
    snprintf(r->files_dir, sizeof r->files_dir, "%s", files_dir);
    char tdir[512];
    snprintf(tdir, sizeof tdir, "%s/textures", files_dir);
    index_textures(r, tdir);
    if (!r->tex_count) fprintf(stderr, "no textures under %s (run tools/imp_extract.py)\n", tdir);

    r->ship_prog = program(SHIP_VS, SHIP_FS);
    r->sea_prog = program(SEA_VS, SEA_FS);
    r->sky_prog = program(SKY_VS, SKY_FS);
    r->line_prog = program(LINE_VS, LINE_FS);

    /* Sea: a grid with fine cells near the centre and coarser ones further out. */
    enum { N = 257 };
    static float xz[N * N * 2];
    for (int j = 0; j < N; j++)
        for (int i = 0; i < N; i++) {
            float u = (float)i / (N - 1) * 2 - 1, v = (float)j / (N - 1) * 2 - 1;
            float su = u * u * u * 0.85f + u * 0.15f, sv = v * v * v * 0.85f + v * 0.15f;
            xz[(j * N + i) * 2] = su * 30000.0f;
            xz[(j * N + i) * 2 + 1] = sv * 30000.0f;
        }
    static uint32_t idx[(N - 1) * (N - 1) * 6];
    int k = 0;
    for (int j = 0; j < N - 1; j++)
        for (int i = 0; i < N - 1; i++) {
            uint32_t a = (uint32_t)(j * N + i), b = a + 1, c = a + N, d = c + 1;
            idx[k++] = a; idx[k++] = c; idx[k++] = b;
            idx[k++] = b; idx[k++] = c; idx[k++] = d;
        }
    r->sea_index_count = k;
    GLuint vbo;
    glGenVertexArrays(1, &r->sea_vao);
    glBindVertexArray(r->sea_vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof xz, xz, GL_STATIC_DRAW);
    glGenBuffers(1, &r->sea_ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r->sea_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof idx, idx, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, 0);
    glEnableVertexAttribArray(0);

    glGenVertexArrays(1, &r->sky_vao);
    glGenVertexArrays(1, &r->line_vao);
    glBindVertexArray(r->line_vao);
    glGenBuffers(1, &r->line_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->line_vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 28, 0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 28, (void *)12);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    /* UI quads: (x, y, u, v, r, g, b, a). */
    r->ui_prog = program(UI_VS, UI_FS);
    glGenVertexArrays(1, &r->ui_vao);
    glBindVertexArray(r->ui_vao);
    glGenBuffers(1, &r->ui_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->ui_vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 32, 0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 32, (void *)8);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 32, (void *)16);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);
    glGenTextures(1, &r->font_tex);
    glBindTexture(GL_TEXTURE_2D, r->font_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, FONT_ATLAS_W, FONT_ATLAS_H, 0, GL_RED, GL_UNSIGNED_BYTE, FONT_ATLAS);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    return r;
}

/* ---- UI batch ------------------------------------------------------------------------ */

#define UI_MAX_VERTS 24576
static float g_ui[UI_MAX_VERTS * 8];
static int g_ui_count;

static void ui_vert(float x, float y, float u, float v, const float *c)
{
    float *p = &g_ui[g_ui_count++ * 8];
    p[0] = x; p[1] = y; p[2] = u; p[3] = v;
    p[4] = c[0]; p[5] = c[1]; p[6] = c[2]; p[7] = c[3];
}

static void ui_quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, const float *c)
{
    if (g_ui_count + 6 > UI_MAX_VERTS) return;
    ui_vert(x0, y0, u0, v0, c); ui_vert(x1, y0, u1, v0, c); ui_vert(x1, y1, u1, v1, c);
    ui_vert(x0, y0, u0, v0, c); ui_vert(x1, y1, u1, v1, c); ui_vert(x0, y1, u0, v1, c);
}

void ui_rect(float x0, float y0, float x1, float y1, float r, float g, float b, float a)
{
    const float c[4] = {r, g, b, a};
    ui_quad(x0, y0, x1, y1, -1, -1, -1, -1, c);
}

float ui_text(float x, float y, const char *str, float r, float g, float b, float a)
{
    const float c[4] = {r, g, b, a};
    float x0 = x;
    for (const unsigned char *p = (const unsigned char *)str; *p; p++) {
        if (*p >= 32 && *p < 127) {
            int i = *p - 32, cx = i % FONT_COLS * FONT_CELL_W, cy = i / FONT_COLS * FONT_CELL_H;
            ui_quad(x, y, x + FONT_CELL_W, y + FONT_CELL_H, (float)cx / FONT_ATLAS_W, (float)cy / FONT_ATLAS_H,
                    (float)(cx + FONT_CELL_W) / FONT_ATLAS_W, (float)(cy + FONT_CELL_H) / FONT_ATLAS_H, c);
        }
        x += FONT_CELL_W;
    }
    return x - x0;
}

int ui_mark(void) { return g_ui_count; }

/* Move the bottom edge of the quad that ui_rect emitted at vertex mark. */
void ui_set_bottom(int mark, float y1)
{
    if (mark < 0 || mark + 6 > g_ui_count) return;
    g_ui[(mark + 2) * 8 + 1] = g_ui[(mark + 4) * 8 + 1] = g_ui[(mark + 5) * 8 + 1] = y1;
}

float ui_text_width(const char *str) { return (float)(strlen(str) * FONT_CELL_W); }
float ui_line_height(void) { return FONT_CELL_H; }

void ui_flush(renderer *r, int w, int h)
{
    if (!g_ui_count) return;
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(r->ui_prog);
    glUniform2f(glGetUniformLocation(r->ui_prog, "u_screen"), (float)w, (float)h);
    glUniform1i(glGetUniformLocation(r->ui_prog, "u_font"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, r->font_tex);
    glBindVertexArray(r->ui_vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->ui_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(float) * 8 * (size_t)g_ui_count), g_ui, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, g_ui_count);
    g_ui_count = 0;
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

void renderer_destroy(renderer *r)
{
    free(r);   /* GL objects go with the context */
}

/* ---- drawing ------------------------------------------------------------------- */

mat4 camera_viewproj(const camera *c, int w, int h, vec3 *eye_out)
{
    vec3 eye = v3add(c->target, v3scale(v3(sinf(c->yaw) * cosf(c->pitch), sinf(c->pitch),
                                           cosf(c->yaw) * cosf(c->pitch)), c->dist));
    if (eye_out) *eye_out = eye;
    mat4 proj = m4perspective(0.9f, (float)w / (float)(h > 0 ? h : 1), 5.0f, 60000.0f);
    return m4mul(proj, m4lookat(eye, c->target, v3(0, 1, 0)));
}

static void draw_model(renderer *r, const gpu_model *m, mat4 model, vec3 tint)
{
    glUniformMatrix4fv(glGetUniformLocation(r->ship_prog, "u_model"), 1, GL_FALSE, model.m);
    glUniform4f(glGetUniformLocation(r->ship_prog, "u_tint"), tint.x, tint.y, tint.z, 1);
    for (int i = 0; i < m->mesh_count; i++) {
        const gpu_mesh *g = &m->meshes[i];
        glUniform1i(glGetUniformLocation(r->ship_prog, "u_textured"), m->textured[g->material]);
        glUniform3f(glGetUniformLocation(r->ship_prog, "u_flat"), m->flat[g->material][0], m->flat[g->material][1],
                    m->flat[g->material][2]);
        glBindTexture(GL_TEXTURE_2D, m->textures[g->material]);
        glBindVertexArray(g->vao);
        glDrawElements(GL_TRIANGLES, g->index_count, GL_UNSIGNED_INT, 0);
    }
}

/* Line batch: (x, y, z, r, g, b, a) per vertex. */
typedef struct { float v[7]; } line_vertex;
static line_vertex g_lines[16384];
static int g_line_count;

static void line(vec3 a, vec3 b, float cr, float cg, float cb, float ca)
{
    if (g_line_count + 2 > 16384) return;
    line_vertex p = {{a.x, a.y, a.z, cr, cg, cb, ca}}, q = {{b.x, b.y, b.z, cr, cg, cb, ca}};
    g_lines[g_line_count++] = p;
    g_lines[g_line_count++] = q;
}

static void flush_lines(renderer *r, mat4 vp)
{
    if (!g_line_count) return;
    glUseProgram(r->line_prog);
    glUniformMatrix4fv(glGetUniformLocation(r->line_prog, "u_vp"), 1, GL_FALSE, vp.m);
    glBindVertexArray(r->line_vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->line_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(line_vertex) * (size_t)g_line_count), g_lines, GL_STREAM_DRAW);
    glDrawArrays(GL_LINES, 0, g_line_count);
    g_line_count = 0;
}

/* ---- cross-section damage panel (screen space) ------------------------------------- */

static vec3 health_colour(float hp)
{
    return hp <= 0 ? v3(0.15f, 0.15f, 0.15f) : hp < 0.3f ? v3(0.95f, 0.25f, 0.2f)
         : hp < 0.7f ? v3(0.95f, 0.8f, 0.2f) : v3(0.3f, 0.9f, 0.35f);
}

const ship_profile *renderer_profile(renderer *r, const ship_class *c)
{
    const gpu_model *m = get_model(r, c->model[0]);
    return m && m->prof.ok ? &m->prof : NULL;
}

/* The ship's own side profile, stern left and bow right, cut into compartments:
 * hull tinted by structure, water rising from the keel, fire, wrecked compartments
 * dark; superstructure above the deck line. detail adds modules, crews, and port and
 * starboard water strips under the hull. */
void ui_ship_profile(renderer *r, const ship *s, float x, float y, float w, float h, int detail, float t)
{
    const ship_class *c = s->cls;
    const ship_profile *p = renderer_profile(r, c);
    int n = c->comp_count;
    if (!p || n == 0) {
        ui_rect(x, y + h * 0.4f, x + w, y + h, 0.4f, 0.42f, 0.45f, 1);
        return;
    }
    float L = c->half_length, zr = p->z1 - p->z0;
    /* The models stop near the waterline; draw the hull down to a keel at ~70% of the
     * half-beam (a typical WWII draft), rising toward bow and stern. */
    float keel = fminf(p->ymin, is_shore(s) ? p->ymin : -0.7f * c->half_beam);
    float yr = p->ymax - keel;
    float strips = detail ? 14 : 0;
    float sx = w / zr, sy = fminf(sx * 1.6f, (h - strips) / (yr > 1 ? yr : 1));
    float base = y + h - strips;
    #define PX(z) (x + ((z) - p->z0) * sx)
    #define PY(v) (base - ((v) - keel) * sy)
    #define KEEL(b) (keel + (fminf(p->deck, 0) - keel) * powf(fabsf(((float)(b) + 0.5f) / PROFILE_BINS * 2 - 1), 6))
    float bw = w / PROFILE_BINS + 0.6f;
    for (int b = 0; b < PROFILE_BINS; b++) {
        float z = p->z0 + ((float)b + 0.5f) / PROFILE_BINS * zr;
        int k = 0;
        while (k < n - 1 && z > c->comp_z[k + 1]) k++;
        if (z < -L - 1 || z > L + 1) k = z < 0 ? 0 : n - 1;
        const compartment *cp = &s->comp[k];
        float xb = x + (float)b * (w / PROFILE_BINS);
        float top = p->top[b], bot = KEEL(b), deck = fminf(top, p->deck);
        float st = cp->structure, alive = s->alive ? 1 : 0.4f;
        /* Hull */
        float hr = (0.32f + 0.35f * (1 - st)) * alive, hg = (0.36f - 0.12f * (1 - st)) * alive, hb = (0.4f - 0.18f * (1 - st)) * alive;
        if (st < 0.05f) { hr = 0.25f; hg = 0.08f; hb = 0.06f; }
        ui_rect(xb, PY(deck), xb + bw, PY(bot), hr, hg, hb, 1);
        /* Water: the average of both sides, from the keel up. */
        float wl = (cp->flood[0] + cp->flood[1]) / 2;
        if (wl > 0) ui_rect(xb, PY(bot + (deck - bot) * wl), xb + bw, PY(bot), 0.16f, 0.42f, 0.88f, 0.95f);
        /* Superstructure */
        if (top > deck + 0.5f) ui_rect(xb, PY(top), xb + bw, PY(deck), 0.5f * alive, 0.52f * alive, 0.55f * alive, 1);
        if (cp->fire > 0) {
            float flick = 0.75f + 0.25f * sinf(t * 17 + (float)k * 2.1f + (float)b * 0.7f);
            float fh = (top - bot) * 0.6f * cp->fire * flick;
            ui_rect(xb, PY(deck + fh * 0.5f), xb + bw, PY(deck - fh * 0.5f), 1.0f, 0.45f + 0.2f * flick, 0.1f, 0.85f);
        }
        if (detail) {                                  /* port and starboard water under the hull */
            ui_rect(xb, base + 3, xb + bw, base + 7, 0.1f + 0.1f * cp->flood[0], 0.12f + 0.3f * cp->flood[0], 0.15f + 0.7f * cp->flood[0], 1);
            ui_rect(xb, base + 9, xb + bw, base + 13, 0.1f + 0.1f * cp->flood[1], 0.12f + 0.3f * cp->flood[1], 0.15f + 0.7f * cp->flood[1], 1);
        }
    }
    /* Bulkheads */
    for (int k = 1; k < n; k++) {
        float bx = PX(c->comp_z[k]);
        int b = (int)((c->comp_z[k] - p->z0) / zr * PROFILE_BINS);
        b = b < 0 ? 0 : b >= PROFILE_BINS ? PROFILE_BINS - 1 : b;
        ui_rect(bx - 0.5f, PY(fminf(p->top[b], p->deck)), bx + 0.5f, PY(KEEL(b)), 0.08f, 0.09f, 0.1f, 0.9f);
    }
    /* Modules: on deck (turrets), inside the hull (magazines, engines, steering), on the
     * superstructure (bridge, radar, AA). Lost ones are black. */
    for (int m = 0; m < c->module_count; m++) {
        const module_def *d = &c->modules[m];
        float z = d->kind == MOD_TURRET ? c->mounts[d->mount].pos[2] : (c->comp_z[d->comp] + c->comp_z[d->comp + 1]) / 2;
        if (d->kind == MOD_TURRET && !detail && c->mounts[d->mount].bat != BAT_MAIN) continue;
        if (!detail && d->kind != MOD_TURRET) continue;
        int b = (int)((z - p->z0) / zr * PROFILE_BINS);
        b = b < 0 ? 0 : b >= PROFILE_BINS ? PROFILE_BINS - 1 : b;
        float mx = PX(z), deck = fminf(p->top[b], p->deck), hp = s->module_hp[m];
        vec3 col = health_colour(hp);
        float r0 = detail ? 4 : 2;
        switch (d->kind) {
        case MOD_TURRET: {
            float yy = PY(deck);
            if (c->mounts[d->mount].bat == BAT_MAIN) ui_rect(mx - r0 * 1.6f, yy - r0 * 1.6f, mx + r0 * 1.6f, yy, col.x, col.y, col.z, 1);
            else ui_rect(mx - r0, yy - r0 - (detail ? 6 : 0), mx + r0, yy - (detail ? 6 : 0), col.x, col.y, col.z, 1);
            break;
        }
        case MOD_MAGAZINE:
            ui_rect(mx - 5, PY(KEEL(b)) - 16, mx + 5, PY(KEEL(b)) - 7, 0.4f + col.x * 0.5f, col.y * 0.35f, col.z * 0.2f, 1);
            if (s->comp[d->comp].mag_flooded) ui_rect(mx - 5, PY(KEEL(b)) - 16, mx + 5, PY(KEEL(b)) - 13, 0.3f, 0.6f, 1, 1);
            break;
        case MOD_ENGINE:
            ui_rect(mx - 8, PY(KEEL(b)) - 6, mx + 8, PY(KEEL(b)) - 1, col.x, col.y, col.z, 1);
            break;
        case MOD_STEERING:
            ui_rect(mx - 3, PY(KEEL(b)) - 7, mx + 3, PY(KEEL(b)) - 1, col.x, col.y, col.z, 1);
            break;
        case MOD_BRIDGE: case MOD_RADAR: {
            float yy = PY(p->top[b]) - (d->kind == MOD_RADAR ? 8 : 0);
            ui_rect(mx - 4, yy - 4, mx + 4, yy + 2, col.x, col.y, col.z, 1);
            break;
        }
        case MOD_AA: case MOD_HANGAR_LIFT: case MOD_DRYDOCK: case MOD_SLIPWAY:
            ui_rect(mx - 3, PY(deck) - 12, mx + 3, PY(deck) - 7, col.x, col.y, col.z, 1);
            break;
        case MOD_FLIGHT_DECK:
            ui_rect(x, PY(p->deck) - 2, x + w, PY(p->deck), col.x, col.y, col.z, 1);
            break;
        default: break;
        }
    }
    if (detail)                                           /* damage-control teams */
        for (int k = 0; k < n; k++)
            for (int q = 0; q < s->comp[k].crews; q++) {
                float cx = PX(c->comp_z[k]) + 3 + (float)q * 6;
                int b = (int)((c->comp_z[k] - p->z0) / zr * PROFILE_BINS) + 1;
                b = b < 0 ? 0 : b >= PROFILE_BINS ? PROFILE_BINS - 1 : b;
                float yy = (PY(fminf(p->top[b], p->deck)) + PY(KEEL(b))) / 2;
                ui_rect(cx, yy - 2, cx + 4, yy + 2, 1, 1, 1, 1);
            }
    #undef PX
    #undef PY
    #undef KEEL
}

/* The damage panel: the profile at full size plus list, water and condition gauges. */
static void damage_panel(renderer *r, const ship *s, int w, int h, float t)
{
    float pw = fminf(760, (float)w - 40), x0 = ((float)w - pw) / 2, y0 = (float)h - 170, ph = 150;
    ui_rect(x0 - 10, y0 - 10, x0 + pw + 10, y0 + ph + 10, 0.04f, 0.06f, 0.08f, 0.82f);
    ui_ship_profile(r, s, x0 + 20, y0 + 4, pw - 165, 108, 1, t);
    float gx = x0 + pw - 50, gy = y0 + 50;
    float ca = cosf(s->list), sa = sinf(s->list);
    for (int q = -25; q <= 25; q += 2) ui_rect(gx + ca * (float)q - 1, gy + sa * (float)q - 1, gx + ca * (float)q + 1, gy + sa * (float)q + 1, 0.9f, 0.9f, 0.9f, 1);
    ui_rect(gx - 30, gy - 0.5f, gx + 30, gy + 0.5f, 0.4f, 0.4f, 0.45f, 1);
    const ship_class *c = s->cls;
    float wf = clampf(s->water / c->reserve_buoyancy, 0, 1), cf = s->hp / c->max_health;
    ui_rect(gx - 30, gy + 30, gx + 30, gy + 38, 0.1f, 0.1f, 0.12f, 1);
    ui_rect(gx - 30, gy + 30, gx - 30 + 60 * wf, gy + 38, 0.2f, 0.45f, 0.9f, 1);
    vec3 cc = health_colour(cf);
    ui_rect(gx - 30, gy + 44, gx + 30, gy + 52, 0.1f, 0.1f, 0.12f, 1);
    ui_rect(gx - 30, gy + 44, gx - 30 + 60 * cf, gy + 52, cc.x, cc.y, cc.z, 1);
    if (s->jammed) ui_rect(gx - 32, gy + 56, gx + 32, gy + 58, 0.95f, 0.25f, 0.2f, 1);
}

void renderer_draw(renderer *r, const camera *cam, int w, int h, const world *wd,
                   float alpha, const unsigned char *selected, const drag_box *box, float t)
{
    vec3 eye;
    mat4 vp = camera_viewproj(cam, w, h, &eye), inv;
    m4invert(vp, &inv);
    glViewport(0, 0, w, h);
    glClearColor(0.5f, 0.6f, 0.7f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* Sky */
    glDisable(GL_DEPTH_TEST);
    glUseProgram(r->sky_prog);
    glUniformMatrix4fv(glGetUniformLocation(r->sky_prog, "u_inv_vp"), 1, GL_FALSE, inv.m);
    glUniform3f(glGetUniformLocation(r->sky_prog, "u_eye"), eye.x, eye.y, eye.z);
    glBindVertexArray(r->sky_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);

    /* Sea, centred under the camera target (snapped so the grid doesn't swim). */
    glUseProgram(r->sea_prog);
    glUniformMatrix4fv(glGetUniformLocation(r->sea_prog, "u_vp"), 1, GL_FALSE, vp.m);
    glUniform3f(glGetUniformLocation(r->sea_prog, "u_eye"), eye.x, eye.y, eye.z);
    glUniform2f(glGetUniformLocation(r->sea_prog, "u_origin"),
                floorf(cam->target.x / 400.0f) * 400.0f, floorf(cam->target.z / 400.0f) * 400.0f);
    glUniform1f(glGetUniformLocation(r->sea_prog, "u_t"), t);
    for (int i = 0; i < 4; i++) {
        char name[16];
        snprintf(name, sizeof name, "u_w[%d]", i);
        glUniform4f(glGetUniformLocation(r->sea_prog, name), WAVES[i].dx, WAVES[i].dz, WAVES[i].length, WAVES[i].amp);
        snprintf(name, sizeof name, "u_w2[%d]", i);
        glUniform2f(glGetUniformLocation(r->sea_prog, name), WAVES[i].speed, 0);
    }
    glBindVertexArray(r->sea_vao);
    glDrawElements(GL_TRIANGLES, r->sea_index_count, GL_UNSIGNED_INT, 0);

    /* Ships */
    glUseProgram(r->ship_prog);
    glUniformMatrix4fv(glGetUniformLocation(r->ship_prog, "u_vp"), 1, GL_FALSE, vp.m);
    glUniform3f(glGetUniformLocation(r->ship_prog, "u_eye"), eye.x, eye.y, eye.z);
    glUniform1i(glGetUniformLocation(r->ship_prog, "u_tex"), 0);
    glActiveTexture(GL_TEXTURE0);
    float now = (float)wd->tick - 1 + alpha;          /* display time, in ticks */
    for (int i = 0; i < wd->count; i++) {
        const ship *s = &wd->ships[i];
        int shore = s->cls->hull_type == HULL_SHIPYARD;
        if (!s->alive && s->sink > 90 && !shore) continue;   /* gone under */
        int stage = s->stage < 1 ? 1 : s->stage > 3 ? 3 : s->stage;
        const gpu_model *m = get_model(r, s->cls->model[stage - 1]);
        if (!m) m = get_model(r, s->cls->model[0]);
        if (!m) continue;
        float x = s->prev_x + (s->x - s->prev_x) * alpha, z = s->prev_z + (s->z - s->prev_z) * alpha;
        float hd = s->prev_heading + wrap_angle(s->heading - s->prev_heading) * alpha;
        float y = s->cls->submerged ? -6.0f : shore ? 0 : sea_height(x, z, t) * 0.5f;
        y -= s->water * 25;                              /* lower in the water as she floods */
        float roll = s->list, pitch = s->trim;
        if (!s->alive && !shore) {
            y -= fminf(s->sink * 0.6f, 50);              /* settles over about 80 s */
            if (s->cause == 2) roll += (s->list >= 0 ? 1 : -1) * fminf(s->sink * 0.08f, 2.6f);   /* rolls over */
            else pitch += (s->trim >= 0 ? 1 : -1) * fminf(s->sink * 0.006f, 0.35f);              /* goes down by an end */
        }
        /* m4roll turns +X down for positive angles; list > 0 is to starboard (+X). */
        mat4 model = m4mul(m4translate(v3(x, y, z)), m4mul(m4yaw(hd), m4mul(m4pitch(-pitch), m4roll(-roll))));
        vec3 tint = s->alive ? v3(1, 1, 1) : v3(0.35f, 0.3f, 0.3f);
        draw_model(r, m, model, tint);
        const gpu_model *gun = get_model(r, s->cls->gun_model);
        for (int k = 0; gun && k < m->mts.count; k++) {
            const mount_point *mp = &m->mts.points[k];
            if (!strchr("FRPS", mp->name[0]) || !isdigit((unsigned char)mp->name[1])) continue;
            /* Train the gun model to the sim turret with the same F1/R2... name. */
            float train = 0;
            for (int q = 0; q < s->cls->mount_count; q++) {
                const turret_mount *tm = &s->cls->mounts[q];
                if (tm->name[0] && !strncmp(mp->name, tm->name, 2)) {
                    train = wrap_angle(s->turrets[q].yaw - tm->arc_center);
                    break;
                }
            }
            mat4 gm = m4mul(model, m4mul(m4translate(v3(mp->pos[0], mp->pos[1], mp->pos[2])),
                                         m4mul(m4yaw(train), m4from_rows3(mp->rot))));
            draw_model(r, gun, gm, tint);
        }
        /* Fires: flames and a smoke column over each burning compartment. */
        for (int k = 0; k < s->cls->comp_count; k++) {
            float f = s->comp[k].fire;
            if (f <= 0 || (!s->alive && s->sink > 20)) continue;
            float cz = (s->cls->comp_z[k] + s->cls->comp_z[k + 1]) / 2;
            vec3 base = m4point(model, v3(0, 10, cz));
            float flick = 0.7f + 0.3f * sinf(t * 23 + (float)k * 1.7f);
            for (int q = 0; q < 5; q++) {
                float ox = ((float)q - 2) * 3;
                line(v3(base.x + ox, base.y, base.z), v3(base.x + ox * 0.5f, base.y + (10 + 14 * f) * flick, base.z), 1, 0.45f + 0.2f * flick, 0.1f, 0.95f);
            }
            for (int q = 0; q < 4; q++) {                 /* smoke drifts downwind (+X) as it rises */
                float h0 = (float)q * 35 * f, h1 = h0 + 35 * f;
                line(v3(base.x + h0 * 0.4f, base.y + 15 + h0, base.z), v3(base.x + h1 * 0.4f, base.y + 15 + h1, base.z),
                     0.18f, 0.17f, 0.16f, 0.7f - 0.15f * (float)q);
                line(v3(base.x + h0 * 0.4f + 4, base.y + 15 + h0, base.z + 3), v3(base.x + h1 * 0.4f + 6, base.y + 15 + h1, base.z + 4),
                     0.22f, 0.21f, 0.2f, 0.6f - 0.13f * (float)q);
            }
        }
        if (!s->alive) continue;
        /* Selection ring, orders and ordered targets */
        if (selected && selected[i]) {
            float rad = (m->bbox_max[2] - m->bbox_min[2]) * 0.6f;
            vec3 prev = v3(x + rad, 2, z);
            for (int a = 1; a <= 48; a++) {
                float ang = (float)a / 48 * 2 * PI_F;
                vec3 p = v3(x + cosf(ang) * rad, 2, z + sinf(ang) * rad);
                line(prev, p, 0.3f, 1.0f, 0.4f, 1);
                prev = p;
            }
            vec3 from = v3(x, 3, z);
            for (int k = 0; k < s->wp_count; k++) {
                vec3 to = v3(s->wp[k].x, 3, s->wp[k].z);
                line(from, to, 0.9f, 0.9f, 0.3f, 1);
                line(v3(to.x - 25, 3, to.z), v3(to.x + 25, 3, to.z), 0.9f, 0.9f, 0.3f, 1);
                line(v3(to.x, 3, to.z - 25), v3(to.x, 3, to.z + 25), 0.9f, 0.9f, 0.3f, 1);
                from = to;
            }
            for (int b = BAT_SECONDARY; b >= BAT_MAIN; b--) {   /* main red, secondary orange */
                int tj = s->order_target[b];
                if (tj < 0 || !wd->ships[tj].alive || (b == BAT_SECONDARY && tj == s->order_target[BAT_MAIN])) continue;
                const ship *t2 = &wd->ships[tj];
                vec3 a = v3(x, 6, z), d = v3sub(v3(t2->x, 6, t2->z), a);
                for (int seg = 0; seg < 40; seg += 2)           /* dashed */
                    line(v3add(a, v3scale(d, (float)seg / 40)), v3add(a, v3scale(d, (float)(seg + 1) / 40)),
                         1, b == BAT_MAIN ? 0.2f : 0.6f, 0.15f, 0.9f);
            }
        } else if (s->side == 1) {
            float rad = (m->bbox_max[2] - m->bbox_min[2]) * 0.55f;
            for (int a = 0; a < 24; a += 2) {           /* dashed red ring: enemy */
                float a0 = (float)a / 24 * 2 * PI_F, a1 = (float)(a + 1) / 24 * 2 * PI_F;
                line(v3(x + cosf(a0) * rad, 2, z + sinf(a0) * rad), v3(x + cosf(a1) * rad, 2, z + sinf(a1) * rad), 1, 0.3f, 0.25f, 1);
            }
        }
    }

    /* Aircraft, drawn at the original's 2x scale. */
    for (int k = 0; k < wd->air_count; k++) {
        const aircraft *a = &wd->air[k];
        if (a->state != AIR_OUT && a->state != AIR_ATTACK && a->state != AIR_RETURN && a->state != AIR_FALLING) continue;
        const gpu_model *pm = plane_model(r, wd->ships[a->carrier].cls->nation, a->stage);
        if (!pm) continue;
        vec3 p = v3(a->prev_x + (a->x - a->prev_x) * alpha, a->prev_y + (a->y - a->prev_y) * alpha,
                    a->prev_z + (a->z - a->prev_z) * alpha);
        float hd = a->prev_heading + wrap_angle(a->heading - a->prev_heading) * alpha;
        mat4 m = m4mul(m4translate(p), m4mul(m4yaw(hd), m4mul(m4pitch(a->pitch), m4mul(m4roll(a->bank), m4scale(2)))));
        draw_model(r, pm, m, a->state == AIR_FALLING ? v3(0.4f, 0.35f, 0.3f) : v3(1, 1, 1));
        if (a->state == AIR_FALLING)                        /* smoke trail */
            line(p, v3(p.x - sinf(hd) * 40, p.y + 25, p.z - cosf(hd) * 40), 0.2f, 0.2f, 0.2f, 0.8f);
        if (selected && selected[a->carrier] && a->state != AIR_FALLING)   /* own carrier selected */
            line(v3(p.x, p.y, p.z), v3(p.x, 0, p.z), 0.3f, 1.0f, 0.4f, 0.25f);
    }

    /* Shells in flight (tracers) and torpedo wakes, from their analytic paths. */
    for (int k = 0; k < MAX_PROJECTILES; k++) {
        const projectile *p = &wd->proj[k];
        if (!p->active) continue;
        float tt = (now - (float)p->tick0) * SIM_DT;
        if (tt < 0) continue;
        if (p->ammo == AMMO_TORPEDO) {
            vec3 head = v3(p->x0 + p->vx * tt, 1.0f, p->z0 + p->vz * tt);
            float back = fminf(tt, 1.5f);
            vec3 tail = v3(head.x - p->vx * back, 1.0f, head.z - p->vz * back);
            line(tail, head, 0.85f, 0.95f, 1.0f, 0.9f);
        } else {
            float t0 = fmaxf(tt - 0.12f, 0), t1 = fminf(tt, p->flight);
            vec3 a = v3(p->x0 + p->vx * t0, p->y0 + p->vy * t0 - 0.5f * GRAVITY * t0 * t0, p->z0 + p->vz * t0);
            vec3 b = v3(p->x0 + p->vx * t1, p->y0 + p->vy * t1 - 0.5f * GRAVITY * t1 * t1, p->z0 + p->vz * t1);
            if (p->ammo == AMMO_DC || p->ammo == AMMO_BOMB) line(a, b, 0.15f, 0.15f, 0.15f, 1);
            else line(a, b, 1.0f, 0.85f, 0.4f, 1);
        }
    }

    /* Splashes, hits and flashes from the event ring. */
    for (int k = 0; k < MAX_FX; k++) {
        const fx_event *e = &wd->fx[k];
        if (e->kind == FX_NONE) continue;
        float age = (now - (float)e->tick) * SIM_DT;
        if (age < 0 || age > 4) continue;
        float fade = age < 0.6f ? 1 : 1 - (age - 0.6f) / 3.4f;
        vec3 c = v3(e->x, e->y, e->z);
        switch (e->kind) {
        case FX_SPLASH: case FX_NEAR_MISS: case FX_DC_BLAST: case FX_TORP_HIT: {
            float hmax = e->kind == FX_TORP_HIT ? 130 : e->kind == FX_DC_BLAST ? 60 : e->kind == FX_SPLASH ? 70 : 55;
            float h = hmax * fminf(age / 0.5f, 1), spread = e->kind == FX_TORP_HIT ? 14 : 6;
            for (int q = 0; q < 6; q++) {
                float ang = (float)q / 6 * 2 * PI_F + (float)k;
                vec3 base = v3(c.x + cosf(ang) * spread, 0, c.z + sinf(ang) * spread);
                line(base, v3(base.x + cosf(ang) * spread * 0.6f, h * (0.7f + 0.3f * (float)(q & 1)), base.z + sinf(ang) * spread * 0.6f),
                     0.95f, 0.97f, 1.0f, 0.85f * fade);
            }
            if (e->kind != FX_TORP_HIT) break;
        }
            /* a torpedo hit also burns */
            /* fall through */
        case FX_HIT: {
            if (age < 0.7f) {
                float len = 30 * fminf(age / 0.25f, 1);
                for (int q = 0; q < 10; q++) {
                    float ang = (float)q / 10 * 2 * PI_F, up = (float)(q % 3) * 0.4f;
                    line(c, v3(c.x + cosf(ang) * len, c.y + len * up, c.z + sinf(ang) * len), 1, 0.55f, 0.15f, 1 - age / 0.7f);
                }
            }
            line(v3(c.x, c.y, c.z), v3(c.x + age * 6, c.y + age * 30, c.z), 0.25f, 0.23f, 0.22f, 0.8f * fade);
            line(v3(c.x + 3, c.y, c.z), v3(c.x + age * 8 + 3, c.y + age * 26, c.z + 2), 0.25f, 0.23f, 0.22f, 0.8f * fade);
            break;
        }
        case FX_FLAK:                                     /* dark burst at altitude */
            if (age < 2.5f) {
                float rr = 4 + age * 6;
                for (int q = 0; q < 6; q++) {
                    float ang = (float)q / 6 * 2 * PI_F;
                    line(v3(c.x + cosf(ang) * rr, c.y + sinf(ang) * rr, c.z), v3(c.x + cosf(ang + 1) * rr, c.y + sinf(ang + 1) * rr, c.z),
                         0.15f, 0.14f, 0.13f, 0.9f * (1 - age / 2.5f));
                }
                if (age < 0.15f) line(v3(c.x - 3, c.y, c.z), v3(c.x + 3, c.y, c.z), 1, 0.7f, 0.3f, 1);
            }
            break;
        case FX_AIR_GUN:
            if (age < 0.15f) line(c, v3(c.x, c.y + 3, c.z), 1, 0.9f, 0.5f, 1);
            break;
        case FX_PLANE_DOWN:
            if (age < 0.5f)
                for (int q = 0; q < 8; q++) {
                    float ang = (float)q / 8 * 2 * PI_F;
                    line(c, v3(c.x + cosf(ang) * 15 * age / 0.5f, c.y + sinf(ang) * 15 * age / 0.5f, c.z), 1, 0.5f, 0.1f, 1 - age / 0.5f);
                }
            break;
        case FX_RICOCHET:
            if (age < 0.3f)
                for (int q = 0; q < 4; q++) {
                    float ang = (float)q * 1.6f + (float)k;
                    line(c, v3(c.x + cosf(ang) * 18 * age / 0.3f, c.y + 10 * age / 0.3f, c.z + sinf(ang) * 18 * age / 0.3f), 1, 0.95f, 0.7f, 1 - age / 0.3f);
                }
            break;
        case FX_MAGAZINE: {                               /* the column and the fireball */
            float hh = 400 * fminf(age / 1.2f, 1);
            for (int q = 0; q < 16; q++) {
                float ang = (float)q / 16 * 2 * PI_F;
                float r0 = 20 + 60 * fminf(age, 1);
                line(v3(c.x + cosf(ang) * 10, 0, c.z + sinf(ang) * 10), v3(c.x + cosf(ang) * r0, hh * (0.7f + 0.3f * (float)(q & 1)), c.z + sinf(ang) * r0),
                     age < 0.8f ? 1.0f : 0.3f, age < 0.8f ? 0.6f : 0.28f, age < 0.8f ? 0.2f : 0.26f, fade);
            }
            break;
        }
        case FX_MUZZLE:
            if (age < 0.25f)
                for (int q = 0; q < 6; q++) {
                    float ang = (float)q / 6 * 2 * PI_F;
                    line(c, v3(c.x + cosf(ang) * 10, c.y + 4 + (float)(q & 1) * 6, c.z + sinf(ang) * 10), 1, 0.8f, 0.3f, 1 - age / 0.25f);
                }
            break;
        default:
            break;
        }
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    flush_lines(r, vp);

    /* Health bars in screen space above each ship afloat. */
    mat4 ortho = m4identity();
    ortho.m[0] = 2.0f / (float)w;
    ortho.m[5] = -2.0f / (float)h;
    ortho.m[12] = -1;
    ortho.m[13] = 1;
    for (int i = 0; i < wd->count; i++) {
        const ship *s = &wd->ships[i];
        if (!s->alive) continue;
        float x = s->prev_x + (s->x - s->prev_x) * alpha, z = s->prev_z + (s->z - s->prev_z) * alpha;
        float cx = vp.m[0] * x + vp.m[4] * 70 + vp.m[8] * z + vp.m[12];
        float cy = vp.m[1] * x + vp.m[5] * 70 + vp.m[9] * z + vp.m[13];
        float cw = vp.m[3] * x + vp.m[7] * 70 + vp.m[11] * z + vp.m[15];
        if (cw <= 0) continue;
        float sx = (cx / cw * 0.5f + 0.5f) * (float)w, sy = (1 - (cy / cw * 0.5f + 0.5f)) * (float)h;
        float f = s->hp / s->cls->max_health, half = 20;
        vec3 col = f > 2.0f / 3 ? v3(0.3f, 0.9f, 0.3f) : f > 1.0f / 3 ? v3(0.95f, 0.8f, 0.2f) : v3(0.95f, 0.25f, 0.2f);
        if (s->side == 1) col = v3scale(col, 0.85f);
        for (int row = 0; row < 3; row++) {
            float yy = sy + (float)row;
            line(v3(sx - half - 1, yy, 0), v3(sx + half + 1, yy, 0), 0.05f, 0.05f, 0.05f, 0.7f);
            line(v3(sx - half, yy, 0), v3(sx - half + 2 * half * f, yy, 0), col.x, col.y, col.z, 1);
        }
    }
    flush_lines(r, ortho);
    for (int i = 0; selected && i < wd->count; i++)
        if (selected[i] && wd->ships[i].alive) {
            damage_panel(r, &wd->ships[i], w, h, t);
            break;
        }

    /* Drag box in screen space */
    if (box && box->active) {
        vec3 a = v3((float)box->x0, (float)box->y0, 0), b = v3((float)box->x1, (float)box->y0, 0);
        vec3 c = v3((float)box->x1, (float)box->y1, 0), d = v3((float)box->x0, (float)box->y1, 0);
        line(a, b, 0.4f, 1, 0.5f, 1); line(b, c, 0.4f, 1, 0.5f, 1);
        line(c, d, 0.4f, 1, 0.5f, 1); line(d, a, 0.4f, 1, 0.5f, 1);
        flush_lines(r, ortho);
    }
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

int m4invert(mat4 a, mat4 *out)
{
    const float *m = a.m;
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (fabsf(det) < 1e-12f) return 0;
    for (int i = 0; i < 16; i++) out->m[i] = inv[i] / det;
    return 1;
}
