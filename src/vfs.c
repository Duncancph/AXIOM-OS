/* Virtual file system: an in-memory tree (the system disk for now) plus mounted volumes
 * such as FAT32 USB drives under /usb. Paths are absolute, '/'-separated. */
#include "vfs.h"

void *kmalloc(size_t n);
void kfree(void *p);

struct vnode {
    char name[64];
    bool dir;
    uint8_t *data; uint64_t size, cap;
    struct vnode *parent, *child, *next;
};
/* A mounted volume. USB drives appear as /usbN; the DuncOS disk also provides /home, /apps and
 * /system/config ("system" mounts: at = several components, sub = the folder on the volume). */
#define NMOUNT 8
struct mount { bool used, system; char name[16], label[32], kind[16]; const struct fs_ops *ops; void *vol;
               int depth; char at[3][64]; char sub[64]; };

static struct vnode *root;
static struct mount mounts[NMOUNT];
static volatile int lock_owner;
static int lock_depth;

/* A simple lock that yields to other tasks while waiting (file operations on USB drives sleep). */
static void lock(void) {
    int me = current ? current->pid : -1;
    while (lock_owner && lock_owner != me) kyield();
    lock_owner = me; lock_depth++;
}
static void unlock(void) { if (--lock_depth == 0) lock_owner = 0; }

static void notify_changed(void) {
    struct dunc_event e = { EV_FILES_CHANGED, 0, 0, 0, 0, 0 };
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state && tasks[i].state != PS_ZOMBIE && tasks[i].user) event_push(&tasks[i], &e);
}

const char *fs_error(int e) {
    switch (e) {
    case FS_ENOENT: return "No such file or folder";
    case FS_EEXIST: return "Already exists";
    case FS_ENOTDIR: return "Not a folder";
    case FS_EISDIR: return "Is a folder";
    case FS_ENOSPC: return "Not enough space";
    case FS_EIO: return "Drive read/write error";
    case FS_ENOTEMPTY: return "Folder is not empty";
    case FS_EROFS: return "Read-only";
    case FS_EINVAL: return "Invalid name or path";
    }
    return "Error";
}

/* ---------------- paths ---------------- */
#define MAXC 32
struct path { int n; char c[MAXC][64]; };

static bool parse(const char *p, struct path *out) {
    out->n = 0;
    if (!p || p[0] != '/') return false;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char comp[64]; int k = 0;
        while (*p && *p != '/') { if (k >= 63) return false; comp[k++] = *p++; }
        comp[k] = 0;
        if (!strcmp(comp, ".")) continue;
        if (!strcmp(comp, "..")) { if (out->n) out->n--; continue; }
        if (out->n >= MAXC) return false;
        strncpy0(out->c[out->n++], comp, 64);
    }
    return true;
}

static void join(const struct path *p, int from, char *out, size_t n) {
    size_t k = 0;
    if (from >= p->n) { strncpy0(out, "/", n); return; }
    for (int i = from; i < p->n; i++) {
        if (k + 1 < n) out[k++] = '/';
        for (const char *s = p->c[i]; *s && k + 1 < n; s++) out[k++] = *s;
    }
    out[k] = 0;
}

static bool valid_name(const char *s) {
    if (!*s) return false;
    for (; *s; s++) if (*s < 32 || *s == '/' || *s == '\\' || *s == ':' || *s == '*' || *s == '?' || *s == '"' || *s == '<' || *s == '>' || *s == '|') return false;
    return true;
}

/* Returns the mount for a path inside a volume (and the path relative to it), else 0. */
static struct mount *route(const struct path *p, char *rel, size_t n) {
    if (p->n == 0) return 0;
    struct mount *best = 0;
    for (int i = 0; i < NMOUNT; i++) {
        struct mount *m = &mounts[i];
        if (!m->used || m->depth > p->n || (best && best->depth >= m->depth)) continue;
        bool match = true;
        for (int k = 0; k < m->depth; k++) if (strcmp(m->at[k], p->c[k])) match = false;
        if (match) best = m;
    }
    if (!best) return 0;
    char rest[PATH_MAX_]; join(p, best->depth, rest, sizeof rest);
    if (!best->sub[0]) strncpy0(rel, rest, n);
    else if (!strcmp(rest, "/")) strncpy0(rel, best->sub, n);
    else ksnprintf(rel, n, "%s%s", best->sub, rest);
    return best;
}
static bool is_mount_root(const struct mount *m, const struct path *p) { return m && p->n == m->depth; }

