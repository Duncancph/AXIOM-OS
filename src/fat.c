/* FAT32 file system (read/write, long file names) for USB drives.
 * Every change is written straight through to the drive (no write-back cache),
 * so a drive is consistent as soon as an operation returns. */
#include "kernel.h"
#include "vfs.h"
#include "blockdev.h"

static const char *strchr_(const char *s, char c) { for (; *s; s++) if (*s == c) return s; return 0; }

#define EOC 0x0FFFFFF8u
#define MAXSLOTS 21

struct fatvol {
    struct blockdev *bd;
    uint64_t base, fat_lba, data_lba;
    uint32_t bps, spc, rsvd, nfats, fatsz, root, nclus, cbytes, fsinfo, hint;
    int64_t free_count;
    bool fsinfo_marked;
    struct { bool valid; uint32_t idx; uint8_t *data; } fc[8]; int fc_next;
    char label[16];
};

struct dent {
    char name[256];
    uint8_t attr, sfn[11], raw[32];
    uint32_t clus, size;
    int nslots; uint32_t slot_clus[MAXSLOTS], slot_idx[MAXSLOTS];   /* LFN slots + short entry (last) */
};

/* ---------------- low level ---------------- */
static int rd(struct fatvol *v, uint64_t lba, uint32_t n, void *buf) { return v->bd->read(v->bd->ctx, lba, n, buf) ? FS_EIO : FS_OK; }
static int wr(struct fatvol *v, uint64_t lba, uint32_t n, const void *buf) { return v->bd->write(v->bd->ctx, lba, n, buf) ? FS_EIO : FS_OK; }
static uint64_t clus_lba(struct fatvol *v, uint32_t c) { return v->data_lba + (uint64_t)(c - 2) * v->spc; }
static uint16_t r16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t r32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void w16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void w32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static uint8_t *fat_sec(struct fatvol *v, uint32_t idx) {
    for (int i = 0; i < 8; i++) if (v->fc[i].valid && v->fc[i].idx == idx) return v->fc[i].data;
    int s = v->fc_next; v->fc_next = (v->fc_next + 1) % 8;
    v->fc[s].valid = false;
    if (rd(v, v->fat_lba + idx, 1, v->fc[s].data)) return 0;
    v->fc[s].valid = true; v->fc[s].idx = idx;
    return v->fc[s].data;
}

static int fat_get(struct fatvol *v, uint32_t c, uint32_t *out) {
    uint32_t off = c * 4;
    uint8_t *s = fat_sec(v, off / v->bps);
    if (!s) return FS_EIO;
    *out = r32(s + off % v->bps) & 0x0FFFFFFF;
    return FS_OK;
}

static void mark_fsinfo(struct fatvol *v) {   /* tell other systems the free count must be recounted */
    if (v->fsinfo_marked || !v->fsinfo || v->fsinfo == 0xFFFF) return;
    v->fsinfo_marked = true;
    uint8_t *s = kmalloc(v->bps);
    if (!s) return;
    if (!rd(v, v->base + v->fsinfo, 1, s) && r32(s) == 0x41615252 && r32(s + 484) == 0x61417272) {
        w32(s + 488, 0xFFFFFFFF); w32(s + 492, 0xFFFFFFFF);
        wr(v, v->base + v->fsinfo, 1, s);
    }
    kfree(s);
}

static int fat_set(struct fatvol *v, uint32_t c, uint32_t val) {
    uint32_t off = c * 4, idx = off / v->bps;
    uint8_t *s = fat_sec(v, idx);
    if (!s) return FS_EIO;
    uint8_t *p = s + off % v->bps;
    w32(p, (r32(p) & 0xF0000000) | (val & 0x0FFFFFFF));
    mark_fsinfo(v);
    for (uint32_t k = 0; k < v->nfats; k++)
        if (wr(v, v->fat_lba + (uint64_t)k * v->fatsz + idx, 1, s)) return FS_EIO;
    return FS_OK;
}

static int free_chain(struct fatvol *v, uint32_t c) {
    for (int guard = 0; c >= 2 && c < EOC && guard < 4000000; guard++) {
        uint32_t next;
        if (fat_get(v, c, &next) || fat_set(v, c, 0)) return FS_EIO;
        if (v->free_count >= 0) v->free_count++;
        c = next;
    }
    return FS_OK;
}

/* Allocate a chain of n clusters (n >= 1). Optionally zero them. */
static int alloc_chain(struct fatvol *v, uint32_t n, uint32_t *first, bool zero) {
    uint32_t prev = 0, got = 0, c = v->hint < 2 ? 2 : v->hint, scanned = 0;
    uint8_t *z = 0;
    if (zero && !(z = kmalloc(v->cbytes))) return FS_ENOSPC;
    if (zero) memset(z, 0, v->cbytes);
    *first = 0;
    while (got < n) {
        if (scanned++ > v->nclus) { if (*first) free_chain(v, *first); kfree(z); *first = 0; return FS_ENOSPC; }
        if (c >= v->nclus + 2) c = 2;
        uint32_t val;
        if (fat_get(v, c, &val)) { kfree(z); return FS_EIO; }
        if (val == 0) {
            if (fat_set(v, c, 0x0FFFFFFF)) { kfree(z); return FS_EIO; }
            if (prev) { if (fat_set(v, prev, c)) { kfree(z); return FS_EIO; } } else *first = c;
            if (zero && wr(v, clus_lba(v, c), v->spc, z)) { kfree(z); return FS_EIO; }
            if (v->free_count > 0) v->free_count--;
            prev = c; got++;
        }
        c++;
    }
    v->hint = c;
    kfree(z);
    return FS_OK;
}

