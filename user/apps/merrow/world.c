/* Merrow Lake: the valley. Terrain (the Unity game's height function), the draining lake, buildings, trees,
 * creeks, springs, lakebed finds, the drowned village, the cave and the Gate, sky, weather and wildlife. */
#include "merrow.h"

struct world W;
int w_prof[8];

/* ================= lake shape and terrain (LakeWorld.cs) ================= */
static float delta_angle(float a, float b) { float d = m_repeat(b - a + M_PI_F, M_TAU_F) - M_PI_F; return d; }
static float bump(float a, float c, float w) { float d = delta_angle(a, c); return m_exp(-(d * d) / (w * w)); }

float w_shore(float a) {
    return 1 + 0.10f * m_sin(2 * a + 0.4f) + 0.07f * m_sin(3 * a + 1.3f) + 0.05f * m_sin(5 * a + 2.2f) + 0.03f * m_sin(7 * a + 0.9f)
         + 0.42f * bump(a, 1.25f, 0.32f) + 0.18f * bump(a, 2.35f, 0.30f) - 0.20f * bump(a, -0.95f, 0.20f) + 0.12f * bump(a, -0.45f, 0.25f);
}
void w_planar(float angle, float rr, float *x, float *z) {
    float s = w_shore(angle) * rr;
    *x = LAKE_RX * s * m_cos(angle); *z = LAKE_RZ * s * m_sin(angle);
}
float w_rho(float x, float z, float *a) {
    float u = x / LAKE_RX, v = z / LAKE_RZ, an = m_atan2(v, u);
    if (a) *a = an;
    return m_sqrt(u * u + v * v) / w_shore(an);
}
static float gauss(float dx, float dz, float sx, float sz) { return m_exp(-(dx * dx / sx + dz * dz / sz)); }
static float smooth_max(float a, float b, float k) { float h = m_max(k - m_abs(a - b), 0) / k; return m_max(a, b) + h * h * k * 0.25f; }
static float fbm(float x, float z, int oct) {
    float sum = 0, amp = 1, norm = 0;
    for (int i = 0; i < oct; i++) { sum += m_perlin(x + 13.1f * i, z + 7.7f * i) * amp; norm += amp; amp *= 0.5f; x *= 2.02f; z *= 2.02f; }
    return sum / norm;
}
static float ridged(float x, float z) {
    float sum = 0, amp = 1, norm = 0;
    for (int i = 0; i < 4; i++) {
        float n = 1 - m_abs(2 * m_perlin(x + 7.3f * i + 40, z + 3.1f * i + 40) - 1);
        sum += n * n * amp; norm += amp; amp *= 0.5f; x *= 2.03f; z *= 2.03f;
    }
    return sum / norm;
}
static float mountains(float x, float z) {
    float dist = m_sqrt(x * x + z * z);
    float m = m_smoothstep(0, 1, (dist - 150) / 220.0f);
    if (m <= 0) return 0;
    float big = m_perlin(x * 0.0021f + 5, z * 0.0021f + 9);
    return m * (25 + 0.12f * (dist - 150) + ridged(x * 0.0065f, z * 0.0065f) * (110 + 150 * big));
}
static float chapelX, chapelZ, obsX, obsZ;
#define ISLAND_X 20.0f
#define ISLAND_Z 18.0f
float w_h(float x, float z) {
    float rho = w_rho(x, z, 0);
    if (rho < 1) {
        float k = 1 - rho * rho, sk = m_sqrt(k);
        float h = -(1.6f * sk + LAKE_DEPTH * k * sk * (0.92f + 0.08f * m_sin(x * 0.17f) * m_cos(z * 0.21f)));
        h += k * 1.4f * (m_perlin(x * 0.06f + 31, z * 0.06f + 17) - 0.5f);
        float dx = x - ISLAND_X, dz = z - ISLAND_Z, d = m_sqrt(dx * dx + dz * dz);
        return smooth_max(h, 2.6f - 0.0042f * d * d * d, 2);
    }
    float ramp = m_min(1, (rho - 1) * 3);
    float y = ramp * (0.6f + 0.8f * (0.5f + 0.5f * m_sin(x * 0.09f + 1) * m_cos(z * 0.11f)));
    y += m_smoothstep(0, 1, (rho - 1.1f) / 0.8f) * 7 * fbm(x * 0.018f, z * 0.018f, 3);
    y += 26 * gauss(x - obsX, z - (obsZ - 4), 2600, 700);
    y += 7 * gauss(x - chapelX, z - chapelZ, 400, 300);
    y += 14 * m_max(0, rho - 1.9f);
    return y + mountains(x, z);
}
v3 w_on_ellipse(float angle, float rr) { float x, z; w_planar(angle, rr, &x, &z); return V3(x, w_h(x, z), z); }
static float forest_density(float x, float z) { return m_perlin(x * .011f + 3.7f, z * .011f + 8.1f) * .75f + m_perlin(x * .045f + 1.3f, z * .045f + 2.9f) * .25f; }

/* trails: a loop around the lake at about rr = 1.21, plus spurs out to buildings */
static const float SPURS[7][3] = { { -2.55f, 1.21f, 1.30f }, { -2.95f, 1.21f, 1.45f }, { -1.57f, 1.21f, 2.33f }, { -0.95f, 1.12f, 1.21f },
                                   { -2.05f, 1.07f, 1.21f }, { 0.3f, 1.16f, 1.21f }, { 0.42f, 1.21f, 1.45f } };
static float spurA[7][2], spurB[7][2];
static float path_dist(float x, float z) {
    float a, rho = w_rho(x, z, &a);
    float ring = 1.21f + 0.02f * m_sin(5 * a + 1), ca = m_cos(a), sa = m_sin(a);
    float d = m_abs(rho - ring) * m_sqrt(LAKE_RX * LAKE_RX * ca * ca + LAKE_RZ * LAKE_RZ * sa * sa) * w_shore(a);
    for (int i = 0; i < 7; i++) {
        float abx = spurB[i][0] - spurA[i][0], abz = spurB[i][1] - spurA[i][1];
        float t = m_clamp01(((x - spurA[i][0]) * abx + (z - spurA[i][1]) * abz) / (abx * abx + abz * abz));
        float px = spurA[i][0] + abx * t - x, pz = spurA[i][1] + abz * t - z;
        float dd = m_sqrt(px * px + pz * pz);
        if (dd < d) d = dd;
    }
    return d;
}

/* ================= the height grid ================= */
#define GN 121                      /* vertices per side: 2.5 m cells over 300 m */
#define GCELL (WORLD_SIZE / (GN - 1))
#define CHUNK 10
#define NCH ((GN - 1) / CHUNK)
static float gh[GN * GN];
static v3 gnorm[GN * GN];
static uint32_t gbase[GN * GN], gcol[GN * GN];   /* colour from the land; colour now (the lakebed changes as it dries) */
static uint8_t gbasin[GN * GN];
static float gnoise[GN * GN];
static float chLo[NCH * NCH], chHi[NCH * NCH];
static float paintedLevel = 999;

float w_ground(float x, float z) {
    float fx = (x + WORLD_SIZE / 2) / GCELL, fz = (z + WORLD_SIZE / 2) / GCELL;
    if (fx < 0 || fz < 0 || fx >= GN - 1 || fz >= GN - 1) return w_h(x, z);
    int i = (int)fx, j = (int)fz; float u = fx - i, v = fz - j;
    const float *r0 = gh + j * GN + i, *r1 = r0 + GN;
    /* the same two triangles the renderer draws (split along a-d) */
    if (u > v) return r0[0] + (r0[1] - r0[0]) * u + (r1[1] - r0[1]) * v;
    return r0[0] + (r1[1] - r1[0]) * u + (r1[0] - r0[0]) * v;
}

/* ================= lake volume ================= */
#define NL 400
static float volTable[NL + 1], bMin;
static v3 deepest;
static float extX, extZ;
#define MAXBASIN 26000
static float bx[MAXBASIN], bz[MAXBASIN], bh[MAXBASIN]; static int nbasin;
float w_bmin(void) { return bMin; }
v3 w_deepest(void) { return deepest; }
float w_level_for_geo(float r) {
    if (r >= 1) return LAKE_FULL;
    if (r <= 0) return bMin;
    int lo = 0, hi = NL;
    while (hi - lo > 1) { int m = (lo + hi) >> 1; if (volTable[m] >= r) lo = m; else hi = m; }
    float a = volTable[lo], b = volTable[hi], k = (a - r) / m_max(1e-6f, a - b);
    return m_lerp(LAKE_FULL, bMin, (lo + k) / NL);
}
float w_level(void) { return G.won ? bMin - 1 : w_level_for_geo(1 - g_progress()); }
float w_feet(void) { return m_clamp01((LAKE_FULL - w_level()) / (LAKE_FULL - bMin)) * MAX_FEET; }

static void build_volume(void) {
    for (float a = 0; a < M_TAU_F; a += 0.01f) { float x, z; w_planar(a, 1, &x, &z); extX = m_max(extX, m_abs(x)); extZ = m_max(extZ, m_abs(z)); }
    bMin = 0; nbasin = 0;
    float step = 1.25f;
    for (float x = -extX; x <= extX; x += step)
        for (float z = -extZ; z <= extZ; z += step) {
            float h = w_h(x, z);
            if (h >= LAKE_FULL || nbasin >= MAXBASIN) continue;
            bx[nbasin] = x; bz[nbasin] = z; bh[nbasin] = h; nbasin++;
            if (h < bMin) { bMin = h; deepest = V3(x, h, z); }
        }
    /* volume below height y: sum of (y - h) over samples; sorted heights make this one pass per level */
    double v0 = 0; for (int i = 0; i < nbasin; i++) v0 += LAKE_FULL - bh[i];
    for (int l = 0; l <= NL; l++) {
        float y = m_lerp(LAKE_FULL, bMin, l / (float)NL);
        double v = 0; for (int i = 0; i < nbasin; i++) if (y > bh[i]) v += y - bh[i];
        volTable[l] = (float)(v / v0);
    }
}

/* ================= colours ================= */
#define C_GRASSA 0x5B7A37
#define C_GRASSB 0x7E9550
#define C_DRY    0x9A9A5C
#define C_FOREST 0x3C4F2C
#define C_SAND   0xB9A57E
#define C_PEBBLE 0x8F8670
#define C_PATH   0x8B7353
#define C_ROCK   0x5F5B53
#define C_ROCK2  0x736D62
#define C_ALPINE 0x66704A
#define C_CONIFER 0x34472B
#define C_SNOW   0xEEF2F5
static uint32_t ground_color(float x, float z, float h, float slope, int detail) {
    float rho = w_rho(x, z, 0);
    float n = m_perlin(x * 0.08f + 10, z * 0.08f + 10), patch = m_perlin(x * 0.02f + 3, z * 0.02f + 5);
    uint32_t c = c_lerp(C_GRASSA, C_GRASSB, n);
    c = c_lerp(c, C_DRY, m_smoothstep(0, 1, (patch - 0.55f) / 0.25f) * 0.6f);
    c = c_lerp(c, C_FOREST, m_smoothstep(0, 1, (forest_density(x, z) - 0.42f) / 0.2f) * 0.8f);
    float treeline = 105 + 30 * m_perlin(x * 0.01f, z * 0.01f);
    if (h > 30) c = c_lerp(c, C_CONIFER, m_smoothstep(0, 1, (h - 30) / 30) * 0.7f);
    if (h > treeline - 25) c = c_lerp(c, C_ALPINE, m_smoothstep(0, 1, (h - treeline + 25) / 25));
    uint32_t rock = c_lerp(C_ROCK, C_ROCK2, m_perlin(x * 0.15f, z * 0.15f));
    if (h > treeline) c = c_lerp(c, rock, m_smoothstep(0, 1, (h - treeline) / 20));
    c = c_lerp(c, rock, m_smoothstep(0, 1, (slope - 0.75f) / 0.6f));
    float snowline = 178 + 40 * m_perlin(x * 0.008f + 2, z * 0.008f + 6);
    if (h > snowline - 15 && slope < 1.3f) c = c_lerp(c, C_SNOW, m_smoothstep(0, 1, (h - snowline + 15) / 25) * m_clamp01(1.4f - slope));
    if (!detail) return c;
    float sand = rho < 1 ? m_smoothstep(1, 0, (h - 0.3f) / 1.0f) : m_smoothstep(1, 0, (rho - 1.0f) / 0.07f);
    if (sand > 0) c = c_lerp(c, c_lerp(C_SAND, C_PEBBLE, m_perlin(x * 0.9f, z * 0.9f)), sand);
    if (rho > 0.95f && rho < 3) {
        float pd = path_dist(x, z);
        if (pd < 2.6f) c = c_lerp(c, C_PATH, m_smoothstep(1, 0, (pd - 1.2f) / 1.4f));
    }
    return c;
}

/* world tints */
static uint32_t world_tint(uint32_t c) {
    if (is_drowned()) return c_lerp(c, 0x6E7270, 0.35f);          /* grey, rain-soaked */
    if (other_side()) return c_lerp(c, 0x2E4A44, 0.3f);           /* dusk-green */
    return c;
}

static void paint_basin(float level) {
    for (int k = 0; k < GN * GN; k++) {
        if (!gbasin[k]) continue;
        float h = gh[k];
        float nz = gnoise[k];
        uint32_t c;
        if (h > level) {
            c = c_lerp(0x8A7152, 0xA08868, nz);
            if (nz > 0.62f) c = c_lerp(c, 0x6E5A41, 0.5f);
            c = c_lerp(0x5E4B35, c, m_clamp01((h - level) / 1.2f));
        } else c = c_lerp(0xA39A78, c_lerp(0x3F3122, 0x2B2219, m_clamp01(-h / LAKE_DEPTH)), m_clamp01((level - h) / 3.5f) * .85f + nz * .15f);
        gcol[k] = world_tint(c);
    }
    paintedLevel = level;
}

static void build_grid(void (*progress)(const char *, int)) {
    for (int j = 0; j < GN; j++) {
        if (progress && j % 20 == 0) progress("Raising the hills", 20 + j * 30 / GN);
        for (int i = 0; i < GN; i++) {
            float x = -WORLD_SIZE / 2 + GCELL * i, z = -WORLD_SIZE / 2 + GCELL * j;
            gh[j * GN + i] = w_h(x, z);
        }
    }
    for (int j = 0; j < GN; j++)
        for (int i = 0; i < GN; i++) {
            int k = j * GN + i;
            float hx0 = gh[j * GN + (i > 0 ? i - 1 : i)], hx1 = gh[j * GN + (i < GN - 1 ? i + 1 : i)];
            float hz0 = gh[(j > 0 ? j - 1 : j) * GN + i], hz1 = gh[(j < GN - 1 ? j + 1 : j) * GN + i];
            float dx = (hx1 - hx0) / (2 * GCELL), dz = (hz1 - hz0) / (2 * GCELL);
            gnorm[k] = v_norm(V3(-dx, 1, -dz));
            float x = -WORLD_SIZE / 2 + GCELL * i, z = -WORLD_SIZE / 2 + GCELL * j;
            gbase[k] = ground_color(x, z, gh[k], m_sqrt(dx * dx + dz * dz), 1);
            gbasin[k] = gh[k] < LAKE_FULL + 0.05f;
            if (gbasin[k]) gnoise[k] = m_perlin(i * 0.35f, j * 0.35f) * 0.7f + m_perlin(i * 0.05f, j * 0.05f) * 0.3f;
        }
    for (int cj = 0; cj < NCH; cj++)
        for (int ci = 0; ci < NCH; ci++) {
            float lo = 1e9f, hi = -1e9f;
            for (int j = cj * CHUNK; j <= cj * CHUNK + CHUNK; j++) for (int i = ci * CHUNK; i <= ci * CHUNK + CHUNK; i++) { lo = m_min(lo, gh[j * GN + i]); hi = m_max(hi, gh[j * GN + i]); }
            chLo[cj * NCH + ci] = lo; chHi[cj * NCH + ci] = hi;
        }
}
static void tint_land(void) {
    for (int k = 0; k < GN * GN; k++) gcol[k] = world_tint(gbase[k]);
    paintedLevel = 999;
}

