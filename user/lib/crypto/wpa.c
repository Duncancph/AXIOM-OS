/* WPA2-Personal: SHA-1 (FIPS 180-4), HMAC-SHA1, PBKDF2 (RFC 8018), the 802.11 PRF, AES key wrap (RFC 3394),
 * CCMP (802.11 section 12.5.3: AES-CCM, 8-byte MIC), and the supplicant side of the 4-way and group handshakes. */
#include "wpa.h"
#include "crypto.h"
#include "../../../common/str.h"

/* ---- SHA-1 ---- */
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; uint32_t n; } sha1_ctx;
#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
static void sha1_block(sha1_ctx *c, const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & cc) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ cc ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
        else { f = b ^ cc ^ d; k = 0xCA62C1D6; }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = ROL(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}
static void sha1_init(sha1_ctx *c) { c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE; c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0; c->len = 0; c->n = 0; }
static void sha1_update(sha1_ctx *c, const void *d, size_t n) {
    const uint8_t *p = d; c->len += n;
    while (n) {
        uint32_t k = 64 - c->n; if (k > n) k = (uint32_t)n;
        memcpy(c->buf + c->n, p, k); c->n += k; p += k; n -= k;
        if (c->n == 64) { sha1_block(c, c->buf); c->n = 0; }
    }
}
static void sha1_final(sha1_ctx *c, uint8_t out[20]) {
    uint64_t bits = c->len * 8; uint8_t pad = 0x80, z = 0;
    sha1_update(c, &pad, 1);
    while (c->n != 56) sha1_update(c, &z, 1);
    uint8_t l[8]; for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(c, l, 8);
    for (int i = 0; i < 5; i++) { out[4 * i] = c->h[i] >> 24; out[4 * i + 1] = c->h[i] >> 16; out[4 * i + 2] = c->h[i] >> 8; out[4 * i + 3] = c->h[i]; }
}
void sha1(const void *d, size_t n, uint8_t out[20]) { sha1_ctx c; sha1_init(&c); sha1_update(&c, d, n); sha1_final(&c, out); }

/* HMAC-SHA1 over up to three pieces (saves copying) */
static void hmac_sha1_3(const uint8_t *key, size_t klen, const void *m1, size_t l1, const void *m2, size_t l2, const void *m3, size_t l3, uint8_t out[20]) {
    uint8_t k[64] = {0}, pad[64], ih[20];
    if (klen > 64) sha1(key, klen, k); else memcpy(k, key, klen);
    sha1_ctx c;
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha1_init(&c); sha1_update(&c, pad, 64); sha1_update(&c, m1, l1); if (l2) sha1_update(&c, m2, l2); if (l3) sha1_update(&c, m3, l3); sha1_final(&c, ih);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5C;
    sha1_init(&c); sha1_update(&c, pad, 64); sha1_update(&c, ih, 20); sha1_final(&c, out);
}
void hmac_sha1(const uint8_t *key, size_t klen, const void *msg, size_t mlen, uint8_t out[20]) { hmac_sha1_3(key, klen, msg, mlen, 0, 0, 0, 0, out); }

void wpa_pmk(const char *pass, const uint8_t *ssid, int ssid_len, uint8_t pmk[32]) {
    size_t pl = strlen(pass);
    for (uint32_t blk = 1; blk <= 2; blk++) {
        uint8_t be[4] = { 0, 0, 0, (uint8_t)blk }, u[20], t[20];
        hmac_sha1_3((const uint8_t *)pass, pl, ssid, (size_t)ssid_len, be, 4, 0, 0, u);
        memcpy(t, u, 20);
        for (int i = 1; i < 4096; i++) { hmac_sha1((const uint8_t *)pass, pl, u, 20, u); for (int j = 0; j < 20; j++) t[j] ^= u[j]; }
        memcpy(pmk + (blk - 1) * 20, t, blk == 1 ? 20 : 12);
    }
}

void wpa_prf(const uint8_t *key, size_t klen, const char *label, const uint8_t *data, size_t dlen, uint8_t *out, size_t olen) {
    size_t ll = strlen(label);
    uint8_t lab[100]; memcpy(lab, label, ll); lab[ll] = 0;          /* the label and a zero byte */
    for (uint8_t i = 0; olen; i++) {
        uint8_t h[20];
        hmac_sha1_3(key, klen, lab, ll + 1, data, dlen, &i, 1, h);
        size_t k = olen < 20 ? olen : 20;
        memcpy(out, h, k); out += k; olen -= k;
    }
}

