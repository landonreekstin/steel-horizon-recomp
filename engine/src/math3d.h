/* Small vector/matrix helpers. Column-major 4x4 matrices, as OpenGL expects. */
#ifndef MATH3D_H
#define MATH3D_H

#include <math.h>

typedef struct { float x, y, z; } vec3;
typedef struct { float m[16]; } mat4;

#define PI_F 3.14159265358979f

static inline vec3 v3(float x, float y, float z) { vec3 r = {x, y, z}; return r; }
static inline vec3 v3add(vec3 a, vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline vec3 v3sub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline vec3 v3scale(vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline float v3dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline vec3 v3cross(vec3 a, vec3 b)
{
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float v3len(vec3 a) { return sqrtf(v3dot(a, a)); }
static inline vec3 v3norm(vec3 a)
{
    float l = v3len(a);
    return l > 0 ? v3scale(a, 1.0f / l) : a;
}

static inline float wrap_angle(float a)
{
    while (a > PI_F) a -= 2 * PI_F;
    while (a < -PI_F) a += 2 * PI_F;
    return a;
}

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static inline mat4 m4identity(void)
{
    mat4 r = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
    return r;
}

static inline mat4 m4mul(mat4 a, mat4 b)
{
    mat4 r;
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
            r.m[c * 4 + rr] = s;
        }
    return r;
}

static inline vec3 m4point(mat4 a, vec3 p)
{
    return v3(a.m[0] * p.x + a.m[4] * p.y + a.m[8] * p.z + a.m[12],
              a.m[1] * p.x + a.m[5] * p.y + a.m[9] * p.z + a.m[13],
              a.m[2] * p.x + a.m[6] * p.y + a.m[10] * p.z + a.m[14]);
}

static inline mat4 m4translate(vec3 t)
{
    mat4 r = m4identity();
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}

/* Rotation about +Y; heading 0 faces +Z (the model's bow axis). */
static inline mat4 m4yaw(float a)
{
    float c = cosf(a), s = sinf(a);
    mat4 r = m4identity();
    r.m[0] = c;  r.m[2] = -s;
    r.m[8] = s;  r.m[10] = c;
    return r;
}

/* Nose (+Z) up for positive a. */
static inline mat4 m4pitch(float a)
{
    float c = cosf(a), s = sinf(a);
    mat4 r = m4identity();
    r.m[5] = c;  r.m[9] = s;
    r.m[6] = -s; r.m[10] = c;
    return r;
}

/* About +Z (the nose axis). */
static inline mat4 m4roll(float a)
{
    float c = cosf(a), s = sinf(a);
    mat4 r = m4identity();
    r.m[0] = c;  r.m[4] = -s;
    r.m[1] = s;  r.m[5] = c;
    return r;
}

static inline mat4 m4scale(float k)
{
    mat4 r = m4identity();
    r.m[0] = r.m[5] = r.m[10] = k;
    return r;
}

/* Model-space 3x3 rotation given row-major (as in .pts after Q*M). */
static inline mat4 m4from_rows3(const float *r)
{
    mat4 m = m4identity();
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 3; col++) m.m[col * 4 + row] = r[row * 3 + col];
    return m;
}

static inline mat4 m4perspective(float fovy, float aspect, float zn, float zf)
{
    float f = 1.0f / tanf(fovy * 0.5f);
    mat4 r = {{0}};
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = (zf + zn) / (zn - zf);
    r.m[11] = -1;
    r.m[14] = 2 * zf * zn / (zn - zf);
    return r;
}

static inline mat4 m4lookat(vec3 eye, vec3 at, vec3 up)
{
    vec3 f = v3norm(v3sub(at, eye)), s = v3norm(v3cross(f, up)), u = v3cross(s, f);
    mat4 r = m4identity();
    r.m[0] = s.x; r.m[4] = s.y; r.m[8] = s.z;
    r.m[1] = u.x; r.m[5] = u.y; r.m[9] = u.z;
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
    r.m[12] = -v3dot(s, eye); r.m[13] = -v3dot(u, eye); r.m[14] = v3dot(f, eye);
    return r;
}

/* General 4x4 inverse (for unprojecting mouse rays). Returns 0 if singular. */
int m4invert(mat4 a, mat4 *out);

#endif
