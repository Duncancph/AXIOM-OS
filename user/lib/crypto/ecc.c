/* NIST P-256 / P-384: ECDSA verification (FIPS 186-4) and P-256 ECDH, using Jacobian coordinates. */
#include "bn.h"
#include "crypto.h"
#include "../../../common/str.h"

#define FL 12   /* limbs for the largest field (P-384) */
struct curve {
    int bytes, n;                    /* field size in bytes, limbs */
    mont p, q;                       /* field prime, group order */
    uint32_t b[FL], gx[FL], gy[FL];  /* Montgomery form */
    uint8_t pm2[48], qm2[48];        /* p-2, q-2 for inversion */
    int ready;
};
static struct curve curves[2];

static const char *P256[5] = {
    "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff",
    "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b",
    "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
    "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5",
    "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551" };
static const char *P384[5] = {
    "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff",
    "b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef",
    "aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7",
    "3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f",
    "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973" };

static void unhex(const char *h, uint8_t *out, int n) {
    for (int i = 0; i < n; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) { char c = h[i * 2 + k]; v = v * 16 + (c >= 'a' ? c - 'a' + 10 : c - '0'); }
        out[i] = v;
    }
}

static struct curve *get_curve(int id) {
    struct curve *c = &curves[id];
    if (c->ready) return c;
    const char **h = id == CURVE_P256 ? P256 : P384;
    int nb = id == CURVE_P256 ? 32 : 48;
    uint8_t buf[48];
    uint32_t t[FL];
    c->bytes = nb; c->n = nb / 4;
    unhex(h[0], buf, nb); mont_init(&c->p, buf, nb);
    memcpy(c->pm2, buf, nb); c->pm2[nb - 1] -= 2;
    unhex(h[4], buf, nb); mont_init(&c->q, buf, nb);
    memcpy(c->qm2, buf, nb); c->qm2[nb - 1] -= 2;
    unhex(h[1], buf, nb); bn_from_bytes(t, c->n, buf, nb); mont_to(&c->p, c->b, t);
    unhex(h[2], buf, nb); bn_from_bytes(t, c->n, buf, nb); mont_to(&c->p, c->gx, t);
    unhex(h[3], buf, nb); bn_from_bytes(t, c->n, buf, nb); mont_to(&c->p, c->gy, t);
    c->ready = 1;
    return c;
}

typedef struct { uint32_t x[FL], y[FL], z[FL]; } jpt;   /* z == 0: point at infinity */

#define FM(r, a, b) mont_mul(&c->p, r, a, b)
#define FA(r, a, b) mod_add(&c->p, r, a, b)
#define FS(r, a, b) mod_sub(&c->p, r, a, b)

static void pt_dbl(struct curve *c, jpt *r, const jpt *p) {
    int n = c->n;
    if (bn_is_zero(p->z, n) || bn_is_zero(p->y, n)) { memset(r, 0, sizeof *r); return; }
    uint32_t delta[FL], gamma[FL], beta[FL], alpha[FL], t1[FL], t2[FL], x3[FL], y3[FL], z3[FL];
    FM(delta, p->z, p->z);
    FM(gamma, p->y, p->y);
    FM(beta, p->x, gamma);
    FS(t1, p->x, delta); FA(t2, p->x, delta); FM(alpha, t1, t2);
    FA(t1, alpha, alpha); FA(alpha, t1, alpha);                  /* alpha = 3(x-d)(x+d) */
    FM(x3, alpha, alpha);
    FA(t1, beta, beta); FA(t1, t1, t1); FA(t2, t1, t1);           /* t1 = 4 beta, t2 = 8 beta */
    FS(x3, x3, t2);
    FA(z3, p->y, p->z); FM(z3, z3, z3); FS(z3, z3, gamma); FS(z3, z3, delta);
    FS(t1, t1, x3); FM(y3, alpha, t1);
    FM(t2, gamma, gamma); FA(t2, t2, t2); FA(t2, t2, t2); FA(t2, t2, t2);   /* 8 gamma^2 */
    FS(y3, y3, t2);
    memcpy(r->x, x3, sizeof x3); memcpy(r->y, y3, sizeof y3); memcpy(r->z, z3, sizeof z3);
}

