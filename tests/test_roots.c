/* Parse the system CA bundle and verify every root's self-signature. */
#include <stdio.h>
#include <string.h>
#include "../user/lib/crypto/x509.h"
#include "../user/lib/crypto/crypto.h"
int main(int argc, char **argv) {
    static char pem[400000]; FILE *f = fopen(argv[1], "rb"); size_t n = fread(pem, 1, sizeof pem, f); fclose(f);
    int total = 0; for (size_t i = 0; i + 27 < n; i++) if (!memcmp(pem + i, "-----BEGIN CERTIFICATE-----", 27)) total++;
    int added = x509_add_roots_pem(pem, n);
    printf("certificates in bundle: %d, parsed: %d\n", total, added);
    /* re-parse each to check its signature with its own key */
    int ok = 0, alg = 0, bad = 0, rsa = 0, ec256 = 0, ec384 = 0;
    extern int x509_test_selfsig(int i, int *kt);
    for (int i = 0; i < added; i++) { int kt; int r = x509_test_selfsig(i, &kt); if (r == 0) ok++; else if (r == X509_EALG) alg++; else bad++;
        if (kt == KEY_RSA) rsa++; else if (kt == KEY_EC_P256) ec256++; else if (kt == KEY_EC_P384) ec384++; }
    printf("self-signatures valid: %d, unsupported algorithm: %d, invalid: %d  (keys: RSA %d, P-256 %d, P-384 %d)\n", ok, alg, bad, rsa, ec256, ec384);
    return bad != 0;
}
