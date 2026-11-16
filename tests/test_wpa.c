/* Host driver for the WPA2 code (user/lib/crypto/wpa.c): commands with hex arguments on stdin, results on
 * stdout. tests/test_wpa.py checks them against Python (hashlib, cryptography) and plays the router's side
 * of the 4-way and group key handshakes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "../user/lib/crypto/wpa.h"

static uint8_t A[4000], B[4000], C[4000], D[4000], E[4000], O[4000];
static size_t unh(const char *h, uint8_t *o) { size_t n = strlen(h) / 2; for (size_t i = 0; i < n; i++) sscanf(h + 2 * i, "%2hhx", &o[i]); return n; }
static void ph(const uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) printf("%02x", b[i]); printf("\n"); }
static struct wpa_sup sup;

int main(void) {
    static char line[40000], op[32], a[9000], b[9000], c[9000], d[9000], e[9000];
    setvbuf(stdout, 0, _IOLBF, 0);
    while (fgets(line, sizeof line, stdin)) {
        a[0] = b[0] = c[0] = d[0] = e[0] = 0;
        if (sscanf(line, "%31s %8999s %8999s %8999s %8999s %8999s", op, a, b, c, d, e) < 1) continue;
        if (!strcmp(a, "-")) a[0] = 0;
        if (!strcmp(b, "-")) b[0] = 0;
        size_t la = unh(a, A), lb = unh(b, B), lc = unh(c, C), ld = unh(d, D), le = unh(e, E);
        (void)lc; (void)le;
        if (!strcmp(op, "sha1")) { sha1(A, la, O); ph(O, 20); }
        else if (!strcmp(op, "hmac")) { hmac_sha1(A, la, B, lb, O); ph(O, 20); }
        else if (!strcmp(op, "pmk")) { A[la] = 0; wpa_pmk((char *)A, B, (int)lb, O); ph(O, 32); }
        else if (!strcmp(op, "prf")) { C[lc] = 0; wpa_prf(A, la, (char *)C, B, lb, O, atoi(d)); ph(O, atoi(d)); }
        else if (!strcmp(op, "wrap")) { aes_wrap(A, B, lb, O); ph(O, lb + 8); }
        else if (!strcmp(op, "unwrap")) { if (aes_unwrap(A, B, lb, O)) printf("bad\n"); else ph(O, lb - 8); }
        else if (!strcmp(op, "ccmpenc")) {    /* tk pn(hex 12) keyid(hex 2) hdr plain */
            uint64_t pn = strtoull(b, 0, 16); int n = ccmp_encrypt(A, pn, (int)strtol(c, 0, 16), D, (int)ld, E, (int)le, O); ph(O, n); }
        else if (!strcmp(op, "ccmpdec")) {    /* tk hdr body */
            uint64_t pn; int n = ccmp_decrypt(A, B, (int)lb, C, (int)lc, O, &pn); if (n < 0) printf("bad\n"); else ph(O, n); }
        else if (!strcmp(op, "init")) { wpa_sup_init(&sup, A, B, C, D, (int)ld, E); printf("ok\n"); }   /* pmk aa spa ie snonce */
        else if (!strcmp(op, "rx")) {
            int rl, r = wpa_sup_rx(&sup, A, (int)la, O, &rl);
            printf("%d ", r); if (rl) { for (int i = 0; i < rl; i++) printf("%02x", O[i]); } else printf("-");
            printf(" %s ", sup.why ? sup.why : "-"); ph(sup.ptk, 48); sup.why = 0;
        }
        else if (!strcmp(op, "gtk")) { printf("%d ", sup.gtk_idx); ph(sup.gtk, sup.gtk_len); }
        else printf("?\n");
    }
}