/* the far mountains: a coarse grid around the valley */
#define BN 49
#define BSIZE 1800.0f
static float bh_[BN * BN]; static uint32_t bcol[BN * BN]; static v3 bnorm[BN * BN];
static void build_backdrop(void) {
    float cell = BSIZE / (BN - 1);
    for (int j = 0; j < BN; j++)
        for (int i = 0; i < BN; i++) {
            float x = -BSIZE / 2 + cell * i, z = -BSIZE / 2 + cell * j;
            bh_[j * BN + i] = w_h(x, z);
        }
    for (int j = 0; j < BN; j++)
        for (int i = 0; i < BN; i++) {
            int k = j * BN + i;
            float dx = (bh_[j * BN + (i < BN - 1 ? i + 1 : i)] - bh_[j * BN + (i > 0 ? i - 1 : i)]) / (2 * cell);
            float dz = (bh_[(j < BN - 1 ? j + 1 : j) * BN + i] - bh_[(j > 0 ? j - 1 : j) * BN + i]) / (2 * cell);
            bnorm[k] = v_norm(V3(-dx, 1, -dz));
            float x = -BSIZE / 2 + cell * i, z = -BSIZE / 2 + cell * j;
            bcol[k] = ground_color(x, z, bh_[k], m_sqrt(dx * dx + dz * dz) * 1.4f, 0);
        }
}

/* ================= static meshes ================= */
enum { COND_ALWAYS, COND_WORLD, COND_NOT_WORLD, COND_EXPLORED, COND_NOT_EXPLORED, COND_CREEK_STATE, COND_WON, COND_TAKEN, COND_ITEM, COND_PLANKS };
struct stri { v3 a, b, c, n; uint32_t col; };
struct sobj { int first, count, lod; v3 c; float r; uint8_t scene, cond; int16_t arg, arg2; };
#define MAXTRI 30000
#define MAXOBJ 1400
static struct stri *tris; static int ntris;
static struct sobj objs[MAXOBJ]; static int nobjs;
/* the transform new triangles are built with */
static v3 mo; static float myaw_s, myaw_c, mscale = 1;
static v3 mcenter;                     /* the current primitive's centre: faces point away from it */
static int curScene, curArg2;

static v3 xf(float x, float y, float z) {   /* local -> world */
    x *= mscale; y *= mscale; z *= mscale;
    return V3(mo.x + x * myaw_c + z * myaw_s, mo.y + y, mo.z - x * myaw_s + z * myaw_c);
}
static void m_at(v3 o, float yaw) { mo = o; myaw_s = m_sin(yaw); myaw_c = m_cos(yaw); mscale = 1; }
static void obj_begin(int cond, int arg) { objs[nobjs].first = ntris; objs[nobjs].scene = (uint8_t)curScene; objs[nobjs].cond = (uint8_t)cond; objs[nobjs].arg = (int16_t)arg; objs[nobjs].arg2 = (int16_t)curArg2; objs[nobjs].lod = -1; }
static void obj_lod(void) { objs[nobjs].lod = ntris - objs[nobjs].first; }
static void obj_end(void) {
    struct sobj *o = &objs[nobjs];
    o->count = ntris - o->first;
    if (o->count <= 0 || nobjs >= MAXOBJ - 1) return;
    v3 lo = V3(1e9f, 1e9f, 1e9f), hi = V3(-1e9f, -1e9f, -1e9f);
    for (int i = o->first; i < ntris; i++) {
        v3 p[3] = { tris[i].a, tris[i].b, tris[i].c };
        for (int k = 0; k < 3; k++) {
            lo.x = m_min(lo.x, p[k].x); lo.y = m_min(lo.y, p[k].y); lo.z = m_min(lo.z, p[k].z);
            hi.x = m_max(hi.x, p[k].x); hi.y = m_max(hi.y, p[k].y); hi.z = m_max(hi.z, p[k].z);
        }
    }
    o->c = v_mul(v_add(lo, hi), 0.5f); o->r = v_len(v_sub(hi, lo)) * 0.5f + 0.1f;
    if (o->lod < 0) o->lod = o->count;
    nobjs++;
}
static void emit(v3 a, v3 b, v3 c, uint32_t col) {
    if (ntris >= MAXTRI) return;
    v3 n = v_cross(v_sub(b, a), v_sub(c, a));
    v3 cen = v_mul(v_add(v_add(a, b), c), 1.0f / 3);
    if (v_dot(n, v_sub(cen, mcenter)) < 0) { v3 t = b; b = c; c = t; n = v_mul(n, -1); }
    struct stri *t = &tris[ntris++];
    t->a = a; t->b = b; t->c = c; t->n = v_norm(n); t->col = col;
}
static void quad(v3 a, v3 b, v3 c, v3 d, uint32_t col) { emit(a, b, c, col); emit(a, c, d, col); }

/* a box: centre (local), size, colour; top may differ */
static void box2(float cx, float cy, float cz, float sx, float sy, float sz, uint32_t col, uint32_t top) {
    float x0 = cx - sx / 2, x1 = cx + sx / 2, y0 = cy - sy / 2, y1 = cy + sy / 2, z0 = cz - sz / 2, z1 = cz + sz / 2;
    v3 p[8] = { xf(x0, y0, z0), xf(x1, y0, z0), xf(x1, y0, z1), xf(x0, y0, z1), xf(x0, y1, z0), xf(x1, y1, z0), xf(x1, y1, z1), xf(x0, y1, z1) };
    mcenter = xf(cx, cy, cz);
    quad(p[4], p[5], p[6], p[7], top);
    quad(p[0], p[1], p[5], p[4], col); quad(p[1], p[2], p[6], p[5], c_scale(col, 0.95f));
    quad(p[2], p[3], p[7], p[6], col); quad(p[3], p[0], p[4], p[7], c_scale(col, 0.95f));
    if (y0 > -0.5f) quad(p[0], p[1], p[2], p[3], col);
}
static void box(float cx, float cy, float cz, float sx, float sy, float sz, uint32_t col) { box2(cx, cy, cz, sx, sy, sz, col, col); }
/* a gable roof along z: base y, width sx, length sz, height h */
static void roof(float cx, float y, float cz, float sx, float sz, float h, uint32_t col) {
    float x0 = cx - sx / 2 - 0.3f, x1 = cx + sx / 2 + 0.3f, z0 = cz - sz / 2 - 0.3f, z1 = cz + sz / 2 + 0.3f;
    v3 a = xf(x0, y, z0), b = xf(x1, y, z0), c = xf(x1, y, z1), d = xf(x0, y, z1), r0 = xf(cx, y + h, z0), r1 = xf(cx, y + h, z1);
    mcenter = xf(cx, y + h * 0.3f, cz);
    quad(a, r0, r1, d, col); quad(b, r0, r1, c, c_scale(col, 0.85f));
    emit(a, b, r0, c_scale(col, 0.75f)); emit(d, c, r1, c_scale(col, 0.75f));
}
/* a cylinder (or a cone when r1 = 0, or a frustum) standing on y */
static void cyl(float cx, float y, float cz, float r0, float r1, float h, int seg, uint32_t col, uint32_t top) {
    mcenter = xf(cx, y + h * 0.5f, cz);
    for (int i = 0; i < seg; i++) {
        float a0 = i * M_TAU_F / seg, a1 = (i + 1) * M_TAU_F / seg;
        v3 b0 = xf(cx + m_cos(a0) * r0, y, cz + m_sin(a0) * r0), b1 = xf(cx + m_cos(a1) * r0, y, cz + m_sin(a1) * r0);
        uint32_t sc = c_scale(col, 0.9f + 0.1f * m_cos(a0 * 2));
        if (r1 <= 0.001f) emit(b0, b1, xf(cx, y + h, cz), sc);
        else {
            v3 t0 = xf(cx + m_cos(a0) * r1, y + h, cz + m_sin(a0) * r1), t1 = xf(cx + m_cos(a1) * r1, y + h, cz + m_sin(a1) * r1);
            quad(b0, b1, t1, t0, sc);
            if (top) emit(xf(cx, y + h, cz), t0, t1, top);
        }
    }
}
/* a cylinder lying along x (wheels, logs, gears) */
static void cylx(float cx, float cy, float cz, float r, float len, int seg, uint32_t col) {
    mcenter = xf(cx, cy, cz);
    for (int i = 0; i < seg; i++) {
        float a0 = i * M_TAU_F / seg, a1 = (i + 1) * M_TAU_F / seg;
        v3 p0 = xf(cx - len / 2, cy + m_cos(a0) * r, cz + m_sin(a0) * r), p1 = xf(cx - len / 2, cy + m_cos(a1) * r, cz + m_sin(a1) * r);
        v3 q0 = xf(cx + len / 2, cy + m_cos(a0) * r, cz + m_sin(a0) * r), q1 = xf(cx + len / 2, cy + m_cos(a1) * r, cz + m_sin(a1) * r);
        quad(p0, p1, q1, q0, c_scale(col, 0.9f + 0.1f * m_cos(a0)));
        emit(xf(cx - len / 2, cy, cz), p0, p1, c_scale(col, 0.8f));
        emit(xf(cx + len / 2, cy, cz), q0, q1, c_scale(col, 0.8f));
    }
}
/* a blob: a low-poly ellipsoid */
static void blob(float cx, float cy, float cz, float rx, float ry, float rz, int seg, uint32_t col) {
    mcenter = xf(cx, cy, cz);
    int st = seg / 2 + 1;
    for (int j = 0; j < st; j++) {
        float b0 = -M_PI_F / 2 + j * M_PI_F / st, b1 = b0 + M_PI_F / st;
        for (int i = 0; i < seg; i++) {
            float a0 = i * M_TAU_F / seg, a1 = (i + 1) * M_TAU_F / seg;
            v3 p00 = xf(cx + m_cos(a0) * m_cos(b0) * rx, cy + m_sin(b0) * ry, cz + m_sin(a0) * m_cos(b0) * rz);
            v3 p10 = xf(cx + m_cos(a1) * m_cos(b0) * rx, cy + m_sin(b0) * ry, cz + m_sin(a1) * m_cos(b0) * rz);
            v3 p01 = xf(cx + m_cos(a0) * m_cos(b1) * rx, cy + m_sin(b1) * ry, cz + m_sin(a0) * m_cos(b1) * rz);
            v3 p11 = xf(cx + m_cos(a1) * m_cos(b1) * rx, cy + m_sin(b1) * ry, cz + m_sin(a1) * m_cos(b1) * rz);
            uint32_t c = c_scale(col, 0.85f + 0.15f * (j / (float)st));
            if (j > 0) emit(p00, p10, p11, c);
            if (j < st - 1) emit(p00, p11, p01, c);
        }
    }
}

/* ================= placing things ================= */
static float yaw_to(v3 from, float tx, float tz) { return m_atan2(tx - from.x, tz - from.z); }   /* local +z faces the target */
static v3 ground_at(float x, float z) { return V3(x, w_h(x, z), z); }
static v3 local_point(v3 o, float yaw, float lx, float lz) { float s = m_sin(yaw), c = m_cos(yaw); return ground_at(o.x + lx * c + lz * s, o.z - lx * s + lz * c); }

static float flat_base(v3 p, float r) {   /* the lowest ground around a footprint, so buildings sit on a plinth */
    float lo = p.y;
    for (int k = 0; k < 8; k++) { float a = k * M_PI_F / 4; lo = m_min(lo, w_h(p.x + m_cos(a) * r, p.z + m_sin(a) * r)); }
    return lo;
}



static void build_tree(v3 p, float s, int kind, uint32_t seed) {
    curArg2 = 0;
    obj_begin(COND_ALWAYS, 0);
    m_at(p, m_rndf(&seed) * 6); mscale = s;
    if (kind == 0) {   /* conifer */
        uint32_t c1 = c_lerp(0x2E4A2A, 0x3F5A30, m_rndf(&seed));
        cyl(0, 2.2f, 0, 2.4f, 0, 5.5f, 6, c1, 0);
        obj_lod();
        cyl(0, 4.8f, 0, 1.8f, 0, 4.2f, 6, c_scale(c1, 1.1f), 0);
        cyl(0, -0.3f, 0, 0.3f, 0.25f, 2.6f, 4, 0x5A4632, 0);
    } else {          /* broadleaf */
        uint32_t c1 = c_lerp(0x4E6E30, 0x6C8A3A, m_rndf(&seed));
        blob(0, 4.6f, 0, 2.7f, 2.4f, 2.7f, 6, c1);
        obj_lod();
        cyl(0, -0.3f, 0, 0.35f, 0.25f, 3.2f, 4, 0x5E4A36, 0);
    }
    obj_end();
}

/* ---- the buildings (simplified from LakeWorld.Buildings.cs) ---- */
static void build_store(void) {
    v3 p = W.store; float base = flat_base(p, 6);
    m_at(V3(p.x, base, p.z), W.storeYaw);
    obj_begin(COND_ALWAYS, 0);
    box(0, -0.3f + (p.y - base) * 0, 0, 9.6f, 1.2f, 7.6f, 0x6F6A5F);
    box(0, 2.6f, 0, 9, 4.6f, 7, 0x9C7A55);
    box(0, 3.9f, 3.6f, 9.6f, 7.2f, 0.3f, 0xA8875F);                     /* false front */
    box(0, 6.05f, 3.8f, 6.9f, 1.25f, 0.12f, 0x2F4A3A);                    /* sign board */
    box(0, 6.05f, 3.86f, 6.0f, 0.3f, 0.05f, 0xE8C878);                    /* lettering */
    box(0, 1.6f, 3.78f, 1.8f, 2.7f, 0.1f, 0x3A2617);                      /* door */
    box(-2.8f, 2.35f, 3.78f, 2.2f, 1.9f, 0.1f, 0xE9C87A);                 /* lit windows */
    box(2.8f, 2.35f, 3.78f, 2.2f, 1.9f, 0.1f, 0xE9C87A);
    box(0, 0.45f, 5.05f, 10, 0.3f, 2.6f, 0x8A6A46);                       /* porch */
    roof(0, 4.9f, 0, 9, 7, 2.4f, 0x5A3A2E);
    box(0, 3.8f, 5.05f, 10.4f, 0.18f, 3.1f, 0x5A3A2E);
    for (int s = -1; s <= 1; s += 2) box(4.8f * s, 2.1f, 6.2f, 0.25f, 3.4f, 0.25f, 0x6A4E34);
    box(3.9f, 1.05f, 4.3f, 1, .9f, 1, 0x8A6C4A);                          /* crates */
    box(2.5f, 0.85f, 5.7f, .6f, .3f, .6f, 0x5A3F28);
    obj_end();
}

static void build_tank(void) {
    v3 p = W.tank; float base = flat_base(p, 3);
    m_at(V3(p.x, base, p.z), W.tankYaw);
    obj_begin(COND_ALWAYS, 0);
    for (int k = 0; k < 4; k++) box(k < 2 ? -1.6f : 1.6f, 1.2f, (k & 1) ? -1.6f : 1.6f, 0.3f, 2.4f, 0.3f, 0x4A4640);   /* legs */
    cyl(0, 2.4f, 0, 2.6f, 2.6f, 5.2f, 12, 0x8C9AA0, 0x9AA8AE);            /* galvanised tank */
    for (int k = 0; k < 3; k++) cyl(0, 3.2f + k * 1.6f, 0, 2.65f, 2.65f, 0.12f, 12, 0x6F7C82, 0);
    cyl(0, 7.6f, 0, 2.7f, 0, 1.0f, 12, 0x6F7C82, 0);
    box(2.0f, 1.3f, 2.4f, 1.6f, 0.9f, 1.2f, 0x7A6248);                     /* the hopper you pour into */
    box(-3.2f, 3.0f, 0, 0.12f, 6, 0.12f, 0x5A5550);                        /* ladder */
    obj_end();
}

