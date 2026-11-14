/* Fixed-size big integers with Montgomery multiplication (for RSA and elliptic curves). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#define BN_LIMBS 260          /* up to 8320-bit moduli */
typedef struct { int n; uint32_t d[BN_LIMBS]; } bn;          /* little-endian 32-bit limbs */
typedef struct { int n; uint32_t m[BN_LIMBS], r2[BN_LIMBS]; uint32_t minv; } mont;

void bn_from_bytes(uint32_t *d, int n, const uint8_t *b, size_t len);   /* big-endian bytes */
void bn_to_bytes(const uint32_t *d, int n, uint8_t *b, size_t len);
int  bn_cmp(const uint32_t *a, const uint32_t *b, int n);
int  bn_is_zero(const uint32_t *a, int n);
uint32_t bn_add(uint32_t *r, const uint32_t *a, const uint32_t *b, int n);  /* returns carry */
uint32_t bn_sub(uint32_t *r, const uint32_t *a, const uint32_t *b, int n);  /* returns borrow */
int  mont_init(mont *m, const uint8_t *mod, size_t len);                    /* modulus must be odd */
void mont_mul(const mont *m, uint32_t *r, const uint32_t *a, const uint32_t *b);
void mont_to(const mont *m, uint32_t *r, const uint32_t *a);                /* a < m */
void mont_from(const mont *m, uint32_t *r, const uint32_t *a);
void mont_pow(const mont *m, uint32_t *r, const uint32_t *base_m, const uint8_t *e, size_t elen);  /* base in Montgomery form, result too */
void mod_add(const mont *m, uint32_t *r, const uint32_t *a, const uint32_t *b);
void mod_sub(const mont *m, uint32_t *r, const uint32_t *a, const uint32_t *b);
