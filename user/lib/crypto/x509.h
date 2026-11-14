/* X.509 certificates: parsing, chain verification against trusted roots, hostname checks. */
#pragma once
#include <stdint.h>
#include <stddef.h>

enum { KEY_NONE, KEY_RSA, KEY_EC_P256, KEY_EC_P384 };
enum { SIG_NONE, SIG_RSA_SHA256, SIG_RSA_SHA384, SIG_RSA_SHA512, SIG_ECDSA_SHA256, SIG_ECDSA_SHA384, SIG_ECDSA_SHA512, SIG_UNSUPPORTED };

struct x509 {
    const uint8_t *der; size_t len;
    const uint8_t *tbs; size_t tbs_len;
    const uint8_t *issuer, *subject; size_t issuer_len, subject_len;
    int sig_alg; const uint8_t *sig; size_t sig_len;
    int key_type;
    const uint8_t *rsa_n, *rsa_e, *ec_pub; size_t rsa_n_len, rsa_e_len, ec_pub_len;
    int64_t not_before, not_after;
    int is_ca, path_len, key_cert_sign, server_auth, bad_critical;
    const uint8_t *san; size_t san_len;
    char cn[96];
};

enum { X509_OK = 0, X509_EPARSE = -40, X509_EEXPIRED = -41, X509_EHOST = -42, X509_EUNTRUSTED = -43, X509_ESIG = -44,
       X509_EALG = -45, X509_ENOTCA = -46, X509_EUSAGE = -47 };

int x509_parse(struct x509 *c, const uint8_t *der, size_t len);
int x509_check_sig(const struct x509 *c, const struct x509 *issuer);
int x509_match_host(const struct x509 *c, const char *host);
/* verify a chain (leaf first) for host at time now (unix seconds) against the loaded roots */
int x509_verify_chain(struct x509 *chain, int n, const char *host, int64_t now, char *detail, size_t dlen);
/* trusted roots */
int x509_add_roots_pem(const char *pem, size_t len);   /* returns number added */
int x509_root_count(void);
const char *x509_strerror(int e);
int x509_sig_hash(int sig_alg);   /* H_SHA256 etc */
