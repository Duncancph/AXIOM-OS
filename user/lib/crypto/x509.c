#include "x509.h"
#include "crypto.h"
#include "../../../common/str.h"
#include "../../../common/inet.h"

void *malloc(size_t n);

/* ---------------- DER ---------------- */
static int der(const uint8_t **p, const uint8_t *end, uint8_t *tag, const uint8_t **val, size_t *vlen) {
    if (*p + 2 > end) return -1;
    *tag = (*p)[0];
    size_t len = (*p)[1], hl = 2;
    if (len & 0x80) {
        int nb = len & 0x7F;
        if (nb < 1 || nb > 4 || *p + 2 + nb > end) return -1;
        len = 0;
        for (int i = 0; i < nb; i++) len = len << 8 | (*p)[2 + i];
        hl += nb;
    }
    if (len > (size_t)(end - *p - hl)) return -1;
    *val = *p + hl; *vlen = len;
    *p += hl + len;
    return 0;
}
/* like der() but returns the whole element (header included) */
static int der_raw(const uint8_t **p, const uint8_t *end, uint8_t *tag, const uint8_t **raw, size_t *rlen, const uint8_t **val, size_t *vlen) {
    const uint8_t *s = *p;
    if (der(p, end, tag, val, vlen)) return -1;
    *raw = s; *rlen = *p - s;
    return 0;
}

static int oid_is(const uint8_t *v, size_t l, const uint8_t *o, size_t ol) { return l == ol && !memcmp(v, o, l); }
#define OID(v, l, ...) ({ static const uint8_t _o[] = { __VA_ARGS__ }; oid_is(v, l, _o, sizeof _o); })

/* ---------------- time ---------------- */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
static int num(const uint8_t *p, int n) { int v = 0; for (int i = 0; i < n; i++) { if (p[i] < '0' || p[i] > '9') return -1; v = v * 10 + p[i] - '0'; } return v; }
static int parse_time(uint8_t tag, const uint8_t *v, size_t l, int64_t *out) {
    int y, o;
    if (tag == 0x17 && l >= 13) { y = num(v, 2); y += y < 50 ? 2000 : 1900; o = 2; }
    else if (tag == 0x18 && l >= 15) { y = num(v, 4); o = 4; }
    else return -1;
    int mo = num(v + o, 2), d = num(v + o + 2, 2), h = num(v + o + 4, 2), mi = num(v + o + 6, 2), s = num(v + o + 8, 2);
    if (y < 0 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || mi < 0 || s < 0) return -1;
    *out = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
    return 0;
}

/* ---------------- certificate ---------------- */
static void find_cn(const uint8_t *name, size_t len, char *out, size_t n) {
    const uint8_t *p = name, *end = name + len, *v; size_t vl; uint8_t t;
    out[0] = 0;
    while (p < end && !der(&p, end, &t, &v, &vl)) {            /* SET */
        const uint8_t *q = v, *qe = v + vl, *a; size_t al;
        while (q < qe && !der(&q, qe, &t, &a, &al)) {           /* SEQUENCE { oid, value } */
            const uint8_t *r = a, *re = a + al, *ov, *sv; size_t ol, sl; uint8_t ot, st;
            if (der(&r, re, &ot, &ov, &ol) || der(&r, re, &st, &sv, &sl)) continue;
            if (OID(ov, ol, 0x55, 0x04, 0x03)) {
                size_t k = sl < n - 1 ? sl : n - 1;
                for (size_t i = 0; i < k; i++) out[i] = sv[i] >= 32 && sv[i] < 127 ? sv[i] : '?';
                out[k] = 0;
            }
        }
    }
}

int x509_sig_hash(int s) {
    return s == SIG_RSA_SHA384 || s == SIG_ECDSA_SHA384 ? H_SHA384 : s == SIG_RSA_SHA512 || s == SIG_ECDSA_SHA512 ? H_SHA512 : H_SHA256;
}