static void pt_add(struct curve *c, jpt *r, const jpt *p, const jpt *q) {
    int n = c->n;
    if (bn_is_zero(p->z, n)) { *r = *q; return; }
    if (bn_is_zero(q->z, n)) { *r = *p; return; }
    uint32_t z1z1[FL], z2z2[FL], u1[FL], u2[FL], s1[FL], s2[FL], h[FL], i[FL], j[FL], rr[FL], v[FL], t[FL];
    FM(z1z1, p->z, p->z); FM(z2z2, q->z, q->z);
    FM(u1, p->x, z2z2); FM(u2, q->x, z1z1);
    FM(s1, p->y, q->z); FM(s1, s1, z2z2);
    FM(s2, q->y, p->z); FM(s2, s2, z1z1);
    FS(h, u2, u1); FS(rr, s2, s1);
    if (bn_is_zero(h, n)) {
        if (bn_is_zero(rr, n)) { pt_dbl(c, r, p); return; }
        memset(r, 0, sizeof *r); return;
    }
    FA(rr, rr, rr);
    FA(i, h, h); FM(i, i, i);
    FM(j, h, i);
    FM(v, u1, i);
    jpt o;
    FM(o.x, rr, rr); FS(o.x, o.x, j); FS(o.x, o.x, v); FS(o.x, o.x, v);
    FS(t, v, o.x); FM(o.y, rr, t); FM(t, s1, j); FA(t, t, t); FS(o.y, o.y, t);
    FA(o.z, p->z, q->z); FM(o.z, o.z, o.z); FS(o.z, o.z, z1z1); FS(o.z, o.z, z2z2); FM(o.z, o.z, h);
    *r = o;
}

static int to_affine(struct curve *c, const jpt *p, uint32_t *x, uint32_t *y) {   /* normal (non-Montgomery) form */
    if (bn_is_zero(p->z, c->n)) return -1;
    uint32_t zi[FL], zi2[FL], t[FL];
    mont_pow(&c->p, zi, p->z, c->pm2, c->bytes);
    FM(zi2, zi, zi);
    FM(t, p->x, zi2); mont_from(&c->p, x, t);
    if (y) { FM(t, zi2, zi); FM(t, p->y, t); mont_from(&c->p, y, t); }
    return 0;
}

static int decode_point(struct curve *c, const uint8_t *pub, size_t len, jpt *out) {
    int nb = c->bytes, n = c->n;
    if (len != (size_t)(1 + 2 * nb) || pub[0] != 4) return -1;
    uint32_t x[FL], y[FL], lhs[FL], rhs[FL], t[FL];
    bn_from_bytes(x, n, pub + 1, nb); bn_from_bytes(y, n, pub + 1 + nb, nb);
    if (bn_cmp(x, c->p.m, n) >= 0 || bn_cmp(y, c->p.m, n) >= 0) return -1;
    mont_to(&c->p, out->x, x); mont_to(&c->p, out->y, y);
    /* y^2 == x^3 - 3x + b */
    FM(lhs, out->y, out->y);
    FM(rhs, out->x, out->x); FM(rhs, rhs, out->x);
    FA(t, out->x, out->x); FA(t, t, out->x); FS(rhs, rhs, t); FA(rhs, rhs, c->b);
    if (bn_cmp(lhs, rhs, n)) return -1;
    uint32_t one[FL] = { 1 };
    mont_to(&c->p, out->z, one);
    return 0;
}

static int der_int(const uint8_t **p, const uint8_t *end, const uint8_t **v, size_t *len) {
    if (*p + 2 > end || (*p)[0] != 0x02) return -1;
    size_t l = (*p)[1];
    if (l & 0x80 || *p + 2 + l > end || !l) return -1;
    *v = *p + 2; *len = l; *p += 2 + l;
    while (*len > 1 && **v == 0) { (*v)++; (*len)--; }
    return 0;
}