/* pumps on a pad next to the tank, one model per pump owned */
static void build_pumps(void) {
    for (int t = 0; t < NPUMPS; t++)
        for (int n = 0; n < PUMPS[t].max; n++) {
            v3 p = local_point(W.tank, W.tankYaw, -5.5f - t * 2.4f, -3 + n * 1.6f);
            m_at(p, W.tankYaw);
            obj_begin(COND_ITEM, 100 + t * 16 + n);           /* visible when you own at least n+1 of pump t */
            float s = 0.6f + t * 0.18f;
            box(0, 0.35f * s, 0, 1.0f * s, 0.7f * s, 1.3f * s, PUMPS[t].color);
            cylx(0, 0.9f * s, 0, 0.25f * s, 1.1f * s, 6, 0x3A3A3A);
            obj_end();
        }
}

static void build_building(int i) {
    v3 p = W.building[i]; float yaw = W.buildingYaw[i];
    float base = flat_base(p, 5);
    m_at(V3(p.x, base, p.z), yaw);
    const char *id = BUILDINGS[i].id;
    obj_begin(COND_ALWAYS, 0);
    if (!strcmp(id, "boathouse")) {
        box(0, 1.1f, 0, 7.6f, .3f, 6.8f, 0x7A5A3A);
        box(0, 3.2f, 0, 7, 3.9f, 6, 0x8C3B2E);
        roof(0, 5.15f, 0, 7, 6, 2.2f, 0x3E3A36);
        box(0, 2.95f, 3.06f, 4, 3.3f, .12f, 0x3B2A20);
        box(0, .95f, 8.2f, 2.6f, .2f, 10.4f, 0x7A5A3A);                 /* jetty */
        for (int k = 0; k < 4; k++) box(1.1f, 0, 4 + k * 3, .25f, 2.4f, .25f, 0x5A3F28), box(-1.1f, 0, 4 + k * 3, .25f, 2.4f, .25f, 0x5A3F28);
    } else if (!strcmp(id, "cabin")) {
        box(0, -0.3f, 0, 8.8f, 1.4f, 6.8f, 0x6F6A5F);
        for (int k = 0; k < 8; k++) box(0, .7f + k * .5f, 0, 7.6f, .5f, 5.6f, k & 1 ? 0x6E5034 : 0x7E5E3E);
        box(0, 2.6f, 2.85f, 1.4f, 2.6f, .12f, 0x3A2A1E);
        box(-2.6f, 2.6f, 2.85f, 1.5f, 1.2f, .1f, 0xD9B46A);
        box(2.6f, 2.6f, 2.85f, 1.5f, 1.2f, .1f, 0xD9B46A);
        roof(0, 4.7f, 0, 7.6f, 5.6f, 2.4f, 0x4A3A30);
        box(3, 6.4f, -1.3f, 1.0f, 3.2f, 1.0f, 0x5E5A52);                /* chimney */
        box(0, .45f, 4.2f, 8.8f, .3f, 2.4f, 0x7A5A3A);
    } else if (!strcmp(id, "chapel")) {
        box(0, -0.3f, 0, 6.8f, 1.2f, 10.4f, 0x7D776C);
        box(0, 2.8f, 0, 6, 5.2f, 9.6f, 0xE8E2D2);
        roof(0, 5.4f, 0, 6, 9.6f, 3.2f, 0x4A4A52);
        box(0, 5.6f, 4.1f, 2.4f, 11, 2.4f, 0xE8E2D2);                    /* tower */
        cyl(0, 11.1f, 4.1f, 1.8f, 0, 4.2f, 4, 0x4A4A52, 0);
        box(0, 9.3f, 5.32f, .9f, 1.2f, .1f, 0x2A2420);                   /* belfry */
        box(0, 1.7f, 4.82f, 1.4f, 2.6f, .1f, 0x5A3A26);
        for (int k = -1; k <= 1; k += 2) box(3.02f * k, 3.2f, 0, .1f, 2.2f, 1.2f, 0x9AB0C8);
    } else if (!strcmp(id, "mill")) {
        box(0, -0.3f, 0, 8, 1.4f, 7, 0x6F6A5F);
        box(0, 2.9f, 0, 7, 5.4f, 6, 0x8A7A62);
        roof(0, 5.6f, 0, 7, 6, 2.6f, 0x4E3E34);
        box(0, 1.8f, 3.05f, 1.4f, 2.4f, .12f, 0x3A2A1E);
        obj_end();
        /* the water wheel turns once the mill is explored: drawn as a dynamic object (see draw_dynamic) */
        obj_begin(COND_ALWAYS, 0);
        box(-4.4f, 3.6f, -1.5f, .8f, .4f, 6, 0x5A4632);                   /* flume */
    } else if (!strcmp(id, "lighthouse")) {
        box(0, -0.2f, 0, 6, 1.2f, 6, 0x7D776C);
        cyl(0, 0.3f, 0, 2.4f, 1.7f, 11, 10, 0xEFEAE0, 0);
        for (int k = 0; k < 3; k++) cyl(0, 2.5f + k * 3, 0, 2.3f - k * .22f, 2.2f - k * .22f, 1.2f, 10, 0xB8342C, 0);
        cyl(0, 11.3f, 0, 2.1f, 2.1f, .3f, 10, 0x2A2A2A, 0x3A3A3A);
        cyl(0, 11.6f, 0, 1.3f, 1.3f, 1.8f, 8, 0x9AB4C0, 0);
        cyl(0, 13.4f, 0, 1.6f, 0, 1.4f, 8, 0x2A2A2A, 0);
        box(0, 1.5f, 2.15f, 1.1f, 2.2f, .3f, 0x3A2A1E);
    } else {   /* observatory */
        box(0, -0.3f, 0, 8, 1.4f, 8, 0x6F6A5F);
        cyl(0, 0.4f, 0, 3.6f, 3.6f, 4.4f, 12, 0xD8D2C4, 0);
        blob(0, 4.8f, 0, 3.7f, 3.2f, 3.7f, 12, 0x8E9AA4);
        box(0, 6.6f, 1.2f, .9f, .7f, 3.8f, 0x2A2A30);                    /* slit */
        box(0, 1.7f, 3.55f, 1.3f, 2.6f, .2f, 0x3A2A1E);
    }
    obj_end();
    /* lamp in the window once explored */
    obj_begin(COND_EXPLORED, i);
    m_at(V3(p.x, base, p.z), yaw);
    box(0, 2.6f, 3.4f, 1.2f, 0.8f, 0.6f, 0xFFD27A);
    obj_end();
}

static void build_secret(int i) {
    v3 p = W.secret[i];
    m_at(p, yaw_to(p, 0, 0));
    obj_begin(COND_ALWAYS, 0);
    switch (i) {
    case 0: box(0, .6f, 0, .12f, 1.2f, .12f, 0x5A4632); box(0, 1.35f, 0, .5f, .4f, .8f, 0x8A4A32); break;      /* mailbox */
    case 1: cyl(0, -0.2f, 0, 1.2f, 1.2f, 1.1f, 10, 0x7D776C, 0x1A1A1A); box(0, 1.9f, 0, 2.4f, .15f, .3f, 0x5A4632);
            box(-1.1f, 1.2f, 0, .15f, 1.6f, .15f, 0x5A4632); box(1.1f, 1.2f, 0, .15f, 1.6f, .15f, 0x5A4632); break;   /* well */
    case 2: cyl(0, -0.2f, 0, .9f, .7f, 5, 7, 0x6A5A48, 0); blob(0, 6, 0, 2.6f, 2.2f, 2.6f, 6, 0x5E7A3A);
            box(0, 1.2f, .75f, .7f, 1.2f, .2f, 0x1A140E); break;                                                   /* hollow ash */
    case 3: box(0, .6f, 0, 1, 1.2f, .25f, 0x8A8A86); box(0, 1.25f, 0, .7f, .3f, .25f, 0x8A8A86); break;          /* gravestone */
    default: blob(0, .4f, 0, 1.2f, .6f, 1.2f, 6, 0x7A766C); blob(0, 1.1f, 0, .8f, .45f, .8f, 6, 0x8A867C); blob(0, 1.6f, 0, .5f, .3f, .5f, 6, 0x6E6A62); break;
    }
    obj_end();
}

static void build_item_model(int i) {
    const struct item_def *it = &ITEMS[i];
    float a = i * 2.39996f + 0.6f;
    m_at(W.item[i], a * 1.7f);
    curArg2 = 0;
    obj_begin(COND_ITEM, i);
    float sx = it->sx, sy = it->sy, sz = it->sz;
    switch (it->shape) {
    case SHAPE_CUBE: box(0, sy / 2 - 0.1f, 0, sx, sy, sz, it->color); break;
    case SHAPE_CYL: cyl(0, -0.05f, 0, sx / 2, sx / 2, sy, 10, it->color, c_scale(it->color, 1.1f)); break;
    case SHAPE_SPHERE: blob(0, sy / 2, 0, sx / 2, sy / 2, sz / 2, 8, it->color); break;
    default: /* a capsule lying down */
        blob(0, sx / 2, 0, sx / 2, sx / 2, sy / 2 + sx / 4, 8, it->color); break;
    }
    obj_end();
}

/* ---- lakebed springs, plug ---- */
static void build_springs(void) {
    for (int i = 0; i < NSPRINGS; i++) {
        m_at(W.spring[i], 0);
        obj_begin(COND_ALWAYS, 0);
        for (int k = 0; k < 6; k++) { float a = k * 1.05f; blob(m_cos(a) * 1.3f, .1f, m_sin(a) * 1.3f, .5f, .35f, .5f, 5, 0x6B665B); }
        cyl(0, -0.05f, 0, .8f, .8f, .12f, 8, 0x1A1612, 0x1A1612);
        obj_end();
        obj_begin(COND_CREEK_STATE, 100 + i);   /* the iron cap once capped */
        cyl(0, 0, 0, .9f, .7f, .5f, 8, 0x3B3B3B, 0x4A4A4A);
        obj_end();
    }
}

static void build_plug(void) {
    m_at(deepest, 0);
    obj_begin(COND_WON, 0);   /* arg 0: shown while the lake is not won */
    cyl(0, -0.2f, 0, 3, 3, .9f, 14, 0x6B4A2A, 0x7A5A36);
    cyl(0, .5f, 0, 3.1f, 3.1f, .25f, 14, 0x3B3B3B, 0x3B3B3B);
    for (int i = 0; i < 12; i++) { float a = i * M_TAU_F / 12; box(m_cos(a) * 1.2f, 1.6f + m_sin(a) * 1.2f, 0, .4f, .4f, .4f, 0x3B3B3B); }
    obj_end();
    /* after the plug: a dark hole with a stone doorway */
    m_at(W.caveMouth, yaw_to(deepest, W.tank.x, W.tank.z));
    obj_begin(COND_WON, 1);
    cyl(0, .02f, 0, 2.8f, 2.8f, .05f, 14, 0x050505, 0x050505);
    box(-1.5f, 1.6f, 0, .8f, 3.2f, .9f, 0x6F6A5F); box(1.5f, 1.6f, 0, .8f, 3.2f, .9f, 0x6F6A5F);
    box(0, 3.5f, 0, 4, .7f, 1.1f, 0x6F6A5F);
    box(0, 1.4f, .2f, 2.25f, 2.8f, .1f, 0x000000);
    for (int i = 0; i < 2; i++) box(0, .1f + i * .2f, -1.2f + i * .5f, 2.4f, .25f, .6f, 0x6F6A5F);
    obj_end();
}

/* ---- creeks ---- */
static const float CREEK_SRC[NCREEKS][2] = { { 1.32f, 2.4f }, { 2.2f, 2.5f }, { -0.12f, 2.35f } };
static void plan_creeks(void) {
    for (int c = 0; c < NCREEKS; c++) {
        v3 src = w_on_ellipse(CREEK_SRC[c][0], CREEK_SRC[c][1]);
        float m = m_sqrt(src.x * src.x + src.z * src.z);
        if (m > 120) { src.x *= 120 / m; src.z *= 120 / m; src.y = w_h(src.x, src.z); }
        W.creekHead[c] = src;
        /* downhill: step along the steepest descent with a little momentum, until we reach the lake */
        v3 p = src; float dx = -src.x, dz = -src.z; float l = m_sqrt(dx * dx + dz * dz); dx /= l; dz /= l;
        int n = 0;
        W.creekPath[c][n++] = p;
        while (n < 160 && p.y > LAKE_FULL - 0.3f) {
            float e = 0.8f, gx = (w_h(p.x + e, p.z) - w_h(p.x - e, p.z)) / (2 * e), gz = (w_h(p.x, p.z + e) - w_h(p.x, p.z - e)) / (2 * e);
            float gl = m_sqrt(gx * gx + gz * gz);
            float tx = -p.x, tz = -p.z, tl = m_sqrt(tx * tx + tz * tz); tx /= tl; tz /= tl;      /* pull towards the lake on flats */
            float nx = dx * 0.55f + (gl > 1e-4f ? -gx / gl : 0) * 0.35f + tx * 0.25f, nz = dz * 0.55f + (gl > 1e-4f ? -gz / gl : 0) * 0.35f + tz * 0.25f;
            float nl = m_sqrt(nx * nx + nz * nz); dx = nx / nl; dz = nz / nl;
            p.x += dx * 1.5f; p.z += dz * 1.5f; p.y = w_h(p.x, p.z);
            W.creekPath[c][n++] = p;
        }
        W.creekLen[c] = n;
    }
}

static void build_creek_props(void) {
    for (int c = 0; c < NCREEKS; c++) {
        v3 h = W.creekHead[c];
        float yaw = yaw_to(h, 0, 0);
        m_at(h, yaw);
        obj_begin(COND_ALWAYS, 0);   /* the spring: a ring of rocks, plus a stone spring house for Mill Run */
        for (int k = 0; k < 7; k++) { float a = k * 0.9f + 1.2f; blob(m_cos(a) * 1.8f, .2f, m_sin(a) * 1.8f - .4f, .6f, .45f, .6f, 5, 0x6E6A62); }
        if (c == 2) { box(0, .95f, -1.25f, 2.4f, 1.7f, 2, 0x7D776C); roof(0, 1.8f, -1.25f, 2.4f, 2, .9f, 0x5A4A40); }
        obj_end();
        obj_begin(COND_CREEK_STATE, c * 10 + 1);   /* dammed */
        if (c == 0) for (int k = -2; k <= 2; k++) blob(k * .55f, .35f, .9f, .4f, .4f, .4f, 5, 0x8A857B);
        else if (c == 1) { box(0, .7f, 1, 2.6f, 1.4f, .25f, 0x8A6A46); box(-1.3f, .9f, 1, .25f, 1.8f, .3f, 0x5A3F28); box(1.3f, .9f, 1, .25f, 1.8f, .3f, 0x5A3F28); }
        else cylx(0, 1.4f, -.15f, .32f, .1f, 10, 0x7A4A2A);
        obj_end();
        /* diverted: a bottling shed beside the spring */
        v3 sp = local_point(h, yaw, 4, 1);
        m_at(sp, yaw);
        obj_begin(COND_CREEK_STATE, c * 10 + 2);
        box(0, 1.2f, 0, 2.6f, 2.4f, 2.8f, 0x8A7A62); roof(0, 2.4f, 0, 2.6f, 2.8f, 1.1f, 0x4A6A5A);
        box(0, 2.0f, 1.45f, 2.0f, .5f, .08f, 0xE6BC62);
        obj_end();
    }
    /* river stones along Arm Creek */
    int n = W.creekLen[0];
    for (int k = 0; k < 5; k++) {
        int pi = (int)(n * (.15f + k * .13f) + .5f); if (pi < 1) pi = 1; if (pi > n - 2) pi = n - 2;
        v3 d = v_sub(W.creekPath[0][pi + 1], W.creekPath[0][pi - 1]);
        v3 sd = v_norm(V3(d.z, 0, -d.x)); float off = k % 2 == 0 ? 1.9f : -1.9f;
        v3 p = v_add(W.creekPath[0][pi], v_mul(sd, off)); p.y = w_h(p.x, p.z);
        W.stone[k] = p;
        m_at(p, k);
        obj_begin(COND_TAKEN, k);
        blob(0, .15f, 0, .35f, .25f, .3f, 6, 0x9A958B);
        obj_end();
    }
    /* the plank stack beside Merrow Cabin, 2.4 m past its right-hand wall (the cabin reaches x = 4.4) */
    int ci = building_index("cabin");
    W.plankStack = local_point(W.building[ci], W.buildingYaw[ci], 4.4f + 2.4f, 1.2f);
    m_at(W.plankStack, W.buildingYaw[ci]);
    obj_begin(COND_ALWAYS, 0);
    for (int row = 0; row < 3; row++) for (int k = 0; k < 5 - row; k++) box(-.5f + k * .25f + row * .12f, .1f + row * .14f, 0, .22f, .12f, 2.4f, k % 2 ? 0x8A6A46 : 0xA07A52);
    obj_end();
    /* the valve wheel near the south shore */
    W.valve = w_on_ellipse(-1.95f, 1.75f);
    m_at(W.valve, 0);
    obj_begin(COND_TAKEN, 5);
    cyl(0, 0, 0, .35f, .35f, .08f, 10, 0x7A4A2A, 0x7A4A2A);
    obj_end();
}

