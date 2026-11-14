#include "tls.h"
#include "../../../common/str.h"

#define CT_CCS 20
#define CT_ALERT 21
#define CT_HS 22
#define CT_APP 23

static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put24(uint8_t *p, uint32_t v) { p[0] = v >> 16; p[1] = v >> 8; p[2] = v; }
static uint16_t get16(const uint8_t *p) { return p[0] << 8 | p[1]; }
static uint32_t get24(const uint8_t *p) { return (uint32_t)p[0] << 16 | p[1] << 8 | p[2]; }

/* ---------------- key schedule ---------------- */
static void expand_label(const uint8_t *secret, const char *label, const uint8_t *ctx, size_t clen, uint8_t *out, size_t olen) {
    uint8_t info[2 + 1 + 6 + 32 + 1 + 64];
    size_t ll = strlen(label), n = 0;
    put16(info, (uint16_t)olen); n = 2;
    info[n++] = (uint8_t)(6 + ll);
    memcpy(info + n, "tls13 ", 6); n += 6;
    memcpy(info + n, label, ll); n += ll;
    info[n++] = (uint8_t)clen;
    if (clen) memcpy(info + n, ctx, clen);
    n += clen;
    hkdf_expand(H_SHA256, secret, info, n, out, olen);
}
static void transcript(struct tls *t, uint8_t out[32]) { hash_ctx c = t->th; hash_final(&c, out); }
static void derive(const uint8_t *secret, const char *label, const uint8_t th[32], uint8_t out[32]) { expand_label(secret, label, th, 32, out, 32); }

static void set_keys(struct tls *t, int client, const uint8_t secret[32]) {
    int kl = t->suite == 0x1301 ? 16 : 32;
    uint8_t *key = client ? t->c_key : t->s_key, *iv = client ? t->c_iv : t->s_iv;
    expand_label(secret, "key", 0, 0, key, kl);
    expand_label(secret, "iv", 0, 0, iv, 12);
    if (t->suite == 0x1301) aes128gcm_init(client ? &t->c_gcm : &t->s_gcm, key);
    if (client) t->c_seq = 0; else t->s_seq = 0;
}

/* ---------------- records ---------------- */
static int io_send(struct tls *t, const uint8_t *b, size_t n) {
    while (n) { long r = t->io.send(t->io.ctx, b, n); if (r <= 0) return TLS_EIO; b += r; n -= r; }
    return 0;
}
/* Read exactly n bytes. A timeout (-9) is only reported if nothing has arrived yet;
 * once data starts flowing the rest of it is waited for. */
static int io_recv(struct tls *t, uint8_t *b, size_t n, long timeout) {
    int got_any = 0;
    while (n) {
        long r = t->io.recv(t->io.ctx, b, n, got_any ? 15000 : timeout);
        if (r == 0) return TLS_ECLOSED;
        if (r < 0) return (r == -9 && !got_any) ? -9 : TLS_EIO;      /* -9 = NET_EAGAIN (timeout) */
        got_any = 1;
        b += r; n -= r;
    }
    return 0;
}

static void nonce(const uint8_t iv[12], uint64_t seq, uint8_t out[12]) {
    memcpy(out, iv, 12);
    for (int i = 0; i < 8; i++) out[4 + i] ^= (uint8_t)(seq >> (56 - 8 * i));
}

static int send_record(struct tls *t, int type, const uint8_t *data, size_t len, int encrypt) {
    static uint8_t buf[16384 + 256];
    if (len > 16384) return TLS_EPROTO;
    if (!encrypt) {
        buf[0] = type; buf[1] = 3; buf[2] = type == CT_HS ? 1 : 3; put16(buf + 3, (uint16_t)len);
        memcpy(buf + 5, data, len);
        return io_send(t, buf, 5 + len);
    }
    size_t clen = len + 1 + 16;
    buf[0] = CT_APP; buf[1] = 3; buf[2] = 3; put16(buf + 3, (uint16_t)clen);
    memcpy(buf + 5, data, len); buf[5 + len] = (uint8_t)type;
    uint8_t n[12]; nonce(t->c_iv, t->c_seq++, n);
    if (t->suite == 0x1301) aes128gcm_seal(&t->c_gcm, n, buf, 5, buf + 5, len + 1, buf + 5 + len + 1);
    else chacha20poly1305_seal(t->c_key, n, buf, 5, buf + 5, len + 1, buf + 5 + len + 1);
    return io_send(t, buf, 5 + clen);
}