int x509_parse(struct x509 *c, const uint8_t *d, size_t len) {
    memset(c, 0, sizeof *c);
    c->der = d; c->len = len; c->path_len = -1; c->key_cert_sign = 1; c->server_auth = 1;
    const uint8_t *p = d, *end = d + len, *v; size_t vl; uint8_t t;
    if (der(&p, end, &t, &v, &vl) || t != 0x30) return X509_EPARSE;
    const uint8_t *cp = v, *ce = v + vl;
    if (der_raw(&cp, ce, &t, &c->tbs, &c->tbs_len, &v, &vl) || t != 0x30) return X509_EPARSE;
    const uint8_t *tp = v, *te = v + vl;
    /* signatureAlgorithm + signature */
    const uint8_t *sa, *sv; size_t sal, svl;
    if (der(&cp, ce, &t, &sa, &sal) || t != 0x30) return X509_EPARSE;
    { const uint8_t *q = sa, *ov; size_t ol; if (der(&q, sa + sal, &t, &ov, &ol) || t != 0x06) return X509_EPARSE;
      if (OID(ov, ol, 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0b)) c->sig_alg = SIG_RSA_SHA256;
      else if (OID(ov, ol, 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0c)) c->sig_alg = SIG_RSA_SHA384;
      else if (OID(ov, ol, 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0d)) c->sig_alg = SIG_RSA_SHA512;
      else if (OID(ov, ol, 0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x02)) c->sig_alg = SIG_ECDSA_SHA256;
      else if (OID(ov, ol, 0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x03)) c->sig_alg = SIG_ECDSA_SHA384;
      else if (OID(ov, ol, 0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x04)) c->sig_alg = SIG_ECDSA_SHA512;
      else c->sig_alg = SIG_UNSUPPORTED; }
    if (der(&cp, ce, &t, &sv, &svl) || t != 0x03 || svl < 2 || sv[0]) return X509_EPARSE;
    c->sig = sv + 1; c->sig_len = svl - 1;
    /* TBS */
    if (tp < te && tp[0] == 0xA0) { if (der(&tp, te, &t, &v, &vl)) return X509_EPARSE; }   /* version */
    if (der(&tp, te, &t, &v, &vl) || t != 0x02) return X509_EPARSE;                       /* serial */
    if (der(&tp, te, &t, &v, &vl) || t != 0x30) return X509_EPARSE;                       /* signature alg */
    if (der_raw(&tp, te, &t, &c->issuer, &c->issuer_len, &v, &vl) || t != 0x30) return X509_EPARSE;
    if (der(&tp, te, &t, &v, &vl) || t != 0x30) return X509_EPARSE;                       /* validity */
    { const uint8_t *q = v, *qe = v + vl, *tv; size_t tl; uint8_t tt;
      if (der(&q, qe, &tt, &tv, &tl) || parse_time(tt, tv, tl, &c->not_before)) return X509_EPARSE;
      if (der(&q, qe, &tt, &tv, &tl) || parse_time(tt, tv, tl, &c->not_after)) return X509_EPARSE; }
    if (der_raw(&tp, te, &t, &c->subject, &c->subject_len, &v, &vl) || t != 0x30) return X509_EPARSE;
    find_cn(v, vl, c->cn, sizeof c->cn);
    /* subjectPublicKeyInfo */
    if (der(&tp, te, &t, &v, &vl) || t != 0x30) return X509_EPARSE;
    { const uint8_t *q = v, *qe = v + vl, *alg, *kb; size_t algl, kbl;
      if (der(&q, qe, &t, &alg, &algl) || t != 0x30 || der(&q, qe, &t, &kb, &kbl) || t != 0x03 || kbl < 2 || kb[0]) return X509_EPARSE;
      kb++; kbl--;
      const uint8_t *a = alg, *ae = alg + algl, *ov, *pv; size_t ol, pl = 0; pv = 0;
      if (der(&a, ae, &t, &ov, &ol)) return X509_EPARSE;
      if (a < ae) der(&a, ae, &t, &pv, &pl);
      if (OID(ov, ol, 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01)) {
          const uint8_t *k = kb, *ke = kb + kbl, *s; size_t sl;
          if (der(&k, ke, &t, &s, &sl) || t != 0x30) return X509_EPARSE;
          const uint8_t *r = s, *re = s + sl;
          if (der(&r, re, &t, &c->rsa_n, &c->rsa_n_len) || t != 0x02 || der(&r, re, &t, &c->rsa_e, &c->rsa_e_len) || t != 0x02) return X509_EPARSE;
          while (c->rsa_n_len > 1 && !c->rsa_n[0]) { c->rsa_n++; c->rsa_n_len--; }
          c->key_type = KEY_RSA;
      } else if (OID(ov, ol, 0x2a,0x86,0x48,0xce,0x3d,0x02,0x01) && pv) {
          if (OID(pv, pl, 0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07)) c->key_type = KEY_EC_P256;
          else if (OID(pv, pl, 0x2b,0x81,0x04,0x00,0x22)) c->key_type = KEY_EC_P384;
          c->ec_pub = kb; c->ec_pub_len = kbl;
      }
    }
    /* extensions */
    while (tp < te) {
        if (der(&tp, te, &t, &v, &vl)) return X509_EPARSE;
        if (t != 0xA3) continue;
        const uint8_t *q = v, *qe = v + vl, *ex; size_t exl;
        if (der(&q, qe, &t, &ex, &exl) || t != 0x30) return X509_EPARSE;
        const uint8_t *x = ex, *xe = ex + exl;
        while (x < xe) {
            const uint8_t *e, *ov, *val; size_t el, ol, vl2; int critical = 0;
            if (der(&x, xe, &t, &e, &el) || t != 0x30) return X509_EPARSE;
            const uint8_t *r = e, *re = e + el;
            if (der(&r, re, &t, &ov, &ol) || t != 0x06) return X509_EPARSE;
            if (r < re && r[0] == 0x01) { const uint8_t *bv; size_t bl; der(&r, re, &t, &bv, &bl); critical = bl && bv[0]; }
            if (der(&r, re, &t, &val, &vl2) || t != 0x04) return X509_EPARSE;
            if (OID(ov, ol, 0x55, 0x1d, 0x13)) {                       /* basicConstraints */
                const uint8_t *s = val, *se = val + vl2, *sq; size_t sql;
                if (!der(&s, se, &t, &sq, &sql) && t == 0x30) {
                    const uint8_t *b = sq, *be = sq + sql, *bv; size_t bl;
                    while (b < be && !der(&b, be, &t, &bv, &bl)) {
                        if (t == 0x01 && bl) c->is_ca = bv[0] != 0;
                        if (t == 0x02 && bl && bl <= 2) { c->path_len = 0; for (size_t i = 0; i < bl; i++) c->path_len = c->path_len << 8 | bv[i]; }
                    }
                }
            } else if (OID(ov, ol, 0x55, 0x1d, 0x0f)) {                /* keyUsage */
                const uint8_t *s = val, *bv; size_t bl;
                if (!der(&s, val + vl2, &t, &bv, &bl) && t == 0x03 && bl >= 2) c->key_cert_sign = (bv[1] & 0x04) != 0;
            } else if (OID(ov, ol, 0x55, 0x1d, 0x11)) {                /* subjectAltName */
                const uint8_t *s = val; uint8_t st;
                der(&s, val + vl2, &st, &c->san, &c->san_len);
            } else if (OID(ov, ol, 0x55, 0x1d, 0x25)) {                /* extendedKeyUsage */
                const uint8_t *s = val, *sq; size_t sql;
                c->server_auth = 0;
                if (!der(&s, val + vl2, &t, &sq, &sql)) {
                    const uint8_t *b = sq, *be = sq + sql, *o; size_t olen;
                    while (b < be && !der(&b, be, &t, &o, &olen))
                        if (OID(o, olen, 0x2b,0x06,0x01,0x05,0x05,0x07,0x03,0x01) || OID(o, olen, 0x55,0x1d,0x25,0x00)) c->server_auth = 1;
                }
            } else if (critical && !OID(ov, ol, 0x55, 0x1d, 0x20) && !OID(ov, ol, 0x55, 0x1d, 0x24)) {
                c->bad_critical = 1;       /* an unknown critical extension (e.g. name constraints) */
            }
        }
    }
    return X509_OK;
}

