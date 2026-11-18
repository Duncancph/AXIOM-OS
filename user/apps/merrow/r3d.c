/* Merrow Lake: software 3D renderer (see r3d.h). */
#include "../../lib/dunc.h"
#include "r3d.h"

struct r3 R;
float r_zbias = 1;

int r_init(int w, int h) {
    if (R.px) free(R.px);
    if (R.zb) free(R.zb);
    R.w = w; R.h = h;
    R.px = malloc((size_t)w * h * 4);
    R.zb = malloc((size_t)w * h * 4);
    return R.px && R.zb;
}

void r_camera(v3 pos, float yaw, float pitch, float fov_deg) {
    R.cam = pos; R.yaw = yaw; R.pitch = pitch;
    float sy = m_sin(yaw), cy = m_cos(yaw), sp = m_sin(pitch), cp = m_cos(pitch);
    R.fw = V3(sy * cp, -sp, cy * cp);
    R.up = V3(sy * sp, cp, cy * sp);
    R.rt = V3(cy, 0, -sy);
    float half = fov_deg * 0.5f * M_PI_F / 180;
    R.tanx = m_sin(half) / m_cos(half);
    R.f = (R.w * 0.5f) / R.tanx;
    R.tany = (R.h * 0.5f) / R.f;
    if (R.near <= 0) R.near = 0.25f;
}

int r_project(v3 world, float *sx, float *sy) {
    v3 v = r_view(world);
    if (v.z < R.near) return 0;
    *sx = R.w * 0.5f + v.x * R.f / v.z;
    *sy = R.h * 0.5f - v.y * R.f / v.z;
    return 1;
}

int r_sphere_visible(v3 c, float rad) {
    v3 v = r_view(c);
    if (v.z < -rad || v.z - rad > R.far) return 0;
    /* side planes: |x| <= z * tanx, with the plane normals' length sqrt(1 + tan^2) */
    float kx = m_sqrt(1 + R.tanx * R.tanx), ky = m_sqrt(1 + R.tany * R.tany);
    if (v.x - v.z * R.tanx > rad * kx || -v.x - v.z * R.tanx > rad * kx) return 0;
    if (v.y - v.z * R.tany > rad * ky || -v.y - v.z * R.tany > rad * ky) return 0;
    return 1;
}

void r_fog_table(void) {
    for (int i = 0; i <= 1024; i++) {
        float d = i * 2.0f - R.fogStart;
        float k = d <= 0 ? 0 : 1 - m_exp(-d / R.fogDist);
        R.fogTab[i] = k > R.fogMax ? R.fogMax : k;
    }
}

/* ---- rasteriser ---- */
typedef struct { float x, y, iz, r, g, b; } sv;