/* ---------------- directories ---------------- */
static uint8_t lfn_sum(const uint8_t *sfn) { uint8_t s = 0; for (int i = 0; i < 11; i++) s = ((s & 1) << 7) + (s >> 1) + sfn[i]; return s; }

static void sfn_to_name(const uint8_t *e, char *out) {
    int k = 0;
    bool lb = e[12] & 0x08, le = e[12] & 0x10;
    for (int i = 0; i < 8 && e[i] != ' '; i++) { char c = (i == 0 && e[0] == 0x05) ? (char)0xE5 : e[i]; out[k++] = lb && c >= 'A' && c <= 'Z' ? c + 32 : c; }
    if (e[8] != ' ') { out[k++] = '.'; for (int i = 8; i < 11 && e[i] != ' '; i++) out[k++] = le && e[i] >= 'A' && e[i] <= 'Z' ? e[i] + 32 : e[i]; }
    out[k] = 0;
}

/* Calls cb for every live entry (including . and ..). cb returns 1 to stop. */
typedef int (*dir_cb)(struct fatvol *v, struct dent *d, void *ctx);
static int dir_iter(struct fatvol *v, uint32_t dclus, dir_cb cb, void *ctx) {
    uint8_t *buf = kmalloc(v->cbytes);
    struct dent *d = kmalloc(sizeof *d);
    if (!buf || !d) { kfree(buf); kfree(d); return FS_ENOSPC; }
    uint16_t lname[MAXSLOTS * 13 + 1]; int lfn_n = 0; uint8_t lfn_ck = 0;
    int r = FS_OK, guard = 0;
    d->nslots = 0;
    for (uint32_t c = dclus; c >= 2 && c < EOC && guard++ < 65536;) {
        if (rd(v, clus_lba(v, c), v->spc, buf)) { r = FS_EIO; break; }
        for (uint32_t i = 0; i < v->cbytes / 32; i++) {
            uint8_t *e = buf + i * 32;
            if (e[0] == 0) goto done;
            if (e[0] == 0xE5) { lfn_n = 0; d->nslots = 0; continue; }
            if (e[11] == 0x0F) {
                int ord = e[0] & 0x3F;
                if (e[0] & 0x40) { lfn_n = ord; lfn_ck = e[13]; d->nslots = 0; memset(lname, 0, sizeof lname); }
                if (ord < 1 || ord > MAXSLOTS || e[13] != lfn_ck || d->nslots >= MAXSLOTS - 1) { lfn_n = 0; d->nslots = 0; continue; }
                static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
                for (int k = 0; k < 13; k++) lname[(ord - 1) * 13 + k] = r16(e + pos[k]);
                d->slot_clus[d->nslots] = c; d->slot_idx[d->nslots] = i; d->nslots++;
                continue;
            }
            if (e[11] & 0x08) { lfn_n = 0; d->nslots = 0; continue; }   /* volume label */
            memcpy(d->raw, e, 32); memcpy(d->sfn, e, 11);
            d->attr = e[11];
            d->clus = (uint32_t)r16(e + 20) << 16 | r16(e + 26);
            d->size = r32(e + 28);
            if (lfn_n && lfn_ck == lfn_sum(e)) {
                int k = 0;
                for (; k < lfn_n * 13 && lname[k] && lname[k] != 0xFFFF && k < 255; k++) d->name[k] = lname[k] < 128 ? (char)lname[k] : '?';
                d->name[k] = 0;
            } else { sfn_to_name(e, d->name); d->nslots = 0; }
            d->slot_clus[d->nslots] = c; d->slot_idx[d->nslots] = i; d->nslots++;
            int stop = cb(v, d, ctx);
            lfn_n = 0; d->nslots = 0;
            if (stop) { r = 1; goto done; }
        }
        uint32_t next;
        if (fat_get(v, c, &next)) { r = FS_EIO; break; }
        c = next;
    }
done:
    kfree(buf); kfree(d);
    return r;
}

static bool ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a >= 'a' && *a <= 'z' ? *a - 32 : *a, y = *b >= 'a' && *b <= 'z' ? *b - 32 : *b;
        if (x != y) return false;
    }
    return !*a && !*b;
}

struct find_ctx { const char *name; struct dent *out; bool found; };
static int find_cb(struct fatvol *v, struct dent *d, void *c) {
    struct find_ctx *f = c;
    if (ieq(d->name, f->name)) { memcpy(f->out, d, sizeof *d); f->found = true; return 1; }
    return 0;
}

/* Look up a path. Root is reported as a directory with clus = root. */
static int lookup(struct fatvol *v, const char *rel, struct dent *out) {
    memset(out, 0, sizeof *out);
    out->attr = 0x10; out->clus = v->root;
    const char *p = rel;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char comp[256]; int k = 0;
        while (*p && *p != '/' && k < 255) comp[k++] = *p++;
        comp[k] = 0;
        if (!(out->attr & 0x10)) return FS_ENOTDIR;
        uint32_t dc = out->clus ? out->clus : v->root;
        struct find_ctx f = { comp, out, false };
        int r = dir_iter(v, dc, find_cb, &f);
        if (r < 0) return r;
        if (!f.found) return FS_ENOENT;
        if (!out->clus && (out->attr & 0x10)) out->clus = v->root;    /* ".." of a top-level folder */
    }
    return FS_OK;
}

static void split_parent(const char *rel, char *parent, char *name) {
    const char *slash = rel;
    for (const char *s = rel; *s; s++) if (*s == '/') slash = s;
    int pl = slash - rel;
    memcpy(parent, rel, pl); parent[pl] = 0;
    if (!pl) strncpy0(parent, "/", 2);
    strncpy0(name, slash + 1, 256);
}