/* creek water: a ribbon of quads following the path, drawn while the creek flows */
static void draw_creeks(uint32_t light);

/* ---- the drowned village ---- */
static int free_spot(float x, float z, float r, const float (*av)[2], int nav) {
    for (int i = 0; i < nav; i++) { float dx = x - av[i][0], dz = z - av[i][1]; if (dx * dx + dz * dz < r * r) return 0; }
    return 1;
}
static float spot_slope(float x, float z, float r) {
    float lo = 1e9f, hi = -1e9f;
    for (int k = 0; k < 8; k++) { float a = k * M_PI_F / 4, h = w_h(x + m_cos(a) * r, z + m_sin(a) * r); lo = m_min(lo, h); hi = m_max(hi, h); }
    return hi - lo;
}
static int pick_spot(float minD, float maxD, float clear, float slope, int hasNear, float nx, float nz, float nearR, const float (*av)[2], int nav, float *ox, float *oz, uint32_t *rs) {
    int best = -1; float bestSlope = 99;
    for (int n = 0; n < 2500; n++) {
        int i = m_rnd(rs) % nbasin;
        float d = LAKE_FULL - bh[i];
        if (d < minD || d > maxD || !free_spot(bx[i], bz[i], clear, av, nav)) continue;
        if (hasNear && (bx[i] - nx) * (bx[i] - nx) + (bz[i] - nz) * (bz[i] - nz) > nearR * nearR) continue;
        if ((bx[i] - deepest.x) * (bx[i] - deepest.x) + (bz[i] - deepest.z) * (bz[i] - deepest.z) < 256) continue;
        float sl = spot_slope(bx[i], bz[i], 4.5f);
        if (sl < bestSlope) { bestSlope = sl; best = i; }
        if (sl < slope) break;
    }
    if (best < 0) return 0;
    *ox = bx[best]; *oz = bz[best];
    return 1;
}

static void build_drowned(void) {
    float av[40][2]; int nav = 0;
    for (int i = 0; i < NITEMS; i++) { av[nav][0] = W.item[i].x; av[nav][1] = W.item[i].z; nav++; }
    for (int i = 0; i < NSPRINGS; i++) { av[nav][0] = W.spring[i].x; av[nav][1] = W.spring[i].z; nav++; }
    av[nav][0] = deepest.x; av[nav][1] = deepest.z; nav++;
    uint32_t rs = 1891;
    float cx, cz;
    if (!pick_spot(9.5f, 12, 12, 2.4f, 0, 0, 0, 0, av, nav, &cx, &cz, &rs)) { cx = deepest.x + 20; cz = deepest.z; }
    av[nav][0] = cx; av[nav][1] = cz; nav++;
    W.church = ground_at(cx, cz);
    float base = flat_base(W.church, 5);
    float yaw = yaw_to(W.church, 0, 0) + M_PI_F;
    curArg2 = 0;
    m_at(V3(cx, base, cz), yaw);
    obj_begin(COND_WORLD, 1);
    box(0, 2.6f, 0, 7, 5.2f, 12, 0x6A665C);
    roof(0, 5.2f, 0, 7, 12, 3.4f, 0x3E3C38);
    box(0, 6, -6.6f, 3, 12, 3, 0x6A665C);                 /* the tower */
    cyl(0, 12, -6.6f, 2.2f, 0, 6, 4, 0x3E3C38, 0);        /* the spire that shows above the full lake */
    box(0, 1.7f, -8.12f, 1.6f, 2.8f, .1f, 0x0B0B0A);
    for (int k = -1; k <= 1; k += 2) box(3.52f * k, 3.2f, 1, .1f, 2.2f, 1.2f, 0x0B0B0A);
    obj_end();
    W.churchDoorY = base + 1.4f;
    for (int k = 0; k < NHOUSES; k++) {
        float lo = 2.5f + k * 1.4f, hi = lo + 3.2f, hx, hz;
        if (!pick_spot(lo, hi, 13, 1.6f, 1, cx, cz, 55, av, nav, &hx, &hz, &rs) && !pick_spot(lo, hi + 3, 10, 3, 0, 0, 0, 0, av, nav, &hx, &hz, &rs)) {
            W.house[k] = V3(0, -999, 0); continue;
        }
        av[nav][0] = hx; av[nav][1] = hz; nav++;
        W.house[k] = ground_at(hx, hz);
        float b = flat_base(W.house[k], 3.5f);
        m_at(V3(hx, b, hz), yaw_to(W.house[k], cx, cz));
        obj_begin(COND_WORLD, 1);
        uint32_t wall = c_lerp(0x5E5A50, 0x7A6E5C, (k * 37 % 10) / 10.0f);
        box(0, 1.8f, 0, 5, 3.6f + (k % 3) * .6f, 5.5f, wall);
        roof(0, 3.6f + (k % 3) * .6f, 0, 5, 5.5f, 2, 0x3A3632);
        box(0, 1.2f, 2.8f, 1.1f, 2.0f, .1f, 0x0B0B0A);
        box(-1.5f, 2.4f, 2.8f, .8f, .8f, .1f, 0x0B0B0A);
        obj_end();
        W.houseDoorY[k] = b + 1.0f;
    }
}

/* ---- the cave and the Still Room (their own little scenes around the origin) ---- */
static void build_cave(void) {
    curScene = SCENE_CAVE; curArg2 = 0;
    m_at(V3(0, 0, 0), 0);
    obj_begin(COND_ALWAYS, 0);
    /* floor, ceiling and walls, made of rough blocks */
    uint32_t rs = 1888;
    for (int z = -18; z <= 18; z += 4)
        for (int x = -11; x <= 11; x += 4) {
            float h = m_rndr(&rs, -.1f, .1f);
            box2(x, -.5f + h, z, 4, 1, 4, 0x4F4A42, c_lerp(0x4F4A42, 0x3B3732, m_rndf(&rs)));
        }
    box(0, 11, 0, 26, 1, 40, 0x2E2A26);
    for (int z = -18; z <= 18; z += 4) {
        box(-12.5f, 5, z, 1 + m_rndf(&rs), 12, 4, c_lerp(0x3B3732, 0x4F4A42, m_rndf(&rs)));
        box(12.5f, 5, z, 1 + m_rndf(&rs), 12, 4, c_lerp(0x3B3732, 0x4F4A42, m_rndf(&rs)));
    }
    box(0, 5, 19.5f, 26, 12, 1, 0x3B3732); box(0, 5, -19.5f, 26, 12, 1, 0x3B3732);
    /* the steps back up, boulders and crystals */
    for (int i = 0; i < 4; i++) box(0, .15f + i * .3f, -17.5f + i * .7f, 3, .3f + i * .6f, .7f, 0x6F6A5F);
    for (int i = 0; i < 10; i++) blob(m_rndr(&rs, -10, 10), .3f, m_rndr(&rs, -14, 10), m_rndr(&rs, .6f, 1.4f), m_rndr(&rs, .4f, .9f), m_rndr(&rs, .6f, 1.3f), 5, 0x5A554C);
    for (int i = 0; i < 14; i++) {
        float x = (i & 1 ? 1 : -1) * m_rndr(&rs, 8, 11), z = m_rndr(&rs, -15, 17);
        cyl(x, 0, z, m_rndr(&rs, .25f, .5f), 0, m_rndr(&rs, 1, 2.6f), 4, 0x6FD8E8, 0);
    }
    /* torches */
    for (int z = -12; z <= 12; z += 8) for (int s = -1; s <= 1; s += 2) { box(s * 11.6f, 3, z, .2f, 1.2f, .2f, 0x4A3628); box(s * 11.6f, 3.8f, z, .35f, .5f, .35f, 0xFFB84A); }
    obj_end();
    /* the Gate's ring of carved stone at the far end */
    obj_begin(COND_ALWAYS, 0);
    float gz = 14, R = 3.6f;
    for (int k = 0; k < 16; k++) {
        float a0 = k * M_TAU_F / 16, a1 = (k + 1) * M_TAU_F / 16;
        for (int f = 0; f < 4; f++) {
            float d0 = f < 2 ? -.4f : .4f; (void)d0;
        }
        /* each segment: a box between two points on the ring */
        float am = (a0 + a1) / 2;
        mcenter = V3(0, 4, gz);
        float r0 = R, r1 = R + .9f;
        v3 p[8] = { V3(m_cos(a0) * r0, 4 + m_sin(a0) * r0, gz - .5f), V3(m_cos(a1) * r0, 4 + m_sin(a1) * r0, gz - .5f),
                    V3(m_cos(a1) * r1, 4 + m_sin(a1) * r1, gz - .5f), V3(m_cos(a0) * r1, 4 + m_sin(a0) * r1, gz - .5f),
                    V3(m_cos(a0) * r0, 4 + m_sin(a0) * r0, gz + .5f), V3(m_cos(a1) * r0, 4 + m_sin(a1) * r0, gz + .5f),
                    V3(m_cos(a1) * r1, 4 + m_sin(a1) * r1, gz + .5f), V3(m_cos(a0) * r1, 4 + m_sin(a0) * r1, gz + .5f) };
        uint32_t c = k & 1 ? 0x6A6458 : 0x5E584E;
        (void)am;
        /* faces: front, back, outer, inner (inner faces point to the ring's centre line) */
        mcenter = V3(m_cos(am) * (R + .45f), 4 + m_sin(am) * (R + .45f), gz);
        quad(p[0], p[1], p[2], p[3], c); quad(p[4], p[5], p[6], p[7], c);
        quad(p[3], p[2], p[6], p[7], c_scale(c, .9f)); quad(p[0], p[1], p[5], p[4], c_scale(c, .8f));
    }
    m_at(V3(0, 0, 0), 0);
    box(0, .3f, gz, 9.5f, .6f, 2.4f, 0x6F6A5F);           /* plinth */
    obj_end();
    /* the lectern in the Other Merrow, beside the Gate */
    obj_begin(COND_WORLD, 2);
    box(4.5f, .6f, 12, .4f, 1.2f, .4f, 0x5A3F28); box(4.5f, 1.3f, 12, 1, .15f, .7f, 0x6A4A30); box(4.5f, 1.42f, 12, .6f, .05f, .4f, 0xE8DFC8);
    obj_end();
    /* the Still Room: a round room far below, with the chair and the last page */
    curScene = SCENE_STILL;
    obj_begin(COND_ALWAYS, 0);
    for (int k = 0; k < 22; k++) {
        float a = k * M_TAU_F / 22, R2 = 8.5f;
        m_at(V3(m_cos(a) * R2, 0, m_sin(a) * R2), -a + M_PI_F / 2);
        box(0, 4.5f, 0, 2.6f, 9, 1, k & 1 ? 0x24333A : 0x2A3A42);
        if (k % 2 == 0) box(0, .35f, -.7f, 2.2f, .7f, .3f, 0x3A4A50);
    }
    m_at(V3(0, 0, 0), 0);
    cyl(0, -1, 0, 9, 9, 1, 22, 0x1E2A30, 0x2A3A40);
    cyl(0, 9, 0, 9, 9, .3f, 22, 0x071318, 0x071318);
    cyl(0, 0, 0, 2.8f, 2.8f, .3f, 16, 0x3A4A50, 0x06141A);   /* the still pool in the middle */
    /* the chair */
    box(0, .5f, 4.8f, 1, .15f, 1, 0x5A3F28); box(0, 1.3f, 5.3f, 1, 1.6f, .15f, 0x5A3F28);
    for (int s = -1; s <= 1; s += 2) for (int t = -1; t <= 1; t += 2) box(s * .42f, .25f, 4.8f + t * .42f, .1f, .5f, .1f, 0x4A3220);
    /* the lectern with the last page, and the door home */
    box(-3.5f, .6f, 3.5f, .4f, 1.2f, .4f, 0x5A3F28); box(-3.5f, 1.3f, 3.5f, 1, .15f, .7f, 0x6A4A30); box(-3.5f, 1.42f, 3.5f, .6f, .05f, .4f, 0xE8DFC8);
    box(0, 1.6f, -8.0f, 1.8f, 3.2f, .2f, 0x3A2A1E);
    obj_end();
    curScene = SCENE_OUTSIDE;
}

/* ================= interactables ================= */
enum { IT_TANK, IT_STORE, IT_BUILDING, IT_SECRET, IT_ITEM, IT_SPRING, IT_PLUG, IT_CAVEDOOR, IT_CREEK, IT_STONE, IT_PLANKS, IT_VALVE,
       IT_HOUSE, IT_CHURCH, IT_GATE, IT_LECTERN, IT_CAVE_EXIT, IT_LASTPAGE, IT_STILL_DOOR, IT_SCOOP };
static int interact_kind = -1, interact_arg; static v3 interact_pos;

static int item_exposed(int i) { return w_level() <= W.item[i].y + 0.05f; }
static int spring_visible(int i) { return (G.springsOpen >> i & 1); }
static int house_exposed(int k) { return W.house[k].y > -900 && w_level() < W.houseDoorY[k]; }
static int church_exposed(void) { return w_level() < W.churchDoorY; }

static float d2(v3 a, v3 b) { float dx = a.x - b.x, dz = a.z - b.z, dy = a.y - b.y; return dx * dx + dz * dz + dy * dy * 0.25f; }

