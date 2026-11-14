/* AES-128-GCM (FIPS 197, SP 800-38D) and ChaCha20-Poly1305 (RFC 8439). */
#include "crypto.h"
#include "../../../common/str.h"

/* ---------------- AES-128 ---------------- */
static uint8_t SBOX[256];
static int sbox_ready;
static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }
static void make_sbox(void) {   /* S-box from the multiplicative inverse in GF(2^8) + affine transform */
    uint8_t p = 1, q = 1;
    do {
        p = p ^ (uint8_t)(p << 1) ^ (p & 0x80 ? 0x1B : 0);     /* p *= 3 */
        q ^= q << 1; q ^= q << 2; q ^= q << 4; if (q & 0x80) q ^= 0x09;   /* q /= 3 */
        uint8_t x = q ^ (uint8_t)(q << 1 | q >> 7) ^ (uint8_t)(q << 2 | q >> 6) ^ (uint8_t)(q << 3 | q >> 5) ^ (uint8_t)(q << 4 | q >> 4);
        SBOX[p] = x ^ 0x63;
    } while (p != 1);
    SBOX[0] = 0x63;
    sbox_ready = 1;
}

void aes128_expand(uint32_t rk[44], const uint8_t key[16]) {
    if (!sbox_ready) make_sbox();
    for (int i = 0; i < 4; i++) rk[i] = (uint32_t)key[i * 4] << 24 | key[i * 4 + 1] << 16 | key[i * 4 + 2] << 8 | key[i * 4 + 3];
    uint8_t rcon = 1;
    for (int i = 4; i < 44; i++) {
        uint32_t t = rk[i - 1];
        if (i % 4 == 0) {
            t = (t << 8) | (t >> 24);
            t = (uint32_t)SBOX[t >> 24] << 24 | (uint32_t)SBOX[(t >> 16) & 255] << 16 | (uint32_t)SBOX[(t >> 8) & 255] << 8 | SBOX[t & 255];
            t ^= (uint32_t)rcon << 24; rcon = xt(rcon);
        }
        rk[i] = rk[i - 4] ^ t;
    }
}

void aes128_encrypt(const uint32_t rk[44], const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ (uint8_t)(rk[i / 4] >> (24 - 8 * (i % 4)));
    for (int r = 1; r <= 10; r++) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++) t[i] = SBOX[s[(i + 4 * (i % 4)) % 16]];   /* SubBytes + ShiftRows */
        if (r < 10)
            for (int c = 0; c < 4; c++) {                                    /* MixColumns */
                uint8_t a0 = t[c * 4], a1 = t[c * 4 + 1], a2 = t[c * 4 + 2], a3 = t[c * 4 + 3], x = a0 ^ a1 ^ a2 ^ a3;
                t[c * 4] ^= x ^ xt(a0 ^ a1); t[c * 4 + 1] ^= x ^ xt(a1 ^ a2); t[c * 4 + 2] ^= x ^ xt(a2 ^ a3); t[c * 4 + 3] ^= x ^ xt(a3 ^ a0);
            }
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ (uint8_t)(rk[r * 4 + i / 4] >> (24 - 8 * (i % 4)));
    }
    memcpy(out, s, 16);
}

/* GHASH multiply in GF(2^128) */
static void gmul(uint8_t x[16], const uint8_t h[16]) {
    uint8_t z[16] = { 0 }, v[16];
    memcpy(v, h, 16);
    for (int i = 0; i < 128; i++) {
        uint8_t bit = (x[i / 8] >> (7 - i % 8)) & 1, mask = (uint8_t)-bit;
        for (int k = 0; k < 16; k++) z[k] ^= v[k] & mask;
        uint8_t lsb = v[15] & 1;
        for (int k = 15; k > 0; k--) v[k] = (uint8_t)(v[k] >> 1 | v[k - 1] << 7);
        v[0] >>= 1;
        v[0] ^= 0xE1 & (uint8_t)-lsb;
    }
    memcpy(x, z, 16);
}