/* ---- AES key wrap ---- */
static void aes_decrypt_block(const uint32_t rk[44], const uint8_t in[16], uint8_t out[16]);
void aes_wrap(const uint8_t kek[16], const uint8_t *in, size_t len, uint8_t *out) {
    uint32_t rk[44]; aes128_expand(rk, kek);
    size_t n = len / 8; uint8_t a[8], b[16];
    memset(a, 0xA6, 8); memcpy(out + 8, in, len);
    for (int j = 0; j < 6; j++)
        for (size_t i = 1; i <= n; i++) {
            memcpy(b, a, 8); memcpy(b + 8, out + 8 * i, 8);
            aes128_encrypt(rk, b, b);
            uint64_t t = n * j + i;
            memcpy(a, b, 8); for (int k = 0; k < 8; k++) a[7 - k] ^= (uint8_t)(t >> (8 * k));
            memcpy(out + 8 * i, b + 8, 8);
        }
    memcpy(out, a, 8);
}
int aes_unwrap(const uint8_t kek[16], const uint8_t *in, size_t len, uint8_t *out) {
    if (len < 24 || len % 8) return -1;
    uint32_t rk[44]; aes128_expand(rk, kek);
    size_t n = len / 8 - 1; uint8_t a[8], b[16];
    memcpy(a, in, 8); memcpy(out, in + 8, len - 8);
    for (int j = 5; j >= 0; j--)
        for (size_t i = n; i >= 1; i--) {
            uint64_t t = n * j + i;
            memcpy(b, a, 8); for (int k = 0; k < 8; k++) b[7 - k] ^= (uint8_t)(t >> (8 * k));
            memcpy(b + 8, out + 8 * (i - 1), 8);
            aes_decrypt_block(rk, b, b);
            memcpy(a, b, 8); memcpy(out + 8 * (i - 1), b + 8, 8);
        }
    for (int k = 0; k < 8; k++) if (a[k] != 0xA6) return -1;
    return 0;
}