/* the nearest thing in reach, with its prompt. Returns 1 if there is one. */
int w_find_interact(char *prompt, int n) {
    v3 p = P.pos; float best = 1e9f; int kind = -1, arg = 0;
    float fx = m_sin(P.yaw), fz = m_cos(P.yaw);
    /* things in front of you win over things beside or behind you */
#define CAND(k, a, pos, reach) do { v3 q_ = (pos); float dd = d2(p, q_); if (dd < (reach) * (reach)) {         float l_ = m_sqrt(dd) + 1e-3f, f_ = ((q_.x - p.x) * fx + (q_.z - p.z) * fz) / l_; float sc_ = dd * (f_ > 0.5f ? 0.5f : f_ > 0 ? 1.0f : 2.0f);         if (sc_ < best) { best = sc_; kind = k; arg = a; interact_pos = q_; } } } while (0)
    if (W.scene == SCENE_CAVE) {
        CAND(IT_GATE, 0, V3(0, 0, 12.5f), 5.5f);
        CAND(IT_CAVE_EXIT, 0, V3(0, 0, -16.5f), 3.5f);
        if (other_side()) CAND(IT_LECTERN, 0, V3(4.5f, 0, 12), 2.6f);
    } else if (W.scene == SCENE_STILL) {
        CAND(IT_LASTPAGE, 0, V3(-3.5f, 0, 3.5f), 2.8f);
        CAND(IT_LASTPAGE, 0, V3(0, 0, 4.8f), 2.2f);
        CAND(IT_STILL_DOOR, 0, V3(0, 0, -7.5f), 2.8f);
    } else {
        CAND(IT_TANK, 0, W.tank, 5.5f);
        CAND(IT_STORE, 0, W.store, 7.5f);
        for (int i = 0; i < NBUILD; i++) CAND(IT_BUILDING, i, W.building[i], 7.5f);
        for (int i = 0; i < NSECRETS; i++) CAND(IT_SECRET, i, W.secret[i], 3.4f);
        for (int i = 0; i < NITEMS; i++) if (!found_item(i) && item_exposed(i)) CAND(IT_ITEM, i, W.item[i], 3.2f + m_max(ITEMS[i].sx, ITEMS[i].sz) * .5f);
        for (int i = 0; i < NSPRINGS; i++) if (g_spring_flowing(i) && w_level() < W.spring[i].y + 1.5f) CAND(IT_SPRING, i, W.spring[i], 3.6f);
        if (g_plug_exposed()) CAND(IT_PLUG, 0, deepest, 4.8f);
        if (G.won) CAND(IT_CAVEDOOR, 0, W.caveMouth, 4.0f);
        for (int c = 0; c < NCREEKS; c++) CAND(IT_CREEK, c, W.creekHead[c], 4.5f);
        for (int k = 0; k < 5; k++) if (!(G.taken >> k & 1)) CAND(IT_STONE, k, W.stone[k], 2.6f);
        CAND(IT_PLANKS, 0, W.plankStack, 2.8f);
        if (!(G.taken & TAKEN_VALVE)) CAND(IT_VALVE, 0, W.valve, 2.6f);
        if (is_drowned()) {
            if (church_exposed()) CAND(IT_CHURCH, 0, W.church, 8.0f);
            for (int k = 0; k < NHOUSES; k++) if (house_exposed(k)) CAND(IT_HOUSE, k, W.house[k], 5.0f);
        }
    }
#undef CAND
    /* scooping: standing in the water, or at the shore facing it */
    if (kind < 0 && W.scene == SCENE_OUTSIDE && G.water > 0 && !G.won) {
        float lv = w_level();
        if (w_ground(P.pos.x, P.pos.z) < lv - 0.05f) { kind = IT_SCOOP; interact_pos = V3(P.pos.x, lv, P.pos.z); }
        else for (float r = 1.0f; r <= 4.01f; r += 1.0f) {
            v3 f = V3(P.pos.x + m_sin(P.yaw) * r, 0, P.pos.z + m_cos(P.yaw) * r);
            if (w_ground(f.x, f.z) < lv - 0.1f) { kind = IT_SCOOP; interact_pos = V3(f.x, lv, f.z); break; }
        }
    }
    interact_kind = kind; interact_arg = arg;
    if (kind < 0) return 0;
    char a[24], b[24];
    switch (kind) {
    case IT_TANK: g_tank_prompt(prompt, n); break;
    case IT_STORE: ksnprintf(prompt, n, "[E]  Supply Store: pumps and upgrades"); break;
    case IT_BUILDING: {
        const struct building_def *bd = &BUILDINGS[arg];
        if (G.explored >> arg & 1) ksnprintf(prompt, n, "[E]  %s", bd->name);
        else if (bd->req && !found_item(item_index(bd->req))) ksnprintf(prompt, n, "[E]  %s (locked)", bd->name);
        else ksnprintf(prompt, n, "[E]  Explore the %s", bd->name);
        break;
    }
    case IT_SECRET: ksnprintf(prompt, n, "[E]  %s", SECRETS[arg].name); break;
    case IT_ITEM: ksnprintf(prompt, n, "[E]  Pick up: %s", ITEMS[arg].name); break;
    case IT_SPRING: fmt_money(SPRINGS[arg].cost, a); ksnprintf(prompt, n, "[E]  Cap the %s (%s)", SPRINGS[arg].name, a); break;
    case IT_PLUG: ksnprintf(prompt, n, "[E]  %s", PLUG_NAME); break;
    case IT_CAVEDOOR: ksnprintf(prompt, n, "[E]  Go down into the cave"); break;
    case IT_CREEK: g_creek_prompt(arg, prompt, n); break;
    case IT_STONE: ksnprintf(prompt, n, "[E]  River stone"); break;
    case IT_PLANKS: ksnprintf(prompt, n, G.creekState[1] == 0 ? "[E]  Take a plank (%d/4)" : "Plank stack", G.planks); break;
    case IT_VALVE: ksnprintf(prompt, n, "[E]  Old valve wheel"); break;
    case IT_HOUSE: ksnprintf(prompt, n, (G.taken & TAKEN_HOUSE(arg)) ? "[E]  %s (searched)" : "[E]  Search %s", HOUSES[arg].name); break;
    case IT_CHURCH: ksnprintf(prompt, n, (G.taken & TAKEN_CHURCH) ? "[E]  %s (searched)" : "[E]  Search %s", DROWNED_CHURCH); break;
    case IT_GATE: g_gate_prompt(prompt, n); break;
    case IT_LECTERN: ksnprintf(prompt, n, "[E]  Read the page on the lectern"); break;
    case IT_CAVE_EXIT: ksnprintf(prompt, n, "[E]  Climb back up to the lakebed"); break;
    case IT_LASTPAGE: ksnprintf(prompt, n, "[E]  Read the last page"); break;
    case IT_STILL_DOOR: ksnprintf(prompt, n, "[E]  The door (back up to the cave)"); break;
    case IT_SCOOP: fmt_num(G.carried, a); fmt_num(g_carry_cap(), b); ksnprintf(prompt, n, "[E]  Scoop water  (bucket %s / %s gal)", a, b); break;
    }
    return 1;
}

static void enter_cave(void) { W.scene = SCENE_CAVE; p_teleport(V3(0, 0, -11), 0); }
static void leave_cave(void) { W.scene = SCENE_OUTSIDE; v3 m = W.caveMouth; float y = yaw_to(m, W.tank.x, W.tank.z); p_teleport(V3(m.x + m_sin(y) * 4, 0, m.z + m_cos(y) * 4), y); }
static void leave_still(void) { W.scene = SCENE_CAVE; p_teleport(V3(0, 0, 9), M_PI_F); }

void w_interact(void) {
    if (interact_kind < 0) return;
    int a = interact_arg;
    switch (interact_kind) {
    case IT_TANK: g_tank_action(); break;
    case IT_STORE: hud_open_shop(); break;
    case IT_BUILDING: g_explore_building(a); break;
    case IT_SECRET: g_find_secret(a); break;
    case IT_ITEM: g_collect_item(a); break;
    case IT_SPRING: g_cap_spring(a); break;
    case IT_PLUG: g_ask_pull_plug(); break;
    case IT_CAVEDOOR: hud_fade(enter_cave); break;
    case IT_CREEK: g_creek_action(a); break;
    case IT_STONE: g_take_stone(a); break;
    case IT_PLANKS: if (G.creekState[1] == 0) g_take_plank(); else hud_toast("Fern Brook is already shut."); break;
    case IT_VALVE: g_take_valve(); break;
    case IT_HOUSE: g_search_drowned(a); break;
    case IT_CHURCH: g_search_drowned(-1); break;
    case IT_GATE: g_gate_action(); break;
    case IT_LECTERN: g_read_page(2, "A plain wooden lectern beside the Gate, and on it, weighted with a stone, a page."); break;
    case IT_CAVE_EXIT: hud_fade(leave_cave); break;
    case IT_LASTPAGE: g_read_last_page(); break;
    case IT_STILL_DOOR: hud_fade(leave_still); break;
    case IT_SCOOP: g_scoop(interact_pos); break;
    }
}
v3 w_interact_pos(void) { return interact_pos; }

/* solid things the player bumps into (outside) */
struct solid { float x, z, r, top; int world; };   /* world: -1 every valley */
static struct solid solids[64]; static int nsolids;
static void add_solid_w(v3 p, float r, float h, int world) { if (nsolids < 64) { struct solid *s = &solids[nsolids++]; s->x = p.x; s->z = p.z; s->r = r; s->top = p.y + h; s->world = world; } }
static void add_solid(v3 p, float r) { add_solid_w(p, r, 8, -1); }
/* is this point inside something solid? (for the camera) */
int w_blocked(v3 p) {
    if (W.scene != SCENE_OUTSIDE) return 0;
    for (int i = 0; i < nsolids; i++) {
        const struct solid *s = &solids[i];
        if (s->world >= 0 && s->world != world_no()) continue;
        float dx = p.x - s->x, dz = p.z - s->z;
        if (dx * dx + dz * dz < s->r * s->r && p.y < s->top) return 1;
    }
    return 0;
}
void w_collide(v3 *p) {
    if (W.scene == SCENE_CAVE) { p->x = m_clamp(p->x, -11.2f, 11.2f); p->z = m_clamp(p->z, -17.8f, 17.6f); if (p->z > 12.3f && m_abs(p->x) < 4.6f) p->z = 12.3f; return; }
    if (W.scene == SCENE_STILL) { float r = m_sqrt(p->x * p->x + p->z * p->z); if (r > 7.6f) { p->x *= 7.6f / r; p->z *= 7.6f / r; } return; }
    for (int i = 0; i < nsolids; i++) {
        if (solids[i].world >= 0 && solids[i].world != world_no()) continue;
        if (p->y > solids[i].top - 0.5f) continue;
        float dx = p->x - solids[i].x, dz = p->z - solids[i].z, d = m_sqrt(dx * dx + dz * dz);
        if (d < solids[i].r && d > 1e-3f) { p->x = solids[i].x + dx / d * solids[i].r; p->z = solids[i].z + dz / d * solids[i].r; }
    }
    float r = m_sqrt(p->x * p->x + p->z * p->z);
    if (r > PLAYER_BOUND) { p->x *= PLAYER_BOUND / r; p->z *= PLAYER_BOUND / r; }
}

/* ================= trees ================= */
static void plant_trees(void (*progress)(const char *, int)) {
    uint32_t rs = 1888;
    for (float z = -148; z < 148; z += 5.5f) {
        if (progress && ((int)z % 40) == 0) progress("Planting the forest", 70 + (int)((z + 148) * 20 / 296));
        for (float x = -148; x < 148; x += 5.5f) {
            float px = x + m_rndr(&rs, -2.2f, 2.2f), pz = z + m_rndr(&rs, -2.2f, 2.2f);
            float rho = w_rho(px, pz, 0);
            if (rho < 1.12f) continue;
            float fd = forest_density(px, pz);
            float chance = rho > 2.0f ? m_smooth(0.3f, 0.5f, fd) * 0.8f : m_smooth(0.45f, 0.62f, fd) * 0.75f;
            if (m_rndf(&rs) > chance) { (void)m_rnd(&rs); continue; }
            if (path_dist(px, pz) < 3.2f) continue;
            if (px * px + pz * pz > 145 * 145) continue;
            int bad = 0;
            for (int i = 0; i < nsolids && !bad; i++) { float dx = px - solids[i].x, dz = pz - solids[i].z; if (dx * dx + dz * dz < (solids[i].r + 6) * (solids[i].r + 6)) bad = 1; }
            for (int c = 0; c < NCREEKS && !bad; c++)
                for (int k = 0; k < W.creekLen[c]; k += 2) { v3 q = W.creekPath[c][k]; if ((q.x - px) * (q.x - px) + (q.z - pz) * (q.z - pz) < 16) { bad = 1; break; } }
            for (int k = 0; k < NSECRETS && !bad; k++) if (v_dist2(W.secret[k], V3(px, 0, pz)) < 25) bad = 1;
            if (v_dist2(W.valve, V3(px, 0, pz)) < 16 || v_dist2(W.plankStack, V3(px, 0, pz)) < 16) bad = 1;
            if (bad) continue;
            float h = w_h(px, pz);
            int kind = (h > 18 || m_rndf(&rs) < 0.62f) ? 0 : 1;
            build_tree(V3(px, h, pz), m_rndr(&rs, 0.8f, 1.35f), kind, m_rnd(&rs));
        }
    }
}

/* ================= init ================= */
int w_init(void (*progress)(const char *, int)) {
    tris = malloc(sizeof(struct stri) * MAXTRI);
    if (!tris) return 0;
    w_planar(-2.95f, 1.45f, &chapelX, &chapelZ); w_planar(-1.57f, 2.35f, &obsX, &obsZ);
    for (int i = 0; i < 7; i++) { w_planar(SPURS[i][0], SPURS[i][1], &spurA[i][0], &spurA[i][1]); w_planar(SPURS[i][0], SPURS[i][2], &spurB[i][0], &spurB[i][1]); }
    if (progress) progress("Measuring the lake", 2);
    build_volume();
    build_grid(progress);
    if (progress) progress("Painting the far mountains", 52);
    build_backdrop();
    if (progress) progress("Building the shore", 62);
    /* positions */
    W.tank = v_add(w_on_ellipse(0.3f, 1.16f), V3(0, -0.2f, 0)); W.tankYaw = yaw_to(W.tank, 0, 0);
    W.store = v_add(w_on_ellipse(0.42f, 1.45f), V3(0, -0.2f, 0)); W.storeYaw = yaw_to(W.store, 0, 0);
    for (int i = 0; i < NBUILD; i++) { W.building[i] = w_on_ellipse(BUILDINGS[i].angle, BUILDINGS[i].radius); W.buildingYaw[i] = yaw_to(W.building[i], 0, 0); }
    for (int i = 0; i < NSECRETS; i++) W.secret[i] = w_on_ellipse(SECRETS[i].angle, SECRETS[i].radius);
    for (int i = 0; i < NITEMS; i++) {
        float angle = i * 2.39996f + 0.6f, target = w_level_for_geo(1 - ITEMS[i].g) - 0.05f;
        v3 p = w_on_ellipse(angle, 0.5f);
        for (float rr = 1; rr > 0; rr -= 0.004f) { v3 q = w_on_ellipse(angle, rr); if (q.y <= target) { p = q; break; } }
        W.item[i] = p;
    }
    for (int i = 0; i < NSPRINGS; i++) {
        float target = w_level_for_geo(1 - SPRINGS[i].g) - .1f;
        v3 p = w_on_ellipse(SPRINGS[i].angle, .5f);
        for (float rr = 1; rr > 0; rr -= .004f) { v3 q = w_on_ellipse(SPRINGS[i].angle, rr); if (q.y <= target) { p = q; break; } }
        W.spring[i] = p;
    }
    {   /* the cave doorway: on the highest ground right around the deepest point */
        float top = deepest.y;
        for (int k = 0; k < 8; k++) { float b = k * M_PI_F / 4; top = m_max(top, w_h(deepest.x + m_cos(b) * 2, deepest.z + m_sin(b) * 2)); }
        W.caveMouth = V3(deepest.x, top - .3f, deepest.z);
    }
    plan_creeks();
    add_solid(W.tank, 3.2f); add_solid(W.store, 5.6f);
    for (int i = 0; i < NBUILD; i++) add_solid(W.building[i], 4.4f);
    /* meshes */
    nobjs = ntris = 0; curScene = SCENE_OUTSIDE;
    build_store(); build_tank(); build_pumps();
    for (int i = 0; i < NBUILD; i++) build_building(i);
    for (int i = 0; i < NSECRETS; i++) build_secret(i);
    for (int i = 0; i < NITEMS; i++) build_item_model(i);
    build_springs(); build_plug(); build_creek_props(); build_drowned();
    add_solid_w(W.church, 5.2f, 18, 1);
    for (int k = 0; k < NHOUSES; k++) if (W.house[k].y > -900) add_solid_w(W.house[k], 3.3f, 6, 1);
    build_cave();
    plant_trees(progress);
    if (progress) progress("Filling the lake", 96);
    tint_land();
    W.weatherTimer = 200; W.lightningTimer = 15;
    return 1;
}