static void raster(const sv *p0, const sv *p1, const sv *p2) {
    float dx1 = p1->x - p0->x, dy1 = p1->y - p0->y, dx2 = p2->x - p0->x, dy2 = p2->y - p0->y;
    float det = dx1 * dy2 - dx2 * dy1;
    if (det > -1e-6f && det < 1e-6f) return;
    float id = 1 / det;
#define GRAD(f, gx, gy) float gx = ((p1->f - p0->f) * dy2 - (p2->f - p0->f) * dy1) * id, gy = ((p2->f - p0->f) * dx1 - (p1->f - p0->f) * dx2) * id
    GRAD(iz, dzdx, dzdy);
    GRAD(r, drdx, drdy);
    GRAD(g, dgdx, dgdy);
    GRAD(b, dbdx, dbdy);
#undef GRAD
    int flat = p0->r == p1->r && p0->r == p2->r && p0->g == p1->g && p0->g == p2->g && p0->b == p1->b && p0->b == p2->b;
    uint32_t fcol = flat ? c_rgb((int)p0->r, (int)p0->g, (int)p0->b) : 0;
    /* sort by y */
    const sv *a = p0, *b = p1, *c = p2, *t;
    if (b->y < a->y) { t = a; a = b; b = t; }
    if (c->y < a->y) { t = a; a = c; c = t; }
    if (c->y < b->y) { t = b; b = c; c = t; }
    int y0 = (int)m_floor(a->y + 0.5f), y2 = (int)m_floor(c->y + 0.5f), ym = (int)m_floor(b->y + 0.5f);
    if (y2 > R.h) y2 = R.h;
    if (y0 >= y2 || y2 <= 0) return;
    /* from here on everything is fixed point (16.16), so the per-scanline work is integer only */
    float zk = ZSCALE / r_zbias;
    float sAC = (c->y - a->y) > 1e-6f ? (c->x - a->x) / (c->y - a->y) : 0;
    float sAB = (b->y - a->y) > 1e-6f ? (b->x - a->x) / (b->y - a->y) : 0;
    float sBC = (c->y - b->y) > 1e-6f ? (c->x - b->x) / (c->y - b->y) : 0;
    sAC = m_clamp(sAC, -20000, 20000); sAB = m_clamp(sAB, -20000, 20000); sBC = m_clamp(sBC, -20000, 20000);
    int32_t fAC = (int32_t)(sAC * 65536), fAB = (int32_t)(sAB * 65536), fBC = (int32_t)(sBC * 65536);
    int32_t xAC = (int32_t)((a->x + (y0 + 0.5f - a->y) * sAC) * 65536);
    int32_t xAB = (int32_t)((a->x + (y0 + 0.5f - a->y) * sAB) * 65536);
    int32_t xBC = (int32_t)((b->x + (ym + 0.5f - b->y) * sBC) * 65536);
    int32_t P0X = (int32_t)(p0->x * 65536), P0Y = (int32_t)(p0->y * 65536);
    int32_t Z0 = (int32_t)(p0->iz * zk), DZX = (int32_t)(dzdx * zk), DZY = (int32_t)(dzdy * zk);
    int32_t R0 = (int32_t)(p0->r * 65536), DRX = (int32_t)(drdx * 65536), DRY = (int32_t)(drdy * 65536);
    int32_t G0 = (int32_t)(p0->g * 65536), DGX = (int32_t)(dgdx * 65536), DGY = (int32_t)(dgdy * 65536);
    int32_t B0 = (int32_t)(p0->b * 65536), DBX = (int32_t)(dbdx * 65536), DBY = (int32_t)(dbdy * 65536);
    int W = R.w;
    for (int y = y0; y < y2; y++) {
        int32_t xa = xAC, xb = y < ym ? xAB : xBC;
        xAC += fAC; if (y < ym) xAB += fAB; else xBC += fBC;
        if (y < 0) continue;
        int32_t xl = xa < xb ? xa : xb, xr = xa < xb ? xb : xa;
        int xs = (xl + 32767) >> 16, xe = (xr + 32767) >> 16;     /* pixel centres inside [xl, xr) */
        if (xs < 0) xs = 0;
        if (xe > W) xe = W;
        if (xs >= xe) continue;
        int64_t OX = ((int64_t)xs << 16) + 32768 - P0X, OY = ((int64_t)y << 16) + 32768 - P0Y;
        int32_t z = Z0 + (int32_t)(((int64_t)DZX * OX + (int64_t)DZY * OY) >> 16), dz = DZX;
        uint32_t *px = R.px + y * W + xs;
        int32_t *zb = R.zb + y * W + xs;
        int n = xe - xs;
        if (flat) {
            for (int i = 0; i < n; i++, z += dz)
                if (z > zb[i]) { zb[i] = z; px[i] = fcol; }
        } else {
            int32_t r = R0 + (int32_t)(((int64_t)DRX * OX + (int64_t)DRY * OY) >> 16);
            int32_t g = G0 + (int32_t)(((int64_t)DGX * OX + (int64_t)DGY * OY) >> 16);
            int32_t bl = B0 + (int32_t)(((int64_t)DBX * OX + (int64_t)DBY * OY) >> 16);
            for (int i = 0; i < n; i++, z += dz, r += DRX, g += DGX, bl += DBX) {
                if (z > zb[i]) {
                    zb[i] = z;
                    int ri = r >> 16, gi = g >> 16, bi = bl >> 16;
                    ri = ri < 0 ? 0 : ri > 255 ? 255 : ri;
                    gi = gi < 0 ? 0 : gi > 255 ? 255 : gi;
                    bi = bi < 0 ? 0 : bi > 255 ? 255 : bi;
                    px[i] = (uint32_t)(ri << 16 | gi << 8 | bi);
                }
            }
        }
    }
}