static void send_alert(struct tls *t, int desc, int encrypted) {
    uint8_t a[2] = { desc == 0 ? 1 : 2, (uint8_t)desc };
    send_record(t, CT_ALERT, a, 2, encrypted);
}

/* Read one record. Encrypted records are decrypted in place; *type is the real content type. */
static int read_record(struct tls *t, int *type, uint8_t **data, size_t *len, int decrypt, long timeout) {
    for (;;) {
        uint8_t h[5];
        int r = io_recv(t, h, 5, timeout);
        if (r) return r;
        size_t l = get16(h + 3);
        if (l > sizeof t->rec || (h[1] != 3)) return TLS_EPROTO;
        if ((r = io_recv(t, t->rec, l, 15000))) return r;
        if (h[0] == CT_CCS) continue;                            /* compatibility ChangeCipherSpec: ignore */
        if (h[0] == CT_ALERT && l >= 2) { t->alert = t->rec[1]; return t->rec[1] == 0 ? TLS_ECLOSED : TLS_EALERT; }
        if (!decrypt || h[0] != CT_APP) { *type = h[0]; *data = t->rec; *len = l; return 0; }
        if (l < 17) return TLS_EDECRYPT;
        uint8_t n[12]; nonce(t->s_iv, t->s_seq++, n);
        int bad = t->suite == 0x1301 ? aes128gcm_open(&t->s_gcm, n, h, 5, t->rec, l - 16, t->rec + l - 16)
                                     : chacha20poly1305_open(t->s_key, n, h, 5, t->rec, l - 16, t->rec + l - 16);
        if (bad) return TLS_EDECRYPT;
        size_t pl = l - 16;
        while (pl && !t->rec[pl - 1]) pl--;                      /* strip padding */
        if (!pl) return TLS_EPROTO;
        *type = t->rec[pl - 1]; *data = t->rec; *len = pl - 1;
        if (*type == CT_ALERT && *len >= 2) { t->alert = t->rec[1]; return t->rec[1] == 0 ? TLS_ECLOSED : TLS_EALERT; }
        return 0;
    }
}

/* Get the next complete handshake message (type, body) during the handshake. */
static int next_hs(struct tls *t, int decrypt, uint8_t *mtype, uint8_t **body, size_t *blen, uint8_t **whole, size_t *wlen) {
    if (t->hs_used) { memmove(t->hs, t->hs + t->hs_used, t->hs_len - t->hs_used); t->hs_len -= t->hs_used; t->hs_used = 0; }
    while (t->hs_len < 4 || t->hs_len < 4 + get24(t->hs + 1)) {
        int type; uint8_t *d; size_t l;
        int r = read_record(t, &type, &d, &l, decrypt, 15000);
        if (r) return r;
        if (type != CT_HS) return TLS_EPROTO;
        if (t->hs_len + l > sizeof t->hs) return TLS_EPROTO;
        memcpy(t->hs + t->hs_len, d, l); t->hs_len += l;
    }
    size_t ml = get24(t->hs + 1);
    *mtype = t->hs[0]; *body = t->hs + 4; *blen = ml; *whole = t->hs; *wlen = 4 + ml;
    t->hs_used = 4 + ml;
    return 0;
}

