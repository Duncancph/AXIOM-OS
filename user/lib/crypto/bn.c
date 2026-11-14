#include "bn.h"
typedef int bool;
#include "crypto.h"
#include "../../../common/str.h"

void bn_from_bytes(uint32_t *d, int n, const uint8_t *b, size_t len) {
    for (int i = 0; i < n; i++) d[i] = 0;
    for (size_t i = 0; i < len; i++) {
        size_t bit = (len - 1 - i) * 8;
        if (bit / 32 < (size_t)n) d[bit / 32] |= (uint32_t)b[i] << (bit % 32);
    }
}
void bn_to_bytes(const uint32_t *d, int n, uint8_t *b, size_t len) {
    for (size_t i = 0; i < len; i++) {
        size_t bit = (len - 1 - i) * 8;
        b[i] = bit / 32 < (size_t)n ? (uint8_t)(d[bit / 32] >> (bit % 32)) : 0;
    }
}
int bn_cmp(const uint32_t *a, const uint32_t *b, int n) {
    for (int i = n - 1; i >= 0; i--) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
int bn_is_zero(const uint32_t *a, int n) { uint32_t x = 0; for (int i = 0; i < n; i++) x |= a[i]; return x == 0; }
uint32_t bn_add(uint32_t *r, const uint32_t *a, const uint32_t *b, int n) {
    uint64_t c = 0;
    for (int i = 0; i < n; i++) { c += (uint64_t)a[i] + b[i]; r[i] = (uint32_t)c; c >>= 32; }
    return (uint32_t)c;
}
uint32_t bn_sub(uint32_t *r, const uint32_t *a, const uint32_t *b, int n) {
    int64_t c = 0;
    for (int i = 0; i < n; i++) { c += (int64_t)a[i] - b[i]; r[i] = (uint32_t)c; c >>= 32; }
    return (uint32_t)(c & 1);
}

/* r = a*b*R^-1 mod m (CIOS); inputs < m */
void mont_mul(const mont *m, uint32_t *r, const uint32_t *a, const uint32_t *b) {
    int n = m->n;
    uint32_t t[BN_LIMBS + 2];
    for (int i = 0; i < n + 2; i++) t[i] = 0;
    for (int i = 0; i < n; i++) {
        uint64_t c = 0;
        for (int j = 0; j < n; j++) { c += (uint64_t)a[j] * b[i] + t[j]; t[j] = (uint32_t)c; c >>= 32; }
        c += t[n]; t[n] = (uint32_t)c; t[n + 1] = (uint32_t)(c >> 32);
        uint32_t u = t[0] * m->minv;
        c = (uint64_t)u * m->m[0] + t[0]; c >>= 32;
        for (int j = 1; j < n; j++) { c += (uint64_t)u * m->m[j] + t[j]; t[j - 1] = (uint32_t)c; c >>= 32; }
        c += t[n]; t[n - 1] = (uint32_t)c; c >>= 32;
        t[n] = t[n + 1] + (uint32_t)c;
    }
    uint32_t s[BN_LIMBS];
    uint32_t borrow = bn_sub(s, t, m->m, n);
    /* if t >= m (t[n] set or no borrow) use t - m */
    bool use_sub = t[n] || !borrow;
    for (int i = 0; i < n; i++) r[i] = use_sub ? s[i] : t[i];
}

int mont_init(mont *m, const uint8_t *mod, size_t len) {
    while (len && !*mod) { mod++; len--; }
    int n = (int)((len + 3) / 4);
    if (!n || n > BN_LIMBS || !(mod[len - 1] & 1)) return -1;
    m->n = n;
    bn_from_bytes(m->m, n, mod, len);
    /* minv = -m^-1 mod 2^32 (Newton) */
    uint32_t x = 1;
    for (int i = 0; i < 5; i++) x *= 2 - m->m[0] * x;
    m->minv = (uint32_t)-x;
    /* r2 = 2^(64n) mod m, by doubling */
    uint32_t r[BN_LIMBS];
    for (int i = 0; i < n; i++) r[i] = 0;
    r[0] = 1;
    for (int i = 0; i < 64 * n; i++) {
        uint32_t carry = bn_add(r, r, r, n);
        uint32_t s[BN_LIMBS];
        uint32_t borrow = bn_sub(s, r, m->m, n);
        if (carry || !borrow) for (int k = 0; k < n; k++) r[k] = s[k];
    }
    for (int i = 0; i < n; i++) m->r2[i] = r[i];
    return 0;
}

void mont_to(const mont *m, uint32_t *r, const uint32_t *a) { mont_mul(m, r, a, m->r2); }
void mont_from(const mont *m, uint32_t *r, const uint32_t *a) {
    uint32_t one[BN_LIMBS];
    for (int i = 0; i < m->n; i++) one[i] = 0;
    one[0] = 1;
    mont_mul(m, r, a, one);
}

void mont_pow(const mont *m, uint32_t *r, const uint32_t *base, const uint8_t *e, size_t elen) {
    int n = m->n;
    uint32_t acc[BN_LIMBS], one[BN_LIMBS];
    for (int i = 0; i < n; i++) one[i] = 0;
    one[0] = 1;
    mont_to(m, acc, one);           /* acc = 1 in Montgomery form */
    for (size_t i = 0; i < elen; i++)
        for (int b = 7; b >= 0; b--) {
            mont_mul(m, acc, acc, acc);
            if ((e[i] >> b) & 1) mont_mul(m, acc, acc, base);
        }
    for (int i = 0; i < n; i++) r[i] = acc[i];
}

void mod_add(const mont *m, uint32_t *r, const uint32_t *a, const uint32_t *b) {
    uint32_t t[BN_LIMBS], s[BN_LIMBS];
    uint32_t c = bn_add(t, a, b, m->n);
    uint32_t borrow = bn_sub(s, t, m->m, m->n);
    bool use = c || !borrow;
    for (int i = 0; i < m->n; i++) r[i] = use ? s[i] : t[i];
}
void mod_sub(const mont *m, uint32_t *r, const uint32_t *a, const uint32_t *b) {
    uint32_t t[BN_LIMBS];
    uint32_t borrow = bn_sub(t, a, b, m->n);
    if (borrow) bn_add(t, t, m->m, m->n);
    for (int i = 0; i < m->n; i++) r[i] = t[i];
}
