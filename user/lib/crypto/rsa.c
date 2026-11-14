/* RSA signature verification: PKCS#1 v1.5 and PSS (RFC 8017). Public-key operations only. */
typedef int bool;
#include "bn.h"
#include "crypto.h"
#include "../../../common/str.h"

/* em = sig^e mod n, k bytes */
static int rsa_public(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen, const uint8_t *sig, size_t slen, uint8_t *em, size_t *k) {
    static mont m;
    while (nlen && !*n) { n++; nlen--; }
    if (nlen < 128 || nlen > 1024 || slen != nlen) return -1;      /* 1024..8192-bit keys */
    if (mont_init(&m, n, nlen)) return -1;
    static uint32_t s[BN_LIMBS], sm[BN_LIMBS], r[BN_LIMBS];
    bn_from_bytes(s, m.n, sig, slen);
    if (bn_cmp(s, m.m, m.n) >= 0) return -1;
    mont_to(&m, sm, s);
    mont_pow(&m, r, sm, e, elen);
    mont_from(&m, s, r);
    bn_to_bytes(s, m.n, em, nlen);
    *k = nlen;
    return 0;
}

static const uint8_t DI256[] = { 0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20 };
static const uint8_t DI384[] = { 0x30,0x41,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02,0x05,0x00,0x04,0x30 };
static const uint8_t DI512[] = { 0x30,0x51,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x03,0x05,0x00,0x04,0x40 };

int rsa_verify_pkcs1(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen, int halg, const uint8_t *hsh, const uint8_t *sig, size_t slen) {
    static uint8_t em[1024], want[1024];
    size_t k;
    if (rsa_public(n, nlen, e, elen, sig, slen, em, &k)) return -1;
    const uint8_t *di = halg == H_SHA256 ? DI256 : halg == H_SHA384 ? DI384 : DI512;
    size_t dl = sizeof DI256, hl = hash_len(halg), tl = dl + hl;
    if (k < tl + 11) return -1;
    want[0] = 0; want[1] = 1;
    memset(want + 2, 0xFF, k - tl - 3);
    want[k - tl - 1] = 0;
    memcpy(want + k - tl, di, dl);
    memcpy(want + k - hl, hsh, hl);
    return ct_memcmp(em, want, k) ? -1 : 0;
}

static void mgf1(int halg, const uint8_t *seed, size_t slen, uint8_t *mask, size_t len) {
    int hl = hash_len(halg);
    uint8_t buf[64 + 4], h[64];
    memcpy(buf, seed, slen);
    for (uint32_t c = 0, done = 0; done < len; c++) {
        buf[slen] = c >> 24; buf[slen + 1] = c >> 16; buf[slen + 2] = c >> 8; buf[slen + 3] = c;
        hash(halg, buf, slen + 4, h);
        for (int i = 0; i < hl && done < len; i++) mask[done++] ^= h[i];
    }
}

int rsa_verify_pss(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen, int halg, const uint8_t *hsh, const uint8_t *sig, size_t slen) {
    static uint8_t em_full[1024];
    size_t k;
    if (rsa_public(n, nlen, e, elen, sig, slen, em_full, &k)) return -1;
    while (nlen && !*n) { n++; nlen--; }
    int top = 7; while (top >= 0 && !((n[0] >> top) & 1)) top--;
    size_t modbits = (nlen - 1) * 8 + top + 1, embits = modbits - 1, emlen = (embits + 7) / 8;
    const uint8_t *em = em_full + (k - emlen);
    if (k > emlen && em_full[0]) return -1;
    size_t hl = hash_len(halg), sl = hl;
    if (emlen < hl + sl + 2 || em[emlen - 1] != 0xBC) return -1;
    size_t dblen = emlen - hl - 1;
    static uint8_t db[1024];
    memcpy(db, em, dblen);
    const uint8_t *H = em + dblen;
    uint8_t topmask = (uint8_t)(0xFF >> (8 * emlen - embits));
    if (db[0] & ~topmask) return -1;
    mgf1(halg, H, hl, db, dblen);
    db[0] &= topmask;
    for (size_t i = 0; i < dblen - sl - 1; i++) if (db[i]) return -1;
    if (db[dblen - sl - 1] != 1) return -1;
    uint8_t mp[8 + 64 + 64], hp[64];
    memset(mp, 0, 8); memcpy(mp + 8, hsh, hl); memcpy(mp + 8 + hl, db + dblen - sl, sl);
    hash(halg, mp, 8 + hl + sl, hp);
    return ct_memcmp(hp, H, hl) ? -1 : 0;
}