int x509_check_sig(const struct x509 *c, const struct x509 *iss) {
    uint8_t h[64];
    int ha = x509_sig_hash(c->sig_alg);
    if (c->sig_alg == SIG_UNSUPPORTED || c->sig_alg == SIG_NONE || iss->key_type == KEY_NONE) return X509_EALG;
    hash(ha, c->tbs, c->tbs_len, h);
    if (c->sig_alg <= SIG_RSA_SHA512) {
        if (iss->key_type != KEY_RSA) return X509_ESIG;
        return rsa_verify_pkcs1(iss->rsa_n, iss->rsa_n_len, iss->rsa_e, iss->rsa_e_len, ha, h, c->sig, c->sig_len) ? X509_ESIG : X509_OK;
    }
    if (iss->key_type != KEY_EC_P256 && iss->key_type != KEY_EC_P384) return X509_ESIG;
    return ecdsa_verify(iss->key_type == KEY_EC_P256 ? CURVE_P256 : CURVE_P384, iss->ec_pub, iss->ec_pub_len, h, hash_len(ha), c->sig, c->sig_len) ? X509_ESIG : X509_OK;
}

/* ---------------- hostnames ---------------- */
static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int name_match(const uint8_t *pat, size_t pl, const char *host) {
    size_t hl = strlen(host);
    if (pl >= 3 && pat[0] == '*' && pat[1] == '.') {             /* *.example.com matches exactly one label */
        const char *dot = host; while (*dot && *dot != '.') dot++;
        if (!*dot || dot == host) return 0;
        size_t rest = hl - (dot - host);
        if (rest != pl - 1) return 0;
        for (size_t i = 0; i < rest; i++) if (lc(dot[i]) != lc(pat[1 + i])) return 0;
        return 1;
    }
    if (pl != hl) return 0;
    for (size_t i = 0; i < hl; i++) if (lc(pat[i]) != lc(host[i])) return 0;
    return 1;
}