/* ---------------- ClientHello ---------------- */
static const uint8_t HRR_RANDOM[32] = { 0xCF,0x21,0xAD,0x74,0xE5,0x9A,0x61,0x11,0xBE,0x1D,0x8C,0x02,0x1E,0x65,0xB8,0x91,
                                        0xC2,0xA2,0x11,0x16,0x7A,0xBB,0x8C,0x5E,0x07,0x9E,0x09,0xE2,0xC8,0xA8,0x33,0x9C };

static size_t build_hello(struct tls *t, uint8_t *m, const uint8_t *random, const uint8_t *sid, uint16_t group, const uint8_t *pub, size_t publen,
                          const uint8_t *cookie, size_t cookie_len) {
    size_t n = 4;
    m[n++] = 3; m[n++] = 3;
    memcpy(m + n, random, 32); n += 32;
    m[n++] = 32; memcpy(m + n, sid, 32); n += 32;
    put16(m + n, 4); n += 2; put16(m + n, 0x1301); put16(m + n + 2, 0x1303); n += 4;
    m[n++] = 1; m[n++] = 0;
    size_t ext_at = n; n += 2;
    /* server_name (not for IP addresses) */
    int is_ip = 1; for (const char *s = t->host; *s; s++) if (!((*s >= '0' && *s <= '9') || *s == '.')) is_ip = 0;
    size_t hl = strlen(t->host);
    if (!is_ip) {
        put16(m + n, 0); put16(m + n + 2, hl + 5); put16(m + n + 4, hl + 3); m[n + 6] = 0; put16(m + n + 7, hl);
        memcpy(m + n + 9, t->host, hl); n += 9 + hl;
    }
    static const uint8_t groups[] = { 0x00,0x0a, 0x00,0x06, 0x00,0x04, 0x00,0x1d, 0x00,0x17 };
    memcpy(m + n, groups, sizeof groups); n += sizeof groups;
    static const uint8_t sigalgs[] = { 0x00,0x0d, 0x00,0x12, 0x00,0x10, 0x04,0x03, 0x05,0x03, 0x08,0x04, 0x08,0x05, 0x08,0x06, 0x04,0x01, 0x05,0x01, 0x06,0x01 };
    memcpy(m + n, sigalgs, sizeof sigalgs); n += sizeof sigalgs;
    static const uint8_t versions[] = { 0x00,0x2b, 0x00,0x03, 0x02, 0x03,0x04 };
    memcpy(m + n, versions, sizeof versions); n += sizeof versions;
    static const uint8_t pskmodes[] = { 0x00,0x2d, 0x00,0x02, 0x01, 0x01 };
    memcpy(m + n, pskmodes, sizeof pskmodes); n += sizeof pskmodes;
    static const uint8_t alpn[] = { 0x00,0x10, 0x00,0x0b, 0x00,0x09, 0x08, 'h','t','t','p','/','1','.','1' };
    memcpy(m + n, alpn, sizeof alpn); n += sizeof alpn;
    if (cookie_len) { put16(m + n, 44); put16(m + n + 2, cookie_len); memcpy(m + n + 4, cookie, cookie_len); n += 4 + cookie_len; }
    put16(m + n, 51); put16(m + n + 2, publen + 6); put16(m + n + 4, publen + 4); put16(m + n + 6, group); put16(m + n + 8, publen);
    memcpy(m + n + 10, pub, publen); n += 10 + publen;
    put16(m + ext_at, n - ext_at - 2);
    m[0] = 1; put24(m + 1, n - 4);
    return n;
}

/* ---------------- handshake ---------------- */
static int fail(struct tls *t, int e, int alert, int encrypted) { if (alert >= 0) send_alert(t, alert, encrypted); t->err = e; return e; }