/* ---------------- RAM file system ---------------- */
static struct vnode *child(struct vnode *d, const char *name) {
    for (struct vnode *c = d->child; c; c = c->next) if (!strcmp(c->name, name)) return c;
    return 0;
}
static struct vnode *walk(const struct path *p, int upto) {
    struct vnode *v = root;
    for (int i = 0; i < upto && v; i++) { if (!v->dir) return 0; v = child(v, p->c[i]); }
    return v;
}
static struct vnode *mknode(struct vnode *parent, const char *name, bool dir) {
    struct vnode *v = kmalloc(sizeof *v);
    if (!v) return 0;
    strncpy0(v->name, name, sizeof v->name);
    v->dir = dir; v->parent = parent;
    if (parent) {   /* keep children sorted: folders first, then by name */
        struct vnode **pp = &parent->child;
        while (*pp && ((*pp)->dir > dir || ((*pp)->dir == dir && strcmp((*pp)->name, name) < 0))) pp = &(*pp)->next;
        v->next = *pp; *pp = v;
    }
    return v;
}
static void unlink_node(struct vnode *v) {
    struct vnode **pp = &v->parent->child;
    while (*pp && *pp != v) pp = &(*pp)->next;
    if (*pp) *pp = v->next;
}
static void free_tree(struct vnode *v) {
    while (v->child) { struct vnode *c = v->child; v->child = c->next; free_tree(c); }
    kfree(v->data);
    kfree(v);
}
static uint64_t ram_used;

static int ram_write(struct vnode *dirv, const char *name, const void *buf, uint64_t len) {
    struct vnode *f = child(dirv, name);
    if (f && f->dir) return FS_EISDIR;
    uint8_t *data = 0;
    if (len) { data = kmalloc(len); if (!data) return FS_ENOSPC; memcpy(data, buf, len); }
    if (!f && !(f = mknode(dirv, name, false))) { kfree(data); return FS_ENOSPC; }
    ram_used -= f->size;
    kfree(f->data);
    f->data = data; f->size = f->cap = len;
    ram_used += len;
    return FS_OK;
}

void vfs_init(void) {
    root = mknode(0, "", true);
    static const char *dirs[] = { "/home", "/home/Documents", "/home/Projects", "/home/Downloads", "/home/Trash", "/apps", "/system" };
    for (unsigned i = 0; i < sizeof dirs / sizeof dirs[0]; i++) vfs_mkdir(dirs[i]);
}

/* ---------------- public operations ---------------- */
int vfs_list(const char *path, struct dunc_dirent *out, int max) {
    struct path p; char rel[PATH_MAX_];
    if (!parse(path, &p)) return FS_EINVAL;
    lock();
    int n = 0;
    struct mount *m = route(&p, rel, sizeof rel);
    if (m) n = m->ops->list(m->vol, rel, out, max);
    else {
        struct vnode *d = walk(&p, p.n);
        if (!d) n = FS_ENOENT;
        else if (!d->dir) n = FS_ENOTDIR;
        else {
            for (struct vnode *c = d->child; c && n < max; c = c->next, n++) {
                strncpy0(out[n].name, c->name, sizeof out[n].name);
                out[n].is_dir = c->dir; out[n].size = c->size;
            }
            for (int i = 0; i < NMOUNT && n < max; i++)    /* mount points directly inside this folder */
                if (mounts[i].used && mounts[i].depth == p.n + 1) {
                    bool match = true, dup = false;
                    for (int k = 0; k < p.n; k++) if (strcmp(mounts[i].at[k], p.c[k])) match = false;
                    for (int k = 0; match && k < n; k++) if (!strcmp(out[k].name, mounts[i].at[p.n])) dup = true;
                    if (match && !dup) { strncpy0(out[n].name, mounts[i].at[p.n], 64); out[n].is_dir = 1; out[n].size = 0; n++; }
                }
        }
    }
    unlock();
    return n;
}