static void proj(const rvert *v, sv *o) {
    float iz = 1 / v->z;
    o->x = R.w * 0.5f + v->x * R.f * iz;
    o->y = R.h * 0.5f - v->y * R.f * iz;
    o->iz = iz; o->r = v->r; o->g = v->g; o->b = v->b;
}

static void lerpv(const rvert *a, const rvert *b, float t, rvert *o) {
    o->x = a->x + (b->x - a->x) * t; o->y = a->y + (b->y - a->y) * t; o->z = a->z + (b->z - a->z) * t;
    o->r = a->r + (b->r - a->r) * t; o->g = a->g + (b->g - a->g) * t; o->b = a->b + (b->b - a->b) * t;
}

void r_tri(const rvert *a, const rvert *b, const rvert *c, int cull) {
    float nr = R.near;
    if (a->z < nr && b->z < nr && c->z < nr) return;
    if (a->z > R.far && b->z > R.far && c->z > R.far) return;
    if (cull) {
        float ux = b->x - a->x, uy = b->y - a->y, uz = b->z - a->z, vx = c->x - a->x, vy = c->y - a->y, vz = c->z - a->z;
        float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        if (nx * a->x + ny * a->y + nz * a->z >= 0) return;
    }
    /* quick reject against the side planes */
    float tx = R.tanx * 1.02f, ty = R.tany * 1.02f;
    if (a->x > a->z * tx && b->x > b->z * tx && c->x > c->z * tx) return;
    if (-a->x > a->z * tx && -b->x > b->z * tx && -c->x > c->z * tx) return;
    if (a->y > a->z * ty && b->y > b->z * ty && c->y > c->z * ty) return;
    if (-a->y > a->z * ty && -b->y > b->z * ty && -c->y > c->z * ty) return;
    R.tris++;
    const rvert *in[3] = { a, b, c };
    int nin = (a->z >= nr) + (b->z >= nr) + (c->z >= nr);
    sv s[4];
    if (nin == 3) {
        proj(a, &s[0]); proj(b, &s[1]); proj(c, &s[2]);
        raster(&s[0], &s[1], &s[2]);
        return;
    }
    /* clip against the near plane: walk the edges, keep the inside part */
    rvert out[4]; int n = 0;
    for (int i = 0; i < 3; i++) {
        const rvert *p = in[i], *q = in[(i + 1) % 3];
        int pin = p->z >= nr, qin = q->z >= nr;
        if (pin) out[n++] = *p;
        if (pin != qin) lerpv(p, q, (nr - p->z) / (q->z - p->z), &out[n++]);
    }
    for (int i = 0; i < n; i++) proj(&out[i], &s[i]);
    raster(&s[0], &s[1], &s[2]);
    if (n == 4) raster(&s[0], &s[2], &s[3]);
}

void r_tri_world(v3 a, v3 b, v3 c, uint32_t ca, uint32_t cb, uint32_t cc, int cull) {
    v3 va = r_view(a), vb = r_view(b), vc = r_view(c);
    rvert ra, rb, rc;
    r_rv(&ra, va, ca, r_fogk(va.z));
    r_rv(&rb, vb, cb, r_fogk(vb.z));
    r_rv(&rc, vc, cc, r_fogk(vc.z));
    r_tri(&ra, &rb, &rc, cull);
}