static int verify_cert_verify(struct tls *t, const uint8_t *b, size_t bl, const uint8_t th[32]) {
    if (bl < 4) return TLS_EPROTO;
    uint16_t scheme = get16(b), sl = get16(b + 2);
    if (4 + (size_t)sl > bl) return TLS_EPROTO;
    const uint8_t *sig = b + 4;
    uint8_t content[64 + 34 + 32];
    memset(content, 0x20, 64);
    memcpy(content + 64, "TLS 1.3, server CertificateVerify", 33);
    content[97] = 0;
    memcpy(content + 98, th, 32);
    struct x509 *leaf = &t->chain[0];
    uint8_t h[64];
    switch (scheme) {
    case 0x0403: case 0x0503: {
        int ha = scheme == 0x0403 ? H_SHA256 : H_SHA384, curve = scheme == 0x0403 ? KEY_EC_P256 : KEY_EC_P384;
        if (leaf->key_type != curve) return TLS_ESIG;
        hash(ha, content, sizeof content, h);
        return ecdsa_verify(curve == KEY_EC_P256 ? CURVE_P256 : CURVE_P384, leaf->ec_pub, leaf->ec_pub_len, h, hash_len(ha), sig, sl) ? TLS_ESIG : 0;
    }
    case 0x0804: case 0x0805: case 0x0806: {
        int ha = scheme == 0x0804 ? H_SHA256 : scheme == 0x0805 ? H_SHA384 : H_SHA512;
        if (leaf->key_type != KEY_RSA) return TLS_ESIG;
        hash(ha, content, sizeof content, h);
        return rsa_verify_pss(leaf->rsa_n, leaf->rsa_n_len, leaf->rsa_e, leaf->rsa_e_len, ha, h, sig, sl) ? TLS_ESIG : 0;
    }
    }
    return TLS_ESIG;
}