int vfs_stat(const char *path, struct dunc_stat *st) {
    struct path p; char rel[PATH_MAX_];
    if (!parse(path, &p)) return FS_EINVAL;
    memset(st, 0, sizeof *st);
    lock();
    int r = FS_OK;
    struct mount *m = route(&p, rel, sizeof rel);
    if (m) { r = m->ops->stat(m->vol, rel, st); st->volume = 1 + (m - mounts); }
    else {
        struct vnode *v = walk(&p, p.n);
        if (!v) r = FS_ENOENT; else { st->is_dir = v->dir; st->size = v->size; }
    }
    unlock();
    return r;
}

long vfs_read(const char *path, void *buf, uint64_t off, uint64_t len) {
    struct path p; char rel[PATH_MAX_];
    if (!parse(path, &p)) return FS_EINVAL;
    lock();
    long r;
    struct mount *m = route(&p, rel, sizeof rel);
    if (m) r = m->ops->read(m->vol, rel, buf, off, len);
    else {
        struct vnode *v = walk(&p, p.n);
        if (!v) r = FS_ENOENT;
        else if (v->dir) r = FS_EISDIR;
        else {
            if (off >= v->size) r = 0;
            else { r = (long)(v->size - off < len ? v->size - off : len); memcpy(buf, v->data + off, r); }
        }
    }
    unlock();
    return r;
}

long vfs_write(const char *path, const void *buf, uint64_t len) {
    struct path p; char rel[PATH_MAX_];
    if (!parse(path, &p) || p.n == 0 || !valid_name(p.c[p.n - 1])) return FS_EINVAL;
    lock();
    long r;
    struct mount *m = route(&p, rel, sizeof rel);
    if (m) r = is_mount_root(m, &p) ? FS_EISDIR : m->ops->write(m->vol, rel, buf, len);
    else {
        struct vnode *d = walk(&p, p.n - 1);
        if (!d) r = FS_ENOENT;
        else if (!d->dir) r = FS_ENOTDIR;
        else r = ram_write(d, p.c[p.n - 1], buf, len);
    }
    if (r >= 0) { r = (long)len; notify_changed(); }
    unlock();
    return r;
}

long vfs_append(const char *path, const void *buf, uint64_t len) {
    struct dunc_stat st;
    if (vfs_stat(path, &st) == FS_ENOENT) return vfs_write(path, buf, len);
    if (st.is_dir) return FS_EISDIR;
    struct path p; char rel[PATH_MAX_];
    if (!parse(path, &p) || p.n == 0) return FS_EINVAL;
    lock();
    long r;
    struct mount *m = route(&p, rel, sizeof rel);
    if (m) r = m->ops->append ? m->ops->append(m->vol, rel, buf, len) : FS_EROFS;
    else {
        struct vnode *v = walk(&p, p.n);
        if (!v) r = FS_ENOENT;
        else {
            uint8_t *nd = kmalloc(v->size + len ? v->size + len : 1);
            if (!nd) r = FS_ENOSPC;
            else {
                memcpy(nd, v->data, v->size); memcpy(nd + v->size, buf, len);
                kfree(v->data); v->data = nd; v->size += len; v->cap = v->size; ram_used += len;
                r = (long)len;
            }
        }
    }
    if (r >= 0) notify_changed();
    unlock();
    return r;
}