static void ghash(const uint8_t h[16], const uint8_t *aad, size_t alen, const uint8_t *ct, size_t clen, uint8_t out[16]) {
    uint8_t y[16] = { 0 };
    for (size_t i = 0; i < alen; i += 16) { for (size_t k = 0; k < 16 && i + k < alen; k++) y[k] ^= aad[i + k]; gmul(y, h); }
    for (size_t i = 0; i < clen; i += 16) { for (size_t k = 0; k < 16 && i + k < clen; k++) y[k] ^= ct[i + k]; gmul(y, h); }
    uint64_t ab = (uint64_t)alen * 8, cb = (uint64_t)clen * 8;
    for (int k = 0; k < 8; k++) { y[k] ^= ab >> (56 - 8 * k); y[8 + k] ^= cb >> (56 - 8 * k); }
    gmul(y, h);
    memcpy(out, y, 16);
}

void aes128gcm_init(aes128gcm *g, const uint8_t key[16]) {
    uint8_t z[16] = { 0 };
    aes128_expand(g->rk, key);
    aes128_encrypt(g->rk, z, g->h);
}

static void gctr(const aes128gcm *g, const uint8_t iv[12], uint8_t *data, size_t len) {
    uint8_t ctr[16], ks[16];
    memcpy(ctr, iv, 12);
    uint32_t c = 2;
    for (size_t i = 0; i < len; i += 16, c++) {
        ctr[12] = c >> 24; ctr[13] = c >> 16; ctr[14] = c >> 8; ctr[15] = c;
        aes128_encrypt(g->rk, ctr, ks);
        for (size_t k = 0; k < 16 && i + k < len; k++) data[i + k] ^= ks[k];
    }
}

static void tag_mask(const aes128gcm *g, const uint8_t iv[12], uint8_t s[16]) {
    uint8_t j0[16], ek[16];
    memcpy(j0, iv, 12); j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
    aes128_encrypt(g->rk, j0, ek);
    for (int i = 0; i < 16; i++) s[i] ^= ek[i];
}

void aes128gcm_seal(const aes128gcm *g, const uint8_t iv[12], const uint8_t *aad, size_t alen, uint8_t *data, size_t len, uint8_t tag[16]) {
    gctr(g, iv, data, len);
    ghash(g->h, aad, alen, data, len, tag);
    tag_mask(g, iv, tag);
}

int aes128gcm_open(const aes128gcm *g, const uint8_t iv[12], const uint8_t *aad, size_t alen, uint8_t *data, size_t len, const uint8_t tag[16]) {
    uint8_t t[16];
    ghash(g->h, aad, alen, data, len, t);
    tag_mask(g, iv, t);
    if (ct_memcmp(t, tag, 16)) return -1;
    gctr(g, iv, data, len);
    return 0;
}

/* ---------------- ChaCha20 ---------------- */
#define ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define QR(a, b, c, d) a += b; d ^= a; d = ROTL(d, 16); c += d; b ^= c; b = ROTL(b, 12); a += b; d ^= a; d = ROTL(d, 8); c += d; b ^= c; b = ROTL(b, 7)
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

void chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]) {
    uint32_t s[16], x[16];
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) s[4 + i] = le32(key + 4 * i);
    s[12] = counter; s[13] = le32(nonce); s[14] = le32(nonce + 4); s[15] = le32(nonce + 8);
    memcpy(x, s, 64);
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]); QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]); QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) { uint32_t v = x[i] + s[i]; out[i * 4] = v; out[i * 4 + 1] = v >> 8; out[i * 4 + 2] = v >> 16; out[i * 4 + 3] = v >> 24; }
}

static void chacha20_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t *data, size_t len) {
    uint8_t ks[64];
    for (size_t i = 0; i < len; i += 64, counter++) {
        chacha20_block(key, counter, nonce, ks);
        for (size_t k = 0; k < 64 && i + k < len; k++) data[i + k] ^= ks[k];
    }
}

