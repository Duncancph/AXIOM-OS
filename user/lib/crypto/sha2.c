/* SHA-256, SHA-384, SHA-512 (FIPS 180-4), HMAC (RFC 2104), HKDF (RFC 5869). */
#include "crypto.h"
#include "../../../common/str.h"

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_ctx *c, const uint8_t *p) {
    uint32_t w[64], a, b, cc, d, e, f, g, h;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | p[i * 4 + 1] << 16 | p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void sha256_init(sha256_ctx *c) {
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(c->h, iv, 32); c->len = 0; c->n = 0;
}
void sha256_update(sha256_ctx *c, const void *data, size_t n) {
    const uint8_t *p = data;
    c->len += n;
    while (n) {
        uint32_t k = 64 - c->n < n ? 64 - c->n : (uint32_t)n;
        memcpy(c->buf + c->n, p, k); c->n += k; p += k; n -= k;
        if (c->n == 64) { sha256_block(c, c->buf); c->n = 0; }
    }
}
void sha256_final(sha256_ctx *c, uint8_t *out) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80; sha256_update(c, &pad, 1);
    uint8_t z = 0; while (c->n != 56) sha256_update(c, &z, 1);
    uint8_t l[8]; for (int i = 0; i < 8; i++) l[i] = bits >> (56 - 8 * i);
    sha256_update(c, l, 8);
    for (int i = 0; i < 8; i++) { out[i * 4] = c->h[i] >> 24; out[i * 4 + 1] = c->h[i] >> 16; out[i * 4 + 2] = c->h[i] >> 8; out[i * 4 + 3] = c->h[i]; }
}

static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL,0x142929670a0e6e70ULL,0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
    0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,0xca273eceea26619cULL,0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
    0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL };
#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static void sha512_block(sha512_ctx *c, const uint8_t *p) {
    uint64_t w[80], v[8];
    for (int i = 0; i < 16; i++) { w[i] = 0; for (int k = 0; k < 8; k++) w[i] = w[i] << 8 | p[i * 8 + k]; }
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ROR64(w[i - 15], 1) ^ ROR64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ROR64(w[i - 2], 19) ^ ROR64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (int i = 0; i < 8; i++) v[i] = c->h[i];
    for (int i = 0; i < 80; i++) {
        uint64_t t1 = v[7] + (ROR64(v[4], 14) ^ ROR64(v[4], 18) ^ ROR64(v[4], 41)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) + K512[i] + w[i];
        uint64_t t2 = (ROR64(v[0], 28) ^ ROR64(v[0], 34) ^ ROR64(v[0], 39)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1; v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) c->h[i] += v[i];
}

void sha512_init(sha512_ctx *c, int outlen) {
    static const uint64_t iv512[8] = { 0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
                                       0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL };
    static const uint64_t iv384[8] = { 0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL, 0x152fecd8f70e5939ULL,
                                       0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL, 0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL };
    memcpy(c->h, outlen == 48 ? iv384 : iv512, 64); c->len = 0; c->n = 0; c->outlen = outlen;
}
void sha512_update(sha512_ctx *c, const void *data, size_t n) {
    const uint8_t *p = data;
    c->len += n;
    while (n) {
        uint32_t k = 128 - c->n < n ? 128 - c->n : (uint32_t)n;
        memcpy(c->buf + c->n, p, k); c->n += k; p += k; n -= k;
        if (c->n == 128) { sha512_block(c, c->buf); c->n = 0; }
    }
}
void sha512_final(sha512_ctx *c, uint8_t *out) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80; sha512_update(c, &pad, 1);
    uint8_t z = 0; while (c->n != 112) sha512_update(c, &z, 1);
    uint8_t l[16] = { 0 }; for (int i = 0; i < 8; i++) l[8 + i] = bits >> (56 - 8 * i);
    sha512_update(c, l, 16);
    for (int i = 0; i < c->outlen; i++) out[i] = c->h[i / 8] >> (56 - 8 * (i % 8));
}

int hash_len(int alg) { return alg == H_SHA256 ? 32 : alg == H_SHA384 ? 48 : 64; }
void hash_init(hash_ctx *c, int alg) { c->alg = alg; if (alg == H_SHA256) sha256_init(&c->u.s256); else sha512_init(&c->u.s512, hash_len(alg)); }
void hash_update(hash_ctx *c, const void *d, size_t n) { if (c->alg == H_SHA256) sha256_update(&c->u.s256, d, n); else sha512_update(&c->u.s512, d, n); }
void hash_final(hash_ctx *c, uint8_t *out) { if (c->alg == H_SHA256) sha256_final(&c->u.s256, out); else sha512_final(&c->u.s512, out); }
void hash(int alg, const void *d, size_t n, uint8_t *out) { hash_ctx c; hash_init(&c, alg); hash_update(&c, d, n); hash_final(&c, out); }

void hmac(int alg, const uint8_t *key, size_t klen, const void *msg, size_t mlen, uint8_t *out) {
    int bs = alg == H_SHA256 ? 64 : 128, hl = hash_len(alg);
    uint8_t k[128], pad[128], inner[64];
    memset(k, 0, sizeof k);
    if (klen > (size_t)bs) hash(alg, key, klen, k); else memcpy(k, key, klen);
    hash_ctx c;
    for (int i = 0; i < bs; i++) pad[i] = k[i] ^ 0x36;
    hash_init(&c, alg); hash_update(&c, pad, bs); hash_update(&c, msg, mlen); hash_final(&c, inner);
    for (int i = 0; i < bs; i++) pad[i] = k[i] ^ 0x5c;
    hash_init(&c, alg); hash_update(&c, pad, bs); hash_update(&c, inner, hl); hash_final(&c, out);
    secure_zero(k, sizeof k); secure_zero(pad, sizeof pad);
}

void hkdf_extract(int alg, const uint8_t *salt, size_t slen, const uint8_t *ikm, size_t ilen, uint8_t *prk) {
    uint8_t zero[64] = { 0 };
    if (!salt || !slen) { salt = zero; slen = hash_len(alg); }
    hmac(alg, salt, slen, ikm, ilen, prk);
}

void hkdf_expand(int alg, const uint8_t *prk, const uint8_t *info, size_t ilen, uint8_t *out, size_t olen) {
    int hl = hash_len(alg);
    uint8_t t[64], buf[64 + 300 + 1];
    size_t tl = 0, done = 0;
    for (uint8_t i = 1; done < olen; i++) {
        memcpy(buf, t, tl); memcpy(buf + tl, info, ilen); buf[tl + ilen] = i;
        hmac(alg, prk, hl, buf, tl + ilen + 1, t);
        tl = hl;
        size_t k = olen - done < (size_t)hl ? olen - done : (size_t)hl;
        memcpy(out + done, t, k); done += k;
    }
    secure_zero(t, sizeof t);
}

int ct_memcmp(const void *a, const void *b, size_t n) {
    const volatile uint8_t *x = a, *y = b; uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= x[i] ^ y[i];
    return d;
}
void secure_zero(void *p, size_t n) { volatile uint8_t *v = p; while (n--) *v++ = 0; }