int tls_connect(struct tls *t, const struct tls_io *io, const char *host) {
    memset(t, 0, sizeof *t);
    t->io = *io;
    strncpy0(t->host, host, sizeof t->host);
    uint8_t random[32], sid[32], xpriv[32], xpub[32], ppriv[32], ppub[65];
    io->random(random, 32); io->random(sid, 32); io->random(xpriv, 32);
    x25519_base(xpub, xpriv);
    static uint8_t ch[2048];
    size_t chl = build_hello(t, ch, random, sid, 0x001d, xpub, 32, 0, 0);
    hash_init(&t->th, H_SHA256);
    if (send_record(t, CT_HS, ch, chl, 0)) return fail(t, TLS_EIO, -1, 0);
    hash_update(&t->th, ch, chl);
    uint8_t mt, *b, *w; size_t bl, wl;
    int r, retried = 0;
    uint16_t group = 0x001d;
    const uint8_t *spub = 0; size_t spub_len = 0;
    for (;;) {
        if ((r = next_hs(t, 0, &mt, &b, &bl, &w, &wl))) return fail(t, r == TLS_EALERT && t->alert == 70 ? TLS_EVERSION : r, -1, 0);
        if (mt != 2 || bl < 38) return fail(t, TLS_EPROTO, 10, 0);
        /* ServerHello */
        const uint8_t *p = b + 2 + 32;
        size_t sidl = *p++;
        p += sidl;
        if (p + 3 > b + bl) return fail(t, TLS_EPROTO, 50, 0);
        uint16_t suite = get16(p); p += 2;
        p++;   /* compression */
        int is_hrr = !memcmp(b + 2, HRR_RANDOM, 32);
        uint16_t version = 0, sel_group = 0; const uint8_t *cookie = 0; size_t cookie_len = 0;
        if (p + 2 <= b + bl) {
            const uint8_t *e = p + 2, *ee = e + get16(p);
            if (ee > b + bl) return fail(t, TLS_EPROTO, 50, 0);
            while (e + 4 <= ee) {
                uint16_t et = get16(e), el = get16(e + 2);
                const uint8_t *ev = e + 4;
                if (ev + el > ee) return fail(t, TLS_EPROTO, 50, 0);
                if (et == 43 && el == 2) version = get16(ev);
                if (et == 51 && el >= 2) {
                    sel_group = get16(ev);
                    if (!is_hrr && el >= 4) { spub_len = get16(ev + 2); spub = ev + 4; if (4 + spub_len > el) return fail(t, TLS_EPROTO, 50, 0); }
                }
                if (et == 44 && el >= 2) { cookie_len = get16(ev); cookie = ev + 2; }
                e = ev + el;
            }
        }
        if (version != 0x0304) return fail(t, TLS_EVERSION, 70, 0);
        if (suite != 0x1301 && suite != 0x1303) return fail(t, TLS_EPROTO, 47, 0);
        t->suite = suite;
        if (is_hrr) {
            if (retried || (sel_group != 0x0017 && sel_group != 0x001d) || sel_group == group) return fail(t, TLS_EGROUP, 47, 0);
            retried = 1; group = sel_group;
            /* transcript: message_hash(ClientHello1) + HelloRetryRequest */
            uint8_t h1[32], mh[4 + 32];
            transcript(t, h1);
            mh[0] = 254; put24(mh + 1, 32); memcpy(mh + 4, h1, 32);
            hash_init(&t->th, H_SHA256); hash_update(&t->th, mh, sizeof mh); hash_update(&t->th, w, wl);
            uint8_t ck[512]; size_t ckl = cookie_len < sizeof ck ? cookie_len : 0;
            if (ckl) memcpy(ck, cookie, ckl);
            do io->random(ppriv, 32); while (p256_keygen(ppub, ppriv));
            chl = group == 0x0017 ? build_hello(t, ch, random, sid, 0x0017, ppub, 65, ck, ckl) : build_hello(t, ch, random, sid, 0x001d, xpub, 32, ck, ckl);
            if (send_record(t, CT_HS, ch, chl, 0)) return fail(t, TLS_EIO, -1, 0);
            hash_update(&t->th, ch, chl);
            continue;
        }
        if (sel_group != group || !spub) return fail(t, TLS_EPROTO, 47, 0);
        hash_update(&t->th, w, wl);
        break;
    }
    t->group = group;
    /* shared secret */
    uint8_t shared[32], zero[32] = { 0 };
    if (group == 0x001d) {
        if (spub_len != 32) return fail(t, TLS_EPROTO, 47, 0);
        x25519(shared, xpriv, spub);
        if (!ct_memcmp(shared, zero, 32)) return fail(t, TLS_EPROTO, 47, 0);
    } else if (p256_ecdh(shared, ppriv, spub, spub_len)) return fail(t, TLS_EPROTO, 47, 0);
    secure_zero(xpriv, 32); secure_zero(ppriv, 32);
    /* handshake secrets */
    uint8_t early[32], empty_hash[32], derived[32], th[32];
    hkdf_extract(H_SHA256, 0, 0, zero, 32, early);
    hash(H_SHA256, "", 0, empty_hash);
    derive(early, "derived", empty_hash, derived);
    hkdf_extract(H_SHA256, derived, 32, shared, 32, t->hs_secret);
    secure_zero(shared, 32);
    transcript(t, th);
    derive(t->hs_secret, "c hs traffic", th, t->c_hs);
    derive(t->hs_secret, "s hs traffic", th, t->s_hs);
    set_keys(t, 1, t->c_hs); set_keys(t, 0, t->s_hs);
    /* encrypted handshake */
    int cert_requested = 0; uint8_t req_ctx[256]; size_t req_ctx_len = 0;
    int have_cert = 0, have_cv = 0;
    for (;;) {
        if ((r = next_hs(t, 1, &mt, &b, &bl, &w, &wl))) return fail(t, r, -1, 1);
        if (mt == 8) { hash_update(&t->th, w, wl); continue; }                  /* EncryptedExtensions */
        if (mt == 13) {                                                         /* CertificateRequest */
            cert_requested = 1; req_ctx_len = bl ? b[0] : 0;
            if (req_ctx_len + 1 > bl) return fail(t, TLS_EPROTO, 50, 1);
            memcpy(req_ctx, b + 1, req_ctx_len);
            hash_update(&t->th, w, wl); continue;
        }
        if (mt == 11) {                                                         /* Certificate */
            if (bl < 4) return fail(t, TLS_EPROTO, 50, 1);
            const uint8_t *p = b + 1 + b[0], *end = b + bl;
            if (p + 3 > end) return fail(t, TLS_EPROTO, 50, 1);
            const uint8_t *le = p + 3 + get24(p); p += 3;
            if (le > end) return fail(t, TLS_EPROTO, 50, 1);
            while (p + 3 <= le && t->nchain < 8) {
                size_t cl = get24(p); p += 3;
                if (p + cl > le || t->certs_len + cl > sizeof t->certs) return fail(t, TLS_EPROTO, 50, 1);
                memcpy(t->certs + t->certs_len, p, cl);
                if (x509_parse(&t->chain[t->nchain], t->certs + t->certs_len, cl)) return fail(t, X509_EPARSE, 42, 1);
                t->certs_len += cl; t->nchain++; p += cl;
                if (p + 2 > le) return fail(t, TLS_EPROTO, 50, 1);
                p += 2 + get16(p);
            }
            if (!t->nchain) return fail(t, TLS_EPROTO, 50, 1);
            strncpy0(t->peer, t->chain[0].cn, sizeof t->peer);
            int v = x509_verify_chain(t->chain, t->nchain, t->host, io->now, t->detail, sizeof t->detail);
            if (v) return fail(t, v, v == X509_EEXPIRED ? 45 : v == X509_EUNTRUSTED ? 48 : 42, 1);
            strncpy0(t->issuer, t->detail, sizeof t->issuer);
            have_cert = 1;
            hash_update(&t->th, w, wl); continue;
        }
        if (mt == 15) {                                                         /* CertificateVerify */
            if (!have_cert) return fail(t, TLS_EPROTO, 10, 1);
            transcript(t, th);
            if ((r = verify_cert_verify(t, b, bl, th))) return fail(t, r, 51, 1);
            have_cv = 1;
            hash_update(&t->th, w, wl); continue;
        }
        if (mt == 20) {                                                         /* Finished */
            if (!have_cert || !have_cv) return fail(t, TLS_EPROTO, 10, 1);
            uint8_t fk[32], want[32];
            transcript(t, th);
            expand_label(t->s_hs, "finished", 0, 0, fk, 32);
            hmac(H_SHA256, fk, 32, th, 32, want);
            if (bl != 32 || ct_memcmp(want, b, 32)) return fail(t, TLS_EFINISHED, 51, 1);
            hash_update(&t->th, w, wl);
            break;
        }
        return fail(t, TLS_EPROTO, 10, 1);
    }
    /* application secrets come from the transcript up to the server's Finished */
    uint8_t master[32];
    derive(t->hs_secret, "derived", empty_hash, derived);
    hkdf_extract(H_SHA256, derived, 32, zero, 32, master);
    transcript(t, th);
    derive(master, "c ap traffic", th, t->c_ap);
    derive(master, "s ap traffic", th, t->s_ap);
    /* our reply: (empty Certificate), Finished */
    static const uint8_t ccs[6] = { 20, 3, 3, 0, 1, 1 };
    io_send(t, ccs, 6);
    if (cert_requested) {
        uint8_t m[4 + 1 + 255 + 3];
        size_t n = 4; m[n++] = (uint8_t)req_ctx_len; memcpy(m + n, req_ctx, req_ctx_len); n += req_ctx_len; put24(m + n, 0); n += 3;
        m[0] = 11; put24(m + 1, n - 4);
        if (send_record(t, CT_HS, m, n, 1)) return fail(t, TLS_EIO, -1, 0);
        hash_update(&t->th, m, n);
    }
    uint8_t fk[32], fin[36];
    transcript(t, th);
    expand_label(t->c_hs, "finished", 0, 0, fk, 32);
    fin[0] = 20; put24(fin + 1, 32); hmac(H_SHA256, fk, 32, th, 32, fin + 4);
    if (send_record(t, CT_HS, fin, 36, 1)) return fail(t, TLS_EIO, -1, 0);
    set_keys(t, 1, t->c_ap); set_keys(t, 0, t->s_ap);
    ksnprintf(t->cipher, sizeof t->cipher, "TLS 1.3, %s, %s", t->suite == 0x1301 ? "AES-128-GCM" : "ChaCha20-Poly1305", group == 0x1d ? "X25519" : "P-256");
    t->hs_len = 0;
    return TLS_OK;
}

