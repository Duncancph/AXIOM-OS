/* WPA2-Personal (802.11i, "WPA2-PSK, AES"): the password-to-key step, the four-message handshake with the
 * router, the group key update, and CCMP (AES-CCM) packet encryption. Freestanding; used by the Wi-Fi driver. */
#pragma once
#include <stdint.h>
#include <stddef.h>

void sha1(const void *d, size_t n, uint8_t out[20]);
void hmac_sha1(const uint8_t *key, size_t klen, const void *msg, size_t mlen, uint8_t out[20]);
void wpa_pmk(const char *pass, const uint8_t *ssid, int ssid_len, uint8_t pmk[32]);    /* PBKDF2-SHA1, 4096 rounds */
void wpa_prf(const uint8_t *key, size_t klen, const char *label, const uint8_t *data, size_t dlen, uint8_t *out, size_t olen);
int  aes_unwrap(const uint8_t kek[16], const uint8_t *in, size_t len, uint8_t *out);   /* RFC 3394; out: len-8 bytes; 0 = ok */
void aes_wrap(const uint8_t kek[16], const uint8_t *in, size_t len, uint8_t *out);      /* out: len+8 bytes */

/* CCMP. hdr is the 802.11 header (24-32 bytes); out/body hold the 8-byte CCMP header, the data and the 8-byte MIC. */
int  ccmp_encrypt(const uint8_t tk[16], uint64_t pn, int keyid, const uint8_t *hdr, int hlen, const uint8_t *plain, int plen, uint8_t *out);
int  ccmp_decrypt(const uint8_t tk[16], const uint8_t *hdr, int hlen, const uint8_t *body, int blen, uint8_t *out, uint64_t *pn);  /* plain length, or -1 */

/* The supplicant (our side of the handshake). Feed it each EAPOL frame from the router (the 802.1X frame:
 * version, type, length, body); it may produce a reply to send back. */
enum { WPA_IGNORE = 0, WPA_REPLY = 1, WPA_KEYS = 2, WPA_GROUP = 3, WPA_BAD = -1 };
struct wpa_sup {
    uint8_t pmk[32], aa[6], spa[6], anonce[32], snonce[32];
    uint8_t ptk[48];                       /* KCK 0-15, KEK 16-31, TK 32-47 */
    uint8_t gtk[32]; int gtk_len, gtk_idx;
    uint8_t ie[64]; int ie_len;            /* our RSN element, exactly as in the association request */
    uint64_t replay; uint8_t replay_ok, have_ptk;   /* (not bool: the kernel and the library define it differently) */
    const char *why;                       /* the last problem, for the report */
};
void wpa_sup_init(struct wpa_sup *s, const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6], const uint8_t *ie, int ie_len, const uint8_t snonce[32]);
int  wpa_sup_rx(struct wpa_sup *s, const uint8_t *eapol, int len, uint8_t *reply, int *rlen);