/* write one 32-byte entry in place */
static int ent_write(struct fatvol *v, uint32_t clus, uint32_t idx, const uint8_t *e) {
    uint8_t *s = kmalloc(v->bps);
    if (!s) return FS_ENOSPC;
    uint64_t lba = clus_lba(v, clus) + (idx * 32) / v->bps;
    int r = rd(v, lba, 1, s);
    if (!r) { memcpy(s + (idx * 32) % v->bps, e, 32); r = wr(v, lba, 1, s); }
    kfree(s);
    return r;
}

static int mark_deleted(struct fatvol *v, struct dent *d) {
    for (int i = 0; i < d->nslots; i++) {
        uint8_t e[32];
        uint8_t *s = kmalloc(v->bps);
        if (!s) return FS_ENOSPC;
        uint64_t lba = clus_lba(v, d->slot_clus[i]) + (d->slot_idx[i] * 32) / v->bps;
        int r = rd(v, lba, 1, s);
        if (!r) { s[(d->slot_idx[i] * 32) % v->bps] = 0xE5; r = wr(v, lba, 1, s); }
        kfree(s);
        (void)e;
        if (r) return r;
    }
    return FS_OK;
}

/* Find n consecutive free entries in a directory, growing it if needed. */
static int find_slots(struct fatvol *v, uint32_t dclus, int n, uint32_t *sc, uint32_t *si) {
    uint8_t *buf = kmalloc(v->cbytes);
    if (!buf) return FS_ENOSPC;
    int run = 0;
    uint32_t c = dclus, last = dclus;
    for (int guard = 0; c >= 2 && c < EOC && guard < 65536; guard++) {
        if (rd(v, clus_lba(v, c), v->spc, buf)) { kfree(buf); return FS_EIO; }
        for (uint32_t i = 0; i < v->cbytes / 32; i++) {
            uint8_t b = buf[i * 32];
            if (b == 0 || b == 0xE5) { sc[run] = c; si[run] = i; if (++run == n) { kfree(buf); return FS_OK; } }
            else run = 0;
        }
        last = c;
        uint32_t next;
        if (fat_get(v, c, &next)) { kfree(buf); return FS_EIO; }
        c = next;
    }
    kfree(buf);
    /* extend the directory with a zeroed cluster */
    uint32_t nc;
    int r = alloc_chain(v, 1, &nc, true);
    if (r) return r;
    if (fat_set(v, last, nc)) return FS_EIO;
    for (uint32_t i = 0; run < n; i++) { sc[run] = nc; si[run] = i; run++; }
    return FS_OK;
}

static void fat_time(uint16_t *t, uint16_t *d) {
    struct rtc_time tm; rtc_read_local(&tm);
    *t = (tm.hour << 11) | (tm.min << 5) | (tm.sec / 2);
    *d = ((tm.year - 1980) << 9) | (tm.mon << 5) | tm.day;
}

static bool sfn_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr_("$%'-_@~`!(){}^#&", c));
}

struct sfn_ctx { const uint8_t *sfn; bool taken; };
static int sfn_cb(struct fatvol *v, struct dent *d, void *c) {
    struct sfn_ctx *s = c;
    if (!memcmp(d->sfn, s->sfn, 11)) { s->taken = true; return 1; }
    return 0;
}

/* Build the 8.3 name; returns true if a long-name entry is needed. */
static bool make_sfn(struct fatvol *v, uint32_t dclus, const char *name, uint8_t *sfn, uint8_t *caseflags) {
    memset(sfn, ' ', 11);
    *caseflags = 0;
    const char *dot = 0;
    for (const char *s = name; *s; s++) if (*s == '.') dot = s;
    if (dot == name) dot = 0;
    int bl = dot ? dot - name : (int)strlen(name), el = dot ? (int)strlen(dot + 1) : 0;
    /* does it fit 8.3 exactly? */
    bool fits = bl >= 1 && bl <= 8 && el <= 3;
    bool bu = false, bL = false, eu = false, eL = false;
    for (int i = 0; fits && i < bl; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') { bL = true; c -= 32; } else if (c >= 'A' && c <= 'Z') bu = true;
        if (c == '.' || !sfn_char(c)) fits = false; else sfn[i] = c;
    }
    for (int i = 0; fits && i < el; i++) {
        char c = dot[1 + i];
        if (c >= 'a' && c <= 'z') { eL = true; c -= 32; } else if (c >= 'A' && c <= 'Z') eu = true;
        if (!sfn_char(c)) fits = false; else sfn[8 + i] = c;
    }
    if (fits && !(bu && bL) && !(eu && eL)) {
        if (bL) *caseflags |= 0x08;
        if (eL) *caseflags |= 0x10;
        struct sfn_ctx sc = { sfn, false };
        dir_iter(v, dclus, sfn_cb, &sc);
        if (!sc.taken) return false;
    }
    /* long name: BASIS~N.EXT */
    memset(sfn, ' ', 11);
    *caseflags = 0;
    int k = 0;
    for (int i = 0; i < bl && k < 6; i++) {
        char c = name[i];
        if (c == ' ' || c == '.') continue;
        if (c >= 'a' && c <= 'z') c -= 32;
        sfn[k++] = sfn_char(c) ? c : '_';
    }
    if (!k) sfn[k++] = '_';
    for (int i = 0, j = 0; i < el && j < 3; i++) {
        char c = dot[1 + i];
        if (c == ' ') continue;
        if (c >= 'a' && c <= 'z') c -= 32;
        sfn[8 + j++] = sfn_char(c) ? c : '_';
    }
    for (int n = 1; n < 1000000; n++) {
        char num[8]; ksnprintf(num, sizeof num, "~%d", n);
        int nl = strlen(num), base = k + nl <= 8 ? k : 8 - nl;
        memset(sfn, ' ', 8);
        int kk = 0;
        for (int i = 0; i < bl && kk < base; i++) {
            char c = name[i];
            if (c == ' ' || c == '.') continue;
            if (c >= 'a' && c <= 'z') c -= 32;
            sfn[kk++] = sfn_char(c) ? c : '_';
        }
        if (!kk) sfn[kk++] = '_';
        memcpy(sfn + kk, num, nl);
        struct sfn_ctx sc = { sfn, false };
        dir_iter(v, dclus, sfn_cb, &sc);
        if (!sc.taken) break;
    }
    return true;
}