void w_refresh(void) {
    tint_land();
    if (W.scene != SCENE_OUTSIDE) W.scene = SCENE_OUTSIDE;
    /* the weather each valley starts with */
    if (is_drowned()) { W.cloud = W.cloudT = .9f; W.rain = W.rainT = .8f; W.mist = W.mistT = .2f; }
    else if (other_side()) { W.cloud = W.cloudT = .2f; W.rain = W.rainT = 0; W.mist = W.mistT = .3f; }
    else { W.cloud = W.cloudT = .2f; W.rain = W.rainT = 0; W.mist = W.mistT = 0; }
    v3 s = w_on_ellipse(0.58f, 1.05f);
    p_teleport(s, yaw_to(s, 0, 0));
}

/* ================= time, weather, wildlife ================= */
struct duck { v3 p; float yaw, phase; };
static struct duck ducks[7]; static v3 duckCenter, duckGoal; static float duckFlee, duckGoalTimer; static int duckState;   /* 0 swimming, 1 flying, 2 gone */
struct fish { v3 p; float t, life; };
static struct fish fish[4];
struct particle { v3 p, v; float life; uint32_t col; float size; };
static struct particle parts[96];
static uint32_t wr = 4242;

void w_splash(v3 at) {
    for (int k = 0; k < 12; k++) {
        for (int i = 0; i < 96; i++) if (parts[i].life <= 0) {
            parts[i].p = at; parts[i].v = V3(m_rndr(&wr, -1.5f, 1.5f), m_rndr(&wr, 2.5f, 5), m_rndr(&wr, -1.5f, 1.5f));
            parts[i].life = m_rndr(&wr, .5f, .9f); parts[i].col = 0xDDF0F4; parts[i].size = m_rndr(&wr, .12f, .25f);
            break;
        }
    }
}

void w_clock(char *out) {
    int h = (int)W.hour, m = (int)((W.hour - h) * 60) / 10 * 10;
    ksnprintf(out, 16, "%d:%02d %s", h % 12 == 0 ? 12 : h % 12, m, h < 12 ? "am" : "pm");
}
const char *w_weather_name(void) { return W.rain > .5f ? "Rain" : W.mist > .5f ? "Mist" : W.cloud > .5f ? "Overcast" : "Clear"; }

static float approach(float v, float t, float k) { return v + (t - v) * m_clamp01(k); }

void w_update(float dt) {
    W.time += dt;
    /* time of day: 50 s per daytime hour, nights pass twice as fast. The Other Merrow is always dusk. */
    if (other_side()) W.hour = 19.25f;
    else { int day = W.hour >= 5.5f && W.hour <= 19.5f; W.hour = m_repeat(W.hour + dt / 50.0f * (day ? 1 : 2), 24); }
    /* weather drifts between clear, overcast, rain and morning mist */
    W.weatherTimer -= dt;
    if (W.weatherTimer <= 0) {
        W.weatherTimer = m_rndr(&wr, 150, 320);
        float r = m_rndf(&wr);
        if (is_drowned()) { W.cloudT = .9f; W.rainT = r < .75f ? m_rndr(&wr, .6f, 1) : .2f; W.mistT = r > .75f ? .7f : .2f; }
        else if (other_side()) { W.cloudT = .2f; W.rainT = 0; W.mistT = m_rndr(&wr, .1f, .5f); }
        else if (r < .45f) { W.cloudT = m_rndr(&wr, 0, .3f); W.rainT = 0; W.mistT = 0; }
        else if (r < .7f) { W.cloudT = m_rndr(&wr, .6f, .9f); W.rainT = 0; W.mistT = 0; }
        else if (r < .88f) { W.cloudT = .95f; W.rainT = m_rndr(&wr, .6f, 1); W.mistT = 0; }
        else { W.cloudT = .4f; W.rainT = 0; W.mistT = (W.hour > 4 && W.hour < 10) ? .9f : .5f; }
    }
    W.cloud = approach(W.cloud, W.cloudT, dt / 20); W.rain = approach(W.rain, W.rainT, dt / 15); W.mist = approach(W.mist, W.mistT, dt / 25);
    /* lightning in heavy rain */
    W.flash = m_max(0, W.flash - dt * 3);
    if (W.rain > .7f && !other_side()) { W.lightningTimer -= dt; if (W.lightningTimer <= 0) { W.flash = 1; W.lightningTimer = m_rndr(&wr, 8, 25); } }
    /* night 0..1 from the sun's height */
    float a = (W.hour - 6) / 12 * M_PI_F;
    W.night = m_smooth(0.12f, -0.18f, m_sin(a));
    if (other_side()) W.night = 0.45f;
    /* particles */
    for (int i = 0; i < 96; i++) if (parts[i].life > 0) {
        parts[i].life -= dt; parts[i].v.y -= 12 * dt;
        parts[i].p = v_add(parts[i].p, v_mul(parts[i].v, dt));
    }
    /* ducks: a raft that paddles about, takes off when you get close, and leaves when the lake is nearly gone */
    float lv = w_level();
    int lakeOk = g_progress() < 0.85f && !G.won && !other_side();
    if (duckState == 0 && duckCenter.y == 0 && duckCenter.x == 0) { duckCenter = V3(-20, lv, -8); duckGoal = duckCenter; duckCenter.y = lv; }
    if (duckState == 0) {
        duckGoalTimer -= dt;
        if (duckGoalTimer <= 0 || v_dist2(duckCenter, duckGoal) < 4) {
            duckGoalTimer = m_rndr(&wr, 10, 25);
            for (int t = 0; t < 20; t++) {
                int i = m_rnd(&wr) % nbasin;
                if (bh[i] < lv - 1.5f) { duckGoal = V3(bx[i], lv, bz[i]); break; }
            }
        }
        v3 d = v_sub(duckGoal, duckCenter); d.y = 0; float l = v_len(d);
        if (l > .1f) duckCenter = v_add(duckCenter, v_mul(d, dt * 1.2f / l));
        if (w_ground(duckCenter.x, duckCenter.z) > lv - .4f) duckGoalTimer = 0;
        duckCenter.y = lv;
        if (v_dist2(duckCenter, P.pos) < 12 * 12 || !lakeOk) { duckState = 1; duckFlee = 0; }
    } else if (duckState == 1) {
        duckFlee += dt;
        v3 away = v_norm(V3(duckCenter.x - P.pos.x, 0, duckCenter.z - P.pos.z));
        duckCenter = v_add(duckCenter, V3(away.x * 9 * dt, 4 * dt, away.z * 9 * dt));
        if (duckFlee > 8) {
            if (lakeOk) {   /* land somewhere else on the lake */
                for (int t = 0; t < 30; t++) { int i = m_rnd(&wr) % nbasin; if (bh[i] < lv - 2 && (bx[i] - P.pos.x) * (bx[i] - P.pos.x) + (bz[i] - P.pos.z) * (bz[i] - P.pos.z) > 900) { duckCenter = V3(bx[i], lv, bz[i]); duckGoal = duckCenter; duckState = 0; break; } }
            } else duckState = 2;
        }
    } else if (lakeOk && m_rndf(&wr) < dt / 30) { duckState = 0; duckCenter = V3(0, lv, 0); duckGoalTimer = 0; }
    for (int i = 0; i < 7; i++) {
        ducks[i].phase += dt;
        float a2 = i * 0.9f + ducks[i].phase * 0.15f, r = 1.2f + (i % 3) * 1.1f;
        ducks[i].p = V3(duckCenter.x + m_cos(a2) * r, duckCenter.y + (duckState == 1 ? m_sin(ducks[i].phase * 9 + i) * .3f : 0), duckCenter.z + m_sin(a2) * r);
        ducks[i].yaw = duckState == 1 ? m_atan2(duckCenter.x - P.pos.x, duckCenter.z - P.pos.z) : a2 + M_PI_F / 2;
    }
    /* fish flopping on the fresh mud just above the waterline */
    for (int i = 0; i < 4; i++) {
        fish[i].t += dt;
        if (fish[i].life <= 0) {
            if (g_progress() > 0.03f && !G.won && m_rndf(&wr) < dt / 6) {
                for (int t = 0; t < 40; t++) {
                    int k = m_rnd(&wr) % nbasin;
                    if (bh[k] > lv + .1f && bh[k] < lv + 1.2f) { fish[i].p = V3(bx[k], bh[k], bz[k]); fish[i].life = m_rndr(&wr, 8, 16); fish[i].t = 0; break; }
                }
            }
        } else fish[i].life -= dt;
    }
}

/* ================= rendering ================= */
static float sunR, sunG, sunB, ambR, ambG, ambB;   /* light colours (0..1.5) */
static v3 sunDir;
static int frameNo;

/* sky colour for a view direction's elevation (radians), and the horizon/zenith colours of the moment */
static uint32_t skyHorizon, skyZenith;
static void sky_colors(void) {
    float a = (W.hour - 6) / 12 * M_PI_F, sh = m_sin(a);
    float dusk = m_clamp01(1 - m_abs(sh) * 3.2f);
    uint32_t dayZ = 0x4A86C8, dayH = 0xB9D3E4, nightZ = 0x060A16, nightH = 0x18223A, duskH = 0xE8955A, duskZ = 0x3A4C78;
    float nt = W.night;
    skyZenith = c_lerp(c_lerp(dayZ, duskZ, dusk * (1 - nt)), nightZ, nt);
    skyHorizon = c_lerp(c_lerp(dayH, duskH, dusk * (1 - nt * 0.7f)), nightH, nt);
    if (other_side()) { skyZenith = 0x0C2A2E; skyHorizon = 0x5E7E52; }
    float ov = m_max(W.cloud * .85f, W.rain);
    uint32_t grey = c_lerp(0x9AA2A8, 0x22262C, nt);
    skyZenith = c_lerp(skyZenith, c_scale(grey, .85f), ov * .85f);
    skyHorizon = c_lerp(skyHorizon, grey, ov * .8f);
    skyHorizon = c_lerp(skyHorizon, c_lerp(0xC8D0D4, 0x30363C, nt), W.mist * .7f);
    if (W.flash > 0) { skyZenith = c_lerp(skyZenith, 0xE8EEFF, W.flash * .7f); skyHorizon = c_lerp(skyHorizon, 0xE8EEFF, W.flash * .7f); }

}
uint32_t w_sky_color(float elev) {
    if (elev < 0) return skyHorizon;
    float t = m_clamp01(elev / 0.9f);
    return c_lerp(skyHorizon, skyZenith, m_pow(t, 0.6f));
}

static void setup_light(void) {
    float a = (W.hour - 6) / 12 * M_PI_F, sh = m_sin(a), ch = m_cos(a);
    int moon = sh < 0;
    if (moon) { sh = -sh; ch = -ch; }
    sunDir = v_norm(V3(ch, sh * 0.9f + 0.05f, -0.35f));
    float day = 1 - W.night;
    float ov = m_max(W.cloud * .7f, W.rain * .8f);
    float dusk = m_clamp01(1 - m_abs(m_sin(a)) * 3);
    float s = (moon ? 0.30f * W.night : day * (1 - ov * 0.75f));
    sunR = s * (1 + dusk * .25f); sunG = s * (1 - dusk * .1f); sunB = s * (1 - dusk * .35f);
    if (moon) { sunR *= .7f; sunG *= .85f; sunB *= 1.1f; }
    float amb = 0.28f * day + 0.22f;
    ambR = amb * (1 - W.night * .35f); ambG = amb * (1 - W.night * .25f); ambB = amb * (1 + W.night * .1f);
    if (other_side()) { sunDir = v_norm(V3(-0.9f, 0.18f, -0.3f)); sunR = .55f; sunG = .45f; sunB = .4f; ambR = .26f; ambG = .34f; ambB = .32f; }
    if (W.flash > 0) { ambR += W.flash * .6f; ambG += W.flash * .6f; ambB += W.flash * .7f; }
    if (W.scene == SCENE_CAVE) { sunDir = v_norm(V3(0.3f, 0.8f, -0.4f)); sunR = .55f; sunG = .45f; sunB = .32f; ambR = .32f; ambG = .34f; ambB = .4f; }
    if (W.scene == SCENE_STILL) { sunDir = v_norm(V3(0.3f, 1, -0.5f)); sunR = .45f; sunG = .6f; sunB = .68f; ambR = .42f; ambG = .52f; ambB = .58f; }
}
static inline uint32_t lit(uint32_t c, v3 n) {
    float d = v_dot(n, sunDir); if (d < 0) d = 0;
    float r = ambR + sunR * d, g = ambG + sunG * d, b = ambB + sunB * d;
    return c_rgb((int)((c >> 16 & 255) * r), (int)((c >> 8 & 255) * g), (int)((c & 255) * b));
}

static void draw_sky(void) {
    uint32_t *px = R.px;
    for (int y = 0; y < R.h; y++) {
        /* elevation of the ray through this row's centre */
        float vy = (R.h * 0.5f - (y + 0.5f)) / R.f;
        float ey = R.fw.y + R.up.y * vy;   /* world y of the direction fw + up*vy (unnormalised) */
        float hz = m_sqrt(1 + vy * vy);
        float elev = m_atan2(ey, m_sqrt(m_max(0, hz * hz - ey * ey)));
        uint32_t c = w_sky_color(elev);
        uint32_t *row = px + y * R.w;
        for (int x = 0; x < R.w; x++) row[x] = c;
    }
    memset(R.zb, 0, (size_t)R.w * R.h * 4);
    /* stars at night (directions made once) */
    if (W.night > 0.3f && W.cloud < .7f && W.rain < .3f) {
        static v3 stars[160]; static int made;
        if (!made) { uint32_t sd = 77; for (int i = 0; i < 160; i++) { float az = m_rndf(&sd) * M_TAU_F, el = m_rndf(&sd) * 1.4f + 0.05f; stars[i] = V3(m_cos(az) * m_cos(el), m_sin(el), m_sin(az) * m_cos(el)); } made = 1; }
        uint32_t starc = c_lerp(skyZenith, 0xFFFFFF, W.night - .3f);
        for (int i = 0; i < 160; i++) {
            v3 d = stars[i];
            float vz = v_dot(d, R.fw);
            if (vz <= 0.05f) continue;
            int sx = (int)(R.w * .5f + v_dot(d, R.rt) * R.f / vz), sy = (int)(R.h * .5f - v_dot(d, R.up) * R.f / vz);
            if (sx >= 0 && sy >= 0 && sx < R.w && sy < R.h) R.px[sy * R.w + sx] = starc;
        }
    }
    /* sun or moon */
    {
        float a = (W.hour - 6) / 12 * M_PI_F;
        int moon = m_sin(a) < 0;
        v3 d = sunDir;
        v3 v = V3(v_dot(d, R.rt), v_dot(d, R.up), v_dot(d, R.fw));
        if (v.z > 0.05f && W.cloud < .85f && W.rain < .5f) {
            float sx = R.w * .5f + v.x * R.f / v.z, sy = R.h * .5f - v.y * R.f / v.z;
            float rad = R.f * (moon ? 0.03f : 0.04f);
            uint32_t col = moon ? 0xE8ECF4 : (W.night > 0.05f || m_sin(a) < 0.3f ? 0xFFC070 : 0xFFF6DA);
            if (other_side()) col = 0xFF9A50;
            for (int y = (int)(sy - rad * 3); y <= (int)(sy + rad * 3); y++)
                for (int x = (int)(sx - rad * 3); x <= (int)(sx + rad * 3); x++) {
                    if (x < 0 || y < 0 || x >= R.w || y >= R.h) continue;
                    float dd = m_sqrt((x - sx) * (x - sx) + (y - sy) * (y - sy)) / rad;
                    uint32_t *p = &R.px[y * R.w + x];
                    if (dd < 1) *p = col;
                    else if (dd < 3 && !moon) *p = c_lerp(*p, col, (3 - dd) / 2 * 0.35f * (1 - W.cloud));
                }
        }
    }
    /* the aurora over the Other Merrow: bands of light, every other column, blended in integers */
    if (other_side()) {
        for (int band = 0; band < 3; band++) {
            uint32_t col = band == 1 ? 0x7AF0B0 : 0x58D8A0;
            for (int x = 0; x < R.w; x += 2) {
                float az = R.yaw + (x - R.w * .5f) / R.f;
                float el = 0.35f + band * 0.12f + 0.06f * m_sin(az * 3 + W.time * 0.3f + band);
                v3 d = V3(m_sin(az) * m_cos(el), m_sin(el), m_cos(az) * m_cos(el));
                float vz = v_dot(d, R.fw); if (vz < 0.05f) continue;
                int yy = (int)(R.h * .5f - v_dot(d, R.up) * R.f / vz);
                int inten = (int)((.5f + .5f * m_sin(az * 9 + W.time * 0.5f + band * 2)) * 110);
                for (int k = 0; k < 22; k++) {
                    int y = yy - k; if (y < 0 || y >= R.h) continue;
                    int al = inten * (22 - k) / 22;
                    for (int xx = x; xx < x + 2 && xx < R.w; xx++) {
                        uint32_t *p = &R.px[y * R.w + xx], d0 = *p;
                        uint32_t rb = ((d0 & 0xFF00FF) * (256 - al) + (col & 0xFF00FF) * al) >> 8 & 0xFF00FF;
                        uint32_t g = ((d0 & 0x00FF00) * (256 - al) + (col & 0x00FF00) * al) >> 8 & 0x00FF00;
                        *p = rb | g;
                    }
                }
            }
        }
    }
}

