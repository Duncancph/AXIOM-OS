/* TLS 1.3 client (RFC 8446). Ciphers: AES-128-GCM-SHA256, ChaCha20-Poly1305-SHA256.
 * Key exchange: X25519 (P-256 if the server asks). Certificates are always verified. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "crypto.h"
#include "x509.h"

struct tls_io {
    void *ctx;
    long (*send)(void *ctx, const void *buf, size_t n);                 /* bytes sent or <0 */
    long (*recv)(void *ctx, void *buf, size_t n, long timeout_ms);      /* bytes, 0 = closed, <0 error/timeout */
    void (*random)(void *buf, size_t n);
    int64_t now;                                                       /* unix time, for certificate dates */
};

enum { TLS_OK = 0, TLS_EIO = -60, TLS_EPROTO = -61, TLS_EALERT = -62, TLS_EVERSION = -63, TLS_EDECRYPT = -64,
       TLS_ESIG = -65, TLS_EFINISHED = -66, TLS_EGROUP = -67, TLS_ECLOSED = -68 };

struct tls {
    struct tls_io io;
    char host[256];
    int err, closed, alert;
    uint16_t suite, group;
    hash_ctx th;                                 /* transcript hash */
    uint8_t hs_secret[32], c_hs[32], s_hs[32], c_ap[32], s_ap[32];
    uint8_t c_key[32], s_key[32], c_iv[12], s_iv[12];
    uint64_t c_seq, s_seq;
    aes128gcm c_gcm, s_gcm;
    uint8_t rec[16384 + 512];                    /* one received record */
    uint8_t plain[16384 + 512]; size_t poff, plen;   /* decrypted application data not yet read */
    uint8_t hs[24576]; size_t hs_len, hs_used;   /* handshake messages being reassembled */
    uint8_t certs[16384]; size_t certs_len;      /* the server's certificates (copied) */
    struct x509 chain[8]; int nchain;
    char peer[96], issuer[96], detail[96];
    char cipher[48];
};

int  tls_connect(struct tls *t, const struct tls_io *io, const char *host);
long tls_write(struct tls *t, const void *buf, size_t n);
long tls_read(struct tls *t, void *buf, size_t n, long timeout_ms);
void tls_close(struct tls *t);
const char *tls_strerror(const struct tls *t, int e);