/* Create a directory entry (with long name if needed). */
static int create_entry(struct fatvol *v, uint32_t dclus, const char *name, uint8_t attr, uint32_t clus, uint32_t size, const uint8_t *times) {
    uint8_t sfn[11], cf;
    bool lfn = make_sfn(v, dclus, name, sfn, &cf);
    int nl = strlen(name), nlfn = lfn ? (nl + 12) / 13 : 0;
    if (nlfn > MAXSLOTS - 1) return FS_EINVAL;
    uint32_t sc[MAXSLOTS], si[MAXSLOTS];
    int r = find_slots(v, dclus, nlfn + 1, sc, si);
    if (r) return r;
    uint8_t ck = lfn_sum(sfn);
    static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    for (int k = 0; k < nlfn; k++) {
        int ord = nlfn - k;                     /* physical order: last part first */
        uint8_t e[32]; memset(e, 0, 32);
        e[0] = ord | (k == 0 ? 0x40 : 0);
        e[11] = 0x0F; e[13] = ck;
        for (int j = 0; j < 13; j++) {
            int ci = (ord - 1) * 13 + j;
            uint16_t ch = ci < nl ? (uint8_t)name[ci] : ci == nl ? 0 : 0xFFFF;
            w16(e + pos[j], ch);
        }
        if ((r = ent_write(v, sc[k], si[k], e))) return r;
    }
    uint8_t e[32]; memset(e, 0, 32);
    memcpy(e, sfn, 11);
    e[11] = attr; e[12] = cf;
    if (times) memcpy(e + 13, times + 13, 7);
    uint16_t t, d; fat_time(&t, &d);
    if (!times) { w16(e + 14, t); w16(e + 16, d); w16(e + 18, d); }
    w16(e + 22, t); w16(e + 24, d);
    w16(e + 20, clus >> 16); w16(e + 26, clus & 0xFFFF);
    w32(e + 28, size);
    return ent_write(v, sc[nlfn], si[nlfn], e);
}

/* ---------------- fs_ops ---------------- */
struct list_ctx { struct vfs_dirent *out; int max, n; };
static int list_cb(struct fatvol *v, struct dent *d, void *c) {
    struct list_ctx *l = c;
    if (!strcmp(d->name, ".") || !strcmp(d->name, "..")) return 0;
    if (d->attr & 0x02) return 0;   /* hidden */
    if (l->n >= l->max) return 1;
    strncpy0(l->out[l->n].name, d->name, sizeof l->out[l->n].name);
    l->out[l->n].is_dir = (d->attr & 0x10) != 0;
    l->out[l->n].size = d->size;
    l->n++;
    return 0;
}

static int f_list(void *vol, const char *rel, struct vfs_dirent *out, int max) {
    struct fatvol *v = vol; struct dent *d = kmalloc(sizeof *d);
    if (!d) return FS_ENOSPC;
    int r = lookup(v, rel, d);
    uint32_t dc = d->clus;
    bool dir = d->attr & 0x10;
    kfree(d);
    if (r) return r;
    if (!dir) return FS_ENOTDIR;
    struct list_ctx l = { out, max, 0 };
    r = dir_iter(v, dc, list_cb, &l);
    if (r < 0) return r;
    /* folders first, then names (insertion sort) */
    for (int i = 1; i < l.n; i++) {
        struct vfs_dirent t = out[i]; int j = i - 1;
        while (j >= 0 && (out[j].is_dir < t.is_dir || (out[j].is_dir == t.is_dir && strcmp(out[j].name, t.name) > 0))) { out[j + 1] = out[j]; j--; }
        out[j + 1] = t;
    }
    return l.n;
}

static int f_stat(void *vol, const char *rel, struct dunc_stat *st) {
    struct fatvol *v = vol; struct dent *d = kmalloc(sizeof *d);
    if (!d) return FS_ENOSPC;
    int r = lookup(v, rel, d);
    if (!r) { st->is_dir = (d->attr & 0x10) != 0; st->size = d->size; }
    kfree(d);
    return r;
}