/* terrain: lit colours are cached (refreshed a slice per frame as the light moves); view-space vertices per frame */
static rvert *gcache; static int *gstamp; static uint32_t *glit; static int litPos, litAll = 1;
static uint32_t *tlit;   /* lit colour per static triangle */
static void relight(int all) {
    int n = all ? GN * GN : GN * GN / 12 + 1;
    for (int c = 0; c < n; c++) { int k = (litPos + c) % (GN * GN); glit[k] = lit(gcol[k], gnorm[k]); }
    int m = all ? ntris : ntris / 12 + 1;
    int base = (int)((long)litPos * ntris / (GN * GN));
    for (int c = 0; c < m && ntris; c++) { int k = (base + c) % ntris; tlit[k] = lit(tris[k].col, tris[k].n); }
    litPos = (litPos + n) % (GN * GN);
}
static inline rvert *gvert(int i, int j) {
    int k = j * GN + i;
    if (gstamp[k] != frameNo) {
        gstamp[k] = frameNo;
        v3 p = V3(-WORLD_SIZE / 2 + GCELL * i, gh[k], -WORLD_SIZE / 2 + GCELL * j);
        v3 v = r_view(p);
        r_rv(&gcache[k], v, glit[k], r_fogk(v.z));
    }
    return &gcache[k];
}

static void draw_terrain(void) {
    for (int cj = 0; cj < NCH; cj++)
        for (int ci = 0; ci < NCH; ci++) {
            int i0 = ci * CHUNK, j0 = cj * CHUNK;
            float cx = -WORLD_SIZE / 2 + GCELL * (i0 + CHUNK / 2), cz = -WORLD_SIZE / 2 + GCELL * (j0 + CHUNK / 2);
            float lo = chLo[cj * NCH + ci], hi = chHi[cj * NCH + ci];
            v3 c = V3(cx, (lo + hi) / 2, cz);
            float rad = m_sqrt(2 * (CHUNK * GCELL / 2) * (CHUNK * GCELL / 2) + (hi - lo) * (hi - lo) / 4) + 2;
            if (!r_sphere_visible(c, rad)) continue;
            float dist = v_len(v_sub(c, R.cam)) - rad * 0.5f;
            int step = dist > 110 ? 5 : dist > 45 ? 2 : 1;
            for (int j = j0; j < j0 + CHUNK; j += step)
                for (int i = i0; i < i0 + CHUNK; i += step) {
                    rvert *a = gvert(i, j), *b = gvert(i + step, j), *cc = gvert(i, j + step), *d = gvert(i + step, j + step);
                    r_tri(a, cc, d, 1);
                    r_tri(a, d, b, 1);
                }
        }
}

static uint32_t blit[BN * BN];
static void draw_backdrop(void) {
    float cell = BSIZE / (BN - 1);
    { int n = litAll ? BN * BN : BN * BN / 32 + 1; static int bp;
      for (int c = 0; c < n; c++) { int k = (bp + c) % (BN * BN); blit[k] = world_tint(lit(bcol[k], bnorm[k])); } bp = (bp + n) % (BN * BN); }
    r_zbias = 1.003f;
    for (int j = 0; j < BN - 1; j++)
        for (int i = 0; i < BN - 1; i++) {
            float x0 = -BSIZE / 2 + cell * i, z0 = -BSIZE / 2 + cell * j, x1 = x0 + cell, z1 = z0 + cell;
            if (m_abs(x0) < 140 && m_abs(x1) < 140 && m_abs(z0) < 140 && m_abs(z1) < 140) continue;
            int k = j * BN + i;
            v3 pa = V3(x0, bh_[k], z0), pb = V3(x1, bh_[k + 1], z0), pc = V3(x0, bh_[k + BN], z1), pd = V3(x1, bh_[k + BN + 1], z1);
            v3 cen = V3((x0 + x1) / 2, (bh_[k] + bh_[k + BN + 1]) / 2, (z0 + z1) / 2);
            if (!r_sphere_visible(cen, cell * .75f + m_abs(bh_[k] - bh_[k + BN + 1]))) continue;
            r_tri_world(pa, pc, pd, blit[k], blit[k + BN], blit[k + BN + 1], 1);
            r_tri_world(pa, pd, pb, blit[k], blit[k + BN + 1], blit[k + 1], 1);
        }
    r_zbias = 1;
}

/* the lake: a grid of 3 m cells at the water level, wherever the ground is below it.
 * Depths and colours are cached until the level or the light changes. */
#define WCELL 3.0f
static float *wdep; static uint32_t *wcolc; static int wnx, wnz; static float wLevel = 999; static int wLight = -1;
static void draw_water(void) {
    float lv = w_level();
    if (G.won) return;
    if (!wdep) {
        wnx = (int)((extX + 6) * 2 / WCELL) + 1; wnz = (int)((extZ + 6) * 2 / WCELL) + 1;
        wdep = malloc(sizeof(float) * (wnx + 1) * (wnz + 1)); wcolc = malloc(4 * (wnx + 1) * (wnz + 1));
    }
    float x0 = -(extX + 6), z0 = -(extZ + 6);
    int lightKey = (int)(W.hour * 4) + world_no() * 1000 + (int)(W.rain * 8) * 10000 + (int)(W.cloud * 8) * 100000;
    if (m_abs(lv - wLevel) > 0.01f || lightKey != wLight) {
        wLevel = lv; wLight = lightKey;
        uint32_t shallow = 0x7FB2B0, deep = 0x1D4A5E;
        if (is_drowned()) { shallow = 0x8A9490; deep = 0x3A4648; }
        if (other_side()) { shallow = 0x5E8A70; deep = 0x10302E; }
        shallow = c_lerp(shallow, skyHorizon, .25f); deep = c_lerp(deep, skyZenith, .2f);
        float light = m_clamp(ambR + sunR * .8f, 0.15f, 1.3f);
        for (int j = 0; j <= wnz; j++)
            for (int i = 0; i <= wnx; i++) {
                float x = x0 + i * WCELL, z = z0 + j * WCELL;
                float d = lv - w_ground(x, z);
                int k = j * (wnx + 1) + i;
                wdep[k] = d;
                uint32_t c = c_lerp(shallow, deep, m_clamp01(d / 6));
                c = c_lerp(c, 0xFFFFFF, ((i * 7 + j * 13) % 5) * 0.012f);
                if (W.rain > .3f) c = c_lerp(c, 0x9AA4AA, (W.rain - .3f) * .25f);
                wcolc[k] = c_scale(c, light);
            }
    }
    static rvert *row0, *row1; static int cap;
    if (cap < wnx + 1) { row0 = malloc(sizeof(rvert) * (wnx + 1)); row1 = malloc(sizeof(rvert) * (wnx + 1)); cap = wnx + 1; }
    static uint8_t *wet; if (!wet) wet = malloc(wnz + 1);
    for (int j = 0; j <= wnz; j++) { const float *d = wdep + j * (wnx + 1); wet[j] = 0; for (int i = 0; i <= wnx; i++) if (d[i] > 0) { wet[j] = 1; break; } }
    for (int j = 0; j <= wnz; j++) {
        float z = z0 + j * WCELL;
        const float *d1 = wdep + j * (wnx + 1), *d0 = d1 - (wnx + 1);
        int need = wet[j] || (j > 0 && wet[j - 1]) || (j < wnz && wet[j + 1]);
        if (need) for (int i = 0; i <= wnx; i++) {
            v3 v = r_view(V3(x0 + i * WCELL, lv, z));
            r_rv(&row1[i], v, wcolc[j * (wnx + 1) + i], r_fogk(v.z));
        }
        if (j > 0 && (wet[j] || wet[j - 1])) {
            for (int i = 0; i < wnx; i++) {
                if (d0[i] <= 0 && d0[i + 1] <= 0 && d1[i] <= 0 && d1[i + 1] <= 0) continue;
                r_tri(&row0[i], &row1[i], &row1[i + 1], 0);
                r_tri(&row0[i], &row1[i + 1], &row0[i + 1], 0);
            }
        }
        rvert *t = row0; row0 = row1; row1 = t;
    }
}

static int cond_ok(const struct sobj *o) {
    switch (o->cond) {
    case COND_ALWAYS: return 1;
    case COND_WORLD: return world_no() == o->arg;
    case COND_NOT_WORLD: return world_no() != o->arg;
    case COND_EXPLORED: return G.explored >> o->arg & 1;
    case COND_NOT_EXPLORED: return !(G.explored >> o->arg & 1);
    case COND_CREEK_STATE:
        if (o->arg >= 100) return (G.springsCapped >> (o->arg - 100) & 1) && !explored("chapel");
        return G.creekState[o->arg / 10] == o->arg % 10;
    case COND_WON: return o->arg ? G.won : !G.won;
    case COND_TAKEN: return !(G.taken >> o->arg & 1);
    case COND_ITEM:
        if (o->arg >= 100) { int t = (o->arg - 100) / 16, n = (o->arg - 100) % 16; return G.pumps[t] > n; }
        return !found_item(o->arg);
    }
    return 1;
}

static void draw_objects(void) {
    for (int k = 0; k < nobjs; k++) {
        const struct sobj *o = &objs[k];
        if (o->scene != W.scene || !cond_ok(o)) continue;
        if (!r_sphere_visible(o->c, o->r)) continue;
        v3 oc = r_view(o->c);
        float dist = oc.z;
        if (dist - o->r > R.far) continue;
        if (o->r < 8 && o->r * R.f < dist * 1.2f) continue;          /* smaller than about a pixel */
        int count = dist > 70 ? o->lod : o->count;
        float fk = r_fogk(dist);
        for (int i = o->first; i < o->first + count; i++) {
            const struct stri *t = &tris[i];
            uint32_t c = tlit[i];
            rvert a, b, cc;
            v3 va = r_view(t->a), vb = r_view(t->b), vc = r_view(t->c);
            r_rv(&a, va, c, fk); b = a; cc = a;
            b.x = vb.x; b.y = vb.y; b.z = vb.z; cc.x = vc.x; cc.y = vc.y; cc.z = vc.z;
            r_tri(&a, &b, &cc, 1);
        }
    }
}

/* immediate-mode helpers for moving things */
static v3 dyn_o; static float dyn_s, dyn_c;
static v3 dxf(float x, float y, float z) { return V3(dyn_o.x + x * dyn_c + z * dyn_s, dyn_o.y + y, dyn_o.z - x * dyn_s + z * dyn_c); }
static void dbox(float cx, float cy, float cz, float sx, float sy, float sz, uint32_t col) {
    float x0 = cx - sx / 2, x1 = cx + sx / 2, y0 = cy - sy / 2, y1 = cy + sy / 2, z0 = cz - sz / 2, z1 = cz + sz / 2;
    v3 p[8] = { dxf(x0, y0, z0), dxf(x1, y0, z0), dxf(x1, y0, z1), dxf(x0, y0, z1), dxf(x0, y1, z0), dxf(x1, y1, z0), dxf(x1, y1, z1), dxf(x0, y1, z1) };
    static const int f[6][4] = { { 4, 5, 6, 7 }, { 0, 3, 2, 1 }, { 0, 1, 5, 4 }, { 1, 2, 6, 5 }, { 2, 3, 7, 6 }, { 3, 0, 4, 7 } };
    static const v3 nl[6] = { { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, -1 }, { 1, 0, 0 }, { 0, 0, 1 }, { -1, 0, 0 } };
    for (int i = 0; i < 6; i++) {
        v3 n = V3(nl[i].x * dyn_c + nl[i].z * dyn_s, nl[i].y, -nl[i].x * dyn_s + nl[i].z * dyn_c);
        uint32_t c = lit(col, n);
        /* outward winding: check against the box centre */
        v3 a = p[f[i][0]], b = p[f[i][1]], cc = p[f[i][2]], d = p[f[i][3]];
        v3 nn = v_cross(v_sub(b, a), v_sub(cc, a));
        if (v_dot(nn, n) < 0) { r_tri_world(a, d, cc, c, c, c, 1); r_tri_world(a, cc, b, c, c, c, 1); }
        else { r_tri_world(a, b, cc, c, c, c, 1); r_tri_world(a, cc, d, c, c, c, 1); }
    }
}
static void dyn_at(v3 o, float yaw) { dyn_o = o; dyn_s = m_sin(yaw); dyn_c = m_cos(yaw); }

static void draw_player(void) {
    v3 p = P.pos;
    dyn_at(p, P.yaw);
    float bob = m_sin(W.time * 9) * 0.05f * (P.onGround ? 1 : 0);
    float sink = 0;
    uint32_t coat = 0x2F5A7A, trousers = 0x3A3A42, skin = 0xE0B08A, hat = 0xB8862E;
    if (!P.swimming) { dbox(-.18f, .45f, 0, .22f, .9f, .25f, trousers); dbox(.18f, .45f, 0, .22f, .9f, .25f, trousers); }
    dbox(0, 1.25f + sink + bob, 0, .62f, .75f, .36f, coat);
    dbox(-.42f, 1.25f + sink + bob, 0, .18f, .7f, .2f, coat); dbox(.42f, 1.25f + sink + bob, 0, .18f, .7f, .2f, coat);
    dbox(0, 1.85f + sink + bob, 0, .36f, .38f, .34f, skin);
    dbox(0, 2.07f + sink + bob, 0, .5f, .08f, .5f, hat); dbox(0, 2.17f + sink + bob, 0, .32f, .16f, .32f, hat);
    if (G.carried > 0 && !P.swimming) {   /* the bucket */
        dbox(.55f, .85f, .1f, .34f, .36f, .34f, 0x8A9096);
        dbox(.55f, 1.02f, .1f, .28f, .02f, .28f, 0x4A88B0);
    }
}