long tls_write(struct tls *t, const void *buf, size_t n) {
    if (t->err) return t->err;
    size_t done = 0;
    while (done < n) {
        size_t k = n - done > 16000 ? 16000 : n - done;
        if (send_record(t, CT_APP, (const uint8_t *)buf + done, k, 1)) return t->err = TLS_EIO;
        done += k;
    }
    return (long)done;
}

static void key_update(uint8_t secret[32]) { uint8_t n[32]; expand_label(secret, "traffic upd", 0, 0, n, 32); memcpy(secret, n, 32); }

long tls_read(struct tls *t, void *buf, size_t n, long timeout_ms) {
    while (!t->plen) {
        if (t->closed) return 0;
        if (t->err) return t->err;
        int type; uint8_t *d; size_t l;
        int r = read_record(t, &type, &d, &l, 1, timeout_ms);
        if (r == -9) return -9;                                   /* timeout: nothing yet */
        if (r == TLS_ECLOSED) { t->closed = 1; return 0; }
        if (r) return t->err = r;
        if (type == CT_APP) { memcpy(t->plain, d, l); t->poff = 0; t->plen = l; }
        else if (type == CT_HS) {
            /* post-handshake messages: NewSessionTicket (ignored), KeyUpdate */
            for (size_t o = 0; o + 4 <= l;) {
                size_t ml = get24(d + o + 1);
                if (o + 4 + ml > l) break;
                if (d[o] == 24 && ml == 1) {
                    key_update(t->s_ap); set_keys(t, 0, t->s_ap);
                    if (d[o + 4] == 1) {
                        uint8_t ku[5] = { 24, 0, 0, 1, 0 };
                        send_record(t, CT_HS, ku, 5, 1);
                        key_update(t->c_ap); set_keys(t, 1, t->c_ap);
                    }
                }
                o += 4 + ml;
            }
        }
    }
    size_t k = n < t->plen ? n : t->plen;
    memcpy(buf, t->plain + t->poff, k);
    t->poff += k; t->plen -= k;
    return (long)k;
}

