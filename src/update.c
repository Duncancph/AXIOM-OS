/* Signed system updates. Settings downloads an update; the kernel checks it here before anything is written:
 *   - the manifest must be signed with the AXIOM update key (ECDSA P-256 over SHA-256, src/update_key.h),
 *   - it must be newer than the running system (its build number),
 *   - every file must have exactly the size and SHA-256 the manifest lists.
 * Then the installer writes the new kernel, system files and boot loader to the disk AXIOM runs from.
 *
 * Manifest (text, one item per line; everything before the "signature" line is signed):
 *   AXIOM update 1
 *   version 0.2
 *   build 63
 *   notes <one line>
 *   file kernel <path> <size> <sha256 hex>        (path: where to download it, relative to the manifest;
 *   file initrd <path> <size> <sha256 hex>         gzip-compressed if it ends in .gz; size and hash are
 *   file bootx64 <path> <size> <sha256 hex>        of the uncompressed file)
 *   signature <DER signature, hex>
 */
#include "kernel.h"
#include "disk.h"
#include "update_key.h"
#include "../user/lib/crypto/crypto.h"

static const char *ROLES[UPD_FILES] = { "kernel", "initrd", "bootx64", "biosboot" };   /* biosboot is optional */

struct manifest { long build; long size[UPD_FILES]; uint8_t sha[UPD_FILES][32]; bool have[UPD_FILES]; };

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static long unhex(const char *s, const char *e, uint8_t *out, long max) {
    long n = 0;
    while (s + 1 < e && hexval(s[0]) >= 0 && hexval(s[1]) >= 0) { if (n >= max) return -1; out[n++] = (uint8_t)(hexval(s[0]) << 4 | hexval(s[1])); s += 2; }
    return n;
}
static const char *word(const char *p, const char *e, const char **we) {   /* next space-separated word */
    while (p < e && *p == ' ') p++;
    const char *s = p; while (p < e && *p != ' ' && *p != '\r') p++;
    *we = p; return s;
}
static bool is(const char *s, const char *e, const char *w) { size_t n = strlen(w); return (size_t)(e - s) == n && !memcmp(s, w, n); }
static long num(const char *s, const char *e) { long v = 0; if (s == e) return -1; for (; s < e; s++) { if (*s < '0' || *s > '9') return -1; v = v * 10 + (*s - '0'); } return v; }

/* parse and check the signature; 0 or UPD_E* */
static int check_manifest(const char *m, long n, struct manifest *out) {
    memset(out, 0, sizeof *out);
    out->build = -1;
    if (n < 20 || n > 16384 || (memcmp(m, "AXIOM update 1\n", 15) && memcmp(m, "DuncOS update 1\n", 16))) return UPD_EFORMAT;
    const char *e = m + n, *sigline = 0;
    for (const char *p = m; p < e;) {
        const char *le = p; while (le < e && *le != '\n') le++;
        const char *we, *w = word(p, le, &we);
        if (is(w, we, "build")) { const char *ve, *v = word(we, le, &ve); out->build = num(v, ve); }
        else if (is(w, we, "file")) {
            const char *re, *r = word(we, le, &re), *pe, *pth = word(re, le, &pe), *se, *sz = word(pe, le, &se), *he, *h = word(se, le, &he);
            (void)pth;
            for (int i = 0; i < UPD_FILES; i++) if (is(r, re, ROLES[i])) {
                out->size[i] = num(sz, se);
                if (out->size[i] <= 0 || unhex(h, he, out->sha[i], 32) != 32) return UPD_EFORMAT;
                out->have[i] = true;
            }
        } else if (is(w, we, "signature")) { sigline = p; break; }
        p = le + 1;
    }
    if (!sigline || out->build < 0) return UPD_EFORMAT;
    for (int i = 0; i < 3; i++) if (!out->have[i]) return UPD_EFORMAT;
    uint8_t sig[80], h[32];
    const char *we, *w = word(sigline, e, &we); (void)w;
    const char *le = we; while (le < e && *le != '\n') le++;
    const char *se, *s = word(we, le, &se);
    long sl = unhex(s, se, sig, sizeof sig);
    if (sl < 8) return UPD_EFORMAT;
    hash(H_SHA256, m, (size_t)(sigline - m), h);
    if (ecdsa_verify(CURVE_P256, update_pubkey, sizeof update_pubkey, h, 32, sig, (size_t)sl)) return UPD_ESIG;
    return 0;
}

static uint8_t *staged[UPD_FILES]; static uint64_t staged_len[UPD_FILES];

long update_syscall(int op, const void *a, long b, bool (*user_ok_fn)(const void *, size_t)) {
    if (op == UPDATE_BUILD) return OS_BUILD;
    struct manifest mf;
    if (op == UPDATE_VERIFY) {
        if (b <= 0 || b > 16384 || !user_ok_fn(a, (size_t)b)) return UPD_EFORMAT;
        int r = check_manifest(a, b, &mf);
        if (r) return r;
        return mf.build;                                    /* the update's build number */
    }
    if (op != UPDATE_APPLY) return UPD_EFORMAT;
    if (!user_ok_fn(a, sizeof(struct dunc_update))) return UPD_EFORMAT;
    struct dunc_update u = *(const struct dunc_update *)a;
    if (u.mlen <= 0 || u.mlen > 16384 || !user_ok_fn(u.manifest, (size_t)u.mlen)) return UPD_EFORMAT;
    int r = check_manifest(u.manifest, u.mlen, &mf);
    if (r) return r;
    if (mf.build <= OS_BUILD) return UPD_EOLD;
    int id = disk_system_id();
    if (id < 0) return UPD_ENODISK;
    struct dunc_install st; disk_install_status(&st);
    if (st.state == 1) return UPD_EBUSY;
    for (int i = 0; i < UPD_FILES; i++) {
        if (!mf.have[i]) { if (u.len[i]) return UPD_EHASH; continue; }
        if (u.len[i] != mf.size[i] || !user_ok_fn(u.data[i], (size_t)u.len[i])) return UPD_EHASH;
        uint8_t h[32];
        hash(H_SHA256, u.data[i], (size_t)u.len[i], h);
        if (ct_memcmp(h, mf.sha[i], 32)) return UPD_EHASH;
    }
    for (int i = 0; i < UPD_FILES; i++) {                   /* copy into the kernel, then check again: the app can't change them now */
        if (!mf.have[i]) { staged_len[i] = 0; continue; }
        size_t pages = ((size_t)u.len[i] + 4095) / 4096;
        if (staged[i]) { for (uint64_t k = 0; k < (staged_len[i] + 4095) / 4096; k++) page_free(staged[i] + k * 4096); staged[i] = 0; }
        staged[i] = page_alloc(pages);
        if (!staged[i]) return UPD_ENOMEM;
        memcpy(staged[i], u.data[i], (size_t)u.len[i]);
        staged_len[i] = (uint64_t)u.len[i];
        uint8_t h[32];
        hash(H_SHA256, staged[i], staged_len[i], h);
        if (ct_memcmp(h, mf.sha[i], 32)) return UPD_EHASH;
    }
    LOG_INFO("update: installing build %ld (this is build %d)\n", mf.build, OS_BUILD);
    const uint8_t *const d[UPD_FILES] = { staged[0], staged[1], staged[2], mf.have[3] ? staged[3] : 0 };
    r = disk_update_from(id, d, staged_len);
    return r ? UPD_EBUSY : 0;
}