int vfs_mkdir(const char *path) {
    struct path p; char rel[PATH_MAX_];
    if (!parse(path, &p) || p.n == 0 || !valid_name(p.c[p.n - 1])) return FS_EINVAL;
    lock();
    int r = FS_OK;
    struct mount *m = route(&p, rel, sizeof rel);
    if (m) r = is_mount_root(m, &p) ? FS_EEXIST : m->ops->mkdir(m->vol, rel);
    else {
        struct vnode *d = walk(&p, p.n - 1);
        if (!d) r = FS_ENOENT;
        else if (!d->dir) r = FS_ENOTDIR;
        else if (child(d, p.c[p.n - 1])) r = FS_EEXIST;
        else if (!mknode(d, p.c[p.n - 1], true)) r = FS_ENOSPC;
    }
    if (r == FS_OK && root && root->child) notify_changed();
    unlock();
    return r;
}

int vfs_delete(const char *path) {
    struct path p; char rel[PATH_MAX_];
    if (!parse(path, &p) || p.n == 0) return FS_EINVAL;
    lock();
    int r = FS_OK;
    struct mount *m = route(&p, rel, sizeof rel);
    if (m) {
        if (is_mount_root(m, &p)) r = FS_EINVAL;
        else {
            struct dunc_stat st;
            r = m->ops->stat(m->vol, rel, &st);
            if (r == FS_OK && st.is_dir) {          /* delete contents first */
                struct dunc_dirent *ents = kmalloc(sizeof(struct dunc_dirent) * 128);
                if (!ents) r = FS_ENOSPC;
                while (r == FS_OK) {
                    int n = m->ops->list(m->vol, rel, ents, 128);
                    if (n <= 0) { r = n < 0 ? n : FS_OK; break; }
                    for (int i = 0; i < n && r == FS_OK; i++) {
                        char sub[PATH_MAX_]; ksnprintf(sub, sizeof sub, "%s/%s", path, ents[i].name);
                        r = vfs_delete(sub);
                    }
                }
                kfree(ents);
            }
            if (r == FS_OK) r = m->ops->remove(m->vol, rel);
        }
    } else {
        struct vnode *v = walk(&p, p.n);
        if (!v) r = FS_ENOENT;
        else if (p.n == 1 && (!strcmp(v->name, "home") || !strcmp(v->name, "apps") || !strcmp(v->name, "system"))) r = FS_EROFS;
        else { unlink_node(v); free_tree(v); }
    }
    if (r == FS_OK) notify_changed();
    unlock();
    return r;
}

static int copy_tree(const char *from, const char *to) {
    struct dunc_stat st;
    int r = vfs_stat(from, &st);
    if (r) return r;
    if (!st.is_dir) {
        uint64_t len; void *data = vfs_read_all(from, &len);
        if (!data && len) return FS_EIO;
        long w = vfs_write(to, data ? data : "", len);
        kfree(data);
        return w < 0 ? (int)w : FS_OK;
    }
    r = vfs_mkdir(to);
    if (r && r != FS_EEXIST) return r;
    struct dunc_dirent *ents = kmalloc(sizeof(struct dunc_dirent) * 256);
    if (!ents) return FS_ENOSPC;
    int n = vfs_list(from, ents, 256);
    r = n < 0 ? n : FS_OK;
    for (int i = 0; i < n && r == FS_OK; i++) {
        char a[PATH_MAX_], b[PATH_MAX_];
        ksnprintf(a, sizeof a, "%s/%s", from, ents[i].name);
        ksnprintf(b, sizeof b, "%s/%s", to, ents[i].name);
        r = copy_tree(a, b);
    }
    kfree(ents);
    return r;
}

int vfs_copy(const char *from, const char *to) {
    struct dunc_stat st;
    if (vfs_stat(to, &st) == FS_OK) return FS_EEXIST;
    lock();
    int r = copy_tree(from, to);
    unlock();
    return r;
}

