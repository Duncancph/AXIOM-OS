/* Merrow Lake: small float maths for a freestanding app (no libm). Accurate to about 1e-5, which is plenty for games. */
#pragma once
#include <stdint.h>

#define M_PI_F   3.14159265f
#define M_TAU_F  6.28318531f

static inline float m_sqrt(float x) { float r; if (x <= 0) return 0; __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x)); return r; }
static inline double m_sqrtd(double x) { double r; if (x <= 0) return 0; __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }
static inline float m_abs(float x) { return x < 0 ? -x : x; }
static inline float m_min(float a, float b) { return a < b ? a : b; }
static inline float m_max(float a, float b) { return a > b ? a : b; }
static inline float m_clamp(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static inline float m_clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static inline float m_lerp(float a, float b, float t) { return a + (b - a) * t; }
static inline float m_floor(float x) { int i = (int)x; return (float)(x < (float)i ? i - 1 : i); }
static inline double m_floord(double x) { long i = (long)x; return (double)(x < (double)i ? i - 1 : i); }
static inline float m_smooth(float a, float b, float x) { float t = m_clamp01((x - a) / (b - a)); return t * t * (3 - 2 * t); }
/* Unity's Mathf.SmoothStep(from, to, t) */
static inline float m_smoothstep(float from, float to, float t) { t = m_clamp01(t); t = -2 * t * t * t + 3 * t * t; return to * t + from * (1 - t); }
static inline float m_repeat(float x, float len) { return x - m_floor(x / len) * len; }

static inline float m_sin(float x) {
    x = x - M_TAU_F * m_floor(x / M_TAU_F + 0.5f);          /* -pi..pi */
    if (x > 1.57079633f) x = M_PI_F - x; else if (x < -1.57079633f) x = -M_PI_F - x;
    float x2 = x * x;
    return x * (1 + x2 * (-1.0f / 6 + x2 * (1.0f / 120 + x2 * (-1.0f / 5040 + x2 * (1.0f / 362880)))));
}
static inline float m_cos(float x) { return m_sin(x + 1.57079633f); }

static inline float m_atan(float x) {                        /* any x */
    int inv = 0, neg = 0;
    if (x < 0) { x = -x; neg = 1; }
    if (x > 1) { x = 1 / x; inv = 1; }
    float x2 = x * x;
    float r = x * (0.99997726f + x2 * (-0.33262347f + x2 * (0.19354346f + x2 * (-0.11643287f + x2 * (0.05265332f + x2 * -0.01172120f)))));
    if (inv) r = 1.57079633f - r;
    return neg ? -r : r;
}
static inline float m_atan2(float y, float x) {
    if (x > 0) return m_atan(y / x);
    if (x < 0) return y >= 0 ? m_atan(y / x) + M_PI_F : m_atan(y / x) - M_PI_F;
    return y > 0 ? 1.57079633f : y < 0 ? -1.57079633f : 0;
}

static inline float m_exp(float x) {
    if (x < -80) return 0;
    if (x > 80) x = 80;
    float t = x * 1.44269504f;                               /* 2^t */
    float fi = m_floor(t), f = t - fi;
    float p = 1 + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
    union { float f; uint32_t u; } u; u.u = (uint32_t)((int)fi + 127) << 23;
    return p * u.f;
}
static inline float m_log(float x) {
    if (x <= 0) return -80;
    union { float f; uint32_t u; } u; u.f = x;
    int e = (int)((u.u >> 23) & 255) - 127;
    u.u = (u.u & 0x7FFFFF) | (127u << 23);                  /* mantissa in [1,2) */
    float m = u.f;
    if (m > 1.41421356f) { m *= 0.5f; e++; }
    float y = (m - 1) / (m + 1), y2 = y * y;
    float l = 2 * y * (1 + y2 * (1.0f / 3 + y2 * (1.0f / 5 + y2 * (1.0f / 7 + y2 / 9))));
    return l + e * 0.69314718f;
}
static inline float m_pow(float a, float b) { return a <= 0 ? 0 : m_exp(b * m_log(a)); }
static inline double m_powd(double a, double b) { return (double)m_pow((float)a, (float)b); }

/* a repeatable random number generator */
static inline uint32_t m_rnd(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x ? x : 0x9E3779B9; }
static inline float m_rndf(uint32_t *s) { return (m_rnd(s) & 0xFFFFFF) / 16777216.0f; }
static inline float m_rndr(uint32_t *s, float a, float b) { return a + (b - a) * m_rndf(s); }

/* 2D improved Perlin noise in 0..1 (like Unity's Mathf.PerlinNoise) */
float m_perlin(float x, float y);

typedef struct { float x, y, z; } v3;
static inline v3 V3(float x, float y, float z) { v3 r = { x, y, z }; return r; }
static inline v3 v_add(v3 a, v3 b) { return V3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline v3 v_sub(v3 a, v3 b) { return V3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline v3 v_mul(v3 a, float k) { return V3(a.x * k, a.y * k, a.z * k); }
static inline float v_dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline v3 v_cross(v3 a, v3 b) { return V3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline float v_len(v3 a) { return m_sqrt(v_dot(a, a)); }
static inline v3 v_norm(v3 a) { float l = v_len(a); return l > 1e-9f ? v_mul(a, 1 / l) : V3(0, 1, 0); }
static inline v3 v_lerp(v3 a, v3 b, float t) { return V3(m_lerp(a.x, b.x, t), m_lerp(a.y, b.y, t), m_lerp(a.z, b.z, t)); }
static inline float v_dist2(v3 a, v3 b) { float dx = a.x - b.x, dz = a.z - b.z; return dx * dx + dz * dz; }   /* on the ground */

/* colours: 0x00RRGGBB */
static inline uint32_t c_rgb(int r, int g, int b) { r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b; return (uint32_t)(r << 16 | g << 8 | b); }
static inline uint32_t c_lerp(uint32_t a, uint32_t b, float t) {
    t = m_clamp01(t);
    int ar = a >> 16 & 255, ag = a >> 8 & 255, ab = a & 255, br = b >> 16 & 255, bg = b >> 8 & 255, bb = b & 255;
    return c_rgb(ar + (int)((br - ar) * t), ag + (int)((bg - ag) * t), ab + (int)((bb - ab) * t));
}
static inline uint32_t c_scale(uint32_t a, float k) { return c_rgb((int)((a >> 16 & 255) * k), (int)((a >> 8 & 255) * k), (int)((a & 255) * k)); }
static inline uint32_t c_mul(uint32_t a, uint32_t b) { return c_rgb((a >> 16 & 255) * (b >> 16 & 255) / 255, (a >> 8 & 255) * (b >> 8 & 255) / 255, (a & 255) * (b & 255) / 255); }
