/* DuncOS crypto library (freestanding): hashes, MACs, ciphers, key exchange, signatures. */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* ---- hashes ---- */
enum { H_SHA256, H_SHA384, H_SHA512 };
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; uint32_t n; } sha256_ctx;
typedef struct { uint64_t h[8]; uint64_t len; uint8_t buf[128]; uint32_t n; int outlen; } sha512_ctx;
typedef struct { int alg; union { sha256_ctx s256; sha512_ctx s512; } u; } hash_ctx;
void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *d, size_t n);
void sha256_final(sha256_ctx *c, uint8_t *out);
void sha512_init(sha512_ctx *c, int outlen);       /* 48 = SHA-384, 64 = SHA-512 */
void sha512_update(sha512_ctx *c, const void *d, size_t n);
void sha512_final(sha512_ctx *c, uint8_t *out);
int  hash_len(int alg);
void hash_init(hash_ctx *c, int alg);
void hash_update(hash_ctx *c, const void *d, size_t n);
void hash_final(hash_ctx *c, uint8_t *out);
void hash(int alg, const void *d, size_t n, uint8_t *out);
void hmac(int alg, const uint8_t *key, size_t klen, const void *msg, size_t mlen, uint8_t *out);
void hkdf_extract(int alg, const uint8_t *salt, size_t slen, const uint8_t *ikm, size_t ilen, uint8_t *prk);
void hkdf_expand(int alg, const uint8_t *prk, const uint8_t *info, size_t ilen, uint8_t *out, size_t olen);

/* ---- AEAD ciphers (tag is 16 bytes; open returns 0 if authentic) ---- */
typedef struct { uint32_t rk[44]; uint8_t h[16]; } aes128gcm;
void aes128_expand(uint32_t rk[44], const uint8_t key[16]);
void aes128_encrypt(const uint32_t rk[44], const uint8_t in[16], uint8_t out[16]);
void aes128gcm_init(aes128gcm *g, const uint8_t key[16]);
void aes128gcm_seal(const aes128gcm *g, const uint8_t iv[12], const uint8_t *aad, size_t alen, uint8_t *data, size_t len, uint8_t tag[16]);
int  aes128gcm_open(const aes128gcm *g, const uint8_t iv[12], const uint8_t *aad, size_t alen, uint8_t *data, size_t len, const uint8_t tag[16]);
void chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]);
void chacha20poly1305_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t alen, uint8_t *data, size_t len, uint8_t tag[16]);
int  chacha20poly1305_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t alen, uint8_t *data, size_t len, const uint8_t tag[16]);

/* ---- key exchange ---- */
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void x25519_base(uint8_t out[32], const uint8_t scalar[32]);
int  p256_keygen(uint8_t pub[65], const uint8_t priv[32]);                          /* 0 ok */
int  p256_ecdh(uint8_t shared[32], const uint8_t priv[32], const uint8_t *peer, size_t plen);

/* ---- signatures (return 0 if valid) ---- */
enum { CURVE_P256, CURVE_P384 };
int rsa_verify_pkcs1(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen, int halg, const uint8_t *hsh, const uint8_t *sig, size_t slen);
int rsa_verify_pss(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen, int halg, const uint8_t *hsh, const uint8_t *sig, size_t slen);
int ecdsa_verify(int curve, const uint8_t *pub, size_t plen, const uint8_t *hsh, size_t hlen, const uint8_t *sig_der, size_t slen);

/* constant-time compare: 0 if equal */
int ct_memcmp(const void *a, const void *b, size_t n);
void secure_zero(void *p, size_t n);