int vfs_rename(const char *from, const char *to) {
    struct path a, b; char ra[PATH_MAX_], rb[PATH_MAX_];
    if (!parse(from, &a) || !parse(to, &b) || a.n == 0 || b.n == 0 || !valid_name(b.c[b.n - 1])) return FS_EINVAL;
    if (b.n > a.n) {   /* can't move a folder into itself */
        bool inside = true;
        for (int i = 0; i < a.n; i++) if (strcmp(a.c[i], b.c[i])) inside = false;
        if (inside) return FS_EINVAL;
    }
    lock();
    int r;
    struct mount *ma = route(&a, ra, sizeof ra), *mb = route(&b, rb, sizeof rb);
    struct dunc_stat st;
    if (vfs_stat(to, &st) == FS_OK) r = FS_EEXIST;
    else if (is_mount_root(ma, &a) || is_mount_root(mb, &b)) r = FS_EROFS;
    else if (ma && ma == mb) r = ma->ops->rename(ma->vol, ra, rb);
    else if (!ma && !mb) {
        struct vnode *v = walk(&a, a.n), *d = walk(&b, b.n - 1);
        if (!v) r = FS_ENOENT;
        else if (!d || !d->dir) r = FS_ENOTDIR;
        else if (a.n == 1) r = FS_EROFS;
        else {
            unlink_node(v);
            strncpy0(v->name, b.c[b.n - 1], sizeof v->name);
            v->parent = d;
            struct vnode **pp = &d->child;   /* insert sorted: folders first, then by name */
            while (*pp && ((*pp)->dir > v->dir || ((*pp)->dir == v->dir && strcmp((*pp)->name, v->name) < 0))) pp = &(*pp)->next;
            v->next = *pp; *pp = v;
            r = FS_OK;
        }
    } else {   /* across volumes: copy, then delete the original */
        r = copy_tree(from, to);
        if (r == FS_OK) r = vfs_delete(from);
    }
    if (r == FS_OK) notify_changed();
    unlock();
    return r;
}

void *vfs_read_all(const char *path, uint64_t *len) {
    struct dunc_stat st;
    *len = 0;
    if (vfs_stat(path, &st) || st.is_dir) return 0;
    void *buf = kmalloc(st.size ? st.size : 1);
    if (!buf) return 0;
    long n = vfs_read(path, buf, 0, st.size);
    if (n < 0) { kfree(buf); return 0; }
    *len = n;
    return buf;
}

int vfs_mount(const char *label, const char *kind, const struct fs_ops *ops, void *vol, char *path_out) {
    lock();
    int slot = -1;
    for (int i = 0; i < NMOUNT; i++) if (!mounts[i].used) { slot = i; break; }
    if (slot < 0) { unlock(); return -1; }
    struct mount *m = &mounts[slot];
    memset(m, 0, sizeof *m);
    for (int k = 1; k <= NMOUNT; k++) {          /* first free name: usb, usb2, usb3 ... */
        ksnprintf(m->name, sizeof m->name, k == 1 ? "usb" : "usb%d", k);
        bool taken = false;
        for (int i = 0; i < NMOUNT; i++) if (i != slot && mounts[i].used && mounts[i].depth == 1 && !strcmp(mounts[i].at[0], m->name)) taken = true;
        if (!taken) break;
    }
    m->depth = 1; strncpy0(m->at[0], m->name, 64);
    strncpy0(m->label, label, sizeof m->label);
    strncpy0(m->kind, kind, sizeof m->kind);
    m->ops = ops; m->vol = vol; m->used = true;
    if (path_out) ksnprintf(path_out, 16, "/%s", m->name);
    notify_changed();
    unlock();
    return slot;
}

int vfs_mount_at(const char *at, const char *sub, const char *label, const char *kind, const struct fs_ops *ops, void *vol) {
    struct path p;
    if (!parse(at, &p) || p.n < 1 || p.n > 3) return -1;
    lock();
    int slot = -1;
    for (int i = 0; i < NMOUNT; i++) if (!mounts[i].used) { slot = i; break; }
    if (slot < 0) { unlock(); return -1; }
    struct mount *m = &mounts[slot];
    memset(m, 0, sizeof *m);
    m->depth = p.n; for (int k = 0; k < p.n; k++) strncpy0(m->at[k], p.c[k], 64);
    strncpy0(m->name, p.c[0], sizeof m->name);
    strncpy0(m->sub, sub && strcmp(sub, "/") ? sub : "", sizeof m->sub);
    strncpy0(m->label, label, sizeof m->label); strncpy0(m->kind, kind, sizeof m->kind);
    m->ops = ops; m->vol = vol; m->used = true; m->system = true;
    notify_changed();
    unlock();
    return slot;
}