static void draw_dynamic(void) {
    float t = W.time;
    if (W.scene == SCENE_OUTSIDE) {
        float lv = w_level();
        /* sparkles over finds you can pick up */
        for (int i = 0; i < NITEMS; i++) {
            if (found_item(i)) continue;
            int show = item_exposed(i) || (explored("boathouse") && g_progress() + 0.08f >= ITEMS[i].g);
            if (!show) continue;
            v3 p = v_add(W.item[i], V3(0, ITEMS[i].sy + 1.3f + m_sin(t * 2 + i) * .2f, 0));
            if (!item_exposed(i)) p.y = m_max(p.y, lv + .6f);
            r_billboard(p, .45f, item_exposed(i) ? 0xFFE27A : 0x9AD8E8);
        }
        if (g_plug_exposed()) r_billboard(v_add(deepest, V3(0, 4.2f + m_sin(t * 2) * .2f, 0)), .6f, 0xFFE27A);
        /* gushing springs */
        for (int i = 0; i < NSPRINGS; i++) {
            if (!g_spring_flowing(i)) continue;
            v3 s = W.spring[i];
            float top = m_max(s.y, lv) + 0.6f + 0.25f * m_sin(t * 7 + i);
            for (int k = 0; k < 6; k++) {
                float a = k * 1.05f + t * 2;
                r_billboard(V3(s.x + m_cos(a) * .4f, m_lerp(s.y, top, (k % 3) / 2.0f) + .3f, s.z + m_sin(a) * .4f), .4f, 0xD8EEF3);
            }
        }
        /* creek water */
        draw_creeks(0);
        /* the mill wheel */
        {
            int mi = building_index("mill");
            v3 bp = W.building[mi]; float yaw = W.buildingYaw[mi];
            float base = bp.y;
            dyn_at(V3(bp.x, base, bp.z), yaw);
            float ang = explored("mill") ? t * 0.8f : 0;
            for (int k = 0; k < 8; k++) {
                float a = ang + k * M_PI_F / 4;
                v3 c = dxf(-4.3f, 2.8f + m_sin(a) * 2.2f, m_cos(a) * 2.2f);
                dyn_at(c, yaw); dbox(0, 0, 0, .5f, .3f, .7f, 0x6A4E34);
                dyn_at(V3(bp.x, base, bp.z), yaw);
            }
            dbox(-4.3f, 2.8f, 0, .6f, .6f, .6f, 0x3A3A3A);
        }
        /* the lighthouse beam */
        if (explored("lighthouse") && W.night > 0.2f) {
            int li = building_index("lighthouse"); v3 bp = W.building[li];
            float a = t * 0.9f;
            for (int k = 1; k < 8; k++) r_billboard(V3(bp.x + m_sin(a) * k * 4, bp.y + 12.4f, bp.z + m_cos(a) * k * 4), 1.6f + k * .4f, c_lerp(0xFFF0B0, skyHorizon, k / 9.0f));
        }
        /* ducks */
        if (duckState < 2) for (int i = 0; i < 7; i++) {
            dyn_at(ducks[i].p, ducks[i].yaw);
            dbox(0, .12f, 0, .28f, .22f, .5f, 0x6A5236);
            dbox(0, .32f, .22f, .16f, .18f, .18f, i % 2 ? 0x2E6A3A : 0x6A5236);
            dbox(0, .3f, .36f, .08f, .05f, .12f, 0xE0A030);
            if (duckState == 1) { float f = m_sin(t * 14 + i) * .25f; dbox(-.3f, .2f + f, 0, .35f, .04f, .25f, 0x5A4630); dbox(.3f, .2f + f, 0, .35f, .04f, .25f, 0x5A4630); }
        }
        /* fish */
        for (int i = 0; i < 4; i++) if (fish[i].life > 0) {
            float hop = m_abs(m_sin(fish[i].t * 4)) * .35f;
            dyn_at(v_add(fish[i].p, V3(0, hop + .1f, 0)), fish[i].t * 3);
            dbox(0, 0, 0, .1f, .14f, .45f, 0x8A9AA0); dbox(0, 0, -.28f, .04f, .16f, .12f, 0x6A7A80);
        }
        /* birds over the valley by day */
        if (W.night < .5f && W.rain < .5f) for (int i = 0; i < 4; i++) {
            float a = t * 0.12f + i * 1.6f, r = 60 + i * 12;
            v3 c = V3(m_cos(a) * r, 45 + i * 6 + m_sin(t * .5f + i) * 3, m_sin(a) * r);
            float f = m_sin(t * 8 + i) * .5f;
            r_tri_world(c, v_add(c, V3(-1.2f, f, -.3f)), v_add(c, V3(0, 0, .5f)), 0x2A2A2A, 0x2A2A2A, 0x2A2A2A, 0);
            r_tri_world(c, v_add(c, V3(1.2f, f, -.3f)), v_add(c, V3(0, 0, .5f)), 0x2A2A2A, 0x2A2A2A, 0x2A2A2A, 0);
        }
        /* smoke from the cabin once it's open */
        if (explored("cabin")) {
            int ci = building_index("cabin");
            v3 ch = local_point(W.building[ci], W.buildingYaw[ci], 3, -1.3f);
            for (int k = 0; k < 6; k++) { float u = m_repeat(t * .25f + k / 6.0f, 1); r_billboard(V3(ch.x + u * 2, ch.y + 7 + u * 6, ch.z), .6f + u * 1.4f, c_lerp(0xB8B8B4, skyHorizon, u)); }
        }
    } else if (W.scene == SCENE_CAVE) {
        /* torch flames and socket glows */
        for (int z = -12; z <= 12; z += 8) for (int s = -1; s <= 1; s += 2) r_billboard(V3(s * 11.3f, 4.3f + m_sin(t * 9 + z) * .05f, z), .5f, 0xFFD070);
        for (int i = 0; i < NRELICS; i++) {
            float a = M_PI_F / 2 + (i - 2.5f) * 0.55f;
            int set = G.relicsSet >> i & 1;
            v3 p = V3(m_cos(a) * 4.05f, 4 + m_sin(a) * 4.05f, 13.4f);
            r_billboard(p, .45f, set ? 0xFFC850 : 0x2A2620);
        }
        /* runes and the portal */
        if (G.gateOpen) {
            for (int k = 0; k < 24; k++) {
                float a = k * M_TAU_F / 24 + t * .5f;
                r_billboard(V3(m_cos(a) * 3.9f, 4 + m_sin(a) * 3.9f, 13.45f), .22f, 0x8FF0FF);
            }
            /* a swirling disc: rings of coloured triangles */
            for (int ring = 0; ring < 4; ring++)
                for (int k = 0; k < 16; k++) {
                    float a0 = k * M_TAU_F / 16 + t * (0.6f + ring * 0.25f) * (ring & 1 ? -1 : 1), a1 = a0 + M_TAU_F / 16;
                    float r0 = ring * 0.9f, r1 = r0 + 0.9f;
                    uint32_t c = c_lerp(other_side() ? 0x8AD8FF : 0x6AE0A0, 0x2050A0, ring / 4.0f);
                    c = c_lerp(c, 0xFFFFFF, .2f + .2f * m_sin(a0 * 3 + t * 2));
                    v3 p0 = V3(m_cos(a0) * r0, 4 + m_sin(a0) * r0, 14), p1 = V3(m_cos(a1) * r0, 4 + m_sin(a1) * r0, 14);
                    v3 q0 = V3(m_cos(a0) * r1, 4 + m_sin(a0) * r1, 14), q1 = V3(m_cos(a1) * r1, 4 + m_sin(a1) * r1, 14);
                    r_tri_world(p0, q0, q1, c, c, c, 0);
                    if (ring) r_tri_world(p0, q1, p1, c, c, c, 0);
                }
        } else if (popcount32(G.relicsSet) > 0) {
            for (int k = 0; k < popcount32(G.relicsSet) * 3; k++) { float a = k * M_TAU_F / 18; r_billboard(V3(m_cos(a) * 3.9f, 4 + m_sin(a) * 3.9f, 13.45f), .16f, 0x4FA0B0); }
        }
    } else {
        /* the Still Room: a soft glow over the pool */
        for (int k = 0; k < 10; k++) { float a = k * M_TAU_F / 10 + t * .2f; r_billboard(V3(m_cos(a) * 1.8f, .6f + .2f * m_sin(t + k), m_sin(a) * 1.8f), .2f, 0x9FE8F0); }
    }
    /* splash particles */
    for (int i = 0; i < 96; i++) if (parts[i].life > 0) r_billboard(parts[i].p, parts[i].size, parts[i].col);
    draw_player();
}

static void draw_creeks(uint32_t unused) {
    (void)unused;
    float lv = w_level();
    for (int c = 0; c < NCREEKS; c++) {
        if (G.creekState[c] != 0) continue;
        int n = W.creekLen[c];
        for (int i = 0; i + 1 < n; i++) {
            v3 a = W.creekPath[c][i], b = W.creekPath[c][i + 1];
            if (b.y < lv) break;
            float w0 = m_lerp(.35f, 1.1f, i / (float)n), w1 = m_lerp(.35f, 1.1f, (i + 1) / (float)n);
            v3 d = v_norm(v_sub(b, a)); v3 s = V3(-d.z, 0, d.x);
            v3 a0 = v_add(a, v_mul(s, w0)), a1 = v_sub(a, v_mul(s, w0)), b0 = v_add(b, v_mul(s, w1)), b1 = v_sub(b, v_mul(s, w1));
            a0.y = a1.y = a.y + .12f; b0.y = b1.y = b.y + .12f;
            if (!r_sphere_visible(a, 3)) continue;
            float sh = .5f + .5f * m_sin(i * 1.3f - W.time * 6);
            uint32_t col = c_lerp(0x6FA8C0, 0xD8EEF4, sh * .45f);
            col = c_scale(col, m_clamp(ambR + sunR * .8f, .2f, 1.2f));
            r_tri_world(a0, b0, b1, col, col, col, 0);
            r_tri_world(a0, b1, a1, col, col, col, 0);
        }
    }
}

static void draw_rain(void) {
    if (W.scene != SCENE_OUTSIDE || W.rain < .05f) return;
    int n = (int)(W.rain * R.w * R.h / 300);
    uint32_t s = (uint32_t)(W.time * 60) * 2654435761u + 1;
    uint32_t col = c_lerp(0xB8C4CC, skyHorizon, .3f);
    for (int i = 0; i < n; i++) {
        int x = m_rnd(&s) % R.w, y = m_rnd(&s) % R.h, len = 3 + m_rnd(&s) % 5;
        for (int k = 0; k < len; k++) {
            int yy = y + k, xx = x - k / 3;
            if (yy >= R.h || xx < 0) break;
            uint32_t *p = &R.px[yy * R.w + xx];
            *p = ((*p & 0xFEFEFE) >> 1) + ((col & 0xFCFCFC) >> 2) + ((*p & 0xFCFCFC) >> 2);   /* about 25% rain colour */
        }
    }
}

void w_render(void) {
    frameNo++;
    setup_light();
    static int lastScene = -1, lastWorld = -1;
    if (!glit) {
        glit = malloc(4 * GN * GN); tlit = malloc(4 * MAXTRI);
        gcache = malloc(sizeof(rvert) * GN * GN); gstamp = malloc(sizeof(int) * GN * GN); memset(gstamp, 0xFF, sizeof(int) * GN * GN);
    }
    if (lastScene != W.scene) { lastScene = W.scene; litAll = 1; }
    if (W.scene == SCENE_OUTSIDE && m_abs(w_level() - paintedLevel) > 0.08f) {
        paint_basin(w_level());
        for (int k = 0; k < GN * GN; k++) if (gbasin[k]) glit[k] = lit(gcol[k], gnorm[k]);
    }
    if (lastWorld != world_no()) { lastWorld = world_no(); litAll = 1; }
    relight(litAll);
    if (W.scene == SCENE_OUTSIDE) {
        sky_colors();
        R.fog = skyHorizon;
        R.fogStart = 10;
        R.fogDist = m_lerp(m_lerp(300, 150, W.rain), 55, W.mist);
        R.fogMax = 0.92f;
        R.far = 1200;
        if (W.night > .5f) R.fogDist *= 1.4f;
        R.fogDist = (float)((int)(R.fogDist / 8) * 8 + 8);   /* in steps, so the fog table is rebuilt only now and then */
        { static float lf, lm; if (m_abs(lf - R.fogDist) > 1 || lm != R.fogMax) { lf = R.fogDist; lm = R.fogMax; r_fog_table(); } }
        uint64_t t0 = sys_time();
        draw_sky(); uint64_t t1 = sys_time();
        draw_backdrop(); uint64_t t2 = sys_time();
        draw_terrain(); uint64_t t3 = sys_time();
        draw_water(); uint64_t t4 = sys_time();
        draw_objects(); uint64_t t5 = sys_time();
        draw_dynamic();
        draw_rain(); uint64_t t6 = sys_time();
        litAll = 0;
        w_prof[0] = (int)(t1 - t0); w_prof[1] = (int)(t2 - t1); w_prof[2] = (int)(t3 - t2); w_prof[3] = (int)(t4 - t3); w_prof[4] = (int)(t5 - t4); w_prof[5] = (int)(t6 - t5);
    } else {
        R.fog = W.scene == SCENE_CAVE ? 0x0A0C10 : 0x0C2028;
        R.fogStart = 6; R.fogDist = W.scene == SCENE_CAVE ? 30 : 22; R.fogMax = .85f; R.far = 80;
        r_fog_table();
        for (int i = 0; i < R.w * R.h; i++) R.px[i] = R.fog;
        memset(R.zb, 0, (size_t)R.w * R.h * 4);
        draw_objects();
        draw_dynamic();
        litAll = 0;
    }
}

/* ================= the map ================= */
void w_draw_map(struct canvas *c, int x0, int y0, int size) {
    float lv = w_level();
    int n = 150;   /* map covers 300 m */
    for (int j = 0; j < size; j++) {
        int gj = GN - 1 - j * (GN - 1) / size;
        for (int i = 0; i < size; i++) {
            int gi = i * (GN - 1) / size;
            int k = gj * GN + gi;
            uint32_t col = gcol[k];
            if (gh[k] < lv && !G.won) col = c_lerp(0x7FB2B0, 0x1D4A5E, m_clamp01((lv - gh[k]) / 6));
            float shade = 0.75f + 0.35f * gnorm[k].x;
            col = c_scale(col, shade);
            int px = x0 + i, py = y0 + j;
            if (px >= c->cx0 && py >= c->cy0 && px < c->cx1 && py < c->cy1) c->px[py * c->stride + px] = col;
        }
    }
    (void)n;
}

/* map position of a world point */
int w_map_xy(v3 p, int x0, int y0, int size, int *mx, int *my) {
    *mx = x0 + (int)((p.x + WORLD_SIZE / 2) / WORLD_SIZE * size);
    *my = y0 + (int)((WORLD_SIZE / 2 - p.z) / WORLD_SIZE * size);
    return 1;
}

/* ---- debug helpers (open merrow debug) ---- */
static int dbgSpot;
void w_debug_teleport(void) {
    W.scene = SCENE_OUTSIDE;
    v3 spots[40]; const char *names[40]; int n = 0;
    spots[n] = W.tank; names[n++] = "tank";
    spots[n] = W.store; names[n++] = "store";
    for (int i = 0; i < NBUILD; i++) { spots[n] = W.building[i]; names[n++] = BUILDINGS[i].name; }
    for (int i = 0; i < NCREEKS; i++) { spots[n] = W.creekHead[i]; names[n++] = CREEKS[i].name; }
    spots[n] = W.plankStack; names[n++] = "planks";
    spots[n] = W.valve; names[n++] = "valve";
    spots[n] = W.stone[0]; names[n++] = "stone";
    spots[n] = deepest; names[n++] = "deepest";
    if (is_drowned()) { spots[n] = W.church; names[n++] = "church"; spots[n] = W.house[0]; names[n++] = "house"; }
    for (int i = 0; i < NSPRINGS; i++) { spots[n] = W.spring[i]; names[n++] = SPRINGS[i].name; }
    dbgSpot %= n;
    v3 t = spots[dbgSpot];
    float yaw = yaw_to(t, 0, 0) + M_PI_F;
    p_teleport(V3(t.x - m_sin(yaw) * 5, 0, t.z - m_cos(yaw) * 5), yaw);
    hud_toast("debug: %s", names[dbgSpot]);
    dbgSpot++;
}