static long f_read(void *vol, const char *rel, void *buf, uint64_t off, uint64_t len) {
    struct fatvol *v = vol; struct dent *d = kmalloc(sizeof *d);
    if (!d) return FS_ENOSPC;
    int r = lookup(v, rel, d);
    uint32_t c = d->clus, size = d->size; bool dir = d->attr & 0x10;
    kfree(d);
    if (r) return r;
    if (dir) return FS_EISDIR;
    if (off >= size) return 0;
    if (len > size - off) len = size - off;
    uint8_t *cb = kmalloc(v->cbytes);
    if (!cb) return FS_ENOSPC;
    uint64_t skip = off / v->cbytes, done = 0, inoff = off % v->cbytes;
    for (uint64_t i = 0; i < skip && c >= 2 && c < EOC; i++) if (fat_get(v, c, &c)) { kfree(cb); return FS_EIO; }
    while (done < len && c >= 2 && c < EOC) {
        if (rd(v, clus_lba(v, c), v->spc, cb)) { kfree(cb); return FS_EIO; }
        uint64_t n = v->cbytes - inoff;
        if (n > len - done) n = len - done;
        memcpy((uint8_t *)buf + done, cb + inoff, n);
        done += n; inoff = 0;
        if (fat_get(v, c, &c)) { kfree(cb); return FS_EIO; }
    }
    kfree(cb);
    return (long)done;
}

static long f_write(void *vol, const char *rel, const void *buf, uint64_t len) {
    struct fatvol *v = vol;
    if (len > 0xFFFFFFFFULL) return FS_ENOSPC;
    char parent[PATH_MAX_], name[256];
    split_parent(rel, parent, name);
    struct dent *pd = kmalloc(sizeof *pd), *old = kmalloc(sizeof *old);
    if (!pd || !old) { kfree(pd); kfree(old); return FS_ENOSPC; }
    int r = lookup(v, parent, pd);
    if (!r && !(pd->attr & 0x10)) r = FS_ENOTDIR;
    uint32_t dclus = pd->clus;
    int ex = r ? r : lookup(v, rel, old);
    if (!r && ex == FS_OK && (old->attr & 0x10)) r = FS_EISDIR;
    if (r) { kfree(pd); kfree(old); return r; }
    /* write the new data into fresh clusters first, so a failure leaves the old file intact */
    uint32_t first = 0, n = (uint32_t)((len + v->cbytes - 1) / v->cbytes);
    if (n && (r = alloc_chain(v, n, &first, false))) { kfree(pd); kfree(old); return r; }
    uint8_t *cb = kmalloc(v->cbytes);
    uint32_t c = first;
    for (uint64_t done = 0; !r && done < len; done += v->cbytes) {
        uint64_t k = len - done < v->cbytes ? len - done : v->cbytes;
        memcpy(cb, (const uint8_t *)buf + done, k);
        if (k < v->cbytes) memset(cb + k, 0, v->cbytes - k);
        if (wr(v, clus_lba(v, c), v->spc, cb)) r = FS_EIO;
        else if (fat_get(v, c, &c)) r = FS_EIO;
    }
    kfree(cb);
    if (r) { free_chain(v, first); kfree(pd); kfree(old); return r; }
    if (ex == FS_OK) {
        uint8_t *e = old->raw;
        uint32_t old_clus = old->clus;
        uint16_t t, d; fat_time(&t, &d);
        w16(e + 20, first >> 16); w16(e + 26, first & 0xFFFF); w32(e + 28, (uint32_t)len);
        w16(e + 22, t); w16(e + 24, d); w16(e + 18, d); e[11] |= 0x20;
        r = ent_write(v, old->slot_clus[old->nslots - 1], old->slot_idx[old->nslots - 1], e);
        if (!r && old_clus) free_chain(v, old_clus);
    } else {
        r = create_entry(v, dclus, name, 0x20, first, (uint32_t)len, 0);
        if (r && first) free_chain(v, first);
    }
    kfree(pd); kfree(old);
    return r ? r : (long)len;
}

static int f_mkdir(void *vol, const char *rel) {
    struct fatvol *v = vol;
    char parent[PATH_MAX_], name[256];
    split_parent(rel, parent, name);
    struct dent *pd = kmalloc(sizeof *pd), *ex = kmalloc(sizeof *ex);
    if (!pd || !ex) { kfree(pd); kfree(ex); return FS_ENOSPC; }
    int r = lookup(v, parent, pd);
    if (!r && !(pd->attr & 0x10)) r = FS_ENOTDIR;
    if (!r && lookup(v, rel, ex) == FS_OK) r = FS_EEXIST;
    uint32_t pclus = pd->clus;
    kfree(pd); kfree(ex);
    if (r) return r;
    uint32_t c;
    if ((r = alloc_chain(v, 1, &c, true))) return r;
    uint8_t e[32]; uint16_t t, d; fat_time(&t, &d);
    memset(e, 0, 32); memset(e, ' ', 11); e[0] = '.'; e[11] = 0x10;
    w16(e + 14, t); w16(e + 16, d); w16(e + 18, d); w16(e + 22, t); w16(e + 24, d);
    w16(e + 20, c >> 16); w16(e + 26, c & 0xFFFF);
    if ((r = ent_write(v, c, 0, e))) return r;
    e[1] = '.';
    uint32_t pc = pclus == v->root ? 0 : pclus;
    w16(e + 20, pc >> 16); w16(e + 26, pc & 0xFFFF);
    if ((r = ent_write(v, c, 1, e))) return r;
    r = create_entry(v, pclus, name, 0x10, c, 0, 0);
    if (r) free_chain(v, c);
    return r;
}

static int empty_cb(struct fatvol *v, struct dent *d, void *c) {
    if (strcmp(d->name, ".") && strcmp(d->name, "..")) { *(bool *)c = false; return 1; }
    return 0;
}

static int f_remove(void *vol, const char *rel) {
    struct fatvol *v = vol; struct dent *d = kmalloc(sizeof *d);
    if (!d) return FS_ENOSPC;
    int r = lookup(v, rel, d);
    if (!r && !d->nslots) r = FS_EINVAL;          /* the root */
    if (!r && (d->attr & 0x10)) { bool empty = true; dir_iter(v, d->clus, empty_cb, &empty); if (!empty) r = FS_ENOTEMPTY; }
    if (!r) r = mark_deleted(v, d);
    if (!r && d->clus && d->clus != v->root) r = free_chain(v, d->clus);
    kfree(d);
    return r;
}