void vfs_unmount_at(const char *at) {
    struct path p;
    if (!parse(at, &p)) return;
    lock();
    for (int i = 0; i < NMOUNT; i++) {
        struct mount *m = &mounts[i];
        if (!m->used || m->depth != p.n) continue;
        bool match = true;
        for (int k = 0; k < p.n; k++) if (strcmp(m->at[k], p.c[k])) match = false;
        if (match) m->used = false;
    }
    notify_changed();
    unlock();
}

void vfs_unmount(void *vol) {
    lock();
    for (int i = 0; i < NMOUNT; i++) if (mounts[i].used && mounts[i].vol == vol) mounts[i].used = false;
    notify_changed();
    unlock();
}

int vfs_volumes(struct dunc_volume *out, int max) {
    int n = 0;
    struct mount *home = 0;
    for (int i = 0; i < NMOUNT; i++) if (mounts[i].used && mounts[i].system && mounts[i].depth == 1 && !strcmp(mounts[i].at[0], "home")) home = &mounts[i];
    if (n < max) {
        strncpy0(out[n].path, "/home", 16);
        out[n].writable = 1;
        if (home) {
            strncpy0(out[n].label, "DuncOS", 32); strncpy0(out[n].kind, home->kind, 16);
            out[n].total = out[n].free = 0;
            if (home->ops->space) home->ops->space(home->vol, &out[n].total, &out[n].free);
        } else {
            strncpy0(out[n].label, "DuncOS", 32); strncpy0(out[n].kind, "Memory", 16);
            out[n].total = mem_total(); out[n].free = mem_total() - mem_used();
        }
        n++;
    }
    for (int i = 0; i < NMOUNT && n < max; i++) {
        if (!mounts[i].used || mounts[i].system) continue;
        ksnprintf(out[n].path, 16, "/%s", mounts[i].name);
        strncpy0(out[n].label, mounts[i].label, 32);
        strncpy0(out[n].kind, mounts[i].kind, 16);
        out[n].total = out[n].free = 0;
        if (mounts[i].ops->space) mounts[i].ops->space(mounts[i].vol, &out[n].total, &out[n].free);
        out[n].writable = 1;
        n++;
    }
    return n;
}

bool vfs_home_persistent(void) {
    for (int i = 0; i < NMOUNT; i++) if (mounts[i].used && mounts[i].system && mounts[i].depth == 1 && !strcmp(mounts[i].at[0], "home")) return true;
    return false;
}

/* ---------------- initial files (tar archive loaded by GRUB as a module) ---------------- */
static uint64_t octal(const char *s, int n) { uint64_t v = 0; for (int i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++) v = v * 8 + (s[i] - '0'); return v; }

void initrd_load(const uint8_t *p, const uint8_t *end) {
    int files = 0;
    while (p + 512 <= end && p[0]) {
        const char *h = (const char *)p;
        uint64_t size = octal(h + 124, 12);
        char name[PATH_MAX_], full[PATH_MAX_ + 2];
        if (!memcmp(h + 257, "ustar", 5) && h[345]) ksnprintf(name, sizeof name, "%s/%s", h + 345, h);
        else { memcpy(name, h, 100); name[100] = 0; }
        const char *nm = name;
        while (*nm == '.' || *nm == '/') nm++;
        size_t l = strlen(nm);
        ksnprintf(full, sizeof full, "/%s", nm);
        if (l && full[strlen(full) - 1] == '/') full[strlen(full) - 1] = 0;
        char type = h[156];
        if (*nm) {
            if (type == '5') vfs_mkdir(full);
            else if (type == '0' || type == 0) { if (vfs_write(full, p + 512, size) >= 0) files++; }
        }
        p += 512 + ((size + 511) & ~511ULL);
    }
    LOG_OK("Files: %d loaded into the system disk\n", files);
}
