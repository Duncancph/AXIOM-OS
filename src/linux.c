/* Linux compatibility: programs built for Linux (x86-64, static or position-independent) run on AXIOM. They
 * make Linux system calls with the syscall instruction; this turns the ones ordinary programs use into
 * AXIOM's own: memory maps, threads (clone), futexes, time, randomness, files on AXIOM's volumes and
 * output to the terminal that started them. */
#include "kernel.h"
#include "vfs.h"

/* ---------------- the syscall instruction ---------------- */
extern void syscall_entry(void);
static inline uint64_t rdmsr(uint32_t m) { uint32_t lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m)); return (uint64_t)hi << 32 | lo; }
static inline void wrmsr(uint32_t m, uint64_t v) { __asm__ volatile("wrmsr" ::"c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32))); }
void linux_cpu_init(void) {
    wrmsr(0xC0000080, rdmsr(0xC0000080) | 1);              /* EFER.SCE */
    wrmsr(0xC0000081, (0x10ULL << 48) | (0x08ULL << 32));    /* STAR: sysret base 0x10 (user data 0x18, code 0x20), kernel 0x08 */
    wrmsr(0xC0000082, (uint64_t)syscall_entry);             /* LSTAR */
    wrmsr(0xC0000084, 0x700);                               /* FMASK: interrupts, direction and trap flags off on entry */
}

/* ---------------- a Linux program's state ---------------- */
enum { FD_NONE, FD_TTY_IN, FD_TTY_OUT, FD_FILE, FD_DIR };
struct lfd { int kind; int flags; uint64_t off; char path[200]; };
#define NFD 128
struct lxproc {
    struct lfd fd[NFD];
    char cwd[200];
    struct task *out;              /* the terminal task that started it (its output goes there), or 0 */
    int out_pid;
    char line[256]; int linelen;   /* output waiting for its end of line (for the log) */
};

uint64_t linux_now_ns(void);
#define E(x) ((uint64_t)-(long)(x))
enum { EPERM_ = 1, ENOENT_ = 2, EBADF_ = 9, EAGAIN_ = 11, ENOMEM_ = 12, EFAULT_ = 14, EEXIST_ = 17, ENOTDIR_ = 20, EISDIR_ = 21,
       EINVAL_ = 22, EMFILE_ = 24, ENOTTY_ = 25, ESPIPE_ = 29, ERANGE_ = 34, ENOSYS_ = 38, ETIMEDOUT_ = 110 };

static bool uok(uint64_t p, uint64_t n, bool w) { return n == 0 || user_ok(current->pml4, (const void *)p, n, w); }
static bool ustr(char *dst, uint64_t src, size_t max) {
    for (size_t i = 0; i + 1 < max; i++) {
        if ((i == 0 || ((src + i) & 0xFFF) == 0) && !uok(src + i, 1, false)) return false;
        dst[i] = ((const char *)src)[i];
        if (!dst[i]) return true;
    }
    return false;
}
static void path_join(const char *cwd, const char *p, char *out, size_t n) {
    char tmp[400];
    if (p[0] == '/') strncpy0(tmp, p, sizeof tmp);
    else ksnprintf(tmp, sizeof tmp, "%s/%s", cwd, p);
    /* tidy: //, ., .. */
    char *parts[64]; int np = 0;
    for (char *s = tmp; *s;) {
        while (*s == '/') *s++ = 0;
        if (!*s) break;
        char *st = s; while (*s && *s != '/') s++;
        if (*s) *s++ = 0;
        if (!strcmp(st, ".")) continue;
        if (!strcmp(st, "..")) { if (np) np--; continue; }
        if (np < 64) parts[np++] = st;
    }
    size_t k = 0;
    if (!np) { strncpy0(out, "/", n); return; }
    for (int i = 0; i < np && k + 2 < n; i++) { out[k++] = '/'; for (const char *c = parts[i]; *c && k + 1 < n; c++) out[k++] = *c; }
    out[k] = 0;
}

/* output: to the terminal that started the program (while it waits for it), and to the log by lines */
static void out_write(struct lxproc *lx, const char *s, uint64_t n) {
    struct task *o = lx->out;
    if (o && o->state && o->state != PS_ZOMBIE && o->pid == lx->out_pid && o->capture) {
        for (uint64_t i = 0; i < n && o->cap_len < o->cap_max; i++) o->capture[o->cap_len++] = s[i];
    }
    for (uint64_t i = 0; i < n; i++) {
        if (s[i] == '\n' || lx->linelen >= (int)sizeof lx->line - 1) {
            lx->line[lx->linelen] = 0;
            kprintf("[%s] %s\n", current->proc ? current->proc->name : "linux", lx->line);
            lx->linelen = 0;
            if (s[i] == '\n') continue;
        }
        lx->line[lx->linelen++] = s[i];
    }
}

/* ---------------- loading a program ---------------- */
struct ehdr { uint8_t id[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff;
              uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } __attribute__((packed));
struct phdr { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } __attribute__((packed));

/* copy into another program's memory (its area pages are made as they are reached) */
static bool copy_to(struct mm *m, uint64_t va, const void *src, uint64_t n) {
    const uint8_t *s = src;
    while (n) {
        if (!mm_fault(m, va, true, false, 0)) return false;
        uint64_t chunk = 4096 - (va & 4095); if (chunk > n) chunk = n;
        memcpy((void *)vm_phys(m->pml4, va), s, chunk);
        va += chunk; s += chunk; n -= chunk;
    }
    return true;
}
/* map a position-independent ELF at base: its segments become memory areas with their permissions */
static bool load_elf(struct mm *m, const uint8_t *img, uint64_t size, uint64_t base, uint64_t *entry, uint64_t *phdr, int *phnum, char *interp, size_t ilen) {
    const struct ehdr *e = (const void *)img;
    if (size < sizeof *e || memcmp(e->id, "\x7F" "ELF", 4) || e->id[4] != 2 || e->machine != 62) return false;
    if (e->type != 3) return false;                           /* ET_DYN (position independent) only */
    if (interp) interp[0] = 0;
    uint64_t lo = ~0ULL; *phdr = 0;
    for (int i = 0; i < e->phnum; i++) {
        const struct phdr *ph = (const void *)(img + e->phoff + i * e->phentsize);
        if (ph->type == 1 && ph->vaddr < lo) lo = ph->vaddr;
        if (ph->type == 6) *phdr = base + ph->vaddr;          /* PT_PHDR */
        if (ph->type == 3 && interp && ph->offset + ph->filesz <= size) {   /* PT_INTERP */
            uint64_t l = ph->filesz < ilen - 1 ? ph->filesz : ilen - 1;
            memcpy(interp, img + ph->offset, l); interp[l] = 0;
        }
    }
    for (int i = 0; i < e->phnum; i++) {
        const struct phdr *ph = (const void *)(img + e->phoff + i * e->phentsize);
        if (ph->type != 1) continue;
        if (ph->offset + ph->filesz > size) return false;
        uint64_t s = (base + ph->vaddr) & ~4095ULL, end = (base + ph->vaddr + ph->memsz + 4095) & ~4095ULL;
        if (mm_mmap(m, s, end - s, DPROT_READ | DPROT_WRITE, DMAP_FIXED, 0) < 0) return false;
        if (!copy_to(m, base + ph->vaddr, img + ph->offset, ph->filesz)) return false;
        int prot = (ph->flags & 4 ? DPROT_READ : 0) | (ph->flags & 2 ? DPROT_WRITE : 0) | (ph->flags & 1 ? DPROT_EXEC : 0);
        mm_mprotect(m, s, end - s, prot);
        if (!*phdr && ph->offset == 0) *phdr = base + ph->vaddr + e->phoff;
    }
    (void)lo;
    *entry = base + e->entry; *phnum = e->phnum;
    return true;
}

#define MAIN_BASE   0xC010000000ULL
#define INTERP_BASE 0xC800000000ULL
#define STACK_SIZE  (8ULL << 20)

int linux_spawn(const char *path, int argc, char **argv, struct task *out) {
    uint64_t len; uint8_t *img = vfs_read_all(path, &len);
    if (!img) { kprintf("run: can't read %s\n", path); return -1; }
    struct mm *m = mm_new();
    struct lxproc *lx = kmalloc(sizeof *lx);
    if (!m || !lx) { kfree(img); if (m) mm_put(m); kfree(lx); return -1; }
    memset(lx, 0, sizeof *lx);
    uint64_t entry, phdr; int phnum; char interp[128];
    if (!load_elf(m, img, len, MAIN_BASE, &entry, &phdr, &phnum, interp, sizeof interp)) {
        const struct ehdr *e = (const void *)img;
        kprintf("run: %s isn't a program AXIOM can run%s\n", path, len >= sizeof *e && e->type == 2 ? " (built at a fixed address: build it position-independent, e.g. -static-pie)" : "");
        kfree(img); mm_put(m); kfree(lx); return -1;
    }
    kfree(img);
    uint64_t start = entry, ibase = 0;
    if (interp[0]) {                                           /* a dynamically linked program: its loader first */
        uint64_t il; uint8_t *ii = vfs_read_all(interp, &il);
        uint64_t ie, iph; int ipn;
        if (!ii || !load_elf(m, ii, il, INTERP_BASE, &ie, &iph, &ipn, 0, 0)) { kprintf("run: needs %s, which isn't there\n", interp); kfree(ii); mm_put(m); kfree(lx); return -1; }
        kfree(ii); start = ie; ibase = INTERP_BASE;
    }
    /* the stack: 8 MiB at the top of the map area, then argc, argv, envp and the auxiliary vector */
    uint64_t stop = USER_MMAP_END, sbase = stop - STACK_SIZE;
    mm_mmap(m, sbase, STACK_SIZE, DPROT_READ | DPROT_WRITE, DMAP_FIXED, 0);
    uint64_t sp = stop;
    uint64_t argp[16]; int na = argc < 16 ? argc : 16;
    for (int i = na - 1; i >= 0; i--) { uint64_t l = strlen(argv[i]) + 1; sp -= l; copy_to(m, sp, argv[i], l); argp[i] = sp; }
    static const char *envs[] = { "HOME=/home", "PATH=/bin:/usr/bin", "LANG=en_GB.UTF-8", "TERM=dumb", "USER=user" };
    uint64_t envp[5];
    for (int i = 4; i >= 0; i--) { uint64_t l = strlen(envs[i]) + 1; sp -= l; copy_to(m, sp, envs[i], l); envp[i] = sp; }
    uint8_t rnd[16]; random_bytes(rnd, 16); sp -= 16; copy_to(m, sp, rnd, 16); uint64_t rndp = sp;
    sp -= strlen(path) + 1; copy_to(m, sp, path, strlen(path) + 1); uint64_t execfn = sp;
    uint64_t v[96]; int k = 0;
    v[k++] = na;
    for (int i = 0; i < na; i++) v[k++] = argp[i];
    v[k++] = 0;
    for (int i = 0; i < 5; i++) v[k++] = envp[i];
    v[k++] = 0;
    uint64_t aux[][2] = { { 3, phdr }, { 4, 56 }, { 5, (uint64_t)phnum }, { 6, 4096 }, { 7, ibase }, { 8, 0 }, { 9, entry },
                          { 11, 1000 }, { 12, 1000 }, { 13, 1000 }, { 14, 1000 }, { 16, 0 }, { 17, 100 }, { 23, 0 }, { 25, rndp }, { 31, execfn }, { 0, 0 } };
    for (unsigned i = 0; i < sizeof aux / sizeof aux[0]; i++) { v[k++] = aux[i][0]; v[k++] = aux[i][1]; }
    sp = (sp - k * 8) & ~15ULL;
    copy_to(m, sp, v, k * 8);
    /* files: 0 = input (nothing yet), 1 and 2 = the terminal */
    lx->fd[0].kind = FD_TTY_IN; lx->fd[1].kind = FD_TTY_OUT; lx->fd[2].kind = FD_TTY_OUT;
    strncpy0(lx->cwd, "/home", sizeof lx->cwd);
    lx->out = out; lx->out_pid = out ? out->pid : 0;
    const char *nm = path; for (const char *q = path; *q; q++) if (*q == '/') nm = q + 1;
    struct task *t = task_new_user(nm, m, 0, start, sp);
    mm_put(m);                                                 /* (the task holds it now) */
    if (!t) { kfree(lx); return -1; }
    t->lx = lx;
    strncpy0(t->appid, "linux", sizeof t->appid); t->app = t->appid;
    kprintf("[linux] started %s (pid %d)%s%s\n", path, t->pid, interp[0] ? " with " : "", interp);
    return t->pid;
}
void linux_task_exit(struct task *t) {
    struct lxproc *lx = t->lx;
    if (!lx) return;
    if (lx->linelen) { lx->line[lx->linelen] = 0; kprintf("[%s] %s\n", t->name, lx->line); }
    for (int i = 0; i < MAX_TASKS; i++) if (tasks[i].lx == lx) tasks[i].lx = 0;
    kfree(lx);
}

/* ---------------- the system calls ---------------- */
struct lstat { uint64_t dev, ino, nlink; uint32_t mode, uid, gid, pad; uint64_t rdev; int64_t size, blksize, blocks; uint64_t at[2], mt[2], ct[2]; int64_t r[3]; };
static void fill_stat(struct lstat *st, bool dir, uint64_t size, bool tty) {
    memset(st, 0, sizeof *st);
    st->dev = 1; st->ino = 1; st->nlink = 1; st->uid = st->gid = 1000;
    st->mode = tty ? 0020620 : dir ? 0040755 : 0100644;
    st->size = size; st->blksize = 4096; st->blocks = (size + 511) / 512;
    uint64_t now = (uint64_t)(linux_now_ns() / 1000000000ull);
    st->at[0] = st->mt[0] = st->ct[0] = now;
}
static int fd_new(struct lxproc *lx, int from) { for (int i = from; i < NFD; i++) if (!lx->fd[i].kind) return i; return -1; }
static uint64_t ts_ms(uint64_t uts, bool *forever) {             /* a struct timespec (relative) in ms */
    if (!uts) { *forever = true; return 0; }
    *forever = false;
    if (!uok(uts, 16, false)) return 0;
    int64_t s = ((int64_t *)uts)[0], ns = ((int64_t *)uts)[1];
    if (s < 0) return 0;
    return (uint64_t)s * 1000 + ((uint64_t)ns + 999999) / 1000000;
}

static uint64_t do_open(struct lxproc *lx, int dirfd, uint64_t upath, int flags) {
    char p[200], full[200];
    if (!ustr(p, upath, sizeof p)) return E(EFAULT_);
    (void)dirfd;
    path_join(lx->cwd, p, full, sizeof full);
    struct dunc_stat st;
    bool exists = vfs_stat(full, &st) == 0;
    int acc = flags & 3;
    if (!exists) {
        if (!(flags & 0100)) return E(ENOENT_);                 /* O_CREAT */
        if (vfs_write(full, "", 0) < 0) return E(ENOENT_);
        st.is_dir = 0; st.size = 0;
    } else if ((flags & 0300) == 0300) return E(EEXIST_);         /* O_CREAT|O_EXCL */
    if (exists && st.is_dir && acc) return E(EISDIR_);
    if ((flags & 0200000) && !st.is_dir) return E(ENOTDIR_);      /* O_DIRECTORY */
    if (acc && (flags & 01000)) vfs_write(full, "", 0);            /* O_TRUNC */
    int fd = fd_new(lx, 0);
    if (fd < 0) return E(EMFILE_);
    struct lfd *f = &lx->fd[fd];
    f->kind = st.is_dir ? FD_DIR : FD_FILE; f->flags = flags; f->off = 0;
    strncpy0(f->path, full, sizeof f->path);
    if (flags & 02000) { struct dunc_stat s2; if (vfs_stat(full, &s2) == 0) f->off = s2.size; }   /* O_APPEND */
    return fd;
}

uint64_t linux_syscall(struct frame *f) {
    uint64_t n = f->rax, a = f->rdi, b = f->rsi, c = f->rdx, d = f->r10, e = f->r8;
    struct task *t = current;
    struct lxproc *lx = t->lx;
    if (!lx) return E(ENOSYS_);
    switch (n) {
    case 0: case 17: {                                         /* read, pread64 */
        if (a >= NFD || !lx->fd[a].kind) return E(EBADF_);
        struct lfd *fd = &lx->fd[a];
        if (fd->kind == FD_TTY_IN) return 0;                     /* (no input yet: end of file) */
        if (fd->kind != FD_FILE) return E(EISDIR_);
        if (!uok(b, c, true)) return E(EFAULT_);
        uint64_t off = n == 17 ? d : fd->off;
        long r = vfs_read(fd->path, (void *)b, off, c);
        if (r < 0) return E(EINVAL_);
        if (n == 0) fd->off += r;
        return r;
    }
    case 1: case 18: {                                         /* write, pwrite64 */
        if (a >= NFD || !lx->fd[a].kind) return E(EBADF_);
        if (!uok(b, c, false)) return E(EFAULT_);
        struct lfd *fd = &lx->fd[a];
        if (fd->kind == FD_TTY_OUT) { out_write(lx, (const char *)b, c); return c; }
        if (fd->kind != FD_FILE || !(fd->flags & 3)) return E(EBADF_);
        struct dunc_stat st; uint64_t off = n == 18 ? d : fd->off;
        if (vfs_stat(fd->path, &st) == 0 && off == st.size) { if (vfs_append(fd->path, (const void *)b, c) < 0) return E(EINVAL_); }
        else {                                                  /* writing inside the file: read, change, write back */
            uint64_t sz = st.size > off + c ? st.size : off + c;
            uint8_t *buf = kmalloc(sz ? sz : 1); if (!buf) return E(ENOMEM_);
            memset(buf, 0, sz); vfs_read(fd->path, buf, 0, st.size);
            memcpy(buf + off, (const void *)b, c);
            long r = vfs_write(fd->path, buf, sz); kfree(buf);
            if (r < 0) return E(EINVAL_);
        }
        if (n == 1) fd->off += c;
        return c;
    }
    case 19: case 20: {                                        /* readv, writev */
        if (!uok(b, c * 16, false) || c > 1024) return E(EFAULT_);
        uint64_t total = 0;
        for (uint64_t i = 0; i < c; i++) {
            uint64_t base = ((uint64_t *)b)[i * 2], len = ((uint64_t *)b)[i * 2 + 1];
            if (!len) continue;
            struct frame g = *f; g.rax = n == 19 ? 0 : 1; g.rdi = a; g.rsi = base; g.rdx = len;
            uint64_t r = linux_syscall(&g);
            if ((int64_t)r < 0) return total ? total : r;
            total += r;
            if (r < len) break;
        }
        return total;
    }
    case 2: return do_open(lx, -100, a, (int)b);                 /* open */
    case 257: return do_open(lx, (int)a, b, (int)c);             /* openat */
    case 3:                                                    /* close */
        if (a >= NFD || !lx->fd[a].kind) return E(EBADF_);
        lx->fd[a].kind = FD_NONE; return 0;
    case 8: {                                                  /* lseek */
        if (a >= NFD || !lx->fd[a].kind) return E(EBADF_);
        struct lfd *fd = &lx->fd[a];
        if (fd->kind != FD_FILE) return E(ESPIPE_);
        struct dunc_stat st; uint64_t size = vfs_stat(fd->path, &st) == 0 ? st.size : 0;
        int64_t no = c == 0 ? (int64_t)b : c == 1 ? (int64_t)fd->off + (int64_t)b : c == 2 ? (int64_t)size + (int64_t)b : -1;
        if (no < 0) return E(EINVAL_);
        fd->off = no; return no;
    }
    case 5: {                                                  /* fstat */
        if (a >= NFD || !lx->fd[a].kind) return E(EBADF_);
        if (!uok(b, sizeof(struct lstat), true)) return E(EFAULT_);
        struct lfd *fd = &lx->fd[a]; struct dunc_stat st = { 0 };
        if (fd->kind == FD_FILE || fd->kind == FD_DIR) vfs_stat(fd->path, &st);
        fill_stat((struct lstat *)b, fd->kind == FD_DIR, st.size, fd->kind == FD_TTY_IN || fd->kind == FD_TTY_OUT);
        return 0;
    }
    case 4: case 6: case 262: {                                /* stat, lstat, newfstatat */
        uint64_t up = n == 262 ? b : a, us = n == 262 ? c : b;
        char p[200], full[200];
        if (n == 262 && (d & 0x1000) && (!up || !uok(up, 1, false) || !*(char *)up)) {   /* AT_EMPTY_PATH: fstat */
            struct frame g = *f; g.rax = 5; g.rdi = a; g.rsi = c; return linux_syscall(&g);
        }
        if (!ustr(p, up, sizeof p) || !uok(us, sizeof(struct lstat), true)) return E(EFAULT_);
        path_join(lx->cwd, p, full, sizeof full);
        struct dunc_stat st;
        if (vfs_stat(full, &st) != 0) return E(ENOENT_);
        fill_stat((struct lstat *)us, st.is_dir, st.size, false);
        return 0;
    }
    case 21: case 269: {                                       /* access, faccessat */
        char p[200], full[200];
        if (!ustr(p, n == 21 ? a : b, sizeof p)) return E(EFAULT_);
        path_join(lx->cwd, p, full, sizeof full);
        struct dunc_stat st;
        return vfs_stat(full, &st) == 0 ? 0 : E(ENOENT_);
    }
    case 79: {                                                 /* getcwd */
        uint64_t l = strlen(lx->cwd) + 1;
        if (b < l) return E(ERANGE_);
        if (!uok(a, l, true)) return E(EFAULT_);
        memcpy((void *)a, lx->cwd, l); return l;
    }
    case 80: {                                                 /* chdir */
        char p[200], full[200]; struct dunc_stat st;
        if (!ustr(p, a, sizeof p)) return E(EFAULT_);
        path_join(lx->cwd, p, full, sizeof full);
        if (vfs_stat(full, &st) != 0) return E(ENOENT_);
        if (!st.is_dir) return E(ENOTDIR_);
        strncpy0(lx->cwd, full, sizeof lx->cwd); return 0;
    }
    case 217: {                                                /* getdents64 */
        if (a >= NFD || lx->fd[a].kind != FD_DIR) return E(ENOTDIR_);
        if (!uok(b, c, true)) return E(EFAULT_);
        struct lfd *fd = &lx->fd[a];
        static struct dunc_dirent ents[256];
        int ne = vfs_list(fd->path, ents, 256);
        uint64_t pos = 0;
        while ((int)fd->off < ne) {
            const struct dunc_dirent *de = &ents[fd->off];
            uint64_t nl = strlen(de->name), rl = (19 + nl + 1 + 7) & ~7ULL;
            if (pos + rl > c) break;
            uint8_t *r = (uint8_t *)b + pos;
            *(uint64_t *)r = fd->off + 2; *(int64_t *)(r + 8) = fd->off + 1; *(uint16_t *)(r + 16) = (uint16_t)rl;
            r[18] = de->is_dir ? 4 : 8; memcpy(r + 19, de->name, nl + 1);
            pos += rl; fd->off++;
        }
        return pos;
    }
    case 16:                                                   /* ioctl: no terminal control */
        if (a < NFD && (lx->fd[a].kind == FD_TTY_IN || lx->fd[a].kind == FD_TTY_OUT) && b == 0x5413 && uok(c, 8, true)) {   /* TIOCGWINSZ */
            uint16_t *w = (uint16_t *)c; w[0] = 40; w[1] = 100; w[2] = w[3] = 0; return 0;
        }
        return E(ENOTTY_);
    case 72: return 0;                                         /* fcntl */
    case 32: { if (a >= NFD || !lx->fd[a].kind) return E(EBADF_); int k = fd_new(lx, 0); if (k < 0) return E(EMFILE_); lx->fd[k] = lx->fd[a]; return k; }   /* dup */
    case 33: case 292:                                         /* dup2, dup3 */
        if (a >= NFD || !lx->fd[a].kind || b >= NFD) return E(EBADF_);
        if (a != b) lx->fd[b] = lx->fd[a];
        return b;
    case 89: case 267: return E(EINVAL_);                       /* readlink(at): no links */
    case 83: case 258: {                                       /* mkdir, mkdirat */
        char p[200], full[200];
        if (!ustr(p, n == 83 ? a : b, sizeof p)) return E(EFAULT_);
        path_join(lx->cwd, p, full, sizeof full);
        return vfs_mkdir(full) == 0 ? 0 : E(EEXIST_);
    }
    case 87: case 263: case 84: {                              /* unlink, unlinkat, rmdir */
        char p[200], full[200];
        if (!ustr(p, n == 263 ? b : a, sizeof p)) return E(EFAULT_);
        path_join(lx->cwd, p, full, sizeof full);
        return vfs_delete(full) == 0 ? 0 : E(ENOENT_);
    }
    case 82: case 264: case 316: {                             /* rename, renameat(2) */
        char p[200], q[200], f1[200], f2[200];
        if (!ustr(p, n == 82 ? a : b, sizeof p) || !ustr(q, n == 82 ? b : d, sizeof q)) return E(EFAULT_);
        path_join(lx->cwd, p, f1, sizeof f1); path_join(lx->cwd, q, f2, sizeof f2);
        return vfs_rename(f1, f2) == 0 ? 0 : E(ENOENT_);
    }

    /* ---- memory ---- */
    case 9: {                                                  /* mmap */
        int prot = (int)c & 7, fl = (int)d; long fd = (long)e; uint64_t off = f->r9;
        int dfl = (fl & 0x10) ? DMAP_FIXED : 0;               /* MAP_FIXED */
        if (fl & 0x100000) {                                   /* MAP_FIXED_NOREPLACE: as a hint, then check */
            long r = mm_mmap(t->mm, a, b, prot, 0, &t->proc->pages);
            if (r >= 0 && (uint64_t)r != (a & ~4095ULL)) { mm_munmap(t->mm, r, b, &t->proc->pages); return E(EEXIST_); }
            return r < 0 ? E(ENOMEM_) : (uint64_t)r;
        }
        if (!(fl & 0x20)) {                                    /* a file: read into private memory */
            if (fd < 0 || fd >= NFD || lx->fd[fd].kind != FD_FILE) return E(EBADF_);
            long r = mm_mmap(t->mm, a, b, DPROT_READ | DPROT_WRITE, dfl, &t->proc->pages);
            if (r < 0) return E(ENOMEM_);
            uint8_t *tmp = kmalloc(65536);
            if (!tmp) return E(ENOMEM_);
            for (uint64_t done = 0; done < b;) {
                uint64_t want = b - done < 65536 ? b - done : 65536;
                long k = vfs_read(lx->fd[fd].path, tmp, off + done, want);
                if (k <= 0) break;
                if (!uok(r + done, k, true)) break;
                memcpy((void *)(r + done), tmp, k);
                done += k;
                if ((uint64_t)k < want) break;
            }
            kfree(tmp);
            if (prot != (DPROT_READ | DPROT_WRITE)) mm_mprotect(t->mm, r, b, prot);
            return r;
        }
        long r = mm_mmap(t->mm, a, b, prot, dfl, &t->proc->pages);
        return r < 0 ? (uint64_t)r : (uint64_t)r;
    }
    case 11: { long r = mm_munmap(t->mm, a, b, &t->proc->pages); return r < 0 ? (uint64_t)r : 0; }   /* munmap */
    case 10: { long r = mm_mprotect(t->mm, a, b, (int)c & 7); return r < 0 ? (uint64_t)r : 0; }       /* mprotect */
    case 28:                                                   /* madvise: DONTNEED frees the pages (they come back zeroed) */
        if (c == 4) mm_dontneed(t->mm, a, b, &t->proc->pages);
        return 0;
    case 25: return E(ENOMEM_);                                /* mremap: callers fall back */
    case 12: {                                                 /* brk */
        struct task *P = t->proc;
        if (!P->heap_top) P->heap_top = USER_HEAP;
        if (a <= P->heap_top) return P->heap_top;
        uint64_t want = (a + 4095) & ~4095ULL;
        if (want - USER_HEAP > (1024ull << 20)) return P->heap_top;
        long k = vm_alloc(t->pml4, P->heap_top, want - P->heap_top);
        if (k < 0) return P->heap_top;
        P->pages += k; P->heap_top = want;
        return a;
    }

    /* ---- threads, the process ---- */
    case 56: {                                                 /* clone: threads (CLONE_VM) */
        uint64_t flags = a;
        if (!(flags & 0x100)) return E(ENOSYS_);               /* a new process (fork): not yet */
        long tid = task_clone(f, b ? b : f->rsp, (flags & 0x80000) ? e : t->fs_base, (flags & 0x200000) ? d : 0);
        if (tid < 0) return (uint64_t)tid;
        if ((flags & 0x100000) && uok(c, 4, true)) *(uint32_t *)c = (uint32_t)tid;     /* CLONE_PARENT_SETTID */
        if ((flags & 0x1000000) && uok(d, 4, true)) *(uint32_t *)d = (uint32_t)tid;    /* CLONE_CHILD_SETTID */
        return tid;
    }
    case 435: return E(ENOSYS_);                               /* clone3: callers fall back to clone */
    case 218: t->clear_tid = a; return t->pid;                 /* set_tid_address */
    case 158:                                                  /* arch_prctl */
        if (a == 0x1002) { t->fs_base = b; wrmsr(0xC0000100, b); return 0; }
        if (a == 0x1003) { if (!uok(b, 8, true)) return E(EFAULT_); *(uint64_t *)b = t->fs_base; return 0; }
        return E(EINVAL_);
    case 60: task_thread_exit();                               /* exit (this thread) */
    case 231: task_exit((int)a);                               /* exit_group */
    case 39: return t->proc->pid;                              /* getpid */
    case 186: return t->pid;                                   /* gettid */
    case 110: return 1;                                        /* getppid */
    case 102: case 104: case 107: case 108: return 1000;       /* getuid, getgid, geteuid, getegid */
    case 112: return t->proc->pid;                             /* setsid */
    case 62: case 200: case 234:                               /* kill, tkill, tgkill: ending itself */
        if (n == 62 && a == (uint64_t)t->proc->pid && (b == 6 || b == 9 || b == 15)) task_exit(128 + (int)b);
        if (n != 62 && (c == 6 || c == 9)) task_exit(128 + (int)c);
        return 0;
    case 13: case 14: case 131: case 127: return 0;            /* rt_sigaction, rt_sigprocmask, sigaltstack, rt_sigpending */
    case 15: return 0;                                         /* rt_sigreturn */
    case 24: schedule(); return 0;                             /* sched_yield */
    case 204: {                                                /* sched_getaffinity */
        if (b < 8 || !uok(c, 8, true)) return E(EINVAL_);
        uint64_t mask = ncpus_on >= 64 ? ~0ULL : (1ULL << ncpus_on) - 1;
        memset((void *)c, 0, b < 128 ? b : 128); *(uint64_t *)c = mask; return 8;
    }
    case 203: return 0;                                        /* sched_setaffinity */
    case 302: {                                                /* prlimit64 */
        if (d && uok(d, 16, true)) {
            uint64_t cur = ~0ULL, max = ~0ULL;
            if (b == 3) cur = STACK_SIZE;                      /* RLIMIT_STACK */
            if (b == 7) cur = max = NFD;                       /* RLIMIT_NOFILE */
            ((uint64_t *)d)[0] = cur; ((uint64_t *)d)[1] = max;
        }
        return 0;
    }
    case 97: if (uok(b, 16, true)) { ((uint64_t *)b)[0] = a == 3 ? STACK_SIZE : ~0ULL; ((uint64_t *)b)[1] = ~0ULL; } return 0;   /* getrlimit */
    case 63: {                                                 /* uname */
        if (!uok(a, 390, true)) return E(EFAULT_);
        char *u = (char *)a; memset(u, 0, 390);
        strncpy0(u, "Linux", 65); strncpy0(u + 65, "axiom", 65); strncpy0(u + 130, "6.1.0-axiom", 65);
        ksnprintf(u + 195, 65, "AXIOM build %d", OS_BUILD); strncpy0(u + 260, "x86_64", 65);
        return 0;
    }
    case 99: {                                                 /* sysinfo */
        if (!uok(a, 112, true)) return E(EFAULT_);
        uint64_t *s = (uint64_t *)a; memset(s, 0, 112);
        s[0] = uptime_ms() / 1000; s[4] = mem_total(); s[5] = mem_total() - mem_used();
        *(uint32_t *)((char *)a + 104) = 1;
        return 0;
    }
    case 318: {                                                /* getrandom */
        if (!uok(a, b, true)) return E(EFAULT_);
        random_bytes((void *)a, b); return b;
    }
    case 334: case 324: case 273: case 332: return E(ENOSYS_); /* rseq, membarrier, set_robust_list, statx */
    case 157: return 0;                                        /* prctl */

    /* ---- time ---- */
    case 228: {                                                /* clock_gettime */
        if (!uok(b, 16, true)) return E(EFAULT_);
        uint64_t ns = a == 0 || a == 5 || a == 8 ? linux_now_ns() : (a == 2 || a == 3) ? (cpu_time_us(), t->ticks * 1000) : uptime_ns();
        ((uint64_t *)b)[0] = ns / 1000000000ull; ((uint64_t *)b)[1] = ns % 1000000000ull;
        return 0;
    }
    case 229: if (uok(b, 16, true)) { ((uint64_t *)b)[0] = 0; ((uint64_t *)b)[1] = 1; } return 0;   /* clock_getres */
    case 96: {                                                 /* gettimeofday */
        if (a && uok(a, 16, true)) { uint64_t ns = linux_now_ns(); ((uint64_t *)a)[0] = ns / 1000000000ull; ((uint64_t *)a)[1] = ns % 1000000000ull / 1000; }
        return 0;
    }
    case 201: { uint64_t s = linux_now_ns() / 1000000000ull; if (a && uok(a, 8, true)) *(uint64_t *)a = s; return s; }   /* time */
    case 35: case 230: {                                       /* nanosleep, clock_nanosleep */
        uint64_t ts = n == 35 ? a : c;
        bool forever; uint64_t ms = ts_ms(ts, &forever);
        if (n == 230 && (b & 1) && uok(ts, 16, false)) {       /* TIMER_ABSTIME */
            uint64_t want = ((uint64_t *)ts)[0] * 1000000000ull + ((uint64_t *)ts)[1];
            uint64_t now = a == 0 ? linux_now_ns() : uptime_ns();
            ms = want > now ? (want - now + 999999) / 1000000 : 0;
        }
        if (ms) task_sleep(ms); else schedule();
        if (t->killed) task_exit(-1);
        return 0;
    }

    /* ---- futexes ---- */
    case 202: {
        int op = (int)b & 0x7F;
        if (op == 0 || op == 9) {                              /* WAIT, WAIT_BITSET */
            bool forever; uint64_t ms = ts_ms(d, &forever);
            if (op == 9 && d && uok(d, 16, false)) {          /* (absolute) */
                uint64_t want = ((uint64_t *)d)[0] * 1000000000ull + ((uint64_t *)d)[1];
                uint64_t now = ((int)b & 256) ? linux_now_ns() : uptime_ns();
                ms = want > now ? (want - now + 999999) / 1000000 : 0;
                if (!ms) return E(ETIMEDOUT_);
            }
            long r = futex_wait(a, (uint32_t)c, forever ? -1 : (int64_t)(ms ? ms : 1));
            return r == -DERR_AGAIN ? E(EAGAIN_) : r == -DERR_TIMEDOUT ? E(ETIMEDOUT_) : r == -DERR_FAULT ? E(EFAULT_) : 0;
        }
        if (op == 1 || op == 10) return futex_wake(t->mm, a, (int)c);   /* WAKE, WAKE_BITSET */
        if (op == 4 || op == 3) {                              /* CMP_REQUEUE / REQUEUE: wake them all (correct, if slower) */
            long k = futex_wake(t->mm, a, 1 << 30); return k;
        }
        return E(ENOSYS_);
    }
    }
    static int said;
    if (said++ < 40) kprintf("[linux] %s: system call %lu isn't there yet\n", t->name, n);
    return E(ENOSYS_);
}

uint64_t linux_now_ns(void) {
    static uint64_t boot_epoch_ns;
    if (!boot_epoch_ns) {
        struct rtc_time tm; rtc_read_local(&tm);
        static const int cum[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
        long y = tm.year, days = (y - 1970) * 365 + (y - 1969) / 4 - (y - 1901) / 100 + (y - 1601) / 400 + cum[(tm.mon - 1) % 12] + tm.day - 1;
        if (tm.mon > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) days++;
        long secs = days * 86400 + tm.hour * 3600 + tm.min * 60 + tm.sec - tm.utc_offset_min * 60;
        boot_epoch_ns = (uint64_t)secs * 1000000000ull - uptime_ns();
    }
    return boot_epoch_ns + uptime_ns();
}