static int f_rename(void *vol, const char *from, const char *to) {
    struct fatvol *v = vol;
    char parent[PATH_MAX_], name[256];
    split_parent(to, parent, name);
    struct dent *src = kmalloc(sizeof *src), *pd = kmalloc(sizeof *pd);
    if (!src || !pd) { kfree(src); kfree(pd); return FS_ENOSPC; }
    int r = lookup(v, from, src);
    if (!r && !src->nslots) r = FS_EINVAL;
    if (!r) r = lookup(v, parent, pd);
    if (!r && !(pd->attr & 0x10)) r = FS_ENOTDIR;
    uint32_t newp = pd->clus;
    if (!r) r = create_entry(v, newp, name, src->attr, src->clus, src->size, src->raw);
    if (!r) r = mark_deleted(v, src);
    if (!r && (src->attr & 0x10) && src->clus) {        /* fix the moved folder's ".." */
        uint8_t *cb = kmalloc(v->cbytes);
        if (cb && !rd(v, clus_lba(v, src->clus), v->spc, cb) && cb[32] == '.' && cb[33] == '.') {
            uint32_t pc = newp == v->root ? 0 : newp;
            w16(cb + 32 + 20, pc >> 16); w16(cb + 32 + 26, pc & 0xFFFF);
            r = ent_write(v, src->clus, 1, cb + 32);
        }
        kfree(cb);
    }
    kfree(src); kfree(pd);
    return r;
}

static void f_space(void *vol, uint64_t *total, uint64_t *freeb) {
    struct fatvol *v = vol;
    *total = (uint64_t)v->nclus * v->cbytes;
    if (v->free_count < 0) {           /* count once, then keep it up to date */
        uint32_t per = v->bps / 4, nsec = (v->nclus + 2 + per - 1) / per, cnt = 0;
        uint8_t *buf = kmalloc(v->bps * 64);
        for (uint32_t s = 0; buf && s < nsec; s += 64) {
            uint32_t k = nsec - s < 64 ? nsec - s : 64;
            if (rd(v, v->fat_lba + s, k, buf)) { cnt = 0; break; }
            for (uint32_t i = 0; i < k * per; i++) {
                uint32_t c = s * per + i;
                if (c >= 2 && c < v->nclus + 2 && !(r32(buf + i * 4) & 0x0FFFFFFF)) cnt++;
            }
        }
        kfree(buf);
        v->free_count = cnt;
    }
    *freeb = (uint64_t)v->free_count * v->cbytes;
}

static long f_append(void *vol, const char *rel, const void *buf, uint64_t len) {
    struct fatvol *v = vol;
    struct dent *d = kmalloc(sizeof *d);
    if (!d) return FS_ENOSPC;
    int r = lookup(v, rel, d);
    if (!r && (d->attr & 0x10)) r = FS_EISDIR;
    if (!r && (uint64_t)d->size + len > 0xFFFFFFFFULL) r = FS_ENOSPC;
    if (r) { kfree(d); return r; }
    uint8_t *cb = kmalloc(v->cbytes);
    uint32_t size = d->size, first = d->clus, last = 0, c = first;
    for (int guard = 0; c >= 2 && c < EOC && guard < 4000000; guard++) { last = c; if (fat_get(v, c, &c)) { r = FS_EIO; break; } }
    uint64_t done = 0;
    uint32_t used = size % v->cbytes;
    if (!r && last && used) {                      /* top up the partly used last cluster */
        if (rd(v, clus_lba(v, last), v->spc, cb)) r = FS_EIO;
        else {
            uint64_t n = v->cbytes - used < len ? v->cbytes - used : len;
            memcpy(cb + used, buf, n);
            if (wr(v, clus_lba(v, last), v->spc, cb)) r = FS_EIO;
            done = n;
        }
    }
    if (!r && done < len) {                        /* then add new clusters */
        uint32_t need = (uint32_t)((len - done + v->cbytes - 1) / v->cbytes), nc;
        if ((r = alloc_chain(v, need, &nc, false)) == FS_OK) {
            uint32_t k = nc;
            for (; !r && done < len; done += v->cbytes) {
                uint64_t n = len - done < v->cbytes ? len - done : v->cbytes;
                memcpy(cb, (const uint8_t *)buf + done, n);
                if (n < v->cbytes) memset(cb + n, 0, v->cbytes - n);
                if (wr(v, clus_lba(v, k), v->spc, cb) || fat_get(v, k, &k)) r = FS_EIO;
            }
            if (!r) { if (last) r = fat_set(v, last, nc); else first = nc; }
            if (r) free_chain(v, nc);
        }
    }
    if (!r) {
        uint8_t *e = d->raw; uint16_t t, dt; fat_time(&t, &dt);
        w16(e + 20, first >> 16); w16(e + 26, first & 0xFFFF); w32(e + 28, size + (uint32_t)len);
        w16(e + 22, t); w16(e + 24, dt); e[11] |= 0x20;
        r = ent_write(v, d->slot_clus[d->nslots - 1], d->slot_idx[d->nslots - 1], e);
    }
    kfree(cb); kfree(d);
    return r ? r : (long)len;
}