int ecdsa_verify(int id, const uint8_t *pub, size_t plen, const uint8_t *hsh, size_t hlen, const uint8_t *sig, size_t slen) {
    struct curve *c = get_curve(id);
    int nb = c->bytes, n = c->n;
    jpt Q, G, R;
    if (decode_point(c, pub, plen, &Q)) return -1;
    /* signature: SEQUENCE { INTEGER r, INTEGER s } */
    const uint8_t *p = sig, *end = sig + slen, *rb, *sb;
    size_t rl, sl;
    if (slen < 8 || p[0] != 0x30) return -1;
    if (p[1] & 0x80) { if (p[1] != 0x81) return -1; p += 3; } else p += 2;
    if (der_int(&p, end, &rb, &rl) || der_int(&p, end, &sb, &sl) || rl > (size_t)nb || sl > (size_t)nb) return -1;
    uint32_t r[FL], s[FL], e[FL], w[FL], u1[FL], u2[FL], t[FL];
    bn_from_bytes(r, n, rb, rl); bn_from_bytes(s, n, sb, sl);
    if (bn_is_zero(r, n) || bn_is_zero(s, n) || bn_cmp(r, c->q.m, n) >= 0 || bn_cmp(s, c->q.m, n) >= 0) return -1;
    bn_from_bytes(e, n, hsh, hlen > (size_t)nb ? (size_t)nb : hlen);
    if (bn_cmp(e, c->q.m, n) >= 0) bn_sub(e, e, c->q.m, n);
    /* w = s^-1, u1 = e w, u2 = r w (mod q) */
    mont_to(&c->q, t, s); mont_pow(&c->q, w, t, c->qm2, nb);
    mont_to(&c->q, t, e); mont_mul(&c->q, t, t, w); mont_from(&c->q, u1, t);
    mont_to(&c->q, t, r); mont_mul(&c->q, t, t, w); mont_from(&c->q, u2, t);
    /* R = u1 G + u2 Q (Shamir's trick) */
    uint32_t one[FL] = { 1 };
    memcpy(G.x, c->gx, sizeof G.x); memcpy(G.y, c->gy, sizeof G.y); mont_to(&c->p, G.z, one);
    jpt GQ; pt_add(c, &GQ, &G, &Q);
    memset(&R, 0, sizeof R);
    for (int i = nb * 8 - 1; i >= 0; i--) {
        pt_dbl(c, &R, &R);
        int b1 = (u1[i / 32] >> (i % 32)) & 1, b2 = (u2[i / 32] >> (i % 32)) & 1;
        if (b1 && b2) pt_add(c, &R, &R, &GQ);
        else if (b1) pt_add(c, &R, &R, &G);
        else if (b2) pt_add(c, &R, &R, &Q);
    }
    uint32_t x[FL];
    if (to_affine(c, &R, x, 0)) return -1;
    if (bn_cmp(x, c->q.m, n) >= 0) bn_sub(x, x, c->q.m, n);
    return bn_cmp(x, r, n) ? -1 : 0;
}

/* k * P with a Montgomery ladder (same sequence of operations for every key) */
static void scalar_mul(struct curve *c, jpt *out, const uint8_t *k, const jpt *P) {
    jpt R0, R1;
    memset(&R0, 0, sizeof R0); R1 = *P;
    for (int i = 0; i < c->bytes * 8; i++) {
        int bit = (k[i / 8] >> (7 - i % 8)) & 1;
        if (bit) { pt_add(c, &R0, &R0, &R1); pt_dbl(c, &R1, &R1); }
        else { pt_add(c, &R1, &R0, &R1); pt_dbl(c, &R0, &R0); }
    }
    *out = R0;
}

static int valid_scalar(struct curve *c, const uint8_t *k) {
    uint32_t v[FL];
    bn_from_bytes(v, c->n, k, c->bytes);
    return !bn_is_zero(v, c->n) && bn_cmp(v, c->q.m, c->n) < 0;
}

int p256_keygen(uint8_t pub[65], const uint8_t priv[32]) {
    struct curve *c = get_curve(CURVE_P256);
    if (!valid_scalar(c, priv)) return -1;
    jpt G, R; uint32_t one[FL] = { 1 }, x[FL], y[FL];
    memcpy(G.x, c->gx, sizeof G.x); memcpy(G.y, c->gy, sizeof G.y); mont_to(&c->p, G.z, one);
    scalar_mul(c, &R, priv, &G);
    if (to_affine(c, &R, x, y)) return -1;
    pub[0] = 4; bn_to_bytes(x, c->n, pub + 1, 32); bn_to_bytes(y, c->n, pub + 33, 32);
    return 0;
}

int p256_ecdh(uint8_t shared[32], const uint8_t priv[32], const uint8_t *peer, size_t plen) {
    struct curve *c = get_curve(CURVE_P256);
    jpt Q, R; uint32_t x[FL];
    if (!valid_scalar(c, priv) || decode_point(c, peer, plen, &Q)) return -1;
    scalar_mul(c, &R, priv, &Q);
    if (to_affine(c, &R, x, 0)) return -1;
    bn_to_bytes(x, c->n, shared, 32);
    return 0;
}