/* AES-128 decryption (key unwrap needs it; the library only encrypts) */
static uint8_t inv_sbox[256], sbox_ready;
static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
static uint8_t gmul(uint8_t a, uint8_t b) { uint8_t r = 0; while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; } return r; }
static void make_inv_sbox(void) {
    /* the forward S-box from AES's definition (inverse in GF(2^8), then the affine map), inverted */
    for (int x = 0; x < 256; x++) {
        uint8_t inv = 0;
        if (x) for (int y = 1; y < 256; y++) if (gmul((uint8_t)x, (uint8_t)y) == 1) { inv = (uint8_t)y; break; }
        uint8_t s = inv ^ (uint8_t)((inv << 1) | (inv >> 7)) ^ (uint8_t)((inv << 2) | (inv >> 6)) ^ (uint8_t)((inv << 3) | (inv >> 5)) ^ (uint8_t)((inv << 4) | (inv >> 4)) ^ 0x63;
        inv_sbox[s] = (uint8_t)x;
    }
    sbox_ready = 1;
}
static void aes_decrypt_block(const uint32_t rk[44], const uint8_t in[16], uint8_t out[16]) {
    if (!sbox_ready) make_inv_sbox();
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ (uint8_t)(rk[40 + i / 4] >> (24 - 8 * (i % 4)));
    for (int r = 9; r >= 0; r--) {
        uint8_t t[16];
        for (int c = 0; c < 4; c++) for (int row = 0; row < 4; row++) t[((c + row) % 4) * 4 + row] = s[c * 4 + row];   /* inverse ShiftRows */
        for (int i = 0; i < 16; i++) t[i] = inv_sbox[t[i]];
        for (int i = 0; i < 16; i++) t[i] ^= (uint8_t)(rk[r * 4 + i / 4] >> (24 - 8 * (i % 4)));
        if (r) for (int c = 0; c < 4; c++) {                                                                            /* inverse MixColumns */
            uint8_t a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2], a3 = t[4 * c + 3];
            t[4 * c]     = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
            t[4 * c + 1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
            t[4 * c + 2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
            t[4 * c + 3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
        }
        memcpy(s, t, 16);
    }
    memcpy(out, s, 16);
}

/* ---- CCMP ---- */
static int ccm_aad(const uint8_t *hdr, int hlen, uint8_t *aad, uint8_t *prio) {
    uint8_t fc0 = hdr[0], fc1 = hdr[1];
    bool data = (fc0 & 0x0C) == 0x08, qos = data && (fc0 & 0x80), a4 = (fc1 & 3) == 3;
    int n = 0;
    aad[n++] = data ? (fc0 & 0x8F) : fc0;                   /* data: subtype bits 4-6 masked */
    aad[n++] = (fc1 & 0xC7) | 0x40;                         /* retry, power management, more data off; protected on */
    memcpy(aad + n, hdr + 4, 18); n += 18;                  /* addresses 1-3 */
    aad[n++] = hdr[22] & 0x0F; aad[n++] = 0;                /* sequence control: fragment number only */
    int off = 24;
    if (a4) { memcpy(aad + n, hdr + off, 6); n += 6; off += 6; }
    *prio = 0;
    if (qos) { if (off + 2 > hlen) return -1; aad[n++] = hdr[off] & 0x0F; aad[n++] = 0; *prio = hdr[off] & 0x0F; }
    if (!data && (fc0 & 0x0C) == 0) *prio = 0x10;           /* management frames (protected management frames) */
    return n;
}
static void ccm(const uint8_t tk[16], const uint8_t nonce[13], const uint8_t *aad, int alen, uint8_t *data, int dlen, uint8_t mic[8], bool enc) {
    uint32_t rk[44]; aes128_expand(rk, tk);
    uint8_t x[16], blk[16], a[16], s[16];
    /* CBC-MAC over B0, the header data and the plaintext */
    x[0] = 0x59; memcpy(x + 1, nonce, 13); x[14] = (uint8_t)(dlen >> 8); x[15] = (uint8_t)dlen;
    aes128_encrypt(rk, x, x);
    uint8_t ab[64] = {0}; ab[0] = (uint8_t)(alen >> 8); ab[1] = (uint8_t)alen; memcpy(ab + 2, aad, alen);
    for (int i = 0; i < alen + 2; i += 16) { for (int k = 0; k < 16; k++) x[k] ^= ab[i + k]; aes128_encrypt(rk, x, x); }
    a[0] = 0x01; memcpy(a + 1, nonce, 13);
    for (int i = 0, ctr = 1; i < dlen; i += 16, ctr++) {
        int k = dlen - i < 16 ? dlen - i : 16;
        a[14] = (uint8_t)(ctr >> 8); a[15] = (uint8_t)ctr; aes128_encrypt(rk, a, s);
        memset(blk, 0, 16);
        if (enc) { memcpy(blk, data + i, k); for (int j = 0; j < k; j++) data[i + j] ^= s[j]; }
        else { for (int j = 0; j < k; j++) data[i + j] ^= s[j]; memcpy(blk, data + i, k); }
        for (int j = 0; j < 16; j++) x[j] ^= blk[j];
        aes128_encrypt(rk, x, x);
    }
    a[14] = a[15] = 0; aes128_encrypt(rk, a, s);
    for (int j = 0; j < 8; j++) mic[j] = x[j] ^ s[j];
}
static void ccm_nonce(uint8_t n[13], uint8_t prio, const uint8_t *a2, uint64_t pn) {
    n[0] = prio; memcpy(n + 1, a2, 6);
    for (int i = 0; i < 6; i++) n[7 + i] = (uint8_t)(pn >> (40 - 8 * i));
}
int ccmp_encrypt(const uint8_t tk[16], uint64_t pn, int keyid, const uint8_t *hdr, int hlen, const uint8_t *plain, int plen, uint8_t *out) {
    uint8_t aad[32], prio, nonce[13];
    int alen = ccm_aad(hdr, hlen, aad, &prio);
    if (alen < 0) return -1;
    out[0] = (uint8_t)pn; out[1] = (uint8_t)(pn >> 8); out[2] = 0; out[3] = (uint8_t)(0x20 | (keyid << 6));
    out[4] = (uint8_t)(pn >> 16); out[5] = (uint8_t)(pn >> 24); out[6] = (uint8_t)(pn >> 32); out[7] = (uint8_t)(pn >> 40);
    memcpy(out + 8, plain, plen);
    ccm_nonce(nonce, prio, hdr + 10, pn);
    ccm(tk, nonce, aad, alen, out + 8, plen, out + 8 + plen, true);
    return plen + 16;
}
int ccmp_decrypt(const uint8_t tk[16], const uint8_t *hdr, int hlen, const uint8_t *body, int blen, uint8_t *out, uint64_t *pn_out) {
    if (blen < 16 || !(body[3] & 0x20)) return -1;
    uint64_t pn = (uint64_t)body[0] | (uint64_t)body[1] << 8 | (uint64_t)body[4] << 16 | (uint64_t)body[5] << 24 | (uint64_t)body[6] << 32 | (uint64_t)body[7] << 40;
    uint8_t aad[32], prio, nonce[13], mic[8];
    int alen = ccm_aad(hdr, hlen, aad, &prio);
    if (alen < 0) return -1;
    int dlen = blen - 16;
    memcpy(out, body + 8, dlen);
    ccm_nonce(nonce, prio, hdr + 10, pn);
    ccm(tk, nonce, aad, alen, out, dlen, mic, false);
    uint8_t d = 0; for (int i = 0; i < 8; i++) d |= mic[i] ^ body[8 + dlen + i];
    if (d) return -1;
    if (pn_out) *pn_out = pn;
    return dlen;
}

/* ---- the supplicant ---- */
#define KI_VER     0x0007
#define KI_PAIR    0x0008
#define KI_INSTALL 0x0040
#define KI_ACK     0x0080
#define KI_MIC     0x0100
#define KI_SECURE  0x0200
#define KI_ENC     0x1000
/* offsets in the EAPOL frame (4-byte 802.1X header, then the key descriptor) */
enum { O_DESC = 4, O_INFO = 5, O_KLEN = 7, O_REPLAY = 9, O_NONCE = 17, O_IV = 49, O_RSC = 65, O_MIC = 81, O_DLEN = 97, O_DATA = 99 };

void wpa_sup_init(struct wpa_sup *s, const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6], const uint8_t *ie, int ie_len, const uint8_t snonce[32]) {
    memset(s, 0, sizeof *s);
    memcpy(s->pmk, pmk, 32); memcpy(s->aa, aa, 6); memcpy(s->spa, spa, 6); memcpy(s->snonce, snonce, 32);
    if (ie_len > 64) ie_len = 64;
    memcpy(s->ie, ie, ie_len); s->ie_len = ie_len;
}
static void put16(uint8_t *p, int v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static int get16(const uint8_t *p) { return p[0] << 8 | p[1]; }
static void derive_ptk(struct wpa_sup *s) {
    uint8_t d[76];
    bool a_first = memcmp(s->aa, s->spa, 6) < 0;
    memcpy(d, a_first ? s->aa : s->spa, 6); memcpy(d + 6, a_first ? s->spa : s->aa, 6);
    bool n_first = memcmp(s->anonce, s->snonce, 32) < 0;
    memcpy(d + 12, n_first ? s->anonce : s->snonce, 32); memcpy(d + 44, n_first ? s->snonce : s->anonce, 32);
    wpa_prf(s->pmk, 32, "Pairwise key expansion", d, 76, s->ptk, 48);
}
static void set_mic(const uint8_t kck[16], uint8_t *f, int len) {
    uint8_t h[20];
    memset(f + O_MIC, 0, 16); hmac_sha1(kck, 16, f, len, h); memcpy(f + O_MIC, h, 16);
}
static bool mic_ok(const uint8_t kck[16], const uint8_t *f, int len) {
    uint8_t c[512], h[20];
    if (len > (int)sizeof c) return false;
    memcpy(c, f, len); memset(c + O_MIC, 0, 16); hmac_sha1(kck, 16, c, len, h);
    uint8_t d = 0; for (int i = 0; i < 16; i++) d |= h[i] ^ f[O_MIC + i];
    return !d;
}
static int build(struct wpa_sup *s, uint8_t *r, int info, const uint8_t *replay, const uint8_t *nonce, const uint8_t *data, int dlen) {
    int len = O_DATA + dlen;
    memset(r, 0, len);
    r[0] = 1; r[1] = 3; put16(r + 2, len - 4);
    r[O_DESC] = 2; put16(r + O_INFO, info);
    memcpy(r + O_REPLAY, replay, 8);
    if (nonce) memcpy(r + O_NONCE, nonce, 32);
    put16(r + O_DLEN, dlen); if (dlen) memcpy(r + O_DATA, data, dlen);
    set_mic(s->ptk, r, len);
    return len;
}
/* the GTK from (decrypted) key data: a vendor element 0xDD, OUI 00-0F-AC, type 1 */
static bool find_gtk(struct wpa_sup *s, const uint8_t *kd, int n) {
    for (int i = 0; i + 2 <= n && kd[i] != 0;) {
        int t = kd[i], l = kd[i + 1];
        if (i + 2 + l > n) break;
        if (t == 0xDD && l >= 6 + 16 && kd[i + 2] == 0x00 && kd[i + 3] == 0x0F && kd[i + 4] == 0xAC && kd[i + 5] == 1) {
            s->gtk_idx = kd[i + 6] & 3; s->gtk_len = l - 6; if (s->gtk_len > 32) s->gtk_len = 32;
            memcpy(s->gtk, kd + i + 8, s->gtk_len); return true;
        }
        i += 2 + l;
    }
    return false;
}
int wpa_sup_rx(struct wpa_sup *s, const uint8_t *f, int len, uint8_t *reply, int *rlen) {
    *rlen = 0;
    if (len < O_DATA || f[1] != 3 || (f[O_DESC] != 2 && f[O_DESC] != 254)) return WPA_IGNORE;
    int body = get16(f + 2); if (body + 4 < O_DATA || body + 4 > len) { s->why = "short key frame"; return WPA_BAD; }
    len = body + 4;
    int info = get16(f + O_INFO), dlen = get16(f + O_DLEN);
    if (O_DATA + dlen > len) { s->why = "key data longer than the frame"; return WPA_BAD; }
    if ((info & KI_VER) != 2) { s->why = "not WPA2-AES (key descriptor version)"; return WPA_BAD; }
    uint64_t rc = 0; for (int i = 0; i < 8; i++) rc = rc << 8 | f[O_REPLAY + i];
    if (!(info & KI_ACK)) return WPA_IGNORE;
    bool stale = s->replay_ok && rc <= s->replay;              /* an old (replayed) message */
    if (info & KI_PAIR) {
        if (!(info & KI_MIC)) {                                        /* message 1: ANonce */
            memcpy(s->anonce, f + O_NONCE, 32);
            derive_ptk(s);
            *rlen = build(s, reply, 2 | KI_PAIR | KI_MIC, f + O_REPLAY, s->snonce, s->ie, s->ie_len);
            return WPA_REPLY;
        }
        /* message 3: install, with the GTK */
        if (stale) { s->why = "old replay counter"; return WPA_IGNORE; }
        if (memcmp(s->anonce, f + O_NONCE, 32)) { s->why = "message 3 has a different ANonce"; return WPA_BAD; }
        if (!mic_ok(s->ptk, f, len)) { s->why = "message 3 MIC is wrong (wrong password?)"; return WPA_BAD; }
        s->replay = rc; s->replay_ok = true;
        uint8_t kd[256];
        if (info & KI_ENC) {
            if (dlen > (int)sizeof kd || aes_unwrap(s->ptk + 16, f + O_DATA, dlen, kd)) { s->why = "couldn't decrypt the key data"; return WPA_BAD; }
            dlen -= 8;
        } else memcpy(kd, f + O_DATA, dlen);
        find_gtk(s, kd, dlen);
        *rlen = build(s, reply, 2 | KI_PAIR | KI_MIC | KI_SECURE, f + O_REPLAY, 0, 0, 0);
        s->have_ptk = true;
        return WPA_KEYS;
    }
    /* group key handshake message 1 (a new GTK) */
    if (stale) { s->why = "old replay counter"; return WPA_IGNORE; }
    if (!s->have_ptk || !(info & KI_MIC) || !mic_ok(s->ptk, f, len)) { s->why = "group key message not authentic"; return WPA_BAD; }
    s->replay = rc; s->replay_ok = true;
    uint8_t kd[256];
    if (dlen > (int)sizeof kd || aes_unwrap(s->ptk + 16, f + O_DATA, dlen, kd)) { s->why = "couldn't decrypt the group key"; return WPA_BAD; }
    if (!find_gtk(s, kd, dlen - 8)) { s->why = "no group key in the message"; return WPA_BAD; }
    *rlen = build(s, reply, 2 | KI_MIC | KI_SECURE, f + O_REPLAY, 0, 0, 0);
    return WPA_GROUP;
}