/* the entry's write date and time (local time on FAT, as AXIOM writes them) as Unix seconds */
int64_t days_from_civil(int64_t y, unsigned m, unsigned d);
static int64_t f_mtime(void *vol, const char *rel) {
    struct fatvol *v = vol; struct dent *d = kmalloc(sizeof *d);
    if (!d) return -1;
    int r = lookup(v, rel, d);
    uint16_t t = d->raw[22] | d->raw[23] << 8, dt = d->raw[24] | d->raw[25] << 8;
    kfree(d);
    if (r || !dt) return -1;                                  /* (the root, or no date) */
    unsigned y = 1980 + (dt >> 9), mo = (dt >> 5) & 15, dd = dt & 31;
    if (mo < 1 || mo > 12 || dd < 1) return -1;
    struct rtc_time now; rtc_read_local(&now);
    return days_from_civil(y, mo, dd) * 86400 + (t >> 11) * 3600 + ((t >> 5) & 63) * 60 + (t & 31) * 2 - now.utc_offset_min * 60;
}

const struct fs_ops fat_ops = { f_list, f_read, f_write, f_mkdir, f_remove, f_rename, f_stat, f_space, f_append, f_mtime };

/* ---------------- mount ---------------- */
static bool is_fat32_vbr(const uint8_t *b) {
    return (b[0] == 0xEB || b[0] == 0xE9) && !memcmp(b + 82, "FAT32   ", 8) && r16(b + 11) >= 512;
}
static const char *other_fs(const uint8_t *b) {
    if (!memcmp(b + 3, "EXFAT   ", 8)) return "exFAT";
    if (!memcmp(b + 3, "NTFS    ", 8)) return "NTFS";
    if (!memcmp(b + 54, "FAT16   ", 8) || !memcmp(b + 54, "FAT12   ", 8)) return "FAT16";
    return 0;
}

struct label_ctx { char *out; };
void *fat_mount(struct blockdev *bd, char *label_out, char *why, int why_len) {
    uint8_t *b = kmalloc(bd->bsize), *vbr = kmalloc(bd->bsize);
    struct fatvol *v = 0;
    const char *other = 0;
    if (!b || !vbr || bd->read(bd->ctx, 0, 1, b)) { strncpy0(why, "the drive couldn't be read", why_len); goto out; }
    uint64_t base = (uint64_t)-1;
    if (is_fat32_vbr(b)) base = 0;
    else if (b[510] == 0x55 && b[511] == 0xAA) {
        if (b[446 + 4] == 0xEE) {                       /* GPT */
            uint8_t *h = vbr;
            if (!bd->read(bd->ctx, 1, 1, h) && !memcmp(h, "EFI PART", 8)) {
                uint64_t elba = r32(h + 72) | (uint64_t)r32(h + 76) << 32;
                uint32_t cnt = r32(h + 80), esz = r32(h + 84);
                uint8_t *eb = kmalloc(bd->bsize);
                for (uint32_t i = 0; eb && i < cnt && i < 128 && base == (uint64_t)-1; i++) {
                    uint64_t off = (uint64_t)i * esz;
                    if (bd->read(bd->ctx, elba + off / bd->bsize, 1, eb)) break;
                    uint8_t *e = eb + off % bd->bsize;
                    bool used = false; for (int k = 0; k < 16; k++) if (e[k]) used = true;
                    if (!used) continue;
                    uint64_t first = r32(e + 32) | (uint64_t)r32(e + 36) << 32;
                    if (!bd->read(bd->ctx, first, 1, vbr)) { if (is_fat32_vbr(vbr)) base = first; else if (!other) other = other_fs(vbr); }
                }
                kfree(eb);
            }
        } else {
            for (int i = 0; i < 4 && base == (uint64_t)-1; i++) {
                uint8_t *e = b + 446 + i * 16;
                if (!e[4]) continue;
                uint64_t first = r32(e + 8);
                if (!bd->read(bd->ctx, first, 1, vbr)) { if (is_fat32_vbr(vbr)) base = first; else if (!other) other = other_fs(vbr); }
            }
        }
    }
    if (!other) other = other_fs(b);
    if (base == (uint64_t)-1) {
        if (other) ksnprintf(why, why_len, "it's formatted as %s. AXIOM can use FAT32 drives.", other);
        else strncpy0(why, "no FAT32 file system found. AXIOM can use FAT32 drives.", why_len);
        goto out;
    }
    kfree(b); kfree(vbr);
    return fat_open(bd, base, label_out, why, why_len);
out:
    kfree(b); kfree(vbr);
    return v;
}