/* ---------------- Poly1305 (26-bit limbs) ---------------- */
struct poly { uint32_t r[5], h[5], pad[4]; };
static void poly_init(struct poly *p, const uint8_t key[32]) {
    p->r[0] = le32(key) & 0x3ffffff;
    p->r[1] = (le32(key + 3) >> 2) & 0x3ffff03;
    p->r[2] = (le32(key + 6) >> 4) & 0x3ffc0ff;
    p->r[3] = (le32(key + 9) >> 6) & 0x3f03fff;
    p->r[4] = (le32(key + 12) >> 8) & 0x00fffff;
    for (int i = 0; i < 5; i++) p->h[i] = 0;
    for (int i = 0; i < 4; i++) p->pad[i] = le32(key + 16 + 4 * i);
}
static void poly_block(struct poly *p, const uint8_t m[16], uint32_t hibit) {
    uint32_t r0 = p->r[0], r1 = p->r[1], r2 = p->r[2], r3 = p->r[3], r4 = p->r[4];
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];
    h0 += le32(m) & 0x3ffffff;
    h1 += (le32(m + 3) >> 2) & 0x3ffffff;
    h2 += (le32(m + 6) >> 4) & 0x3ffffff;
    h3 += (le32(m + 9) >> 6) & 0x3ffffff;
    h4 += (le32(m + 12) >> 8) | hibit;
    uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
    uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
    uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
    uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
    uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
    uint32_t c;
    c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff; d1 += c;
    c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff; d2 += c;
    c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff; d3 += c;
    c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff; d4 += c;
    c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    p->h[0] = h0; p->h[1] = h1; p->h[2] = h2; p->h[3] = h3; p->h[4] = h4;
}
static void poly_update(struct poly *p, const uint8_t *m, size_t len) {   /* zero-padded to 16 bytes */
    for (size_t i = 0; i < len; i += 16) {
        uint8_t b[16] = { 0 };
        memcpy(b, m + i, len - i < 16 ? len - i : 16);
        poly_block(p, b, 1u << 24);
    }
}
static void poly_finish(struct poly *p, uint8_t tag[16]) {
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4], c;
    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);
    uint32_t mask = (g4 >> 31) - 1;     /* all ones if h >= p */
    h0 = (h0 & ~mask) | (g0 & mask); h1 = (h1 & ~mask) | (g1 & mask); h2 = (h2 & ~mask) | (g2 & mask);
    h3 = (h3 & ~mask) | (g3 & mask); h4 = (h4 & ~mask) | (g4 & mask);
    uint64_t f;
    h0 = (h0 | (h1 << 26)); h1 = ((h1 >> 6) | (h2 << 20)); h2 = ((h2 >> 12) | (h3 << 14)); h3 = ((h3 >> 18) | (h4 << 8));
    f = (uint64_t)h0 + p->pad[0]; h0 = (uint32_t)f;
    f = (uint64_t)h1 + p->pad[1] + (f >> 32); h1 = (uint32_t)f;
    f = (uint64_t)h2 + p->pad[2] + (f >> 32); h2 = (uint32_t)f;
    f = (uint64_t)h3 + p->pad[3] + (f >> 32); h3 = (uint32_t)f;
    uint32_t o[4] = { h0, h1, h2, h3 };
    for (int i = 0; i < 4; i++) { tag[i * 4] = o[i]; tag[i * 4 + 1] = o[i] >> 8; tag[i * 4 + 2] = o[i] >> 16; tag[i * 4 + 3] = o[i] >> 24; }
}

static void cp_tag(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t alen, const uint8_t *ct, size_t clen, uint8_t tag[16]) {
    uint8_t blk[64];
    chacha20_block(key, 0, nonce, blk);
    struct poly p; poly_init(&p, blk);
    poly_update(&p, aad, alen);
    poly_update(&p, ct, clen);
    uint8_t lens[16];
    for (int i = 0; i < 8; i++) { lens[i] = (uint64_t)alen >> (8 * i); lens[8 + i] = (uint64_t)clen >> (8 * i); }
    poly_block(&p, lens, 1u << 24);
    poly_finish(&p, tag);
    secure_zero(blk, sizeof blk);
}

void chacha20poly1305_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t alen, uint8_t *data, size_t len, uint8_t tag[16]) {
    chacha20_xor(key, 1, nonce, data, len);
    cp_tag(key, nonce, aad, alen, data, len, tag);
}

int chacha20poly1305_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t alen, uint8_t *data, size_t len, const uint8_t tag[16]) {
    uint8_t t[16];
    cp_tag(key, nonce, aad, alen, data, len, t);
    if (ct_memcmp(t, tag, 16)) return -1;
    chacha20_xor(key, 1, nonce, data, len);
    return 0;
}