int x509_match_host(const struct x509 *c, const char *host) {
    struct ipaddr ip; int is_ip = ip_parse(host, &ip), iplen = ip.v6 ? 16 : 4;
    if (c->san) {
        const uint8_t *p = c->san, *end = c->san + c->san_len, *v; size_t vl; uint8_t t;
        while (p < end && !der(&p, end, &t, &v, &vl)) {
            if (t == 0x82 && !is_ip && name_match(v, vl, host)) return 1;
            if (t == 0x87 && is_ip && vl == (size_t)iplen && !memcmp(v, ip.b, iplen)) return 1;
        }
        return 0;
    }
    return !is_ip && name_match((const uint8_t *)c->cn, strlen(c->cn), host);
}

/* ---------------- trust store ---------------- */
#define MAX_ROOTS 256
static struct x509 roots[MAX_ROOTS];
static int nroots;

static int b64v(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1; }

int x509_add_roots_pem(const char *pem, size_t len) {
    static const char B[] = "-----BEGIN CERTIFICATE-----", E[] = "-----END CERTIFICATE-----";
    int added = 0;
    for (size_t i = 0; i + sizeof B < len && nroots < MAX_ROOTS; i++) {
        if (memcmp(pem + i, B, sizeof B - 1)) continue;
        size_t s = i + sizeof B - 1, e = s;
        while (e + sizeof E - 1 <= len && memcmp(pem + e, E, sizeof E - 1)) e++;
        if (e + sizeof E - 1 > len) break;
        uint8_t *der = malloc((e - s) * 3 / 4 + 4);
        if (!der) break;
        size_t n = 0; uint32_t acc = 0; int bits = 0;
        for (size_t k = s; k < e; k++) {
            int v = b64v(pem[k]);
            if (v < 0) continue;
            acc = acc << 6 | v; bits += 6;
            if (bits >= 8) { bits -= 8; der[n++] = (uint8_t)(acc >> bits); }
        }
        if (x509_parse(&roots[nroots], der, n) == X509_OK) { nroots++; added++; }
        i = e;
    }
    return added;
}
int x509_root_count(void) { return nroots; }