void r_billboard(v3 p, float size, uint32_t col) {
    v3 v = r_view(p);
    if (v.z < R.near) return;
    float h = size * 0.5f;
    rvert q[4];
    float k = r_fogk(v.z);
    r_rv(&q[0], V3(v.x - h, v.y - h, v.z), col, k);
    r_rv(&q[1], V3(v.x + h, v.y - h, v.z), col, k);
    r_rv(&q[2], V3(v.x + h, v.y + h, v.z), col, k);
    r_rv(&q[3], V3(v.x - h, v.y + h, v.z), col, k);
    r_tri(&q[0], &q[1], &q[2], 0);
    r_tri(&q[0], &q[2], &q[3], 0);
}

void r_present(uint32_t *dst, int dw, int dh, int dstride, int scale, int ox, int oy) {
    for (int y = 0; y < R.h; y++) {
        const uint32_t *s = R.px + y * R.w;
        for (int k = 0; k < scale; k++) {
            int dy = oy + y * scale + k;
            if (dy < 0 || dy >= dh) continue;
            uint32_t *d = dst + dy * dstride + ox;
            if (k > 0) { memcpy(d, d - dstride, (size_t)R.w * scale * 4); continue; }
            if (scale == 1) memcpy(d, s, (size_t)R.w * 4);
            else if (scale == 2) for (int x = 0; x < R.w; x++) { uint32_t c = s[x]; d[0] = c; d[1] = c; d += 2; }
            else if (scale == 3) for (int x = 0; x < R.w; x++) { uint32_t c = s[x]; d[0] = c; d[1] = c; d[2] = c; d += 3; }
            else for (int x = 0; x < R.w; x++) { uint32_t c = s[x]; for (int j = 0; j < scale; j++) *d++ = c; }
        }
    }
}

/* ---- Perlin noise ---- */
static const uint8_t perm0[256] = {
    151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,8,99,37,240,21,10,23,190,6,148,
    247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175,
    74,165,71,134,139,48,27,166,77,146,158,231,83,111,229,122,60,211,133,230,220,105,92,41,55,46,245,40,244,102,143,54,
    65,25,63,161,1,216,80,73,209,76,132,187,208,89,18,169,200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64,
    52,217,226,250,124,123,5,202,38,147,118,126,255,82,85,212,207,206,59,227,47,16,58,17,182,189,28,42,223,183,170,213,
    119,248,152,2,44,154,163,70,221,153,101,155,167,43,172,9,129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,
    218,246,97,228,251,34,242,193,238,210,144,12,191,179,162,241,81,51,145,235,249,14,239,107,49,192,214,31,181,199,106,157,
    184,84,204,176,115,121,50,45,127,4,150,254,138,236,205,93,222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180 };
static float grad2(int h, float x, float y) {
    switch (h & 7) {
    case 0: return x + y; case 1: return -x + y; case 2: return x - y; case 3: return -x - y;
    case 4: return x; case 5: return -x; case 6: return y; default: return -y;
    }
}
float m_perlin(float x, float y) {
    float fx = m_floor(x), fy = m_floor(y);
    int X = (int)fx & 255, Y = (int)fy & 255;
    x -= fx; y -= fy;
    float u = x * x * x * (x * (x * 6 - 15) + 10), v = y * y * y * (y * (y * 6 - 15) + 10);
    int A = perm0[X] + Y, B = perm0[(X + 1) & 255] + Y;
    float n = m_lerp(m_lerp(grad2(perm0[A & 255], x, y), grad2(perm0[B & 255], x - 1, y), u),
                     m_lerp(grad2(perm0[(A + 1) & 255], x, y - 1), grad2(perm0[(B + 1) & 255], x - 1, y - 1), u), v);
    return m_clamp01(n * 0.5f + 0.5f);
}