void tls_close(struct tls *t) {
    if (!t->err && !t->closed) send_alert(t, 0, 1);
    secure_zero(t->c_key, 32); secure_zero(t->s_key, 32); secure_zero(t->c_ap, 32); secure_zero(t->s_ap, 32);
    secure_zero(t->hs_secret, 32); secure_zero(&t->c_gcm, sizeof t->c_gcm); secure_zero(&t->s_gcm, sizeof t->s_gcm);
}

const char *tls_strerror(const struct tls *t, int e) {
    static char buf[200];
    switch (e) {
    case TLS_EIO: return "the connection failed";
    case TLS_EPROTO: return "the server sent something unexpected";
    case TLS_EALERT: ksnprintf(buf, sizeof buf, "the server refused the secure connection (alert %d)", t ? t->alert : 0); return buf;
    case TLS_EVERSION: return "the server doesn't support TLS 1.3";
    case TLS_EDECRYPT: return "a message failed its integrity check";
    case TLS_ESIG: return "the server couldn't prove it owns its certificate";
    case TLS_EFINISHED: return "the handshake check failed";
    case TLS_EGROUP: return "no shared key-exchange method";
    case TLS_ECLOSED: return "the server closed the connection";
    case X509_EHOST: ksnprintf(buf, sizeof buf, "the certificate is for '%s', not this site", t ? t->detail : "?"); return buf;
    }
    if (e <= X509_EPARSE && e >= X509_EUSAGE) return x509_strerror(e);
    return "secure connection error";
}
