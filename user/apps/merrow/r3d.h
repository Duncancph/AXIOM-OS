/* Merrow Lake: a small software 3D renderer. Draws triangles into a low-resolution colour buffer with a depth buffer
 * (1/z), Gouraud-shaded colours and distance fog, then scales the picture up into the app's window. */
#pragma once
#include "mmath.h"

struct r3 {
    int w, h;                 /* render size in pixels */
    uint32_t *px;             /* colour buffer, w*h */
    int32_t *zb;              /* depth buffer: 1/z in fixed point (ZSCALE), 0 = nothing drawn yet (infinitely far) */
    /* camera */
    v3 cam; float yaw, pitch;
    v3 rt, up, fw;            /* camera axes in the world */
    float f, near, far;       /* focal length in pixels, near and far clip distances */
    float tanx, tany;         /* half-width / half-height of the view at distance 1 */
    /* fog: colour, and the curve 1 - exp(-d / fogDist), never more than fogMax */
    uint32_t fog; float fogDist, fogMax, fogStart;
    float fogTab[1025];       /* fog amount by view depth, 2 m steps (r_fog_table builds it) */
    int tris;                 /* triangles drawn this frame */
};
extern struct r3 R;

typedef struct { float x, y, z, r, g, b; } rvert;   /* a vertex in view space with its colour (0..255 each) */

int  r_init(int w, int h);                       /* (re)allocate the buffers; 0 on failure */
void r_camera(v3 pos, float yaw, float pitch, float fov_deg);
static inline v3 r_view(v3 p) { v3 d = v_sub(p, R.cam); return V3(v_dot(d, R.rt), v_dot(d, R.up), v_dot(d, R.fw)); }
int  r_project(v3 world, float *sx, float *sy);  /* screen position of a world point; 0 if behind the camera */
int  r_sphere_visible(v3 c, float rad);          /* frustum test for a bounding sphere */
void r_fog_table(void);                          /* call after changing fogDist/fogMax/fogStart */
static inline float r_fogk(float d) { int i = (int)(d * 0.5f); if (i < 0) i = 0; if (i > 1024) i = 1024; return R.fogTab[i]; }
#define ZSCALE 268435456.0f    /* 2^28: 1/z in fixed point */
static inline void r_rv(rvert *o, v3 view, uint32_t col, float fogk) {
    o->x = view.x; o->y = view.y; o->z = view.z;
    float k = 1 - fogk;
    o->r = (col >> 16 & 255) * k + (R.fog >> 16 & 255) * fogk;
    o->g = (col >> 8 & 255) * k + (R.fog >> 8 & 255) * fogk;
    o->b = (col & 255) * k + (R.fog & 255) * fogk;
}
/* draw a triangle given in view space. cull: skip it when seen from the back (winding: cross(b-a, c-a) = front) */
void r_tri(const rvert *a, const rvert *b, const rvert *c, int cull);
/* a world-space triangle with a colour per corner (fogged here) */
void r_tri_world(v3 a, v3 b, v3 c, uint32_t ca, uint32_t cb, uint32_t cc, int cull);
/* a camera-facing square ("sprite") at a world point, size in metres */
void r_billboard(v3 p, float size, uint32_t col);
/* depth bias for following triangles: z is multiplied by this before the depth test (1 = none) */
extern float r_zbias;
void r_present(uint32_t *dst, int dw, int dh, int dstride, int scale, int ox, int oy);