/* open the FAT32 file system that starts at block base */
void *fat_open(struct blockdev *bd, uint64_t base, char *label_out, char *why, int why_len) {
    struct fatvol *v = 0;
    uint8_t *vbr = kmalloc(bd->bsize);
    if (!vbr || bd->read(bd->ctx, base, 1, vbr) || !is_fat32_vbr(vbr)) { strncpy0(why, "no FAT32 file system there", why_len); goto out; }
    v = kmalloc(sizeof *v);
    if (!v) goto out;
    memset(v, 0, sizeof *v);
    v->bd = bd; v->base = base;
    v->bps = r16(vbr + 11); v->spc = vbr[13]; v->rsvd = r16(vbr + 14); v->nfats = vbr[16];
    v->fatsz = r32(vbr + 36); v->root = r32(vbr + 44); v->fsinfo = r16(vbr + 48);
    uint32_t tot = r16(vbr + 19) ? r16(vbr + 19) : r32(vbr + 32);
    if (v->bps != bd->bsize || !v->spc || !v->nfats || !v->fatsz || v->root < 2 || (uint64_t)v->bps * v->spc > 65536) {
        strncpy0(why, "its FAT32 layout isn't supported", why_len);
        kfree(v); v = 0; goto out;
    }
    v->cbytes = v->bps * v->spc;
    v->fat_lba = base + v->rsvd;
    v->data_lba = v->fat_lba + (uint64_t)v->nfats * v->fatsz;
    v->nclus = (tot - v->rsvd - v->nfats * v->fatsz) / v->spc;
    v->free_count = -1; v->hint = 2;
    for (int i = 0; i < 8; i++) v->fc[i].data = kmalloc(v->bps);
    /* label: volume label entry in the root folder, else the boot sector's */
    char lab[12] = "";
    memcpy(lab, vbr + 71, 11); lab[11] = 0;
    uint8_t *rb = kmalloc(v->cbytes);
    if (rb && !rd(v, clus_lba(v, v->root), v->spc, rb))
        for (uint32_t i = 0; i < v->cbytes / 32 && rb[i * 32]; i++)
            if (rb[i * 32] != 0xE5 && rb[i * 32 + 11] == 0x08) { memcpy(lab, rb + i * 32, 11); lab[11] = 0; break; }
    kfree(rb);
    int l = strlen(lab); while (l && lab[l - 1] == ' ') lab[--l] = 0;
    if (!strcmp(lab, "NO NAME")) lab[0] = 0;
    strncpy0(v->label, lab, sizeof v->label);
    strncpy0(label_out, lab, 16);
out:
    kfree(vbr);
    return v;
}

/* Create an empty FAT32 file system on blocks [base, base + count). Returns 0 or an FS_ error. */
int fat_format(struct blockdev *bd, uint64_t base, uint64_t count, const char *label) {
    uint32_t bps = bd->bsize;
    if (count > 0xFFFFFFFFull) count = 0xFFFFFFFFull;
    uint32_t n = (uint32_t)count, rsvd = 32, nfats = 2;
    uint64_t bytes = (uint64_t)n * bps;
    uint32_t cbytes = bytes <= (8ull << 30) ? 4096 : bytes <= (16ull << 30) ? 8192 : bytes <= (32ull << 30) ? 16384 : 32768;
    uint32_t spc = cbytes / bps ? cbytes / bps : 1, fatsz = 0, nclus = 0;
    for (;;) {
        fatsz = (uint32_t)((((uint64_t)(n - rsvd) / spc + 2) * 4 + bps - 1) / bps);
        nclus = (n - rsvd - nfats * fatsz) / spc;
        if (nclus >= 65525 || spc == 1) break;
        spc /= 2;                                            /* small disk: smaller clusters so it is still FAT32 */
    }
    if (nclus < 65525) return FS_ENOSPC;
    uint32_t chunk = 65536 / bps;
    uint8_t *buf = kmalloc(65536);
    if (!buf) return FS_ENOSPC;
    int r = FS_OK;
    #define WR(lba, cnt, b) do { if (r == FS_OK && bd->write(bd->ctx, (lba), (cnt), (b))) r = FS_EIO; } while (0)
    /* zero the reserved area, both FATs and the root folder's cluster */
    memset(buf, 0, 65536);
    uint64_t zend = base + rsvd + (uint64_t)nfats * fatsz + spc;
    for (uint64_t l = base; l < zend && r == FS_OK; l += chunk) WR(l, (uint32_t)(zend - l < chunk ? zend - l : chunk), buf);
    /* boot sector and FSInfo, plus their backups at sectors 6 and 7 */
    uint8_t *v = buf;
    v[0] = 0xEB; v[1] = 0x58; v[2] = 0x90; memcpy(v + 3, "AXIOM   ", 8);
    w16(v + 11, bps); v[13] = spc; w16(v + 14, rsvd); v[16] = nfats; v[21] = 0xF8;
    w16(v + 24, 63); w16(v + 26, 255); w32(v + 28, (uint32_t)base); w32(v + 32, n);
    w32(v + 36, fatsz); w32(v + 44, 2); w16(v + 48, 1); w16(v + 50, 6);
    v[64] = 0x80; v[66] = 0x29;
    uint32_t id; random_bytes(&id, 4); w32(v + 67, id);
    char lab[11]; memset(lab, ' ', 11);
    for (int i = 0; i < 11 && label[i]; i++) lab[i] = label[i] >= 'a' && label[i] <= 'z' ? label[i] - 32 : label[i];
    memcpy(v + 71, lab, 11); memcpy(v + 82, "FAT32   ", 8);
    v[510] = 0x55; v[511] = 0xAA;
    uint8_t *fi = buf + bps;
    w32(fi, 0x41615252); w32(fi + 484, 0x61417272); w32(fi + 488, nclus - 1); w32(fi + 492, 3); w32(fi + 508, 0xAA550000);
    WR(base, 2, buf);
    WR(base + 6, 2, buf);
    memset(buf, 0, 2 * bps);
    /* first FAT sector of each copy: media, end-of-chain, root folder */
    w32(buf, 0x0FFFFFF8); w32(buf + 4, 0x0FFFFFFF); w32(buf + 8, 0x0FFFFFFF);
    for (uint32_t f = 0; f < nfats; f++) WR(base + rsvd + (uint64_t)f * fatsz, 1, buf);
    /* the root folder holds the volume label */
    memset(buf, 0, bps);
    memcpy(buf, lab, 11); buf[11] = 0x08;
    WR(base + rsvd + (uint64_t)nfats * fatsz, 1, buf);
    #undef WR
    kfree(buf);
    return r;
}

void fat_unmount(void *vol) {
    struct fatvol *v = vol;
    if (!v) return;
    for (int i = 0; i < 8; i++) kfree(v->fc[i].data);
    kfree(v);
}