const char *x509_strerror(int e) {
    switch (e) {
    case X509_EPARSE: return "the certificate couldn't be read";
    case X509_EEXPIRED: return "the certificate has expired or isn't valid yet (check the computer's clock)";
    case X509_EHOST: return "the certificate belongs to a different site";
    case X509_EUNTRUSTED: return "the certificate was issued by an authority AXIOM doesn't trust";
    case X509_ESIG: return "a certificate signature is invalid";
    case X509_EALG: return "the certificate uses an algorithm AXIOM doesn't support";
    case X509_ENOTCA: return "a certificate in the chain isn't allowed to issue certificates";
    case X509_EUSAGE: return "the certificate isn't meant for web servers";
    }
    return "the certificate couldn't be verified";
}

static int valid_at(const struct x509 *c, int64_t now) { return now + 86400 >= c->not_before && now - 86400 <= c->not_after; }  /* 1 day clock slack */
static int same_name(const uint8_t *a, size_t al, const uint8_t *b, size_t bl) { return al == bl && !memcmp(a, b, al); }

int x509_verify_chain(struct x509 *ch, int n, const char *host, int64_t now, char *detail, size_t dlen) {
    if (n < 1) return X509_EPARSE;
    struct x509 *leaf = &ch[0];
    if (detail && dlen) detail[0] = 0;
    if (!x509_match_host(leaf, host)) {
        if (detail) { size_t k = 0; const char *s = leaf->cn; while (*s && k + 1 < dlen) detail[k++] = *s++; detail[k] = 0; }
        return X509_EHOST;
    }
    if (!leaf->server_auth) return X509_EUSAGE;
    int used[16] = { 0 };
    struct x509 *cur = leaf;
    for (int depth = 0; depth < 8; depth++) {
        if (!valid_at(cur, now)) return X509_EEXPIRED;
        if (cur->bad_critical) return X509_EALG;
        /* issued directly by a trusted root? */
        int sig_err = 0;
        for (int r = 0; r < nroots; r++) {
            if (!same_name(roots[r].subject, roots[r].subject_len, cur->issuer, cur->issuer_len)) continue;
            int e = x509_check_sig(cur, &roots[r]);
            if (e == X509_OK) { if (detail) { size_t k = 0; const char *s = roots[r].cn; while (*s && k + 1 < dlen) detail[k++] = *s++; detail[k] = 0; } return X509_OK; }
            sig_err = e;
        }
        /* the presented certificate may itself be a trusted root */
        for (int r = 0; r < nroots; r++) if (roots[r].len == cur->len && !memcmp(roots[r].der, cur->der, cur->len)) return X509_OK;
        /* otherwise find its issuer among the certificates the server sent */
        struct x509 *next = 0;
        for (int i = 1; i < n && i < 16; i++) {
            if (used[i] || !same_name(ch[i].subject, ch[i].subject_len, cur->issuer, cur->issuer_len)) continue;
            int e = x509_check_sig(cur, &ch[i]);
            if (e) { sig_err = e; continue; }
            if (!ch[i].is_ca || !ch[i].key_cert_sign) return X509_ENOTCA;
            if (ch[i].path_len >= 0 && ch[i].path_len < depth) return X509_ENOTCA;
            used[i] = 1; next = &ch[i];
            break;
        }
        if (!next) return sig_err ? sig_err : X509_EUNTRUSTED;
        cur = next;
    }
    return X509_EUNTRUSTED;
}

#ifdef X509_TESTING
int x509_test_selfsig(int i, int *kt) { *kt = roots[i].key_type; return x509_check_sig(&roots[i], &roots[i]); }
#endif
