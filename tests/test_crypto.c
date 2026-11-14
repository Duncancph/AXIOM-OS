/* Host-side driver for the crypto library: reads commands with hex arguments on stdin,
 * prints results. tests/test_crypto.py compares them with Python's cryptography (OpenSSL). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../user/lib/crypto/crypto.h"

static uint8_t A[9000], B[9000], C[9000], D[9000], E[9000], O[9000];
static size_t unh(const char *h, uint8_t *o) { size_t n = strlen(h) / 2; for (size_t i = 0; i < n; i++) sscanf(h + 2 * i, "%2hhx", &o[i]); return n; }
static void ph(const uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) printf("%02x", b[i]); printf("\n"); }

int main(void) {
    static char line[80000], op[32], a[20000], b[20000], c[20000], d[20000], e[20000];
    while (fgets(line, sizeof line, stdin)) {
        a[0] = b[0] = c[0] = d[0] = e[0] = 0;
        int k = sscanf(line, "%31s %19999s %19999s %19999s %19999s %19999s", op, a, b, c, d, e);
        if (k < 1) continue;
        if (!strcmp(a, "-")) a[0] = 0;
        if (!strcmp(b, "-")) b[0] = 0;
        size_t la = unh(a, A), lb = unh(b, B), lc = unh(c, C), ld = unh(d, D), le = unh(e, E);
        if (!strcmp(op, "sha256")) { hash(H_SHA256, A, la, O); ph(O, 32); }
        else if (!strcmp(op, "sha384")) { hash(H_SHA384, A, la, O); ph(O, 48); }
        else if (!strcmp(op, "sha512")) { hash(H_SHA512, A, la, O); ph(O, 64); }
        else if (!strcmp(op, "hmac256")) { hmac(H_SHA256, A, la, B, lb, O); ph(O, 32); }
        else if (!strcmp(op, "hmac384")) { hmac(H_SHA384, A, la, B, lb, O); ph(O, 48); }
        else if (!strcmp(op, "hkdf256")) { uint8_t prk[64]; hkdf_extract(H_SHA256, A, la, B, lb, prk); hkdf_expand(H_SHA256, prk, C, lc, O, atoi(d)); ph(O, atoi(d)); }
        else if (!strcmp(op, "gcm")) { aes128gcm g; aes128gcm_init(&g, A); uint8_t tag[16]; memcpy(O, D, ld); aes128gcm_seal(&g, B, C, lc, O, ld, tag); memcpy(O + ld, tag, 16); ph(O, ld + 16);
            /* and decrypt it back */ if (aes128gcm_open(&g, B, C, lc, O, ld, tag) || memcmp(O, D, ld)) printf("OPENFAIL\n"); }
        else if (!strcmp(op, "gcmopen")) { aes128gcm g; aes128gcm_init(&g, A); memcpy(O, D, ld - 16); printf("%s\n", aes128gcm_open(&g, B, C, lc, O, ld - 16, D + ld - 16) ? "bad" : "ok"); }
        else if (!strcmp(op, "chacha")) { uint8_t tag[16]; memcpy(O, D, ld); chacha20poly1305_seal(A, B, C, lc, O, ld, tag); memcpy(O + ld, tag, 16); ph(O, ld + 16);
            if (chacha20poly1305_open(A, B, C, lc, O, ld, tag) || memcmp(O, D, ld)) printf("OPENFAIL\n"); }
        else if (!strcmp(op, "x25519")) { x25519(O, A, B); ph(O, 32); }
        else if (!strcmp(op, "x25519base")) { x25519_base(O, A); ph(O, 32); }
        else if (!strcmp(op, "p256pub")) { if (p256_keygen(O, A)) printf("err\n"); else ph(O, 65); }
        else if (!strcmp(op, "p256ecdh")) { if (p256_ecdh(O, A, B, lb)) printf("err\n"); else ph(O, 32); }
        else if (!strcmp(op, "rsapkcs1") || !strcmp(op, "rsapss")) {   /* n e halg hash sig */
            int halg = atoi(c);
            int r = !strcmp(op, "rsapkcs1") ? rsa_verify_pkcs1(A, la, B, lb, halg, D, E, le) : rsa_verify_pss(A, la, B, lb, halg, D, E, le);
            printf("%s\n", r ? "bad" : "ok");
        }
        else if (!strcmp(op, "ecdsa")) {   /* curve pub hash sig */
            printf("%s\n", ecdsa_verify(atoi(a), B, lb, C, lc, D, ld) ? "bad" : "ok");
        }
        fflush(stdout);
    }
    return 0;
}
