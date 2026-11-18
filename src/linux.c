/* Linux compatibility: programs built for Linux (x86-64, static or position-independent) run on AXIOM. They
 * make Linux system calls with the syscall instruction; this turns the ones ordinary programs use into
 * AXIOM's own: memory maps, threads (clone), futexes, time, randomness, files on AXIOM's volumes and
 * output to the terminal that started them. */
#include "kernel.h"
#include "vfs.h"
#include "axiom_window.h"
#include "net.h"

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
enum { FD_NONE, FD_TTY_IN, FD_TTY_OUT, FD_FILE, FD_DIR, FD_CHAN, FD_EVENT, FD_EPOLL, FD_SHM, FD_DEV, FD_WIN, FD_SND, FD_TIMER, FD_INET,
       FD_INOTIFY };
/* sound devices (linux_snd.c): /dev/snd/controlC0 and pcmC0D0p */
void *snd_open(int kind); void snd_put(void *o); long snd_ioctl(void *o, uint32_t cmd, uint64_t arg, bool nonblock);
long snd_write(void *o, uint64_t ubuf, uint64_t bytes, bool nonblock); int snd_ready(void *o, bool *waiting);
void *snd_mmap_obj(void *o, uint64_t off);
static bool snd_waiting;                                      /* a poll found a sound device not ready: look again soon */
struct chan;
/* FD_CHAN: a pipe end or a socket-pair end. It reads from rx and writes to tx (either may be 0: a pipe's
 * other end, or after shutdown) and holds a reader count on rx and a writer count on tx. */
struct ulisten;
struct lfd { int kind; int flags; uint64_t off; char path[200]; struct chan *rx, *tx; bool sock; uint8_t stype;
             bool cloexec;          /* closed by execve (O_CLOEXEC, FD_CLOEXEC) */
             struct ulisten *lis;   /* FD_CHAN: a listening socket (path in path[]); bound: path[] set */
             int peer;              /* a connected socket: the other end's process (SO_PEERCRED) */
             uint8_t dev;    /* FD_DEV: which of devs[] */
             void *obj; };   /* FD_EVENT: a struct evfd, FD_EPOLL: a struct epoll, FD_SHM: a struct shm,
                                FD_WIN: a struct axwin (shared by duplicates), FD_INET: a struct isock; FD_CHAN after shutdown: its
                                identity from before (compared only, never used) */
struct shmmap;
#define NFD 1024                   /* (Linux's usual limit; Chromium keeps hundreds open) */
struct lxproc {
    struct lfd fd[NFD];
    char cwd[200];
    char exe[200];                 /* the program, as Linux paths see it (/proc/self/exe) */
    struct task *out;              /* the terminal task that started it (its output goes there), or 0 */
    int out_pid;
    char line[256]; int linelen;   /* output waiting for its end of line (for the log) */
    struct shmmap *maps; int nmaps, capmaps;   /* where shared memory (memfd) is mapped */
    struct fmap *fmaps; int nfmaps, capfmaps;  /* which parts of memory came from which file (/proc/self/maps) */
    struct { uint64_t handler, flags, restorer, mask; } sa[65];   /* signal handlers (rt_sigaction), by number */
    int ppid;                      /* the Linux program that forked it (0: AXIOM started it) */
};
/* a range of a program's memory read from a file (its own segments, its loader's, mmap'd libraries): only
 * for /proc/self/maps, which Chromium (and debuggers) read to find code and print stack traces */
/* a part of memory that came from a file (/proc/self/maps, crash reports). lazy: its pages are read from the
 * file when first touched (lx_fill), up to data_end (zeros after: an ELF segment's bss, or past the end of
 * the file); vpath is the file as AXIOM names it */
struct fmap { uint64_t s, e, off; char path[120]; bool lazy; uint64_t data_end; char vpath[200];
              uint64_t fid; };   /* fid: the file, for the page cache (0: not worked out yet; 1: can't be shared) */
/* the file a code address is in (for crash reports) */
static const struct fmap *fmap_in(struct lxproc *lx, uint64_t a) {
    if (!lx) return 0;
    for (int i = 0; i < lx->nfmaps; i++) if (a >= lx->fmaps[i].s && a < lx->fmaps[i].e) return &lx->fmaps[i];
    return 0;
}
static const struct fmap *fmap_at(uint64_t a) {
    struct lxproc *lx = current ? current->lx : 0;
    if (!lx) return 0;
    for (int i = 0; i < lx->nfmaps; i++) if (a >= lx->fmaps[i].s && a < lx->fmaps[i].e) return &lx->fmaps[i];
    return 0;
}
bool linux_where_quiet(uint64_t a) { return fmap_at(a) != 0; }
void linux_where(const char *what, uint64_t a) {
    const struct fmap *f = fmap_at(a);
    if (f) kprintf("[protect] %s %s+%lx\n", what, f->path, a - f->s + f->off);
}
static void fmap_add(struct lxproc *lx, uint64_t s, uint64_t e, uint64_t off, const char *path) {
    if (!lx) return;
    if (lx->nfmaps == lx->capfmaps) {
        int nc = lx->capfmaps ? lx->capfmaps * 2 : 64;
        struct fmap *n = kmalloc(nc * sizeof *n);
        if (!n) return;
        if (lx->fmaps) memcpy(n, lx->fmaps, lx->nfmaps * sizeof *n);
        kfree(lx->fmaps); lx->fmaps = n; lx->capfmaps = nc;
    }
    struct fmap *f = &lx->fmaps[lx->nfmaps++];
    memset(f, 0, sizeof *f);
    f->s = s; f->e = e; f->off = off;
    strncpy0(f->path, !strncmp(path, "/system/linux/", 14) ? path + 13 : path, sizeof f->path);   /* (as Linux paths see it) */
}
static void fmap_add_lazy(struct lxproc *lx, uint64_t s, uint64_t e, uint64_t off, const char *path, uint64_t data_end) {
    fmap_add(lx, s, e, off, path);
    if (!lx || !lx->nfmaps || lx->fmaps[lx->nfmaps - 1].s != s) return;
    struct fmap *f = &lx->fmaps[lx->nfmaps - 1];
    f->lazy = true; f->data_end = data_end; strncpy0(f->vpath, path, sizeof f->vpath);
}
/* m->fill for a Linux program: a new page inside a lazy file mapping gets its bytes from the file, and the
 * pages around it (256 KiB, read in one go) are put in too, so a big program or file loads a piece at a time
 * as it's used rather than all at once */
static uint64_t fill_calls, fill_reads, fill_bytes, fill_us, fill_anon;   /* (Ctrl+\ dump) */
#define FILL_CHUNK 262144                 /* (256 KiB: the disk reads it in one go) */
/* files a Linux program has opened for writing, renamed or deleted (by their path's hash): their pages aren't
 * shared through the page cache, which knows a file only by its path, size and date (FAT's dates move in steps
 * of 2 s), so it couldn't tell a rewritten file from the old one. (SQLite maps its databases read-only while it
 * writes them.) Program files and libraries are only read, so they're the ones shared. */
#define PCW_SLOTS 4096
static uint64_t pc_written[PCW_SLOTS];
static uint64_t path_hash(const char *p) {
    uint64_t h = 1469598103934665603ull;
    for (; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull;
    return h | 1;
}
static bool pc_was_written(const char *p) {
    uint64_t h = path_hash(p);
    for (unsigned i = 0; i < PCW_SLOTS; i++) {
        uint64_t *e = &pc_written[(h + i) % PCW_SLOTS];
        if (!*e) return false;
        if (*e == h) return true;
    }
    return true;                                       /* (full: share nothing new) */
}
static void pc_file_written(const char *p) {
    uint64_t h = path_hash(p);
    for (unsigned i = 0; i < PCW_SLOTS; i++) {
        uint64_t *e = &pc_written[(h + i) % PCW_SLOTS];
        if (*e == h) return;
        if (!*e) { *e = h; return; }
    }
}

static bool lx_fill(struct mm *m, uint64_t va, uint64_t pa, uint64_t *pages) {
    struct lxproc *lx = m->fill_ctx;
    if (!lx) return true;
    struct fmap *f = 0;
    for (int i = lx->nfmaps - 1; i >= 0; i--)                     /* (the newest mapping wins) */
        if (lx->fmaps[i].lazy && va >= lx->fmaps[i].s && va < lx->fmaps[i].e) { f = &lx->fmaps[i]; break; }
    fill_calls++;
    if (!f) { fill_anon++; return true; }                        /* (not from a file: stays zero) */
    uint64_t t0 = uptime_us();
    uint64_t start = va & ~(uint64_t)(FILL_CHUNK - 1), end = start + FILL_CHUNK;
    if (start < f->s) start = f->s;
    if (end > f->e) end = f->e;
    /* read-only (program code, read-only data): from the page cache, shared with every program that maps it */
    int prot = mm_prot_at(m, va);
    if (prot && !(prot & DPROT_WRITE)) {
        if (!f->fid) {                                         /* the file: its path, size and date */
            struct dunc_stat st; int64_t mt = vfs_mtime(f->vpath);
            uint64_t h = path_hash(f->vpath);
            if (!pc_was_written(f->vpath) && vfs_stat(f->vpath, &st) == FS_OK) { h = (h ^ st.size) * 1099511628211ull; h = (h ^ (uint64_t)mt) * 1099511628211ull; f->fid = h | 2; }
            else f->fid = 1;
        }
        if (f->fid != 1) {
            #define PG_VALID(p) (uint32_t)((p) >= f->data_end ? 0 : f->data_end - (p) >= 4096 ? 4096 : f->data_end - (p))
            if (mm_map_cached(m, va, f->fid, f->off + (va - f->s), PG_VALID(va), 0)) {   /* there already: no reading */
                for (uint64_t p = start; p < end; p += 4096) if (p != va) mm_map_cached(m, p, f->fid, f->off + (p - f->s), PG_VALID(p), 0);
                fill_us += uptime_us() - t0;
                return true;                                   /* (mm_fault sees the page there and drops its own) */
            }
            uint8_t *cb = kmalloc(FILL_CHUNK);
            if (cb) {
                memset(cb, 0, FILL_CHUNK);
                uint64_t to = end < f->data_end ? end : f->data_end;
                if (to > start) { long k = vfs_read(f->vpath, cb, f->off + (start - f->s), to - start); fill_reads++; if (k > 0) fill_bytes += (uint64_t)k; }
                bool ok = mm_map_cached(m, va, f->fid, f->off + (va - f->s), PG_VALID(va), cb + (va - start));
                for (uint64_t p = start; ok && p < end; p += 4096) if (p != va) mm_map_cached(m, p, f->fid, f->off + (p - f->s), PG_VALID(p), cb + (p - start));
                kfree(cb);
                if (ok) { fill_us += uptime_us() - t0; return true; }
            }
            #undef PG_VALID
        }
    }
    uint8_t *buf = kmalloc(FILL_CHUNK);
    if (!buf) return false;
    memset(buf, 0, FILL_CHUNK);
    uint64_t data_to = end < f->data_end ? end : f->data_end;
    if (data_to > start) {
        long k = vfs_read(f->vpath, buf, f->off + (start - f->s), data_to - start);
        fill_reads++; if (k > 0) fill_bytes += (uint64_t)k;
        if (k < 0) {
            static int said;
            if (said++ < 3) kprintf("[linux] %s: can't read %s for its memory\n", current->name, f->vpath);
        }
    }
    memcpy(mmio_virt(pa), buf + (va - start), 4096);         /* (where the kernel reaches that page) */
    for (uint64_t p = start; p < end; p += 4096)                  /* the neighbours, if not there yet */
        if (p != va) mm_add_page(m, p, buf + (p - start), pages);
    kfree(buf);
    fill_us += uptime_us() - t0;
    return true;
}
static void fmap_cut(struct lxproc *lx, uint64_t s, uint64_t e) {   /* [s, e) unmapped or mapped over */
    for (int i = 0; i < lx->nfmaps;) {
        struct fmap *f = &lx->fmaps[i];
        if (f->e <= s || f->s >= e) { i++; continue; }
        if (f->s >= s && f->e <= e) { lx->fmaps[i] = lx->fmaps[--lx->nfmaps]; continue; }
        if (f->s < s && f->e > e) {                               /* a hole: keep the first part, add the rest */
            struct fmap r = *f; r.off += e - f->s; r.s = e; f->e = s;
            fmap_add(lx, r.s, r.e, r.off, r.path);
            i++; continue;
        }
        if (f->s < s) f->e = s; else { f->off += e - f->s; f->s = e; }
        i++;
    }
}


uint64_t linux_now_ns(void);
#define E(x) ((uint64_t)-(long)(x))
enum { EPERM_ = 1, ENOENT_ = 2, EBADF_ = 9, EAGAIN_ = 11, ENOMEM_ = 12, EFAULT_ = 14, EEXIST_ = 17, ENOTDIR_ = 20, EISDIR_ = 21,
       EACCES_ = 13, EBUSY_ = 16, ENODEV_ = 19, ENOSPC_ = 28, EINVAL_ = 22, EMFILE_ = 24, ENOTTY_ = 25, ESPIPE_ = 29, EPIPE_ = 32, ERANGE_ = 34, ENOSYS_ = 38, ENOTSOCK_ = 88,
       EMSGSIZE_ = 90, ENOPROTOOPT_ = 92, EOPNOTSUPP_ = 95, EAFNOSUPPORT_ = 97, ETIMEDOUT_ = 110, EINTR_ = 4, EPROTONOSUPPORT_ = 93,
       EADDRINUSE_ = 98, ENETDOWN_ = 100, ENETUNREACH_ = 101, ECONNRESET_ = 104, ENOBUFS_ = 105, EISCONN_ = 106, ENOTCONN_ = 107,
       ECONNREFUSED_ = 111, ENOTEMPTY_ = 39, E2BIG_ = 7, ENOEXEC_ = 8, ECHILD_ = 10, ESRCH_ = 3, EHOSTUNREACH_ = 113, EALREADY_ = 114, EINPROGRESS_ = 115, EDESTADDRREQ_ = 89 };

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
    /* Linux's own folders (/lib, /usr, /etc, /tmp...) live in AXIOM's /system/linux; AXIOM's own stay as they are */
    static const char *axiom[] = { "home", "apps", "system", "usb0", "usb1", "usb2", "usb3" };
    bool ours = false;
    for (unsigned i = 0; i < sizeof axiom / sizeof axiom[0]; i++) if (!strcmp(parts[0], axiom[i])) ours = true;
    if (!ours) for (const char *c = "/system/linux"; *c && k + 1 < n; c++) out[k++] = *c;
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
/* map a position-independent ELF file at base, reading it part by part: its segments become memory areas */
static bool load_elf(struct mm *m, struct lxproc *lx, const char *path, uint64_t base, uint64_t *entry, uint64_t *phdr, int *phnum, char *interp, size_t ilen, bool *fixed) {
    static uint8_t hdr[8192];
    if (fixed) *fixed = false;
    long hl = vfs_read(path, hdr, 0, sizeof hdr);
    const struct ehdr *e = (const void *)hdr;
    if (hl < (long)sizeof *e || memcmp(e->id, "\x7F" "ELF", 4) || e->id[4] != 2 || e->machine != 62) return false;
    if (e->type != 3) { if (fixed) *fixed = e->type == 2; return false; }   /* ET_DYN (position independent) only */
    if (e->phoff + (uint64_t)e->phnum * e->phentsize > (uint64_t)hl) return false;
    struct phdr ph[64]; int n = e->phnum < 64 ? e->phnum : 64;
    for (int i = 0; i < n; i++) memcpy(&ph[i], hdr + e->phoff + i * e->phentsize, sizeof ph[i]);
    uint64_t ent = e->entry, phoff = e->phoff;
    if (interp) interp[0] = 0;
    *phdr = 0;
    for (int i = 0; i < n; i++) {
        if (ph[i].type == 6) *phdr = base + ph[i].vaddr;      /* PT_PHDR */
        if (ph[i].type == 3 && interp) {                      /* PT_INTERP */
            uint64_t l = ph[i].filesz < ilen - 1 ? ph[i].filesz : ilen - 1;
            if (vfs_read(path, interp, ph[i].offset, l) != (long)l) return false;
            interp[l] = 0;
        }
    }
    /* segments are read as they're used (lazy file mappings), unless two share a page (old layouts): then all now */
    bool lazy = lx != 0;
    uint64_t prev_end = 0;
    for (int i = 0; i < n; i++) {
        if (ph[i].type != 1) continue;
        uint64_t s = (base + ph[i].vaddr) & ~4095ULL, end = (base + ph[i].vaddr + ph[i].memsz + 4095) & ~4095ULL;
        if (s < prev_end || (ph[i].offset & 4095) != (ph[i].vaddr & 4095)) lazy = false;
        prev_end = end;
    }
    if (lazy) {
        for (int i = 0; i < n; i++) {
            if (ph[i].type != 1) continue;
            uint64_t s = (base + ph[i].vaddr) & ~4095ULL, end = (base + ph[i].vaddr + ph[i].memsz + 4095) & ~4095ULL;
            int prot = (ph[i].flags & 4 ? DPROT_READ : 0) | (ph[i].flags & 2 ? DPROT_WRITE : 0) | (ph[i].flags & 1 ? DPROT_EXEC : 0);
            if (mm_mmap(m, s, end - s, prot ? prot : DPROT_READ, DMAP_FIXED, 0) < 0) return false;
            fmap_add_lazy(lx, s, end, ph[i].offset & ~4095ULL, path, base + ph[i].vaddr + ph[i].filesz);
            if (!*phdr && ph[i].offset == 0) *phdr = base + ph[i].vaddr + phoff;
        }
        *entry = base + ent; *phnum = e->phnum;
        return true;
    }
    uint8_t *buf = kmalloc(65536);
    if (!buf) return false;
    for (int i = 0; i < n; i++) {
        if (ph[i].type != 1) continue;
        uint64_t s = (base + ph[i].vaddr) & ~4095ULL, end = (base + ph[i].vaddr + ph[i].memsz + 4095) & ~4095ULL;
        if (mm_mmap(m, s, end - s, DPROT_READ | DPROT_WRITE, DMAP_FIXED, 0) < 0) { kfree(buf); return false; }
        for (uint64_t done = 0; done < ph[i].filesz;) {
            uint64_t want = ph[i].filesz - done < 65536 ? ph[i].filesz - done : 65536;
            long k = vfs_read(path, buf, ph[i].offset + done, want);
            if (k <= 0 || !copy_to(m, base + ph[i].vaddr + done, buf, k)) { kfree(buf); return false; }
            done += k;
        }
        int prot = (ph[i].flags & 4 ? DPROT_READ : 0) | (ph[i].flags & 2 ? DPROT_WRITE : 0) | (ph[i].flags & 1 ? DPROT_EXEC : 0);
        mm_mprotect(m, s, end - s, prot);
        fmap_add(lx, s, end, ph[i].offset & ~4095ULL, path);
        if (!*phdr && ph[i].offset == 0) *phdr = base + ph[i].vaddr + phoff;
    }
    kfree(buf);
    *entry = base + ent; *phnum = e->phnum;
    return true;
}

#define MAIN_BASE   0xC010000000ULL
#define INTERP_BASE 0xC800000000ULL
#define STACK_SIZE  (8ULL << 20)

static void lx_net_files(void);
/* AXIOM's environment for the Linux programs it starts (execve passes on the caller's own) */
static const char *lx_envs[] = { "HOME=/home", "PATH=/bin:/usr/bin", "LANG=en_GB.UTF-8", "TERM=dumb", "USER=user",
    "XDG_RUNTIME_DIR=/tmp", "WAYLAND_DISPLAY=wayland-0", "XDG_SESSION_TYPE=wayland", "GDK_BACKEND=wayland", "QT_QPA_PLATFORM=wayland", 0 };
/* load a program (and its loader, if it's dynamically linked) into m; lx gets its file mappings.
 * false, with why: it can't be run */
static bool lx_load(struct mm *m, struct lxproc *lx, const char *path, uint64_t *start, uint64_t *entry, uint64_t *phdr, int *phnum,
                    uint64_t *ibase, char *why, int wn) {
    char interp[128]; bool fixed;
    if (!load_elf(m, lx, path, MAIN_BASE, entry, phdr, phnum, interp, sizeof interp, &fixed)) {
        struct dunc_stat st;
        if (vfs_stat(path, &st) != 0) ksnprintf(why, wn, "can't find %s", path);
        else ksnprintf(why, wn, "%s isn't a program AXIOM can run%s", path, fixed ? " (built at a fixed address: build it position-independent, e.g. -static-pie)" : "");
        return false;
    }
    *start = *entry; *ibase = 0;
    if (interp[0]) {                                           /* a dynamically linked program: its loader first */
        char ip[200]; path_join("/", interp, ip, sizeof ip);
        uint64_t ie, iph; int ipn;
        if (!load_elf(m, lx, ip, INTERP_BASE, &ie, &iph, &ipn, 0, 0, 0)) { ksnprintf(why, wn, "%s needs %s, which isn't there", path, interp); return false; }
        *start = ie; *ibase = INTERP_BASE;
    }
    return true;
}
/* the stack: 8 MiB at the top of the map area, then argc, argv, envp and the auxiliary vector. 0: no room */
static uint64_t lx_stack(struct mm *m, const char *path, int argc, char **argv, const char **envp,
                         uint64_t entry, uint64_t phdr, int phnum, uint64_t ibase) {
    uint64_t stop = USER_MMAP_END, sbase = stop - STACK_SIZE;
    mm_mmap(m, sbase, STACK_SIZE, DPROT_READ | DPROT_WRITE, DMAP_FIXED, 0);
    int envc = 0; while (envp[envc]) envc++;
    if (argc > 4096 || envc > 4096) return 0;
    uint64_t *ptrs = kmalloc((uint64_t)(argc + envc + 64) * 2 * 8);   /* (argp, envp, then the vector) */
    if (!ptrs) return 0;
    uint64_t sp = stop, used = 0;
    for (int i = argc - 1; i >= 0; i--) { uint64_t l = strlen(argv[i]) + 1; used += l; if (used > (1u << 20)) { kfree(ptrs); return 0; } sp -= l; copy_to(m, sp, argv[i], l); ptrs[i] = sp; }
    for (int i = envc - 1; i >= 0; i--) { uint64_t l = strlen(envp[i]) + 1; used += l; if (used > (1u << 20)) { kfree(ptrs); return 0; } sp -= l; copy_to(m, sp, envp[i], l); ptrs[argc + i] = sp; }
    uint8_t rnd[16]; random_bytes(rnd, 16); sp -= 16; copy_to(m, sp, rnd, 16); uint64_t rndp = sp;
    sp -= strlen(path) + 1; copy_to(m, sp, path, strlen(path) + 1); uint64_t execfn = sp;
    uint64_t *v = ptrs + argc + envc; int k = 0;
    v[k++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) v[k++] = ptrs[i];
    v[k++] = 0;
    for (int i = 0; i < envc; i++) v[k++] = ptrs[argc + i];
    v[k++] = 0;
    uint64_t aux[][2] = { { 3, phdr }, { 4, 56 }, { 5, (uint64_t)phnum }, { 6, 4096 }, { 7, ibase }, { 8, 0 }, { 9, entry },
                          { 11, 1000 }, { 12, 1000 }, { 13, 1000 }, { 14, 1000 }, { 16, 0 }, { 17, 100 }, { 23, 0 }, { 25, rndp }, { 31, execfn }, { 0, 0 } };
    for (unsigned i = 0; i < sizeof aux / sizeof aux[0]; i++) { v[k++] = aux[i][0]; v[k++] = aux[i][1]; }
    sp = (sp - (uint64_t)k * 8) & ~15ULL;
    copy_to(m, sp, v, (uint64_t)k * 8);
    kfree(ptrs);
    return sp;
}
int linux_spawn(const char *path, int argc, char **argv, struct task *out) {
    lx_net_files();                                              /* /etc/resolv.conf: the DNS server AXIOM uses now */
    struct mm *m = mm_new();
    struct lxproc *lx = kmalloc(sizeof *lx);
    if (!m || !lx) { if (m) mm_put(m); kfree(lx); return -1; }
    memset(lx, 0, sizeof *lx);
    m->fill = lx_fill; m->fill_ctx = lx;
    uint64_t start, entry, phdr, ibase; int phnum; char why[200];
    if (!lx_load(m, lx, path, &start, &entry, &phdr, &phnum, &ibase, why, sizeof why)) {
        kprintf("run: %s\n", why);
        mm_put(m); kfree(lx->fmaps); kfree(lx); return -1;
    }
    uint64_t sp = lx_stack(m, path, argc, argv, lx_envs, entry, phdr, phnum, ibase);
    if (!sp) { kprintf("run: no room for the arguments\n"); mm_put(m); kfree(lx->fmaps); kfree(lx); return -1; }
    /* files: 0 = input (nothing yet), 1 and 2 = the terminal */
    lx->fd[0].kind = FD_TTY_IN; lx->fd[1].kind = FD_TTY_OUT; lx->fd[2].kind = FD_TTY_OUT;
    strncpy0(lx->cwd, "/home", sizeof lx->cwd);
    strncpy0(lx->exe, !strncmp(path, "/system/linux/", 14) ? path + 13 : path, sizeof lx->exe);
    lx->out = out; lx->out_pid = out ? out->pid : 0;
    const char *nm = path; for (const char *q = path; *q; q++) if (*q == '/') nm = q + 1;
    struct task *t = task_new_user(nm, m, 0, start, sp);
    mm_put(m);                                                 /* (the task holds it now) */
    if (!t) { kfree(lx); return -1; }
    t->lx = lx;
    strncpy0(t->appid, "linux", sizeof t->appid); t->app = t->appid;
    kprintf("[linux] started %s (pid %d)%s\n", path, t->pid, ibase ? " (dynamically linked)" : "");
    return t->pid;
}
static void fd_close(struct lfd *f);
static void shm_forget_all(struct lxproc *lx);
static void linux_task_exit_free(struct lxproc *lx) {      /* a fork's copy that never ran */
    for (int i = 0; i < NFD; i++) if (lx->fd[i].kind) fd_close(&lx->fd[i]);
    shm_forget_all(lx); kfree(lx->fmaps); kfree(lx);
}
static void lx_child_ended(struct task *t, struct lxproc *lx);
void linux_task_exit(struct task *t) {
    struct lxproc *lx = t->lx;
    if (!lx) return;
    lx_child_ended(t, lx);
    if (lx->linelen) { lx->line[lx->linelen] = 0; kprintf("[%s] %s\n", t->name, lx->line); }
    for (int i = 0; i < NFD; i++) if (lx->fd[i].kind) fd_close(&lx->fd[i]);   /* (the other ends see end of file) */
    shm_forget_all(lx);
    if (t->mm && t->mm->fill_ctx == lx) { t->mm->fill = 0; t->mm->fill_ctx = 0; }
    kfree(lx->fmaps);
    for (int i = 0; i < MAX_TASKS; i++) if (tasks[i].lx == lx) tasks[i].lx = 0;
    kfree(lx);
}


static void chan_wake(void);
static void lx_abort_trace(struct lxproc *lx, struct frame *f);
/* ---------------- signals ----------------
 * A signal is a bit in a thread's sig_pending. On the way back to the program (after a system call, or an
 * interrupt that stopped it), a pending signal it doesn't block runs its handler: as Linux does, its registers
 * (and FPU state) go in a frame on its stack, and the handler returns through its restorer, which calls
 * rt_sigreturn to put them back. Signals without a handler do what Linux does by default: these programs
 * mostly send them to themselves (abort) or between their own threads (Boehm GC's stop-the-world, Go). */
#define SIGBIT(n) (1ULL << ((n) - 1))
#define SIG_UNBLOCKABLE (SIGBIT(9) | SIGBIT(19))       /* SIGKILL, SIGSTOP */
#define SA_SIGINFO_ 4
#define SA_RESTORER_ 0x04000000
#define SA_NODEFER_ 0x40000000
#define SA_RESETHAND_ 0x80000000
static bool sig_default_ignored(int n) { return n == 17 || n == 23 || n == 28 || n == 18 || n == 21 || n == 22 || n == 20; }   /* CHLD URG WINCH CONT TTIN TTOU TSTP */
/* a signal for the running thread: is one waiting that would run a handler (so a blocking call stops, EINTR)? */
static bool sig_now(void) { struct task *t = current; return t && t->lx && (t->sig_pending & ~t->sig_blocked); }
/* post signal n to thread t (of a Linux program); false if t isn't one */
static bool sig_post(struct task *t, int n, int from) {
    if (!t || !t->lx || n < 1 || n > 64) return false;
    struct lxproc *lx = t->lx;
    uint64_t h = lx->sa[n].handler;
    if (h == 1 || (h == 0 && sig_default_ignored(n))) return true;          /* SIG_IGN, or ignored by default */
    t->sig_pending |= SIGBIT(n); t->sig_from = from;
    task_wake(t);                                                          /* stop what it waits for (EINTR) */
    chan_wake();
    return true;
}
static void sig_frame_fail(struct frame *f, int n) {
    kprintf("[linux] %s: no room on its stack for signal %d's handler\n", current->name, n);
    task_exit(128 + n);
}
/* the terminal's "threads": what each Linux program's threads are doing (for finding where one is stuck) */
void linux_threads(void) {
    static const char *st[] = { "-", "ready", "running", "sleeping", "waiting", "ended" };
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (!t->state || !t->lx) continue;
        kprintf("%5d %-16s %-8s syscall %-4lu cpu %lu ms  signals pending %lx blocked %lx%s\n", t->pid, t->name, st[t->state < 6 ? t->state : 0],
                t->sig_sysno, t->ticks / 1000, t->sig_pending, t->sig_blocked, t->futex_addr ? "  (futex)" : "");
        const struct fmap *fm = fmap_in(t->lx, t->last_rip);
        if (fm) kprintf("        at %s+%lx\n", fm->path, t->last_rip - fm->s + fm->off);
    }
}
void linux_signals(struct frame *f) {
    struct task *t = current; struct lxproc *lx = t->lx;
    uint64_t ready = t->sig_pending & ~t->sig_blocked;
    if (!ready || !lx) return;
    int n = __builtin_ctzll(ready) + 1;
    t->sig_pending &= ~SIGBIT(n);
    uint64_t h = lx->sa[n].handler;
    if (h == 1) return;                                                    /* SIG_IGN */
    if (h == 0) {                                                          /* default: ignore, or end the program */
        if (sig_default_ignored(n)) return;
        kprintf("[linux] %s: ended by signal %d\n", t->name, n);
        if (n == 6) lx_abort_trace(lx, f);
        if (t->proc) t->proc->exit_signal = n;
        task_exit(128 + n);
    }
    /* a system call the signal interrupted: with SA_RESTART it runs again after the handler (as on Linux), except
     * those that always say EINTR (waiting for events or time: poll, select, epoll_wait, nanosleep, sigsuspend...) */
    if (f->vec == 0x81 && f->rax == E(EINTR_) && (lx->sa[n].flags & 0x10000000)) {
        uint64_t sc = t->sig_sysno;
        bool never = sc == 7 || sc == 271 || sc == 23 || sc == 270 || sc == 232 || sc == 281 || sc == 441 || sc == 35 || sc == 230 || sc == 130 || sc == 128 || sc == 34;
        if (!never) { f->rax = sc; f->rip -= 2; }                          /* (back to the syscall instruction) */
    }
    /* the frame, as Linux's: [restorer address] ucontext siginfo ... FPU state (XSAVE), below the red zone */
    uint64_t sp = f->rsp - 128;
    uint32_t fsz = fpu_user_size();
    uint64_t fp = (sp - fsz) & ~63ULL;
    uint64_t base = ((fp - (8 + 304 + 128)) & ~15ULL) - 8;                 /* (rsp % 16 == 8 at the handler, as after a call) */
    if (!uok(base, fp + fsz - base, true)) sig_frame_fail(f, n);
    uint8_t *fr = (uint8_t *)base; memset(fr, 0, fp + fsz - base);
    *(uint64_t *)fr = lx->sa[n].restorer;
    uint8_t *uc = fr + 8, *info = uc + 304;
    uint64_t *g = (uint64_t *)(uc + 40);                                   /* uc_mcontext: r8..r15 rdi rsi rbp rbx rdx rax rcx rsp rip eflags */
    g[0] = f->r8; g[1] = f->r9; g[2] = f->r10; g[3] = f->r11; g[4] = f->r12; g[5] = f->r13; g[6] = f->r14; g[7] = f->r15;
    g[8] = f->rdi; g[9] = f->rsi; g[10] = f->rbp; g[11] = f->rbx; g[12] = f->rdx; g[13] = f->rax; g[14] = f->rcx;
    g[15] = f->rsp; g[16] = f->rip; g[17] = f->rflags; g[18] = 0x33 | (0x2bULL << 48);   /* cs, gs, fs, ss */
    g[21] = t->sig_blocked;                                                /* oldmask */
    g[23] = fp;                                                            /* fpstate */
    *(uint64_t *)(uc + 296) = t->sig_restore ? t->sig_saved : t->sig_blocked;   /* the mask to put back */
    *(uint64_t *)(uc + 16) = 0; *(uint32_t *)(uc + 24) = 2;                /* uc_stack: SS_DISABLE */
    *(int32_t *)info = n; *(int32_t *)(info + 8) = -6;                     /* si_code: SI_TKILL */
    *(int32_t *)(info + 16) = t->sig_from; *(int32_t *)(info + 20) = 1000;
    fpu_save_user((uint8_t *)fp);                                         /* (the program's own FPU registers are live here) */
    t->sig_restore = false;
    if (!(lx->sa[n].flags & SA_NODEFER_)) t->sig_blocked |= SIGBIT(n);
    t->sig_blocked = (t->sig_blocked | lx->sa[n].mask) & ~SIG_UNBLOCKABLE;
    if (lx->sa[n].flags & SA_RESETHAND_) lx->sa[n].handler = 0;
    f->rip = h; f->rsp = base; f->rdi = (uint64_t)n; f->rsi = (uint64_t)info; f->rdx = (uint64_t)uc; f->rax = 0;
    f->rflags &= ~0x400ULL;                                                /* DF clear, as the ABI wants */
}
static uint64_t sig_return(struct frame *f) {                              /* rt_sigreturn: the handler's done */
    struct task *t = current;
    uint8_t *uc = (uint8_t *)f->rsp;                                       /* (the restorer's ret popped the address) */
    if (!uok((uint64_t)uc, 304, false)) { kprintf("[linux] %s: bad signal frame\n", t->name); task_exit(139); }
    uint64_t *g = (uint64_t *)(uc + 40);
    f->r8 = g[0]; f->r9 = g[1]; f->r10 = g[2]; f->r11 = g[3]; f->r12 = g[4]; f->r13 = g[5]; f->r14 = g[6]; f->r15 = g[7];
    f->rdi = g[8]; f->rsi = g[9]; f->rbp = g[10]; f->rbx = g[11]; f->rdx = g[12]; f->rcx = g[14];
    f->rsp = g[15]; f->rip = g[16];
    f->rflags = (f->rflags & ~0xDD5ULL) | (g[17] & 0xDD5ULL);              /* the flags a program may set (CF PF AF ZF SF TF? DF OF) */
    uint64_t fp = g[23];
    if (fp && !(fp & 15) && uok(fp, 512, false))
        fpu_restore_user((const uint8_t *)fp, uok(fp, fpu_user_size(), false));
    t->sig_blocked = *(uint64_t *)(uc + 296) & ~SIG_UNBLOCKABLE;
    return g[13];                                                          /* rax, as it was */
}

/* ---------------- the system calls ---------------- */
struct lstat { uint64_t dev, ino, nlink; uint32_t mode, uid, gid, pad; uint64_t rdev; int64_t size, blksize, blocks; uint64_t at[2], mt[2], ct[2]; int64_t r[3]; };
static uint64_t path_ino(const char *p) { uint64_t h = 1469598103934665603ull; for (; p && *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull; return h >> 1 | 1; }
static void fill_stat(struct lstat *st, bool dir, uint64_t size, bool tty, const char *path) {
    memset(st, 0, sizeof *st);
    st->dev = 1; st->ino = path_ino(path);   /* (loaders tell files apart by these) */ st->nlink = 1; st->uid = st->gid = 1000;
    st->mode = tty ? 0020620 : dir ? 0040755 : 0100644;
    st->size = size; st->blksize = 4096; st->blocks = (size + 511) / 512;
    int64_t t = path && !tty ? vfs_mtime(path) : -1;            /* (the disk's date; else now) */
    uint64_t when = t > 0 ? (uint64_t)t : (uint64_t)(linux_now_ns() / 1000000000ull);
    st->at[0] = st->mt[0] = st->ct[0] = when;
}
static int fd_new(struct lxproc *lx, int from) {
    for (int i = from; i < NFD; i++) if (!lx->fd[i].kind) return i;
    static int said;
    if (said++ < 3) kprintf("[linux] %s: all %d files in use\n", current->name, NFD);
    return -1;
}
static uint64_t ts_ms(uint64_t uts, bool *forever) {             /* a struct timespec (relative) in ms */
    if (!uts) { *forever = true; return 0; }
    *forever = false;
    if (!uok(uts, 16, false)) return 0;
    int64_t s = ((int64_t *)uts)[0], ns = ((int64_t *)uts)[1];
    if (s < 0) return 0;
    return (uint64_t)s * 1000 + ((uint64_t)ns + 999999) / 1000000;
}

/* ---------------- pipes and socket pairs ----------------
 * A chan is one direction: a ring of bytes, and for packet sockets (SOCK_SEQPACKET / SOCK_DGRAM) the
 * lengths of the messages in it, so each read gets one whole message. A pipe is one chan; a socket pair
 * is two, crossed. Everything here runs under the big kernel lock. Waiting uses the futex queue with a
 * kernel address: any change to any chan wakes every waiter, which then looks again (poll, read, write). */
#define CHAN_CAP  (64 * 1024)
#define CHAN_MSGS 256
struct chan {
    uint8_t *data; uint32_t head, len;            /* ring of CHAN_CAP bytes */
    uint32_t msg[CHAN_MSGS]; int mhead, mcount;   /* packet sockets: message lengths, oldest first */
    bool packet;
    int readers, writers;                         /* ends that can still read / write it */
    uint64_t wpos, rpos;                          /* bytes ever written / read: where attached files sit */
    struct anc *anc, *anc_tail;                   /* files sent along (SCM_RIGHTS), oldest first */
};
/* Files passed over a socket pair (sendmsg with SCM_RIGHTS) travel with the first byte they were sent with:
 * they're copies of the sender's (holding their own references) until recvmsg gives them to the receiver.
 * As on Linux, a stream read ends with the bytes that were sent with files (and gets those files once it
 * reaches them); read() without recvmsg drops them. */
#define ANC_MAX 32
struct anc { struct anc *next; uint64_t pos, len; int n; struct lfd fd[ANC_MAX]; };   /* pos, len: the bytes sent with them */
static void anc_free(struct anc *a) {
    if (!a) return;
    for (int i = 0; i < a->n; i++) fd_close(&a->fd[i]);
    kfree(a);
}
static int io_tag;
#define IO_MM   ((struct mm *)&io_tag)
#define IO_ADDR ((uint64_t)&io_tag)
/* Like Linux's wakeup preemption: a system call that woke a waiter gives up the core when it's done (see
 * linux_syscall), so the woken threads run, in the order they were woken, before the waker goes on. Chromium
 * counts on that order: on one core, a thread that wrote a message to another thread and then posted more work
 * could otherwise run that work first (the browser's InvalidateFrameSinkId reached viz after its teardown). */
static bool chan_woke;
static void chan_wake(void) { if (futex_wake(IO_MM, IO_ADDR, 1 << 30)) chan_woke = true; }
/* sleep until some chan changes or ms pass (ms < 0: no time limit); false if the time ran out */
static bool chan_wait(int64_t ms) {
    struct task *t = current;
    if (ms == 0) return false;
    t->futex_mm = IO_MM; t->futex_addr = IO_ADDR; t->futex_woken = false;
    t->wake_at = ms > 0 ? uptime_ms() + (uint64_t)ms + 1 : 0;   /* (+1: never early) */
    t->state = PS_BLOCKED;
    schedule();
    bool woken = t->futex_woken;
    t->futex_mm = 0; t->futex_addr = 0; t->futex_woken = false;
    if (t->killed) task_exit(-1);
    return woken;
}
static struct chan *chan_new(bool packet) {
    struct chan *c = kmalloc(sizeof *c);
    if (!c) return 0;
    memset(c, 0, sizeof *c);
    if (!(c->data = kmalloc(CHAN_CAP))) { kfree(c); return 0; }
    c->packet = packet;
    return c;
}
static void chan_drop(struct chan *c, bool reader) {
    if (!c) return;
    if (reader) c->readers--; else c->writers--;
    if (c->readers <= 0 && c->writers <= 0) {
        struct anc *a = c->anc;
        kfree(c->data); kfree(c);
        while (a) { struct anc *nx = a->next; anc_free(a); a = nx; }   /* (files nobody received) */
    }
    chan_wake();
}
/* a named, listening Unix socket (see "Unix sockets with names" below) */
#define ULQ 32
struct pend { struct chan *rx, *tx; int pid; };
struct ulisten {
    int refs;                                     /* (first, as fd_copy expects) */
    char name[200]; uint8_t stype; int pid;
    struct pend q[ULQ]; int qh, qn; bool closed;
    struct ulisten *next;
};
struct evfd { int refs; uint64_t count; bool sem; };
/* timerfd: a deadline (in uptime ns, 0: disarmed) and a repeat interval; expirations are counted when looked at */
struct tfd { int refs; int clock; uint64_t next, interval, ticks; };
static uint64_t timer_wake_at;                                 /* the soonest deadline a poll is waiting for */
static void tfd_update(struct tfd *x) {
    if (!x->next) return;
    uint64_t now = uptime_ns();
    if (now < x->next) return;
    if (x->interval) { uint64_t k = (now - x->next) / x->interval + 1; x->ticks += k; x->next += k * x->interval; }
    else { x->ticks++; x->next = 0; }
}
struct shm;
static void shm_put(struct shm *o);
#define AXQ 256
struct axwin {
    int refs;                         /* (first: fd_close/fd_copy count duplicates like the others) */
    struct task *owner; int id; bool resizable;
    int px, py;                       /* AXWIN_MOVE before AXWIN_CREATE: where it will appear (WM_ANYWHERE: not said) */
    struct shm *pix;                  /* the window holds one reference; each mapping one more */
    struct axwin_event q[AXQ]; int qh, qt;
    struct axwin *next;
};
static void axwin_put(struct axwin *x);
struct isock;
static void isock_put(struct isock *s);
static int isock_ready(struct isock *s);
/* ---------------- inotify ----------------
 * An inotify file is a list of watched paths and a queue of events, as Linux's struct inotify_event (16 bytes,
 * then the name, padded with zeros to a multiple of 16). Events come from the file system as it changes
 * (vfs_watch), whoever changed it: Linux programs or AXIOM's own apps. A watch is on a path, not an inode:
 * a renamed watched file or folder keeps its watch under the new path, as on Linux. */
#define IN_MODIFY_        0x2u
#define IN_ATTRIB_        0x4u
#define IN_CLOSE_WRITE_   0x8u
#define IN_MOVED_FROM_    0x40u
#define IN_MOVED_TO_      0x80u
#define IN_CREATE_        0x100u
#define IN_DELETE_        0x200u
#define IN_DELETE_SELF_   0x400u
#define IN_MOVE_SELF_     0x800u
#define IN_Q_OVERFLOW_    0x4000u
#define IN_IGNORED_       0x8000u
#define IN_ALL_EVENTS_    0xfffu
#define IN_ONLYDIR_       0x1000000u
#define IN_MASK_CREATE_   0x10000000u
#define IN_MASK_ADD_      0x20000000u
#define IN_ISDIR_         0x40000000u
#define IN_ONESHOT_       0x80000000u
#define INOT_WATCHES 128
#define INOT_QUEUE   65536
struct iwatch { int wd; uint32_t mask; char path[200]; };
struct inot { int refs; struct inot *next; int nw, next_wd; struct iwatch w[INOT_WATCHES];
              uint32_t qlen, last; bool overflowed; uint8_t q[INOT_QUEUE]; };
static struct inot *inots;
static uint32_t inot_cookie;
static void inot_push(struct inot *in, int wd, uint32_t mask, uint32_t cookie, const char *name) {
    uint32_t nl = name && *name ? (uint32_t)strlen(name) + 1 : 0, len = (nl + 15) & ~15u, sz = 16 + len;
    if (in->qlen) {                                    /* the same as the last unread event: once is enough (as Linux) */
        uint8_t *l = in->q + in->last;
        if (*(int32_t *)l == wd && *(uint32_t *)(l + 4) == mask && *(uint32_t *)(l + 8) == cookie && *(uint32_t *)(l + 12) == len &&
            (!len || !strcmp((char *)l + 16, name))) return;
    }
    if (in->qlen + sz > INOT_QUEUE - 16) {             /* full: one overflow event, until it's read */
        if (in->overflowed) return;
        in->overflowed = true; wd = -1; mask = IN_Q_OVERFLOW_; cookie = 0; len = 0; nl = 0; sz = 16;
    }
    uint8_t *e = in->q + in->qlen;
    *(int32_t *)e = wd; *(uint32_t *)(e + 4) = mask; *(uint32_t *)(e + 8) = cookie; *(uint32_t *)(e + 12) = len;
    memset(e + 16, 0, len); if (nl) memcpy(e + 16, name, nl);
    in->last = in->qlen; in->qlen += sz;
    chan_wake();
}
static void inot_drop(struct inot *in, int i) {         /* a watch ends: IN_IGNORED */
    inot_push(in, in->w[i].wd, IN_IGNORED_, 0, 0);
    in->w[i] = in->w[--in->nw];
}
static bool path_in(const char *p, const char *dir) {    /* p is dir, or inside it */
    uint64_t n = strlen(dir);
    return !strncmp(p, dir, n) && (p[n] == 0 || p[n] == '/');
}
static const char *path_base(const char *p, uint64_t *dirlen) {   /* the name after the last '/'; dirlen: its folder's */
    const char *b = p; for (const char *c = p; *c; c++) if (*c == '/') b = c + 1;
    *dirlen = b > p + 1 ? (uint64_t)(b - p - 1) : 1; return b;
}
/* one change to path: child_ev to watches on its folder (with its name), self_ev to watches on it */
static void inot_event(const char *path, uint32_t child_ev, uint32_t self_ev, uint32_t cookie, bool dir) {
    uint64_t dl; const char *base = path_base(path, &dl);
    for (struct inot *in = inots; in; in = in->next)
        for (int i = 0; i < in->nw; i++) {
            struct iwatch *w = &in->w[i]; uint32_t ev = 0; const char *name = 0;
            if (child_ev && strlen(w->path) == dl && !strncmp(w->path, path, dl)) { ev = child_ev; name = base; }
            else if (self_ev && !strcmp(w->path, path)) ev = self_ev;
            if (!ev || !(ev & w->mask & IN_ALL_EVENTS_)) continue;
            inot_push(in, w->wd, ev | (name && dir ? IN_ISDIR_ : 0), cookie, name);
            if (w->mask & IN_ONESHOT_) { inot_drop(in, i); i--; }
        }
}
static void linux_vfs_watch(int what, const char *path, const char *to, bool dir) {
    char p[200], q[200];
    path_join("/", path, p, sizeof p);
    switch (what) {
    case VW_CREATE: inot_event(p, IN_CREATE_, 0, 0, dir); break;
    case VW_MODIFY: inot_event(p, IN_MODIFY_, IN_MODIFY_, 0, false); break;
    case VW_ATTRIB: inot_event(p, IN_ATTRIB_, IN_ATTRIB_, 0, dir); break;
    case VW_DELETE:
        inot_event(p, IN_DELETE_, 0, 0, dir);
        for (struct inot *in = inots; in; in = in->next)        /* watches on it (or inside it): gone */
            for (int i = 0; i < in->nw; i++)
                if (path_in(in->w[i].path, p)) { if (in->w[i].mask & IN_DELETE_SELF_) inot_push(in, in->w[i].wd, IN_DELETE_SELF_, 0, 0); inot_drop(in, i); i--; }
        break;
    case VW_RENAME: {
        path_join("/", to, q, sizeof q);
        uint32_t ck = ++inot_cookie;
        inot_event(p, IN_MOVED_FROM_, IN_MOVE_SELF_, ck, dir);
        inot_event(q, IN_MOVED_TO_, 0, ck, dir);
        uint64_t pl = strlen(p);
        for (struct inot *in = inots; in; in = in->next)        /* watches move with what they watch */
            for (int i = 0; i < in->nw; i++)
                if (path_in(in->w[i].path, p)) { char n[200]; ksnprintf(n, sizeof n, "%s%s", q, in->w[i].path + pl); strncpy0(in->w[i].path, n, sizeof in->w[i].path); }
        break;
    }
    }
}
static void inot_put(struct inot *in) {
    if (--in->refs > 0) return;
    for (struct inot **pp = &inots; *pp; pp = &(*pp)->next) if (*pp == in) { *pp = in->next; break; }
    if (!inots) vfs_watch = 0;                            /* (no one watching: the file system needn't say) */
    kfree(in); chan_wake();
}

struct epitem { int fd; uint32_t events; uint64_t data; const void *id; bool off; };
struct epoll { int refs, n, next; struct epitem it[NFD]; };
static void ulisten_put(struct ulisten *l);
static void fd_close(struct lfd *f) {
    if (f->kind == FD_CHAN) { chan_drop(f->rx, true); chan_drop(f->tx, false); if (f->lis) ulisten_put(f->lis); }
    if ((f->kind == FD_EVENT || f->kind == FD_EPOLL || f->kind == FD_TIMER) && f->obj && --*(int *)f->obj <= 0) { kfree(f->obj); chan_wake(); }   /* (refs come first in both) */
    if (f->kind == FD_SHM) shm_put(f->obj);
    if (f->kind == FD_WIN) axwin_put(f->obj);
    if (f->kind == FD_SND) snd_put(f->obj);
    if (f->kind == FD_INET) isock_put(f->obj);
    if (f->kind == FD_INOTIFY && f->obj) inot_put(f->obj);
    if (f->kind == FD_FILE && (f->flags & 3) && inots) inot_event(f->path, IN_CLOSE_WRITE_, IN_CLOSE_WRITE_, 0, false);
    memset(f, 0, sizeof *f);
}
static void fd_copy(struct lfd *dst, const struct lfd *src) {      /* dup: one more reference to the same ends */
    if (dst == src) return;
    fd_close(dst);
    *dst = *src;
    if (dst->kind == FD_CHAN) { if (dst->rx) dst->rx->readers++; if (dst->tx) dst->tx->writers++; if (dst->lis) (*(int *)dst->lis)++; }
    if ((dst->kind == FD_EVENT || dst->kind == FD_EPOLL || dst->kind == FD_SHM || dst->kind == FD_WIN || dst->kind == FD_SND || dst->kind == FD_TIMER || dst->kind == FD_INET || dst->kind == FD_INOTIFY) && dst->obj) (*(int *)dst->obj)++;
}
/* move bytes in or out of the ring (to / from a kernel buffer) */
static void ring_put(struct chan *c, const uint8_t *s, uint32_t n) {
    for (uint32_t done = 0; done < n;) {
        uint32_t at = (c->head + c->len) % CHAN_CAP, k = CHAN_CAP - at; if (k > n - done) k = n - done;
        memcpy(c->data + at, s + done, k); c->len += k; done += k;
    }
    c->wpos += n;
}
static void ring_get(struct chan *c, uint8_t *d, uint32_t n, bool peek) {
    uint32_t h = c->head;
    for (uint32_t done = 0; done < n;) {
        uint32_t k = CHAN_CAP - h; if (k > n - done) k = n - done;
        if (d) memcpy(d + done, c->data + h, k);
        h = (h + k) % CHAN_CAP; done += k;
    }
    if (!peek) { c->head = h; c->len -= n; c->rpos += n; }
}
/* read up to n bytes into buf (the running program's memory if user, else a kernel buffer): blocks unless
 * nonblock; 0 = end of file */
static int64_t chan_read(struct lfd *f, uint8_t *buf, uint64_t n, bool user, bool nonblock, bool peek, bool *trunc, struct anc **got) {
    struct chan *c = f->rx;
    if (trunc) *trunc = false;
    if (!c) return 0;                                           /* shut down for reading */
    while (!c->len) {
        if (!c->writers) return 0;                              /* nobody can write any more: end of file */
        if (nonblock) return (int64_t)E(EAGAIN_);
        chan_wait(-1); if (sig_now()) return (int64_t)E(EINTR_);
        /* another thread shut it down (or closed it) meanwhile: end of file, as on Linux, and c may be gone.
         * (Chromium stops its audio thread that way: CancelableSyncSocket::Shutdown wakes the read it waits in) */
        if (f->rx != c) return 0;
    }
    if (user && !uok((uint64_t)buf, n, true)) return (int64_t)E(EFAULT_);   /* (another thread may have unmapped it meanwhile) */
    if (got) *got = 0;
    struct anc *a = c->anc;
    uint64_t from = c->rpos;
    if (!c->packet && a && a->pos + a->len > from && a->pos + a->len - from < n) n = a->pos + a->len - from;   /* end with them */
    uint64_t k;
    if (c->packet) {
        uint32_t m = c->msg[c->mhead];
        k = m < n ? m : n;
        if (trunc) *trunc = k < m;
        ring_get(c, buf, (uint32_t)k, peek);
        if (!peek) {
            if (k < m) ring_get(c, 0, m - (uint32_t)k, false);  /* the rest of the message is lost, as on Linux */
            c->mhead = (c->mhead + 1) % CHAN_MSGS; c->mcount--;
        }
    } else {
        k = c->len < n ? c->len : n;
        ring_get(c, buf, (uint32_t)k, peek);
    }
    if (!peek && a && (a->pos < from + k || (c->packet && a->pos == from))) {   /* reached the bytes that have files */
        c->anc = a->next; if (!c->anc) c->anc_tail = 0;
        a->next = 0;
        if (got) *got = a; else anc_free(a);
    }
    if (!peek) chan_wake();
    return (int64_t)k;
}
/* write n bytes from buf (as for chan_read): a stream writes all of it (blocking as needed) unless
 * nonblock; a packet goes in whole or not at all */
static void anc_attach(struct chan *c, struct anc **a, uint64_t len) {   /* the files go with the next len bytes written */
    if (!a || !*a) return;
    (*a)->pos = c->wpos; (*a)->len = len; (*a)->next = 0;
    if (c->anc_tail) c->anc_tail->next = *a; else c->anc = *a;
    c->anc_tail = *a; *a = 0;
}
static int64_t chan_write(struct lfd *f, const uint8_t *buf, uint64_t n, bool user, bool nonblock, struct anc **a) {
    struct chan *c = f->tx;
    if (!c || !c->readers) return (int64_t)E(EPIPE_);
    if (c->packet) {
        if (n > CHAN_CAP) return (int64_t)E(EMSGSIZE_);
        while (CHAN_CAP - c->len < n || c->mcount == CHAN_MSGS) {
            if (nonblock) return (int64_t)E(EAGAIN_);
            chan_wait(-1); if (sig_now()) return (int64_t)E(EINTR_);
            if (f->tx != c || !c->readers) return (int64_t)E(EPIPE_);   /* (shut down meanwhile: c may be gone) */
        }
        if (user && !uok((uint64_t)buf, n, false)) return (int64_t)E(EFAULT_);
        anc_attach(c, a, n);
        ring_put(c, buf, (uint32_t)n);
        c->msg[(c->mhead + c->mcount) % CHAN_MSGS] = (uint32_t)n; c->mcount++;
        chan_wake();
        return (int64_t)n;
    }
    uint64_t done = 0;
    while (done < n) {
        uint32_t space = CHAN_CAP - c->len;
        if (!space) {
            if (nonblock) { if (done) break; return (int64_t)E(EAGAIN_); }   /* (blocking: wait for room for the rest) */
            chan_wait(-1); if (sig_now()) return done ? (int64_t)done : (int64_t)E(EINTR_);
            if (f->tx != c || !c->readers) return done ? (int64_t)done : (int64_t)E(EPIPE_);
            continue;
        }
        uint32_t k = n - done < space ? (uint32_t)(n - done) : space;
        if (user && !uok((uint64_t)buf + done, k, false)) return done ? (int64_t)done : (int64_t)E(EFAULT_);
        if (!done) anc_attach(c, a, k);
        ring_put(c, buf + done, k); done += k;
        chan_wake();
    }
    return (int64_t)done;
}
enum { POLLIN_ = 1, POLLPRI_ = 2, POLLOUT_ = 4, POLLERR_ = 8, POLLHUP_ = 16, POLLNVAL_ = 32, POLLRDHUP_ = 0x2000 };
static int epoll_scan(struct lxproc *lx, struct epoll *ep, uint8_t *out, int max);
static int fd_ready(struct lxproc *lx, int fd) {
    if (fd < 0 || fd >= NFD || !lx->fd[fd].kind) return POLLNVAL_;
    struct lfd *f = &lx->fd[fd];
    switch (f->kind) {
    case FD_TTY_IN: return POLLIN_;                              /* (reads give end of file) */
    case FD_TTY_OUT: return POLLOUT_;
    case FD_CHAN: {
        int r = 0;
        if (f->lis) return f->lis->qn ? POLLIN_ : 0;            /* a listening socket: a connection to accept */
        if (f->rx) { if (f->rx->len || !f->rx->writers) r |= POLLIN_; if (!f->rx->writers) r |= f->sock ? POLLRDHUP_ | (f->tx ? 0 : POLLHUP_) : POLLHUP_; }
        else if (f->sock) r |= POLLIN_ | POLLRDHUP_;              /* shut down for reading: reads give end of file */
        if (f->tx) { if (!f->tx->readers) r |= f->sock ? POLLOUT_ | POLLHUP_ : POLLERR_; else if (f->tx->len < CHAN_CAP && f->tx->mcount < CHAN_MSGS) r |= POLLOUT_; }
        return r;
    }
    case FD_EVENT: { struct evfd *ev = f->obj; return (ev->count ? POLLIN_ : 0) | (ev->count < ~0ULL - 1 ? POLLOUT_ : 0); }
    case FD_TIMER: {
        struct tfd *x = f->obj; tfd_update(x);
        if (x->ticks) return POLLIN_;
        if (x->next && (!timer_wake_at || x->next < timer_wake_at)) timer_wake_at = x->next;   /* (wake the poll then) */
        return 0;
    }
    case FD_EPOLL: return epoll_scan(lx, f->obj, 0, 1) ? POLLIN_ : 0;
    case FD_WIN: { struct axwin *x = f->obj; return (x->qh != x->qt ? POLLIN_ : 0) | POLLOUT_; }
    case FD_SND: return snd_ready(f->obj, &snd_waiting);
    case FD_INET: return isock_ready(f->obj);
    case FD_INOTIFY: return ((struct inot *)f->obj)->qlen ? POLLIN_ : 0;
    default: return POLLIN_ | POLLOUT_;                          /* files are always ready */
    }
}
/* poll / ppoll: fds is a user array of {int fd; short events, revents} */
static uint64_t do_poll(struct lxproc *lx, uint64_t ufds, uint64_t nfds, int64_t ms) {
    if (nfds > NFD * 8) return E(EINVAL_);
    if (!uok(ufds, nfds * 8, true)) return E(EFAULT_);
    uint64_t end = ms > 0 ? uptime_ms() + (uint64_t)ms + 1 : 0;   /* (+1: a whole ms, never early) */
    for (;;) {
        int count = 0;
        for (uint64_t i = 0; i < nfds; i++) {
            uint8_t *p = (uint8_t *)ufds + i * 8;
            int fd = *(int32_t *)p; uint16_t ev = *(uint16_t *)(p + 4), re = 0;
            if (fd >= 0) re = (uint16_t)(fd_ready(lx, fd) & (ev | POLLERR_ | POLLHUP_ | POLLNVAL_));
            *(uint16_t *)(p + 6) = re;
            if (re) count++;
        }
        if (count || ms == 0) return count;
        int64_t left = -1;
        if (ms > 0) { uint64_t now = uptime_ms(); if (now >= end) return 0; left = (int64_t)(end - now); }
        if (sig_now()) return E(EINTR_);                         /* a signal: its handler runs first */
        if (snd_waiting) { snd_waiting = false; if (left < 0 || left > 5) left = 5; }
        if (timer_wake_at) {                                    /* a timerfd's deadline */
            uint64_t now = uptime_ns(), w = timer_wake_at > now ? (timer_wake_at - now + 999999) / 1000000 : 1;
            timer_wake_at = 0;
            if (left < 0 || (int64_t)w < left) left = (int64_t)w;
        }   /* (sound plays without waking anyone) */
        chan_wait(left);
    }
}
/* ---------------- eventfd and epoll ----------------
 * An eventfd is a counter: writes add, a read takes it all (or one, EFD_SEMAPHORE) and waits while it's 0.
 * An epoll is a list of files to watch. Readiness is worked out when asked (as poll does), so it is level-
 * triggered; EPOLLET is treated the same way, which only costs extra wake-ups for programs that read until
 * EAGAIN (as they must with EPOLLET). EPOLLONESHOT turns an entry off once it's reported, until EPOLL_CTL_MOD. */
#define EPOLLONESHOT_ (1u << 30)
#define EPOLLET_      (1u << 31)
static const void *fd_id(const struct lfd *f) {               /* which open file an fd number refers to */
    if (f->kind == FD_CHAN) return f->obj ? f->obj : f->rx ? (const void *)f->rx : (const void *)f->tx;   /* (obj: kept from before shutdown) */
    if (f->kind == FD_EVENT || f->kind == FD_EPOLL || f->kind == FD_SHM || f->kind == FD_WIN || f->kind == FD_TIMER || f->kind == FD_INET || f->kind == FD_INOTIFY) return f->obj;
    return f;
}
/* ready entries into out (12-byte struct epoll_event, packed), up to max; out 0: just count (no one-shot change) */
static int epoll_scan(struct lxproc *lx, struct epoll *ep, uint8_t *out, int max) {
    static int depth;
    if (depth > 2) return 0;                                    /* (epolls watching epolls) */
    depth++;
    for (int i = 0; i < ep->n;) {                                /* drop files closed since (Linux drops them too) */
        struct epitem *it = &ep->it[i];
        struct lfd *f = it->fd >= 0 && it->fd < NFD ? &lx->fd[it->fd] : 0;
        if (!f || !f->kind || fd_id(f) != it->id) ep->it[i] = ep->it[--ep->n]; else i++;
    }
    int k = 0, start = ep->n ? ep->next % ep->n : 0, last = -1;
    for (int j = 0; j < ep->n && k < max; j++) {
        int i = (start + j) % ep->n;                             /* round robin, so busy files don't starve the rest */
        struct epitem *it = &ep->it[i];
        if (it->off) continue;
        uint32_t r = (uint32_t)fd_ready(lx, it->fd) & (it->events | POLLERR_ | POLLHUP_) & ~(EPOLLONESHOT_ | EPOLLET_);
        if (!r) continue;
        if (out) {
            *(uint32_t *)(out + k * 12) = r; *(uint64_t *)(out + k * 12 + 4) = it->data;
            if (it->events & EPOLLONESHOT_) it->off = true;
            last = i;
        }
        k++;
    }
    if (last >= 0) ep->next = last + 1;                          /* next time, start after the last one reported */
    depth--;
    return k;
}
static uint64_t do_epoll_wait(struct lxproc *lx, uint64_t epfd, uint64_t uev, int max, int64_t ms) {
    if (epfd >= NFD || lx->fd[epfd].kind != FD_EPOLL) return epfd < NFD && lx->fd[epfd].kind ? E(EINVAL_) : E(EBADF_);
    if (max <= 0 || max > 4096) return E(EINVAL_);
    if (!uok(uev, (uint64_t)max * 12, true)) return E(EFAULT_);
    uint64_t end = ms > 0 ? uptime_ms() + (uint64_t)ms + 1 : 0;   /* (+1: a whole ms, never early) */
    for (;;) {
        struct epoll *ep = lx->fd[epfd].obj;
        if (lx->fd[epfd].kind != FD_EPOLL) return E(EBADF_);   /* (closed by another thread while waiting) */
        int k = epoll_scan(lx, ep, (uint8_t *)uev, max);
        if (k || ms == 0) return (uint64_t)k;
        int64_t left = -1;
        if (ms > 0) { uint64_t now = uptime_ms(); if (now >= end) return 0; left = (int64_t)(end - now); }
        if (sig_now()) return E(EINTR_);                         /* a signal: its handler runs first */
        if (snd_waiting) { snd_waiting = false; if (left < 0 || left > 5) left = 5; }
        if (timer_wake_at) {                                    /* a timerfd's deadline */
            uint64_t now = uptime_ns(), w = timer_wake_at > now ? (timer_wake_at - now + 999999) / 1000000 : 1;
            timer_wake_at = 0;
            if (left < 0 || (int64_t)w < left) left = (int64_t)w;
        }
        chan_wait(left);
    }
}
/* ---------------- internet sockets: TCP and UDP through AXIOM's network stack (net.c) ----------------
 * AF_INET and AF_INET6 (an IPv6 socket also reaches IPv4 addresses, written ::ffff:a.b.c.d). Nothing in net.c
 * waits: a socket that would block waits here (chan_wait) and net.c's thread wakes the waiters whenever a
 * connection or a UDP port of a Linux program changes (linux_net_wake). No listening (accept) yet. */
struct isock { int refs; int type; int fam;          /* type 1 SOCK_STREAM, 2 SOCK_DGRAM; fam 2 AF_INET, 10 AF_INET6 */
               int h;                                /* the net.c handle, or -1 */
               bool peer_set; struct ipaddr peer; uint16_t pport;   /* connected to (UDP: the default destination) */
               uint16_t bport; int err; bool rd_shut, wr_shut, connecting; };
void linux_net_wake(void) { chan_wake(); }
static uint64_t neterr(long r) {
    switch (r) {
    case NET_EAGAIN: return E(EAGAIN_);
    case NET_EREFUSED: return E(ECONNREFUSED_);
    case NET_ERESET: return E(ECONNRESET_);
    case NET_ETIMEDOUT: return E(ETIMEDOUT_);
    case NET_EUNREACH: return E(EHOSTUNREACH_);
    case NET_ENOTCONN: case NET_ENODEV: return E(ENETUNREACH_);
    case NET_ENOMEM: return E(ENOBUFS_);
    default: return E(EINVAL_);
    }
}
static void isock_put(struct isock *s) {
    if (!s || --s->refs > 0) return;
    if (s->h >= 0) { if (s->type == 1) tcp_sock_close(s->h, s); else udp_sock_close(s->h, s); }
    kfree(s);
}
static int isock_ready(struct isock *s) {
    if (s->type == 2) return (s->h >= 0 && udp_sock_pending(s->h, s) ? POLLIN_ : 0) | POLLOUT_;
    if (s->h < 0) return s->err ? POLLERR_ | POLLHUP_ | POLLOUT_ : POLLOUT_ | POLLHUP_;   /* (as Linux says for a socket never connected) */
    struct tcp_sock_info i;
    if (tcp_sock_info(s->h, s, &i)) return POLLERR_ | POLLHUP_;
    if (i.state == 0) return 0;                                  /* still connecting */
    int r = 0;
    if (i.rlen || i.eof || s->rd_shut) r |= POLLIN_;
    if (i.eof || s->rd_shut) r |= POLLRDHUP_;
    if (i.state == 1 && i.sfree) r |= POLLOUT_;
    if (i.state == 2) r |= POLLIN_ | POLLOUT_ | POLLHUP_ | (i.err ? POLLERR_ : 0);
    if (i.state == 3 && i.eof) r |= POLLHUP_;
    return r;
}
/* a user sockaddr: AF_INET (16 bytes) or AF_INET6 (28); ::ffff:a.b.c.d becomes the IPv4 address.
 * Returns 0, 1 for AF_UNSPEC, or an error */
static int64_t sa_get(uint64_t ua, uint64_t len, struct ipaddr *ip, uint16_t *port) {
    if (len < 2 || !uok(ua, len, false)) return (int64_t)E(EFAULT_);
    const uint8_t *a = (const uint8_t *)ua;
    uint16_t fam = *(const uint16_t *)a;
    memset(ip, 0, sizeof *ip);
    if (fam == 0) return 1;
    if (fam == 2) { if (len < 16) return (int64_t)E(EINVAL_); *port = (uint16_t)(a[2] << 8 | a[3]); memcpy(ip->b, a + 4, 4); return 0; }
    if (fam == 10) {
        if (len < 24) return (int64_t)E(EINVAL_);
        *port = (uint16_t)(a[2] << 8 | a[3]);
        static const uint8_t mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
        if (!memcmp(a + 8, mapped, 12)) { memcpy(ip->b, a + 20, 4); return 0; }
        ip->v6 = 1; memcpy(ip->b, a + 8, 16); return 0;
    }
    return (int64_t)E(EAFNOSUPPORT_);
}
/* write an address back in the socket's family (ulen: the user's socklen_t, in and out) */
static void sa_put(const struct isock *s, uint64_t ua, uint64_t ulen, const struct ipaddr *ip, uint16_t port) {
    if (!ua || !ulen || !uok(ulen, 4, true)) return;
    uint8_t b[28]; memset(b, 0, sizeof b);
    uint32_t n;
    b[2] = (uint8_t)(port >> 8); b[3] = (uint8_t)port;
    if (s->fam == 2) { *(uint16_t *)b = 2; if (!ip->v6) memcpy(b + 4, ip->b, 4); n = 16; }
    else {
        *(uint16_t *)b = 10; n = 28;
        if (ip->v6) memcpy(b + 8, ip->b, 16);
        else if (ip->b[0] | ip->b[1] | ip->b[2] | ip->b[3]) { b[18] = b[19] = 0xFF; memcpy(b + 20, ip->b, 4); }
    }
    uint32_t have = *(uint32_t *)ulen, k = have < n ? have : n;
    if (k && uok(ua, k, true)) memcpy((void *)ua, b, k);
    *(uint32_t *)ulen = n;
}
static int64_t isock_connect(struct isock *s, const struct ipaddr *ip, uint16_t port, bool nb) {
    if (s->fam == 2 && ip->v6) return (int64_t)E(EAFNOSUPPORT_);
    if (s->type == 2) {
        if (s->h < 0) { int h = udp_sock_open(s, s->bport); if (h < 0) return (int64_t)neterr(h); s->h = h; }
        s->peer = *ip; s->pport = port; s->peer_set = true;
        return 0;
    }
    if (s->h >= 0) return (int64_t)E(s->connecting ? EALREADY_ : EISCONN_);
    int h = tcp_sock_open(ip, port, s);
    if (h < 0) return (int64_t)neterr(h);
    s->h = h; s->peer = *ip; s->pport = port; s->peer_set = true; s->connecting = true;
    if (nb) return (int64_t)E(EINPROGRESS_);
    struct tcp_sock_info i;
    while (!tcp_sock_info(s->h, s, &i) && i.state == 0) { chan_wait(-1); if (sig_now()) return (int64_t)E(EINTR_); }
    s->connecting = false;
    if (i.state == 2 && i.err) { s->err = 0; return (int64_t)neterr(i.err); }
    return 0;
}
/* the result of a connect started without waiting (SO_ERROR), once */
static int isock_soerror(struct isock *s) {
    if (s->err) { int e = s->err; s->err = 0; return e; }
    if (s->type != 1 || s->h < 0 || !s->connecting) return 0;
    struct tcp_sock_info i;
    if (tcp_sock_info(s->h, s, &i) || i.state == 0) return 0;
    s->connecting = false;
    return i.state == 2 && i.err ? (int)-(int64_t)neterr(i.err) : 0;
}
static int64_t isock_send(struct isock *s, const uint8_t *buf, uint64_t n, const struct ipaddr *to, uint16_t tport, bool nb) {
    if (s->type == 2) {
        if (!to) { if (!s->peer_set) return (int64_t)E(EDESTADDRREQ_); to = &s->peer; tport = s->pport; }
        if (s->fam == 2 && to->v6) return (int64_t)E(EAFNOSUPPORT_);
        if (s->h < 0) { int h = udp_sock_open(s, s->bport); if (h < 0) return (int64_t)neterr(h); s->h = h; }
        if (n > 1440) return (int64_t)E(EMSGSIZE_);
        long r = udp_sock_send(s->h, s, to, tport, buf, (uint32_t)n);
        return r < 0 ? (int64_t)neterr(r) : r;
    }
    if (s->h < 0) return (int64_t)E(ENOTCONN_);
    if (s->wr_shut) return (int64_t)E(EPIPE_);
    uint64_t done = 0;
    for (;;) {                                                   /* blocking: all of it; O_NONBLOCK: what fits */
        long r = done < n ? tcp_sock_send(s->h, s, buf + done, (uint32_t)(n - done > (1u << 20) ? 1u << 20 : n - done)) : 0;
        if (r > 0) { done += (uint64_t)r; continue; }
        if (done == n) break;
        if (r == NET_EAGAIN) { if (nb) break; chan_wait(-1); if (sig_now()) return (int64_t)E(EINTR_); continue; }   /* (also while still connecting) */
        if (done) break;
        return (int64_t)(r == NET_ERESET ? E(EPIPE_) : neterr(r));
    }
    return done || !n ? (int64_t)done : (int64_t)E(EAGAIN_);
}
static int64_t isock_recv(struct isock *s, uint8_t *buf, uint64_t n, bool nb, bool peek, struct ipaddr *from, uint16_t *fport, uint32_t *full) {
    if (n > (1u << 30)) n = 1u << 30;
    for (;;) {
        long r;
        if (s->type == 2) {
            if (s->h < 0) r = NET_EAGAIN;
            else r = udp_sock_recv(s->h, s, buf, (uint32_t)n, from, fport, peek, full);
        } else {
            if (s->rd_shut) return 0;
            if (s->h < 0) return (int64_t)E(ENOTCONN_);
            r = tcp_sock_recv(s->h, s, buf, (uint32_t)n, peek);
            if (r == NET_EAGAIN) { struct tcp_sock_info i; if (!tcp_sock_info(s->h, s, &i) && i.state == 3 && i.eof) r = 0; }
            if (from && r >= 0) { *from = s->peer; *fport = s->pport; }
        }
        if (r != NET_EAGAIN || nb) return r < 0 ? (int64_t)neterr(r) : r;
        chan_wait(-1); if (sig_now()) return (int64_t)E(EINTR_);
    }
}
static uint64_t isock_new(struct lxproc *lx, int fam, int type, int flags) {
    struct isock *s = kmalloc(sizeof *s);
    if (!s) return E(ENOMEM_);
    int k = fd_new(lx, 0);
    if (k < 0) { kfree(s); return E(EMFILE_); }
    memset(s, 0, sizeof *s); s->refs = 1; s->type = type; s->fam = fam; s->h = -1;
    struct lfd *fd = &lx->fd[k]; memset(fd, 0, sizeof *fd);
    fd->kind = FD_INET; fd->obj = s; fd->sock = true; fd->stype = (uint8_t)type; fd->flags = 2 | (flags & 04000);
    return (uint64_t)k;
}
/* /etc/resolv.conf and /etc/hosts for the C library's name lookups: the DNS server AXIOM uses now */
static void lx_net_files(void) {
    struct dunc_netinfo ni; net_get_info(&ni);
    char txt[128], ip[IP_STRLEN];
    bool have = ni.dns_now.v6 || ni.dns_now.b[0] || ni.dns_now.b[1] || ni.dns_now.b[2] || ni.dns_now.b[3];
    if (have) ip_format(&ni.dns_now, ip); else strncpy0(ip, "10.0.2.3", sizeof ip);
    int n = ksnprintf(txt, sizeof txt, "# made by AXIOM: the DNS server it uses now\nnameserver %s\n", ip);
    char old[128]; long k = vfs_read("/system/linux/etc/resolv.conf", old, 0, sizeof old - 1);
    if (k != n || memcmp(old, txt, n)) vfs_write("/system/linux/etc/resolv.conf", txt, n);
    struct dunc_stat st;
    if (vfs_stat("/system/linux/etc/hosts", &st) != 0) {
        static const char hosts[] = "127.0.0.1 localhost\n::1 localhost ip6-localhost ip6-loopback\n";
        vfs_write("/system/linux/etc/hosts", hosts, sizeof hosts - 1);
    }
}

/* ---------------- shared memory: memfd_create ----------------
 * A memfd is a list of pages, given as they're first needed. Mapping it MAP_SHARED puts those same pages in
 * the program's page tables (as SHARED: page tables never free them). It lives while an fd or a mapping
 * holds it; each mapping is remembered (struct shmmap) so munmap, MAP_FIXED over it and exit let go. */
struct shm { int refs; uint64_t size, npg; uint64_t *pg; int seals;   /* pg: physical addresses (0: not made yet) */
             struct task *win_owner; int win_id; };   /* a window's pixels: the pages are the window's, and when the last
                                                          mapping or file lets go of them the window closes */
enum { SEAL_SEAL_ = 1, SEAL_SHRINK_ = 2, SEAL_GROW_ = 4, SEAL_WRITE_ = 8, SEAL_FUTURE_WRITE_ = 16, SEAL_EXEC_ = 32 };
struct shmmap { uint64_t s, e; struct shm *obj; uint64_t pgoff; };
static void shm_put(struct shm *o) {
    if (!o || --o->refs > 0) return;
    if (o->win_id) wm_close(o->win_owner, o->win_id);           /* (frees its pages; nothing maps them any more) */
    else for (uint64_t i = 0; i < o->npg; i++) if (o->pg[i]) upage_free(o->pg[i]);
    kfree(o->pg); kfree(o);
}
static bool shm_resize(struct shm *o, uint64_t size) {        /* (pages beyond a smaller size stay until the end:
                                                                   they may still be mapped) */
    uint64_t np = (size + 4095) / 4096;
    if (np > o->npg) {
        uint64_t *n = kmalloc(np * 8);
        if (!n) return false;
        memset(n, 0, np * 8);
        if (o->pg) memcpy(n, o->pg, o->npg * 8);
        kfree(o->pg); o->pg = n; o->npg = np;
    }
    if (size < o->size)                                         /* what's cut off reads as zeros if it grows again */
        for (uint64_t a = size; a < o->size && a / 4096 < o->npg;) {
            uint64_t i = a / 4096, in = a % 4096, k = 4096 - in;
            if (o->pg[i]) memset((uint8_t *)mmio_virt(o->pg[i]) + in, 0, k);
            a += k;
        }
    o->size = size;
    return true;
}
static uint8_t *shm_page(struct shm *o, uint64_t i) {           /* the page, made if need be */
    if (i >= o->npg) return 0;
    if (!o->pg[i]) o->pg[i] = upage_alloc();                    /* (programs' memory: above 4 GiB when there is any) */
    return o->pg[i] ? (uint8_t *)mmio_virt(o->pg[i]) : 0;
}
/* copy between the memfd at off and buf (user memory, already checked); returns bytes copied */
static int64_t shm_rw(struct shm *o, uint64_t off, uint8_t *buf, uint64_t n, bool write) {
    if (write) { if (off + n > o->size && !shm_resize(o, off + n)) return (int64_t)E(ENOMEM_); }
    else { if (off >= o->size) return 0; if (n > o->size - off) n = o->size - off; }
    for (uint64_t done = 0; done < n;) {
        uint64_t a = off + done, in = a % 4096, k = 4096 - in; if (k > n - done) k = n - done;
        uint8_t *pg = shm_page(o, a / 4096);
        if (!pg) return done ? (int64_t)done : (int64_t)E(ENOMEM_);
        if (write) memcpy(pg + in, buf + done, k); else memcpy(buf + done, pg + in, k);
        done += k;
    }
    return (int64_t)n;
}
/* for linux_snd.c: a sound device's ring, shared with the program through mmap */
void *lx_shm_make(uint64_t size) {
    struct shm *o = kmalloc(sizeof *o);
    if (!o) return 0;
    memset(o, 0, sizeof *o); o->refs = 1;
    if (!shm_resize(o, size)) { kfree(o); return 0; }
    return o;
}
void lx_shm_put(void *o) { shm_put(o); }
void lx_shm_rw(void *o, uint64_t off, void *buf, uint64_t n, bool write) { shm_rw(o, off, buf, n, write); }
/* [s, e) of the program's memory is going away (munmap, MAP_FIXED over it): forget the shared mappings there */
static void shm_unmapped(struct lxproc *lx, uint64_t s, uint64_t e) {
    for (int i = 0; i < lx->nmaps;) {
        struct shmmap *m = &lx->maps[i];
        if (m->e <= s || m->s >= e) { i++; continue; }
        if (m->s >= s && m->e <= e) {                            /* all of it */
            shm_put(m->obj); lx->maps[i] = lx->maps[--lx->nmaps]; continue;
        }
        if (m->s < s && m->e > e) {                              /* a hole in the middle: two mappings now */
            if (lx->nmaps == lx->capmaps) {                      /* (no room to split: keep it whole, which only holds the memory longer) */
                i++; continue;
            }
            struct shmmap r = { e, m->e, m->obj, m->pgoff + (e - m->s) / 4096 };
            m->e = s; m->obj->refs++; lx->maps[lx->nmaps++] = r;
            i++; continue;
        }
        if (m->s < s) m->e = s;                                  /* its end */
        else { m->pgoff += (e - m->s) / 4096; m->s = e; }        /* its start */
        i++;
    }
}
static void shm_forget_all(struct lxproc *lx) {              /* at exit (its page tables go too; they never free these) */
    for (int i = 0; i < lx->nmaps; i++) shm_put(lx->maps[i].obj);
    kfree(lx->maps); lx->maps = 0; lx->nmaps = lx->capmaps = 0;
}
/* map pages [pgoff, ...) of o at [va, va + len) of the program (the area is already made) */
static void shm_map_pages(struct mm *mm, struct shm *o, uint64_t va, uint64_t len, uint64_t pgoff, int prot) {
    for (uint64_t i = 0; i < len / 4096 && pgoff + i < o->npg && (pgoff + i) * 4096 < o->size; i++) {
        if (shm_page(o, pgoff + i)) mm_map_shared(mm, va + i * 4096, o->pg[pgoff + i], prot);   /* (physical) */
    }
}

static int chan_fds(struct lxproc *lx, struct chan *r0, struct chan *w0, struct chan *r1, struct chan *w1, bool sock, int flags, int out[2]) {
    int a = fd_new(lx, 0); if (a < 0) return -1;
    lx->fd[a].kind = FD_CHAN;                                    /* (reserve it while finding the second) */
    int b = fd_new(lx, 0); if (b < 0) { lx->fd[a].kind = FD_NONE; return -1; }
    struct lfd *fa = &lx->fd[a], *fb = &lx->fd[b];
    memset(fa, 0, sizeof *fa); memset(fb, 0, sizeof *fb);
    fa->kind = fb->kind = FD_CHAN; fa->sock = fb->sock = sock;
    fa->flags = fb->flags = 2 | (flags & 04000);                 /* O_RDWR, and O_NONBLOCK if asked for */
    fa->rx = r0; fa->tx = w0; fb->rx = r1; fb->tx = w1;
    if (r0) r0->readers++;
    if (w0) w0->writers++;
    if (r1) r1->readers++;
    if (w1) w1->writers++;
    out[0] = a; out[1] = b;
    return 0;
}
/* ---------------- Unix sockets with names (bind / listen / connect / accept) ----------------
 * A listening socket is a name in a kernel table (not a file on disk: stat sees nothing there, which is what
 * servers that clean up an old socket file expect). connect makes a socket pair's two chans, keeps the
 * server's ends in the listener's queue, and accept hands them to a new file. Abstract names (a leading 0
 * byte) are kept as "@name". */
static struct ulisten *ulistens;
static bool wayland_starting;
static void ulisten_put(struct ulisten *l) {
    if (--l->refs > 0) return;
    for (struct ulisten **pp = &ulistens; *pp; pp = &(*pp)->next) if (*pp == l) { *pp = l->next; break; }
    while (l->qn) {                                /* connections nobody accepted: their clients see the end */
        struct pend *p = &l->q[l->qh]; chan_drop(p->rx, true); chan_drop(p->tx, false);
        l->qh = (l->qh + 1) % ULQ; l->qn--;
    }
    kfree(l);
    chan_wake();
}
/* a sockaddr_un from the program as a table name; false if it isn't one */
static bool un_name(struct lxproc *lx, uint64_t ua, uint64_t len, char *out, size_t n) {
    if (len < 3 || len > 110 || !uok(ua, len, false)) return false;
    const char *sa = (const char *)ua;
    if (*(uint16_t *)sa != 1) return false;        /* AF_UNIX */
    const char *p = sa + 2; size_t pl = len - 2;
    if (!p[0]) {                                   /* abstract: the bytes after the 0, as given */
        if (pl - 1 + 2 > n) return false;
        out[0] = '@'; memcpy(out + 1, p + 1, pl - 1); out[pl] = 0;
        for (size_t i = 1; i < pl; i++) if (!out[i]) out[i] = '?';
        return true;
    }
    char tmp[110]; size_t k = 0; while (k < pl && k < sizeof tmp - 1 && p[k]) { tmp[k] = p[k]; k++; } tmp[k] = 0;
    path_join(lx->cwd, tmp, out, n);
    return true;
}
static struct ulisten *ulisten_find(const char *name) {
    for (struct ulisten *l = ulistens; l; l = l->next) if (!strcmp(l->name, name)) return l;
    return 0;
}
/* write a sockaddr_un with name (or just the family) to the program's addr / addrlen */
static void un_addr_out(uint64_t ua, uint64_t ulen, const char *name) {
    if (!ua || !ulen || !uok(ulen, 4, true)) return;
    uint32_t cap = *(uint32_t *)ulen;
    uint8_t sa[110]; memset(sa, 0, sizeof sa); *(uint16_t *)sa = 1;
    uint32_t len = 2;
    if (name && name[0]) {
        size_t nl = strlen(name); if (nl > 107) nl = 107;
        if (name[0] == '@') { sa[2] = 0; memcpy(sa + 3, name + 1, nl - 1); len = 2 + (uint32_t)nl; }
        else { memcpy(sa + 2, name, nl); len = 3 + (uint32_t)nl; }
    }
    uint32_t k = len < cap ? len : cap;
    if (k && uok(ua, k, true)) memcpy((void *)ua, sa, k);
    *(uint32_t *)ulen = len;
}
/* ---------------- /dev: the few devices every Linux program expects ---------------- */
enum { DEV_NULL = 1, DEV_ZERO, DEV_FULL, DEV_RANDOM, DEV_URANDOM, DEV_WINDOW, DEV_SNDCTL, DEV_SNDPCM };
static const struct { const char *name; int minor; } devs[] = {
    { 0, 0 }, { "null", 3 }, { "zero", 5 }, { "full", 7 }, { "random", 8 }, { "urandom", 9 },
    { "axiom/window", 0 },                                       /* (char device 240:0, a "local use" number) */
    { "snd/controlC0", 0 }, { "snd/pcmC0D0p", 16 },             /* sound (ALSA: char devices 116:0 and 116:16) */
};
static int dev_of(const char *full) {                           /* full: a joined path (/system/linux/dev/...) */
    if (strncmp(full, "/system/linux/dev/", 18)) return 0;
    for (int i = 1; i < (int)(sizeof devs / sizeof devs[0]); i++) if (!strcmp(full + 18, devs[i].name)) return i;
    return 0;
}
static void dev_stat(struct lstat *st, int dev) {
    fill_stat(st, false, 0, false, devs[dev].name);
    st->mode = 0020666; st->rdev = dev == DEV_WINDOW ? 240 << 8 : dev >= DEV_SNDCTL ? (116 << 8) | (uint64_t)devs[dev].minor : (1 << 8) | (uint64_t)devs[dev].minor; st->blocks = 0;
}

/* ---------------- AXIOM windows (common/axiom_window.h) ----------------
 * One file, one window. Its pixels are a struct shm borrowing the window's pages, so mmap, munmap and exit
 * use the shared-memory code; the window closes when the file and every mapping have let go. Events for
 * a Linux program's windows come here (event_push) instead of the task's own queue. */
static struct axwin *axwins;
static struct shm *axwin_pixels(struct task *owner, int id) {    /* the window's current pages as a shm */
    int pages, w, h;
    uint32_t *buf = wm_buffer(owner, id, &pages, &w, &h);
    struct shm *o = buf ? kmalloc(sizeof *o) : 0;
    if (!o) return 0;
    memset(o, 0, sizeof *o);
    if (!(o->pg = kmalloc((uint64_t)pages * 8))) { kfree(o); return 0; }
    for (int i = 0; i < pages; i++) o->pg[i] = (uint64_t)buf + (uint64_t)i * 4096;
    o->npg = (uint64_t)pages; o->size = (uint64_t)w * h * 4; o->refs = 1;
    o->seals = SEAL_SEAL_ | SEAL_SHRINK_ | SEAL_GROW_;
    o->win_owner = owner; o->win_id = id;
    return o;
}
static void axwin_put(struct axwin *x) {
    if (!x || --x->refs > 0) return;
    for (struct axwin **pp = &axwins; *pp; pp = &(*pp)->next) if (*pp == x) { *pp = x->next; break; }
    shm_put(x->pix);                                             /* (closes the window unless still mapped) */
    kfree(x);
}
bool linux_window_event(struct task *t, const struct dunc_event *ev) {
    for (struct axwin *x = axwins; x; x = x->next) {
        if (x->owner != t || x->id != ev->win) continue;
        if (ev->type == EV_MOUSE_MOVE && x->qh != x->qt) {        /* coalesce motion, as AXIOM does for its apps */
            struct axwin_event *last = &x->q[(x->qh + AXQ - 1) % AXQ];
            if (last->type == EV_MOUSE_MOVE) { memcpy(last, ev, sizeof *last); return true; }
        }
        int nh = (x->qh + 1) % AXQ;
        if (nh != x->qt) { memcpy(&x->q[x->qh], ev, sizeof x->q[0]); x->qh = nh; }   /* (full: dropped) */
        chan_wake();
        return true;
    }
    return true;                     /* (a closed one's: a Linux program never reads the task's own queue) */
}
static uint64_t axwin_ioctl(struct lxproc *lx, struct lfd *fd, uint64_t req, uint64_t arg) {
    struct axwin *x = fd->obj;
    struct task *P = current->proc ? current->proc : current;
    switch (req) {
    case AXWIN_CLIP_SET: {
        if (!uok(arg, sizeof(struct axwin_clip), true)) return E(EFAULT_);
        struct axwin_clip *c = (struct axwin_clip *)arg;
        if (c->len > (1u << 20)) return E(EINVAL_);
        if (c->len && !uok(c->data, c->len, false)) return E(EFAULT_);
        char *k = kmalloc(c->len ? c->len : 1);
        if (!k) return E(ENOMEM_);
        memcpy(k, (void *)c->data, c->len);
        int r = clip_set_utf8(k, c->len);
        kfree(k);
        if (r) return E(ENOMEM_);
        const char *d; clip_get_utf8(&d, &c->seq);
        return 0;
    }
    case AXWIN_CLIP_GET: {
        if (!uok(arg, sizeof(struct axwin_clip), true)) return E(EFAULT_);
        struct axwin_clip *c = (struct axwin_clip *)arg;
        const char *d; uint64_t seq, n = clip_get_utf8(&d, &seq), room = c->len;
        if (c->data) {
            uint64_t k = n < room ? n : room;
            if (k && !uok(c->data, k, true)) return E(EFAULT_);
            if (k) memcpy((void *)c->data, d, k);
        }
        c->len = n; c->seq = seq;
        return 0;
    }
    case AXWIN_CREATE: {
        if (x->id) return E(EEXIST_);
        if (!uok(arg, sizeof(struct axwin_create), true)) return E(EFAULT_);
        struct axwin_create *c = (struct axwin_create *)arg;
        char title[64]; memcpy(title, c->title, 63); title[63] = 0;
        uint32_t *buf; int pages;
        int fl = ((c->flags & AXWIN_RESIZABLE) ? WIN_RESIZABLE : 0) | ((c->flags & AXWIN_POPUP) ? WIN_POPUP : 0) |
                 ((c->flags & AXWIN_MOVE_EVENTS) ? WIN_MOVE_EVENTS : 0) | ((c->flags & AXWIN_HIDDEN) ? WIN_HIDDEN : 0);
        bool placed = x->px != 0 || x->py != 0;
        struct window *w = wm_create_at(P, c->w, c->h, title, &buf, &pages, 0, fl, placed ? x->px - 1 : WM_ANYWHERE, placed ? x->py - 1 : WM_ANYWHERE);
        if (!w) return E(EINVAL_);                               /* (too big or small for the screen, or too many) */
        x->owner = P; x->id = wm_id(w); x->resizable = c->flags & AXWIN_RESIZABLE;
        if (!(x->pix = axwin_pixels(P, x->id))) { wm_close(P, x->id); x->id = 0; return E(ENOMEM_); }
        if (!(fl & (WIN_POPUP | WIN_HIDDEN))) {                 /* it has the focus: the desktop said so before it had an id here */
            struct dunc_event fe = { EV_FOCUS, x->id, 0, 0, 0, 0 };
            linux_window_event(P, &fe);
        }
        return (uint64_t)x->id;
    }
    case AXWIN_INFO: {                                          /* (before AXWIN_CREATE: just the screen's size) */
        if (!uok(arg, sizeof(struct axwin_info), true)) return E(EFAULT_);
        int pages, w = 0, h = 0, sw, sh;
        if (x->id && !wm_buffer(x->owner, x->id, &pages, &w, &h)) return E(ENOENT_);
        wm_screen(&sw, &sh);
        *(struct axwin_info *)arg = (struct axwin_info){ x->id, w, h, w * 4, sw, sh };
        return 0;
    }
    case AXWIN_FLUSH: {
        if (!x->id) return E(ENOENT_);
        if (!uok(arg, sizeof(struct axwin_rect), false)) return E(EFAULT_);
        struct axwin_rect *r = (struct axwin_rect *)arg;
        return wm_flush(x->owner, x->id, r->x, r->y, r->w, r->h) ? 0 : E(ENOENT_);
    }
    case AXWIN_TITLE: {
        if (!x->id) return E(ENOENT_);
        char title[64];
        if (!uok(arg, 1, false) || !ustr(title, arg, sizeof title)) return E(EFAULT_);
        return wm_set_title(x->owner, x->id, title) ? 0 : E(ENOENT_);
    }
    case AXWIN_MOVE: {
        if (!uok(arg, sizeof(struct axwin_rect), false)) return E(EFAULT_);
        struct axwin_rect *r = (struct axwin_rect *)arg;
        if (!x->id) { x->px = r->x + 1; x->py = r->y + 1; return 0; }   /* (+1: 0 means not said) */
        return wm_move(x->owner, x->id, r->x, r->y) ? 0 : E(ENOENT_);
    }
    case AXWIN_APP: {
        if (!x->id) return E(ENOENT_);
        if (!uok(arg, 4, false)) return E(EFAULT_);
        int pid = *(int32_t *)arg;
        for (int i = 0; i < MAX_TASKS; i++)
            if (tasks[i].state && tasks[i].state != PS_ZOMBIE && tasks[i].pid == pid)
                return wm_set_app(x->owner, x->id, tasks[i].app ? tasks[i].app : "", tasks[i].name) ? 0 : E(ENOENT_);
        return E(ESRCH_);
    }
    case AXWIN_SHOW: case AXWIN_CURSOR: {
        if (!x->id) return E(ENOENT_);
        if (!uok(arg, 4, false)) return E(EFAULT_);
        int v = *(int32_t *)arg;
        bool ok = req == AXWIN_SHOW ? wm_show(x->owner, x->id, v != 0) : wm_set_cursor(x->owner, x->id, v);
        return ok ? 0 : E(ENOENT_);
    }
    case AXWIN_POS: {
        if (!x->id) return E(ENOENT_);
        if (!uok(arg, sizeof(struct axwin_rect), true)) return E(EFAULT_);
        int px, py, pages, w, h;
        if (!wm_pos(x->owner, x->id, &px, &py) || !wm_buffer(x->owner, x->id, &pages, &w, &h)) return E(ENOENT_);
        *(struct axwin_rect *)arg = (struct axwin_rect){ px, py, w, h };
        return 0;
    }
    case AXWIN_RESIZE: case AXWIN_RESIZE_TO: {
        if (!x->id) return E(ENOENT_);
        if (!x->resizable) return E(EINVAL_);
        int nw, nh, tw = 0, th = 0;
        if (req == AXWIN_RESIZE_TO) {
            if (!uok(arg, sizeof(struct axwin_rect), false)) return E(EFAULT_);
            tw = ((struct axwin_rect *)arg)->w; th = ((struct axwin_rect *)arg)->h;
            if (tw <= 0 || th <= 0) return E(EINVAL_);
        }
        if (x->pix->refs > 1) return E(EBUSY_);                 /* still mapped (by any process sharing it): munmap first (its pages are about to go) */
        if (!(req == AXWIN_RESIZE_TO ? wm_resize_to(x->owner, x->id, tw, th, &nw, &nh) : wm_resize(x->owner, x->id, &nw, &nh))) return E(ENOMEM_);
        struct shm *np = axwin_pixels(x->owner, x->id);
        if (!np) return E(ENOMEM_);
        kfree(x->pix->pg); kfree(x->pix);                        /* (the old pages were the window's; wm_resize freed them) */
        x->pix = np;
        return 0;
    }
    }
    (void)lx;
    return E(ENOTTY_);
}

/* /proc/self/maps: the program's memory areas, with the file each part came from; a snapshot in memory */
static struct shm *proc_maps(struct lxproc *lx, struct mm *m) {
    struct shm *o = kmalloc(sizeof *o);
    char *txt = kmalloc(65536);
    if (!o || !txt) { kfree(o); kfree(txt); return 0; }
    memset(o, 0, sizeof *o); o->refs = 1;
    uint64_t n = 0, stack = USER_MMAP_END - STACK_SIZE;
    for (int i = 0; i < m->nv && n + 300 < 65536; i++) {
        uint64_t cur = m->v[i].s, end = m->v[i].e; int prot = m->v[i].prot;
        while (cur < end && n + 300 < 65536) {
            const struct fmap *in = 0; uint64_t next = end;
            for (int k = 0; k < lx->nfmaps; k++) {
                const struct fmap *f = &lx->fmaps[k];
                if (f->s <= cur && f->e > cur) { in = f; break; }
                if (f->s > cur && f->s < next) next = f->s;
            }
            uint64_t pe = in ? (in->e < end ? in->e : end) : next;
            const char *name = in ? in->path : cur >= stack ? "[stack]" : "";
            n += (uint64_t)ksnprintf(txt + n, 65536 - n, "%lx-%lx %c%c%cp %08lx 00:00 %lu %s\n", cur, pe,
                                     prot & DPROT_READ ? 'r' : '-', prot & DPROT_WRITE ? 'w' : '-', prot & DPROT_EXEC ? 'x' : '-',
                                     in ? in->off + (cur - in->s) : 0, in ? path_ino(in->path) & 0xFFFFFFF : 0, name);
            cur = pe;
        }
    }
    shm_rw(o, 0, (uint8_t *)txt, n, true);
    kfree(txt);
    return o;
}

/* /proc/cpuinfo and /proc/meminfo: generated text, read like a file (Chromium reads both at start-up) */
static struct shm *proc_text(const char *which) {
    struct shm *o = kmalloc(sizeof *o);
    char *txt = kmalloc(32768);
    if (!o || !txt) { kfree(o); kfree(txt); return 0; }
    memset(o, 0, sizeof *o); o->refs = 1;
    int n = 0;
    if (!strcmp(which, "cpuinfo")) {
        uint32_t r[12] = { 0 }, a, b, c, d, v[3]; char brand[49] = { 0 }, vendor[13] = { 0 };
        cpuid(0, 0, &a, &v[0], &v[2], &v[1]); memcpy(vendor, v, 12);
        cpuid(1, 0, &a, &b, &c, &d);
        uint32_t family = (a >> 8) & 15, model = (a >> 4) & 15, stepping = a & 15;
        if (family == 15) family += (a >> 20) & 255;
        if (family >= 6) model |= ((a >> 16) & 15) << 4;
        cpuid(0x80000000, 0, &a, &b, &c, &d);
        if (a >= 0x80000004) for (int i = 0; i < 3; i++) cpuid(0x80000002 + i, 0, &r[i * 4], &r[i * 4 + 1], &r[i * 4 + 2], &r[i * 4 + 3]);
        memcpy(brand, r, 48);
        const char *bp = brand; while (*bp == ' ') bp++;
        for (int i = 0; i < ncpus_on && n < 30000; i++)
            n += ksnprintf(txt + n, 32768 - n, "processor\t: %d\nvendor_id\t: %s\ncpu family\t: %u\nmodel\t\t: %u\n"
                           "model name\t: %s\nstepping\t: %u\ncpu MHz\t\t: %lu.000\nphysical id\t: 0\nsiblings\t: %d\n"
                           "core id\t\t: %d\ncpu cores\t: %d\nflags\t\t: fpu tsc msr pae cx8 apic sep cmov pat clflush mmx fxsr sse sse2 ht syscall nx lm sse3 ssse3 sse4_1 sse4_2 popcnt\n\n",
                           i, vendor, family, model, *bp ? bp : "x86-64 processor", stepping, tsc_hz() / 1000000,
                           ncpus_on, i, ncpus_on);
    } else {
        uint64_t tot = mem_total() / 1024, fr = (mem_total() - mem_used()) / 1024;
        n = ksnprintf(txt, 32768, "MemTotal:       %lu kB\nMemFree:        %lu kB\nMemAvailable:   %lu kB\nBuffers:               0 kB\n"
                      "Cached:                0 kB\nSwapCached:            0 kB\nActive:                0 kB\nInactive:              0 kB\n"
                      "Active(file):          0 kB\nInactive(file):        0 kB\nSwapTotal:             0 kB\nSwapFree:              0 kB\n"
                      "Dirty:                 0 kB\nShmem:                 0 kB\nSReclaimable:          0 kB\n", tot, fr, fr);
    }
    shm_rw(o, 0, (uint8_t *)txt, (uint64_t)n, true);
    kfree(txt);
    return o;
}

static struct task *proc_task(int pid);
/* a path given to an *at call: relative to the folder open as dirfd (AT_FDCWD -100: the working folder) */
static int64_t at_join(struct lxproc *lx, int dirfd, const char *p, char *out, size_t n) {
    if (p[0] == '/' || dirfd == -100) { path_join(lx->cwd, p, out, n); return 0; }
    if (dirfd < 0 || dirfd >= NFD || !lx->fd[dirfd].kind) return E(EBADF_);
    if (lx->fd[dirfd].kind != FD_DIR) return E(ENOTDIR_);
    path_join(lx->fd[dirfd].path, p, out, n);
    return 0;
}

/* /proc as folders (Chromium counts a process's threads by the links of /proc/self/task, and waits for a
 * stopped thread's /proc/self/task/<tid> to go before it seals its sandbox; it lists /proc/self/fd to close
 * a child's files). Its files (maps, cpuinfo, meminfo, exe) are made in do_open and readlink. */
static bool proc_norm(const char *full, char *out, size_t n) { /* AXIOM's /system/linux/proc/... -> /proc/... */
    if (strncmp(full, "/system/linux/proc", 18) || (full[18] && full[18] != '/')) return false;
    ksnprintf(out, n, "/proc%s", full + 18);
    return true;
}
static bool proc_alive(const struct task *t) { return t->state && t->state != PS_ZOMBIE && t->lx && t->user; }
static int proc_nthreads(const struct task *p) {
    int k = 0;
    for (int i = 0; i < MAX_TASKS; i++) if (proc_alive(&tasks[i]) && tasks[i].proc == p) k++;
    return k;
}
static const struct task *proc_thread(const struct task *p, int tid) {
    for (int i = 0; i < MAX_TASKS; i++) if (proc_alive(&tasks[i]) && tasks[i].proc == p && tasks[i].pid == tid) return &tasks[i];
    return 0;
}
static bool proc_num(const char *s, int *v) {
    if (*s < '0' || *s > '9') return false;
    int x = 0; for (; *s; s++) { if (*s < '0' || *s > '9') return false; x = x * 10 + (*s - '0'); }
    *v = x; return true;
}
/* what a /proc path is: 0 nothing, 1 a folder, 2 a file; *proc and *what say whose and which */
enum { PN_ROOT = 1, PN_PID, PN_TASK, PN_TID, PN_FD, PN_FILE };
static int proc_node(const char *pp, struct task **proc, int *what, int *nlink) {
    char parts[6][32]; int np = 0;
    for (const char *s = pp + 5; *s && np < 6;) {             /* after "/proc" */
        while (*s == '/') s++;
        if (!*s) break;
        int k = 0; while (*s && *s != '/') { if (k < 31) parts[np][k++] = *s; s++; }
        parts[np++][k] = 0;
    }
    *proc = 0; *nlink = 2;
    if (np == 0) { *what = PN_ROOT; *nlink = 3; return 1; }
    if (np == 1 && (!strcmp(parts[0], "cpuinfo") || !strcmp(parts[0], "meminfo"))) { *what = PN_FILE; *nlink = 1; return 2; }
    struct task *p = 0; int v, tid = 0;
    if (!strcmp(parts[0], "self")) p = current->proc;
    else if (!strcmp(parts[0], "thread-self")) { p = current->proc; tid = current->pid; }
    else if (proc_num(parts[0], &v)) {                         /* a process, or (as Linux) a thread by its id */
        p = proc_task(v);
        if (!p) for (int i = 0; i < MAX_TASKS; i++) if (proc_alive(&tasks[i]) && tasks[i].pid == v) { p = tasks[i].proc; break; }
    }
    if (!p) return 0;
    *proc = p;
    int i = 1;
    if (tid) {                                                 /* thread-self: as task/<tid> */
        if (np == 1) { *what = PN_TID; return 1; }
    } else {
        if (np == 1) { *what = PN_PID; return 1; }
        if (!strcmp(parts[1], "task")) {
            if (np == 2) { *what = PN_TASK; *nlink = 2 + proc_nthreads(p); return 1; }
            if (!proc_num(parts[2], &tid) || !proc_thread(p, tid)) return 0;
            if (np == 3) { *what = PN_TID; return 1; }
            i = 3;
        }
    }
    if (np == i + 2 && !strcmp(parts[i], "fd") && proc_num(parts[i + 1], &v)) { *what = PN_FILE; *nlink = 1; return 2; }
    if (np != i + 1) return 0;                                 /* one name inside <pid> or task/<tid> */
    if (!strcmp(parts[i], "fd")) { *what = PN_FD; return 1; }
    if (!strcmp(parts[i], "maps") || !strcmp(parts[i], "exe")) { *what = PN_FILE; *nlink = 1; return 2; }
    return 0;
}

static int proc_list(struct lxproc *lx, const char *pp, struct vfs_dirent *out, int max) {   /* a /proc folder's names */
    struct task *who; int what, nl, k = 0;
    if (proc_node(pp, &who, &what, &nl) != 1) return 0;
    #define PL_ADD(dir, ...) do { if (k < max) { ksnprintf(out[k].name, sizeof out[k].name, __VA_ARGS__); out[k].is_dir = dir; out[k].size = 0; k++; } } while (0)
    if (what == PN_ROOT) {
        PL_ADD(1, "self"); PL_ADD(1, "thread-self"); PL_ADD(0, "cpuinfo"); PL_ADD(0, "meminfo");
        for (int i = 0; i < MAX_TASKS; i++) if (proc_alive(&tasks[i]) && tasks[i].proc == &tasks[i]) PL_ADD(1, "%d", tasks[i].pid);
    } else if (what == PN_PID || what == PN_TID) {
        if (what == PN_PID) PL_ADD(1, "task");
        PL_ADD(1, "fd"); PL_ADD(0, "maps"); PL_ADD(0, "exe");
    } else if (what == PN_TASK) {
        for (int i = 0; i < MAX_TASKS; i++) if (proc_alive(&tasks[i]) && tasks[i].proc == who) PL_ADD(1, "%d", tasks[i].pid);
    } else if (what == PN_FD && who == current->proc) {        /* (another program's files: not listed) */
        for (int i = 0; i < NFD; i++) if (lx->fd[i].kind) PL_ADD(0, "%d", i);
    }
    #undef PL_ADD
    return k;
}

static uint64_t do_open(struct lxproc *lx, int dirfd, uint64_t upath, int flags) {
    char p[200], full[200];
    if (!ustr(p, upath, sizeof p)) return E(EFAULT_);
    int64_t aj = at_join(lx, dirfd, p, full, sizeof full);
    if (aj) return (uint64_t)aj;
    char pp[200];
    if (proc_norm(full, pp, sizeof pp)) {                      /* /proc: its own names, then as below */
        struct task *who; int what, nl, k = proc_node(pp, &who, &what, &nl);
        if (!k) return E(ENOENT_);
        if (k == 1) {                                          /* a folder */
            if (flags & 3) return E(EISDIR_);
            int fd = fd_new(lx, 0);
            if (fd < 0) return E(EMFILE_);
            struct lfd *f = &lx->fd[fd]; memset(f, 0, sizeof *f);
            f->kind = FD_DIR; f->flags = flags; strncpy0(f->path, full, sizeof f->path);
            return fd;
        }
        if (what == PN_FILE && who && who != current->proc) return E(EACCES_);   /* (only its own maps) */
        if (who) { char *sl = pp + 6; while (*sl && *sl != '/') sl++; ksnprintf(p, sizeof p, "/proc/self%s", sl); }
        else strncpy0(p, pp, sizeof p);
    }
    char selfmaps[40]; ksnprintf(selfmaps, sizeof selfmaps, "/proc/%d/maps", current->proc ? current->proc->pid : current->pid);
    bool pt = !strcmp(p, "/proc/cpuinfo") || !strcmp(p, "/proc/meminfo");
    if (pt || !strcmp(p, "/proc/self/maps") || !strcmp(p, selfmaps)) {   /* a snapshot, read like a file */
        struct shm *o = pt ? proc_text(p + 6) : proc_maps(lx, current->mm);
        int fd = o ? fd_new(lx, 0) : -1;
        if (fd < 0) { if (o) shm_put(o); return o ? E(EMFILE_) : E(ENOMEM_); }
        struct lfd *f = &lx->fd[fd]; memset(f, 0, sizeof *f);
        f->kind = FD_SHM; f->obj = o; f->flags = 0;            /* (O_RDONLY) */
        return fd;
    }
    if (!strncmp(p, "/proc/self/fd/", 14) || !strncmp(p, "/dev/fd/", 8)) {   /* an open file again, with its own access mode */
        const char *num = p + (p[1] == 'p' ? 14 : 8);
        if (*num < '0' || *num > '9') return E(ENOENT_);
        uint64_t from = katoi(num);
        if (from >= NFD || !lx->fd[from].kind) return E(ENOENT_);
        int fd = fd_new(lx, 0);
        if (fd < 0) return E(EMFILE_);
        fd_copy(&lx->fd[fd], &lx->fd[from]);
        lx->fd[fd].flags = (flags & (3 | 04000 | 02000)) | (lx->fd[fd].kind == FD_CHAN ? lx->fd[fd].flags & ~(3 | 04000 | 02000) : 0);
        if (lx->fd[fd].kind == FD_FILE || lx->fd[fd].kind == FD_DIR || lx->fd[fd].kind == FD_SHM) lx->fd[fd].off = 0;
        return fd;
    }
    if (!strcmp(p, "/proc/self/exe")) path_join("/", lx->exe, full, sizeof full);   /* the program's own file */
    int dv = dev_of(full);
    if (dv) {
        int fd = fd_new(lx, 0);
        if (fd < 0) return E(EMFILE_);
        struct lfd *f = &lx->fd[fd]; memset(f, 0, sizeof *f);
        f->kind = FD_DEV; f->dev = (uint8_t)dv; f->flags = flags & (3 | 04000 | 02000);
        if (dv == DEV_WINDOW) {                                  /* a window (made by AXWIN_CREATE) */
            struct axwin *x = kmalloc(sizeof *x);
            if (!x) { f->kind = FD_NONE; return E(ENOMEM_); }
            memset(x, 0, sizeof *x); x->refs = 1;
            x->next = axwins; axwins = x;
            f->kind = FD_WIN; f->obj = x;
        }
        if (dv == DEV_SNDCTL || dv == DEV_SNDPCM) {
            void *o = snd_open(dv == DEV_SNDPCM);
            if (!o) { f->kind = FD_NONE; return E(ENODEV_); }        /* (no sound hardware) */
            f->kind = FD_SND; f->obj = o;
        }
        strncpy0(f->path, full, sizeof f->path);
        return fd;
    }
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
    if (acc && !st.is_dir) pc_file_written(full);
    if (acc && (flags & 01000)) vfs_write(full, "", 0);            /* O_TRUNC */
    int fd = fd_new(lx, 0);
    if (fd < 0) return E(EMFILE_);
    struct lfd *f = &lx->fd[fd];
    f->kind = st.is_dir ? FD_DIR : FD_FILE; f->flags = flags; f->off = 0;
    strncpy0(f->path, full, sizeof f->path);
    if (flags & 02000) { struct dunc_stat s2; if (vfs_stat(full, &s2) == 0) f->off = s2.size; }   /* O_APPEND */
    return fd;
}


/* a program ending itself with SIGABRT (abort(), a failed CHECK): signal handlers don't run here, so Chromium
 * can't print its own stack trace; log the return addresses found on its stack instead (file + offset, for
 * llvm-symbolizer / addr2line), so the cause can be found */
static void lx_abort_trace(struct lxproc *lx, struct frame *f) {
    kprintf("[linux] %s aborted; addresses on its stack (file offset):\n", current->name);
    int shown = 0;
    for (int i = -1; i < 2048 && shown < 48; i++) {
        uint64_t v;
        if (i < 0) v = f->rip;
        else { uint64_t p = f->rsp + 8 * (uint64_t)i; if (!uok(p, 8, false)) break; v = *(uint64_t *)p; }
        for (int k = 0; k < lx->nfmaps; k++) {
            struct fmap *m = &lx->fmaps[k];
            if (v >= m->s && v < m->e) { kprintf("[linux]   #%d %s+0x%lx\n", shown++, m->path, v - m->s + m->off); break; }
        }
    }
}

/* Debugging a Linux program that hangs: Ctrl+\ (0x1C) on the serial line lists each Linux thread, the system
 * call it's in (vector 0x81) or where it was stopped, and the return addresses on its stack (file + offset) */
void linux_dump_threads(void) {
    uint64_t up = uptime_ms();
    kprintf("[dump] page fills: %lu (%lu not from files), %lu file reads, %lu KiB, %lu ms\n", fill_calls, fill_anon, fill_reads, fill_bytes >> 10, fill_us / 1000);
    for (int c = 0; c < ncpus_on; c++)                           /* how busy each core has been since start-up */
        kprintf("[dump] core %d: busy %lu ms of %lu (%lu%%)\n", c, cpus[c].busy_us / 1000, up, up ? cpus[c].busy_us / 10 / up : 0);
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (!t->state || !t->lx || !t->user || !t->kstack) continue;
        struct frame *f = (struct frame *)(t->kstack_top - sizeof *f);
        kprintf("[dump] pid %d '%s' state %d: %s %lu (%lx, %lx, %lx) futex %lx rip %lx; core %d, %lu ms of CPU\n", t->pid, t->name, t->state,
                f->vec == 0x81 ? "system call" : "interrupt", f->vec == 0x81 ? f->rax : f->vec, f->rdi, f->rsi, f->rdx,
                t->futex_addr, f->rip, t->cpu, t->ticks / 1000);
        struct lxproc *lx = t->lx; int shown = 0;
        for (int k = -1; k < 1024 && shown < 16; k++) {
            uint64_t v;
            if (k < 0) v = f->rip;
            else { uint64_t a = f->rsp + 8 * (uint64_t)k, ph = vm_phys(t->pml4, a); if (!ph) break; v = *(uint64_t *)ph; }
            for (int j = 0; j < lx->nfmaps; j++) {
                struct fmap *m = &lx->fmaps[j];
                if (v >= m->s && v < m->e) { kprintf("[dump]   %s+0x%lx\n", m->path, v - m->s + m->off); shown++; break; }
            }
        }
    }
}
/* ---------------- processes: fork, vfork, execve, wait ----------------
 * A fork copies the address space (mm_fork) and the program's state here: its files (each one more reference:
 * pipes and sockets are shared, as on Linux), shared-memory mappings, file mappings, handlers. A vfork (what
 * posix_spawn and system() use) shares the address space until the child calls execve or ends, and its parent
 * waits till then. execve loads a new program into a fresh address space for the same process. When a child
 * ends, its status waits in zombies[] for its parent's wait4, and the parent gets SIGCHLD. */
#define NZOMB 128
static struct { int ppid, pid, status; bool used; } zombies[NZOMB];
static int vforking[32];                                       /* children whose parent waits for their execve/exit */
static void vfork_release(int pid) {
    for (int i = 0; i < 32; i++) if (vforking[i] == pid) { vforking[i] = 0; chan_wake(); }
}
static bool vfork_waiting(int pid) { for (int i = 0; i < 32; i++) if (vforking[i] == pid) return true; return false; }
static struct task *proc_task(int pid) {                       /* a Linux program's first thread, by its pid */
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state && tasks[i].state != PS_ZOMBIE && tasks[i].pid == pid && tasks[i].lx && tasks[i].proc == &tasks[i]) return &tasks[i];
    return 0;
}
static void lx_child_ended(struct task *t, struct lxproc *lx) {
    vfork_release(t->pid);
    if (!lx->ppid) return;
    int sig = t->exit_signal ? t->exit_signal : t->exit_code == -1 ? 9 : 0;
    int status = sig ? (sig & 0x7f) : ((t->exit_code & 0xff) << 8);
    int k = -1;
    for (int i = 0; i < NZOMB; i++) if (!zombies[i].used) { k = i; break; }
    if (k < 0) k = 0;                                          /* (full: the oldest goes; its parent never asked) */
    zombies[k].used = true; zombies[k].ppid = lx->ppid; zombies[k].pid = t->pid; zombies[k].status = status;
    struct task *p = proc_task(lx->ppid);
    if (p) sig_post(p, 17, t->pid);                            /* SIGCHLD (ignored unless it has a handler) */
    chan_wake();
}
static struct lxproc *lx_fork(struct lxproc *p) {
    struct lxproc *c = kmalloc(sizeof *c);
    if (!c) return 0;
    memcpy(c, p, sizeof *c);
    c->linelen = 0;
    c->maps = 0; c->nmaps = c->capmaps = 0; c->fmaps = 0; c->nfmaps = c->capfmaps = 0;
    if (p->nfmaps && (c->fmaps = kmalloc((uint64_t)p->capfmaps * sizeof *c->fmaps))) {
        memcpy(c->fmaps, p->fmaps, (uint64_t)p->nfmaps * sizeof *c->fmaps); c->nfmaps = p->nfmaps; c->capfmaps = p->capfmaps;
    }
    if (p->nmaps && (c->maps = kmalloc((uint64_t)p->capmaps * sizeof *c->maps))) {
        memcpy(c->maps, p->maps, (uint64_t)p->nmaps * sizeof *c->maps); c->nmaps = p->nmaps; c->capmaps = p->capmaps;
        for (int i = 0; i < c->nmaps; i++) c->maps[i].obj->refs++;
    }
    for (int i = 0; i < NFD; i++) { memset(&c->fd[i], 0, sizeof c->fd[i]); if (p->fd[i].kind) fd_copy(&c->fd[i], &p->fd[i]); }
    return c;
}
static uint64_t lx_clone_process(struct frame *f, uint64_t flags, uint64_t stack, uint64_t ptid, uint64_t ctid) {
    struct task *t = current; struct lxproc *lx = t->lx;
    bool vfork = (flags & 0x100) != 0;                         /* CLONE_VM: shares memory (vfork, posix_spawn) */
    struct mm *m = 0; uint64_t pages = 0;
    if (!vfork && !(m = mm_fork(t->mm, &pages))) return E(ENOMEM_);
    struct lxproc *c = lx_fork(lx);
    if (!c) { if (m) mm_put(m); return E(ENOMEM_); }
    struct task *ch = task_fork(f, m, stack);
    if (!ch) { if (m) mm_put(m); linux_task_exit_free(c); return E(EAGAIN_); }
    c->ppid = t->proc->pid;
    ch->lx = c; ch->pages = pages;
    if (!vfork) ch->mm->fill = lx_fill, ch->mm->fill_ctx = c;
    if (flags & 0x200000) ch->clear_tid = ctid;                /* CLONE_CHILD_CLEARTID */
    if (flags & 0x1000000) {                                   /* CLONE_CHILD_SETTID: in the child's memory */
        uint32_t tid = (uint32_t)ch->pid;
        if (vfork) { if (uok(ctid, 4, true)) *(uint32_t *)ctid = tid; } else copy_to(ch->mm, ctid, &tid, 4);
    }
    if ((flags & 0x100000) && uok(ptid, 4, true)) *(uint32_t *)ptid = (uint32_t)ch->pid;   /* CLONE_PARENT_SETTID */
    int pid = ch->pid;
    if (vfork) for (int i = 0; i < 32; i++) if (!vforking[i]) { vforking[i] = pid; break; }
    task_start(ch);
    if (vfork && (flags & 0x4000)) {                           /* CLONE_VFORK: wait till it execs or ends */
        while (vfork_waiting(pid) && task_alive(pid)) chan_wait(-1);
        vfork_release(pid);
    }
    return (uint64_t)pid;
}
/* wait4 / waitid: a child that ended (pid -1: any), or EAGAIN-like 0 with WNOHANG */
static int64_t lx_wait(int pid, bool nohang, int *status, int *wpid) {
    struct task *t = current; int me = t->proc->pid;
    for (;;) {
        for (int i = 0; i < NZOMB; i++)
            if (zombies[i].used && zombies[i].ppid == me && (pid <= 0 || zombies[i].pid == pid)) {
                *status = zombies[i].status; *wpid = zombies[i].pid; zombies[i].used = false; return 1;
            }
        bool any = false;                                      /* children still running */
        for (int i = 0; i < MAX_TASKS; i++)
            if (tasks[i].state && tasks[i].state != PS_ZOMBIE && tasks[i].lx && tasks[i].proc == &tasks[i] && tasks[i].lx->ppid == me && (pid <= 0 || tasks[i].pid == pid)) any = true;
        for (int i = 0; i < MAX_TASKS && !any; i++)            /* (ended, not yet reaped: its status comes) */
            if (tasks[i].state == PS_ZOMBIE && tasks[i].lx && tasks[i].lx->ppid == me && (pid <= 0 || tasks[i].pid == pid)) any = true;
        if (!any) return (int64_t)E(ECHILD_);
        if (nohang) return 0;
        if (sig_now()) return (int64_t)E(EINTR_);
        chan_wait(50);                                         /* (reaping happens on its own; look again) */
    }
}
/* execve: the program in path, with these arguments and environment, in place of this one */
static uint64_t lx_execve(struct frame *f, const char *upath, uint64_t uargv, uint64_t uenvp) {
    struct task *t = current; struct lxproc *lx = t->lx;
    if (t != t->proc) return E(EINVAL_);                       /* (from a thread other than the first: not yet) */
    char p0[200], path[200];
    if (!ustr(p0, (uint64_t)upath, sizeof p0)) return E(EFAULT_);
    if (!strcmp(p0, "/proc/self/exe")) strncpy0(p0, lx->exe, sizeof p0);   /* (Chromium starts its helpers this way) */
    path_join(lx->cwd, p0, path, sizeof path);
    /* the arguments and environment, copied out before the old memory goes */
    enum { MAXA = 2048, MAXB = 256 * 1024 };
    char **argv = kmalloc((MAXA + 4) * 8), **envv = kmalloc((MAXA + 1) * 8), *buf = kmalloc(MAXB);
    if (!argv || !envv || !buf) { kfree(argv); kfree(envv); kfree(buf); return E(ENOMEM_); }
    uint64_t used = 0; int argc = 0, envc = 0; uint64_t r = 0;
    for (int which = 0; which < 2 && !r; which++) {
        uint64_t arr = which ? uenvp : uargv; char **out = which ? envv : argv; int *n = which ? &envc : &argc;
        for (int i = 0; arr && !r; i++) {
            if (!uok(arr + (uint64_t)i * 8, 8, false)) { r = E(EFAULT_); break; }
            uint64_t sp = *(uint64_t *)(arr + (uint64_t)i * 8);
            if (!sp) break;
            if (*n >= MAXA) { r = E(E2BIG_); break; }
            if (!ustr(buf + used, sp, MAXB - used)) { r = E(E2BIG_); break; }
            out[(*n)++] = buf + used; used += strlen(buf + used) + 1;
        }
    }
    argv[argc] = 0; envv[envc] = 0;
    /* a script: #!interpreter [one argument] */
    for (int depth = 0; !r && depth < 3; depth++) {
        char head[160]; long hn = vfs_read(path, head, 0, sizeof head - 1);
        struct dunc_stat st;
        if (hn < 0 || vfs_stat(path, &st) != FS_OK) { r = E(ENOENT_); break; }
        if (st.is_dir) { r = E(EACCES_); break; }
        if (hn < 2 || head[0] != '#' || head[1] != '!') break;
        head[hn] = 0; char *e = head + 2; while (*e == ' ' || *e == '\t') e++;
        char *ip = e; while (*e && *e != ' ' && *e != '\t' && *e != '\n') e++;
        char *ia = 0;
        if (*e == ' ' || *e == '\t') { *e++ = 0; while (*e == ' ' || *e == '\t') e++; ia = e; while (*e && *e != '\n') e++; while (e > ia && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--; }
        *e = 0; if (ia && !*ia) ia = 0;
        if (!*ip || argc + 3 > MAXA || used + 600 > MAXB) { r = E(ENOEXEC_); break; }
        int extra = ia ? 2 : 1;                                /* argv: interpreter [arg] script args[1..] */
        for (int i = argc; i >= 1; i--) argv[i + extra] = argv[i];
        argv[0] = buf + used; strncpy0(buf + used, ip, 200); used += strlen(buf + used) + 1;
        if (ia) { argv[1] = buf + used; strncpy0(buf + used, ia, 200); used += strlen(buf + used) + 1; }
        argv[extra] = buf + used; strncpy0(buf + used, p0, 200); used += strlen(buf + used) + 1;
        argc += extra; if (argc < extra + 1) argc = extra + 1; argv[argc] = 0;
        path_join(lx->cwd, ip, path, sizeof path);
    }
    struct mm *m = r ? 0 : mm_new();
    if (!r && !m) r = E(ENOMEM_);
    struct fmap *ofm = lx->fmaps; int onf = lx->nfmaps, ocap = lx->capfmaps;
    uint64_t start = 0, sp = 0;
    if (!r) {
        lx->fmaps = 0; lx->nfmaps = lx->capfmaps = 0;          /* (the new program's: kept apart until it's loaded) */
        m->fill = lx_fill; m->fill_ctx = lx;
        uint64_t entry, phdr, ibase; int phnum; char why[200];
        if (!lx_load(m, lx, path, &start, &entry, &phdr, &phnum, &ibase, why, sizeof why)) {
            r = E(ENOEXEC_);
            static int said; if (said++ < 20) kprintf("[linux] %s: exec %s: %s\n", t->name, path, why);
        }
        else if (!(sp = lx_stack(m, path, argc, argv, (const char **)envv, entry, phdr, phnum, ibase))) r = E(E2BIG_);
        if (r) { kfree(lx->fmaps); lx->fmaps = ofm; lx->nfmaps = onf; lx->capfmaps = ocap; }
    }
    if (r) { if (m) { m->fill = 0; mm_put(m); } kfree(argv); kfree(envv); kfree(buf); return r; }
    kfree(ofm);
    /* past the point of no return: this process is the new program now */
    task_kill_others(t);
    shm_forget_all(lx);
    for (int i = 0; i < NFD; i++) if (lx->fd[i].kind && lx->fd[i].cloexec) fd_close(&lx->fd[i]);
    for (int i = 1; i <= 64; i++) if (lx->sa[i].handler > 1) { lx->sa[i].handler = 0; lx->sa[i].flags = 0; lx->sa[i].mask = 0; }
    struct mm *old = t->mm;
    if (old->fill_ctx == lx) { old->fill = 0; old->fill_ctx = 0; }
    t->mm = m; t->pml4 = m->pml4;
    __asm__ volatile("mov %0, %%cr3" :: "r"((uint64_t)m->pml4) : "memory");
    mm_put(old);                                               /* (a vfork's parent still has it) */
    t->pages = 0;
    vfork_release(t->pid);
    strncpy0(lx->exe, !strncmp(path, "/system/linux/", 14) ? path + 13 : path, sizeof lx->exe);
    const char *nm = path; for (const char *q = path; *q; q++) if (*q == '/') nm = q + 1;
    strncpy0(t->name, nm, sizeof t->name);
    kprintf("[linux] %d: exec %s\n", t->pid, path);
    kfree(argv); kfree(envv); kfree(buf);
    memset(f, 0, offsetof(struct frame, vec));                 /* registers: zero, as a new program starts */
    f->rip = start; f->rsp = sp; f->rflags = 0x202;
    t->fs_base = 0; wrmsr(0xC0000100, 0);
    uint32_t mx = 0x1f80; __asm__ volatile("fninit; ldmxcsr %0" :: "m"(mx));
    t->clear_tid = 0;
    return 0;
}

static uint64_t linux_syscall_(struct frame *f);
/* Linux reads int arguments (file numbers, flags) from the low 32 bits of their registers; programs that make
 * system calls themselves may leave anything in the upper half, so those are cut off here */
uint64_t linux_syscall(struct frame *f) {
    switch (f->rax) {
    case 0: case 1: case 3: case 5: case 8: case 16: case 17: case 18: case 19: case 20: case 32: case 33: case 72:
    case 77: case 138: case 217: case 285: case 292: case 44: case 45: case 46: case 47: case 48: case 232: case 281: case 54: case 55:
    case 221: case 286: case 287: case 441: case 74: case 75: case 81: case 42: case 43: case 49: case 50: case 51: case 52: case 288: case 73: case 299: case 307:
        f->rdi = (uint32_t)f->rdi; break;                        /* the first argument is a file */
    case 233: f->rdi = (uint32_t)f->rdi; f->rdx = (uint32_t)f->rdx; break;   /* epoll_ctl: two */
    case 9: f->r8 = (uint64_t)(int64_t)(int32_t)f->r8; f->rdx = (uint32_t)f->rdx; f->r10 = (uint32_t)f->r10; break;   /* mmap: fd (-1 for none), prot, flags */
    case 257: case 262: case 269: case 267: case 258: case 263: case 264: case 316: case 265:
        f->rdi = (uint64_t)(int64_t)(int32_t)f->rdi; break;      /* *at: a directory file or AT_FDCWD (-100) */
    }
    chan_woke = false;
    current->sig_sysno = f->rax;
    /* calls that make files: which numbers were free, and whether the new ones close on execve */
    uint64_t n = f->rax; int ce = -1;
    switch (n) {
    case 2: ce = (f->rsi & 02000000) != 0; break;                       /* open */
    case 257: ce = (f->rdx & 02000000) != 0; break;                     /* openat */
    case 22: case 32: case 33: ce = 0; break;                           /* pipe, dup, dup2 */
    case 293: case 41: case 53: case 290: case 283: ce = (f->rsi & 02000000) != 0; break;   /* pipe2 socket socketpair eventfd2 timerfd */
    case 288: ce = (f->r10 & 02000000) != 0; break;                     /* accept4 */
    case 43: case 284: case 213: ce = 0; break;                         /* accept, eventfd, epoll_create */
    case 292: ce = (f->rdx & 02000000) != 0; break;                     /* dup3 */
    case 291: ce = (f->rdi & 02000000) != 0; break;                     /* epoll_create1 */
    case 319: ce = (f->rsi & 1) != 0; break;                            /* memfd_create: MFD_CLOEXEC */
    case 47: ce = (f->rdx & 0x40000000) != 0; break;                    /* recvmsg: MSG_CMSG_CLOEXEC */
    case 72: if (f->rsi == 0 || f->rsi == 1030) ce = f->rsi == 1030; break;   /* F_DUPFD, F_DUPFD_CLOEXEC */
    }
    struct lxproc *lx0 = current->lx; uint64_t was_free[(NFD + 63) / 64] = { 0 };
    if (ce >= 0 && lx0) for (int i = 0; i < NFD; i++) if (!lx0->fd[i].kind) was_free[i >> 6] |= 1ULL << (i & 63);
    uint64_t r = linux_syscall_(f);
    if (ce >= 0 && current->lx == lx0 && lx0 && (int64_t)r >= 0) {
        for (int i = 0; i < NFD; i++) if ((was_free[i >> 6] >> (i & 63) & 1) && lx0->fd[i].kind) lx0->fd[i].cloexec = ce;
        if ((n == 33 || n == 292) && r < NFD) lx0->fd[r].cloexec = ce;  /* (dup2/dup3 onto an open number) */
    }
    if (chan_woke) { chan_woke = false; task_yield(); }
    return r;
}
static uint64_t linux_syscall_(struct frame *f) {
    uint64_t n = f->rax, a = f->rdi, b = f->rsi, c = f->rdx, d = f->r10, e = f->r8;
    struct task *t = current;
    struct lxproc *lx = t->lx;
    if (!lx) return E(ENOSYS_);
    /* Unix sockets (socket(AF_UNIX), and bind/listen/connect/accept... on one): here; internet sockets: below */
    if ((n == 41 && a == 1) || ((n == 42 || n == 43 || n == 49 || n == 50 || n == 51 || n == 52 || n == 288) &&
                                a < NFD && lx->fd[a].kind == FD_CHAN && lx->fd[a].sock)) switch (n) {
    case 41: {                                                 /* socket: Unix sockets only */
        int type = (int)b & 0xF;
        if (a != 1) return E(EAFNOSUPPORT_);                   /* (the network is AXIOM's own, not Linux sockets) */
        if (type != 1 && type != 2 && type != 5) return E(EINVAL_);
        if ((b & ~(uint64_t)0xF) & ~(uint64_t)(04000 | 02000000)) return E(EINVAL_);
        int k = fd_new(lx, 0); if (k < 0) return E(EMFILE_);
        struct lfd *fd = &lx->fd[k];
        memset(fd, 0, sizeof *fd);
        fd->kind = FD_CHAN; fd->sock = true; fd->stype = (uint8_t)type; fd->flags = 2 | ((int)b & 04000);
        return (uint64_t)k;
    }
    case 49: {                                                 /* bind */
        if (a >= NFD || !lx->fd[a].kind) return E(EBADF_);
        struct lfd *fd = &lx->fd[a];
        if (fd->kind != FD_CHAN || !fd->sock) return E(ENOTSOCK_);
        char nm[200];
        if (!un_name(lx, b, c, nm, sizeof nm)) return E(EINVAL_);
        if (fd->path[0] || fd->rx || fd->tx) return E(EINVAL_);
        if (ulisten_find(nm)) return E(EADDRINUSE_);
        strncpy0(fd->path, nm, sizeof fd->path);
        return 0;
    }
    case 50: {                                                 /* listen */
        if (a >= NFD || !lx->fd[a].kind) return E(EBADF_);
        struct lfd *fd = &lx->fd[a];
        if (fd->kind != FD_CHAN || !fd->sock) return E(ENOTSOCK_);
        if (fd->lis) return 0;
        if (!fd->path[0] || fd->rx || fd->tx) return E(EINVAL_);   /* (autobind isn't there) */
        if (ulisten_find(fd->path)) return E(EADDRINUSE_);
        struct ulisten *l = kmalloc(sizeof *l); if (!l) return E(ENOMEM_);
        memset(l, 0, sizeof *l);
        l->refs = 1; strncpy0(l->name, fd->path, sizeof l->name); l->stype = fd->stype; l->pid = t->proc->pid;
        l->next = ulistens; ulistens = l;
        fd->lis = l;
        chan_wake();                                           /* (someone may be waiting for this name) */
        return 0;
    }
    case 42: {                                                 /* connect */
        if (a >= NFD || !lx->fd[a].kind) return E(EBADF_);
        struct lfd *fd = &lx->fd[a];
        if (fd->kind != FD_CHAN || !fd->sock) return E(ENOTSOCK_);
        if (fd->lis) return E(EINVAL_);
        if (fd->rx || fd->tx) return E(EISCONN_);
        char nm[200];
        if (!un_name(lx, b, c, nm, sizeof nm)) return E(EINVAL_);
        struct ulisten *l = ulisten_find(nm);
        if (!l && !strcmp(nm, "/system/linux/tmp/wayland-0") && !wayland_starting) {
            /* a desktop app looking for the compositor: start AXIOM's (axwl) and give it a moment */
            char *av[] = { "axwl", 0 };
            wayland_starting = true;
            struct dunc_stat ws; int pid = vfs_stat("/system/bin/axwl", &ws) == FS_OK ? linux_spawn("/system/bin/axwl", 1, av, 0) : -1;
            for (uint64_t t0 = uptime_ms(); pid > 0 && !(l = ulisten_find(nm)) && task_alive(pid) && uptime_ms() - t0 < 5000;) chan_wait(100);
            wayland_starting = false;
            if (lx->fd[a].kind != FD_CHAN || fd->rx || fd->tx) return E(EBADF_);   /* (changed meanwhile) */
        }
        if (!l) return nm[0] == '@' ? E(ECONNREFUSED_) : E(ENOENT_);
        if (l->stype != fd->stype) return E(ECONNREFUSED_);   /* (Linux: EPROTOTYPE) */
        if (l->qn == ULQ) return (fd->flags & 04000) ? E(EAGAIN_) : E(ECONNREFUSED_);
        struct chan *up = chan_new(fd->stype != 1), *down = chan_new(fd->stype != 1);
        if (!up || !down) { if (up) { kfree(up->data); kfree(up); } if (down) { kfree(down->data); kfree(down); } return E(ENOMEM_); }
        fd->tx = up; up->writers++; fd->rx = down; down->readers++;
        fd->peer = l->pid;
        struct pend *p = &l->q[(l->qh + l->qn) % ULQ]; l->qn++;
        p->rx = up; up->readers++; p->tx = down; down->writers++; p->pid = t->proc->pid;
        chan_wake();
        return 0;
    }
    case 43: case 288: {                                       /* accept, accept4 */
        if (a >= NFD || !lx->fd[a].kind) return E(EBADF_);
        struct lfd *fd = &lx->fd[a];
        if (fd->kind != FD_CHAN || !fd->sock) return E(ENOTSOCK_);
        struct ulisten *l = fd->lis;
        if (!l) return E(EINVAL_);
        int flags = n == 288 ? (int)d : 0;
        if (flags & ~(04000 | 02000000)) return E(EINVAL_);
        while (!l->qn) {
            if (fd->flags & 04000) return E(EAGAIN_);
            chan_wait(-1); if (sig_now()) return E(EINTR_);
            if (a >= NFD || lx->fd[a].kind != FD_CHAN || lx->fd[a].lis != l) return E(EBADF_);   /* (closed meanwhile) */
        }
        int k = fd_new(lx, 0); if (k < 0) return E(EMFILE_);
        struct pend p = l->q[l->qh]; l->qh = (l->qh + 1) % ULQ; l->qn--;
        struct lfd *nf = &lx->fd[k];
        memset(nf, 0, sizeof *nf);
        nf->kind = FD_CHAN; nf->sock = true; nf->stype = l->stype; nf->flags = 2 | (flags & 04000);
        nf->rx = p.rx; nf->tx = p.tx; nf->peer = p.pid;      /* (the queue's counts move to the file) */
        strncpy0(nf->path, l->name, sizeof nf->path);
        un_addr_out(b, c, 0);                                  /* the client is unnamed */
        return (uint64_t)k;
    }
    case 51: case 52: {                                        /* getsockname, getpeername */
        if (a >= NFD || !lx->fd[a].kind) return E(EBADF_);
        struct lfd *fd = &lx->fd[a];
        if (fd->kind != FD_CHAN || !fd->sock) return E(ENOTSOCK_);
        if (n == 52 && !fd->rx && !fd->tx) return E(ENOTCONN_);
        un_addr_out(b, c, n == 51 ? fd->path : 0);
        return 0;
    }
    }
    switch (n) {
    case 0: case 17: {                                         /* read, pread64 */
        if (a >= NFD || !lx->fd[a].kind) return E(EBADF_);
        struct lfd *fd = &lx->fd[a];
        if (fd->kind == FD_TTY_IN) return 0;                     /* (no input yet: end of file) */
        if (fd->kind == FD_INET) {
            if (n == 17) return E(ESPIPE_);
            if (!uok(b, c, true)) return E(EFAULT_);
            return (uint64_t)isock_recv(fd->obj, (uint8_t *)b, c, fd->flags & 04000, false, 0, 0, 0);
        }
        if (fd->kind == FD_WIN) {                               /* window events: whole records */
            struct axwin *x = fd->obj;
            if (n == 17) return E(ESPIPE_);
            if (c < sizeof(struct axwin_event)) return E(EINVAL_);
            while (x->qh == x->qt) { if (fd->flags & 04000) return E(EAGAIN_); chan_wait(-1); if (sig_now()) return E(EINTR_); }
            if (!uok(b, c, true)) return E(EFAULT_);
            uint64_t k = 0;
            while (x->qh != x->qt && k + sizeof(struct axwin_event) <= c) {
                memcpy((uint8_t *)b + k, &x->q[x->qt], sizeof x->q[0]); x->qt = (x->qt + 1) % AXQ; k += sizeof x->q[0];
            }
            return k;
        }
        if (fd->kind == FD_DEV) {
            if (fd->dev == DEV_NULL) return 0;
            if (!uok(b, c, true)) return E(EFAULT_);
            if (fd->dev == DEV_RANDOM || fd->dev == DEV_URANDOM) random_bytes((void *)b, c); else memset((void *)b, 0, c);
            return c;
        }
        if (fd->kind == FD_TIMER) {
            struct tfd *x = fd->obj;
            if (n == 17) return E(ESPIPE_);
            if (c < 8) return E(EINVAL_);
            for (;;) {
                tfd_update(x);
                if (x->ticks) break;
                if (fd->flags & 04000) return E(EAGAIN_);
                uint64_t now = uptime_ns();
                chan_wait(x->next ? (int64_t)((x->next - now + 999999) / 1000000) : -1); if (sig_now()) return E(EINTR_);
                if (lx->fd[a].kind != FD_TIMER) return E(EBADF_);
            }
            if (!uok(b, 8, true)) return E(EFAULT_);
            *(uint64_t *)b = x->ticks; x->ticks = 0;
            return 8;
        }
        if (fd->kind == FD_INOTIFY) {                          /* whole events, as many as fit */
            struct inot *in = fd->obj;
            if (n == 17) return E(ESPIPE_);
            while (!in->qlen) { if (fd->flags & 04000) return E(EAGAIN_); chan_wait(-1); if (sig_now()) return E(EINTR_); if (lx->fd[a].kind != FD_INOTIFY) return E(EBADF_); }
            uint32_t take = 0;
            while (take < in->qlen) { uint32_t sz = 16 + *(uint32_t *)(in->q + take + 12); if (take + sz > c) break; take += sz; }
            if (!take) return E(EINVAL_);                          /* (the buffer can't hold the next event) */
            if (!uok(b, take, true)) return E(EFAULT_);
            memcpy((void *)b, in->q, take);
            memmove(in->q, in->q + take, in->qlen - take); in->qlen -= take;
            in->last = 0;                                          /* (the last event: found again) */
            for (uint32_t o = 0; o < in->qlen; o += 16 + *(uint32_t *)(in->q + o + 12)) in->last = o;
            if (in->qlen < INOT_QUEUE / 2) in->overflowed = false;
            return take;
        }
        if (fd->kind == FD_EVENT) {
            struct evfd *ev = fd->obj;
            if (n == 17) return E(ESPIPE_);
            if (c < 8) return E(EINVAL_);
            while (!ev->count) { if (fd->flags & 04000) return E(EAGAIN_); chan_wait(-1); if (sig_now()) return E(EINTR_); }
            if (!uok(b, 8, true)) return E(EFAULT_);
            uint64_t v = ev->sem ? 1 : ev->count;
            ev->count -= v; *(uint64_t *)b = v; chan_wake();
            return 8;
        }
        if (fd->kind == FD_CHAN) {
            if (n == 17) return E(ESPIPE_);
            if (!uok(b, c, true)) return E(EFAULT_);
            return (uint64_t)chan_read(fd, (uint8_t *)b, c, true, fd->flags & 04000, false, 0, 0);
        }
        if (fd->kind == FD_EPOLL) return E(EINVAL_);
        if (fd->kind == FD_SHM) {
            if (!uok(b, c, true)) return E(EFAULT_);
            int64_t r = shm_rw(fd->obj, n == 17 ? d : fd->off, (uint8_t *)b, c, false);
            if (r > 0 && n == 0) fd->off += r;
            return (uint64_t)r;
        }
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
        if (fd->kind == FD_WIN) return E(EINVAL_);
        if (fd->kind == FD_SND) { long r = snd_write(fd->obj, b, c, fd->flags & 04000); return r < 0 ? E(-r) : (uint64_t)r; }
        if (fd->kind == FD_INET) return n == 18 ? E(ESPIPE_) : (uint64_t)isock_send(fd->obj, (const uint8_t *)b, c, 0, 0, fd->flags & 04000);
        if (fd->kind == FD_DEV) return fd->dev == DEV_FULL ? E(ENOSPC_) : c;   /* (written to random: mixed in on Linux; here, dropped) */
        if (fd->kind == FD_EVENT) {
            struct evfd *ev = fd->obj;
            if (n == 18) return E(ESPIPE_);
            if (c < 8) return E(EINVAL_);
            uint64_t v = *(uint64_t *)b;
            if (v == ~0ULL) return E(EINVAL_);
            while (ev->count + v < ev->count || ev->count + v == ~0ULL) { if (fd->flags & 04000) return E(EAGAIN_); chan_wait(-1); if (sig_now()) return E(EINTR_); }
            ev->count += v; if (v) chan_wake();
            return 8;
        }
        if (fd->kind == FD_EPOLL) return E(EINVAL_);
        if (fd->kind == FD_SHM) {
            struct shm *o = fd->obj;
            if (!(fd->flags & 3)) return E(EBADF_);
            uint64_t off = n == 18 ? d : (fd->flags & 02000) ? o->size : fd->off;
            if (o->seals & (SEAL_WRITE_ | SEAL_FUTURE_WRITE_)) return E(EPERM_);
            if ((o->seals & SEAL_GROW_) && off + c > o->size) { if (off >= o->size) return E(EPERM_); c = o->size - off; }
            int64_t r = shm_rw(fd->obj, off, (uint8_t *)b, c, true);
            if (r > 0 && n == 1) fd->off = off + r;
            return (uint64_t)r;
        }
        if (fd->kind == FD_CHAN) return n == 18 ? E(ESPIPE_) : (uint64_t)chan_write(fd, (const uint8_t *)b, c, true, fd->flags & 04000, 0);
        if (fd->kind != FD_FILE || !(fd->flags & 3)) return E(EBADF_);
        struct dunc_stat st; uint64_t off = n == 18 ? d : fd->off;
        if (vfs_stat(fd->path, &st) == 0 && off == st.size) { if (vfs_append(fd->path, (const void *)b, c) < 0) return E(EINVAL_); }
        else if (vfs_pwrite(fd->path, (const void *)b, off, c) == (long)c) { }   /* in place: only what changes */
        else {                                                  /* (a volume that can't: read, change, write back) */
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
        fd_close(&lx->fd[a]); return 0;
    case 8: {                                                  /* lseek */
        if (a >= NFD || !lx->fd[a].kind) return E(EBADF_);
        struct lfd *fd = &lx->fd[a];
        if (fd->kind == FD_DEV) return 0;
        if (fd->kind != FD_FILE && fd->kind != FD_SHM) return E(ESPIPE_);
        struct dunc_stat st; uint64_t size = fd->kind == FD_SHM ? ((struct shm *)fd->obj)->size : vfs_stat(fd->path, &st) == 0 ? st.size : 0;
        int64_t no = c == 0 ? (int64_t)b : c == 1 ? (int64_t)fd->off + (int64_t)b : c == 2 ? (int64_t)size + (int64_t)b : -1;
        if (no < 0) return E(EINVAL_);
        fd->off = no; return no;
    }
    case 5: {                                                  /* fstat */
        if (a >= NFD || !lx->fd[a].kind) return E(EBADF_);
        if (!uok(b, sizeof(struct lstat), true)) return E(EFAULT_);
        struct lfd *fd = &lx->fd[a]; struct dunc_stat st = { 0 };
        if (fd->kind == FD_FILE || fd->kind == FD_DIR) vfs_stat(fd->path, &st);
        fill_stat((struct lstat *)b, fd->kind == FD_DIR, st.size, fd->kind == FD_TTY_IN || fd->kind == FD_TTY_OUT, fd->kind == FD_FILE || fd->kind == FD_DIR ? fd->path : (a == 0 ? "<stdin>" : "<tty>"));
        char pp[200];
        if (fd->kind == FD_DIR && proc_norm(fd->path, pp, sizeof pp)) {   /* a /proc folder: its links now */
            struct task *who; int what, nl;
            if (proc_node(pp, &who, &what, &nl)) ((struct lstat *)b)->nlink = (uint64_t)nl;
        }
        if (fd->kind == FD_DEV) dev_stat((struct lstat *)b, fd->dev);
        if (fd->kind == FD_WIN) dev_stat((struct lstat *)b, DEV_WINDOW);
        if (fd->kind == FD_SND) dev_stat((struct lstat *)b, fd->dev);
        if (fd->kind == FD_SHM) {                              /* a file in memory */
            struct lstat *ls = (struct lstat *)b;
            ls->mode = 0100600; ls->size = (int64_t)((struct shm *)fd->obj)->size; ls->blocks = (ls->size + 511) / 512;
            ls->ino = path_ino("<memfd>") ^ (uint64_t)fd->obj;
        }
        if (fd->kind == FD_CHAN) {                             /* a socket or a fifo, told apart by its chans */
            struct lstat *ls = (struct lstat *)b;
            ls->mode = fd->sock ? 0140777 : 0010600; ls->size = 0; ls->blocks = 0;
            ls->ino = path_ino("<chan>") ^ (uint64_t)(fd->rx ? fd->rx : fd->tx);
        }
        return 0;
    }
    case 4: case 6: case 262: {                                /* stat, lstat, newfstatat */
        uint64_t up = n == 262 ? b : a, us = n == 262 ? c : b;
        char p[200], full[200];
        if (n == 262 && (d & 0x1000) && (!up || !uok(up, 1, false) || !*(char *)up)) {   /* AT_EMPTY_PATH: fstat */
            struct frame g = *f; g.rax = 5; g.rdi = a; g.rsi = c; return linux_syscall(&g);
        }
        if (!ustr(p, up, sizeof p) || !uok(us, sizeof(struct lstat), true)) return E(EFAULT_);
        int64_t aj = at_join(lx, n == 262 ? (int)a : -100, p, full, sizeof full);
        if (aj) return (uint64_t)aj;
        if (dev_of(full)) { dev_stat((struct lstat *)us, dev_of(full)); return 0; }
        char pp[200];
        if (proc_norm(full, pp, sizeof pp)) {                  /* /proc */
            struct task *who; int what, nl, k = proc_node(pp, &who, &what, &nl);
            if (!k) return E(ENOENT_);
            fill_stat((struct lstat *)us, k == 1, 0, false, full);
            ((struct lstat *)us)->nlink = (uint64_t)nl;
            if (k == 2) ((struct lstat *)us)->mode = 0100444;
            return 0;
        }
        struct dunc_stat st;
        if (vfs_stat(full, &st) != 0) return E(ENOENT_);
        fill_stat((struct lstat *)us, st.is_dir, st.size, false, full);
        return 0;
    }
    case 137: case 138: {                                      /* statfs, fstatfs */
        bool mem = false;                                      /* /dev, /dev/shm: memory (tmpfs) */
        if (n == 137) {
            char p[200], full[200]; struct dunc_stat st;
            if (!ustr(p, a, sizeof p)) return E(EFAULT_);
            path_join(lx->cwd, p, full, sizeof full);
            mem = !strcmp(full, "/system/linux/dev") || !strcmp(full, "/system/linux/dev/shm") || dev_of(full);
            if (!mem && vfs_stat(full, &st) != 0) return E(ENOENT_);
        } else if (a >= NFD || !lx->fd[a].kind) return E(EBADF_);
        if (!uok(b, 120, true)) return E(EFAULT_);
        uint64_t *sf = (uint64_t *)b; memset(sf, 0, 120);
        bool file = !mem && (n == 137 || lx->fd[a].kind == FD_FILE || lx->fd[a].kind == FD_DIR);
        sf[0] = file ? 0x4d44 : 0x01021994;                    /* f_type: MSDOS_SUPER_MAGIC (FAT), else TMPFS_MAGIC */
        sf[1] = 4096; sf[2] = 1 << 20; sf[3] = sf[4] = 1 << 19; /* f_bsize, f_blocks, f_bfree, f_bavail (nominal) */
        if (!file) { sf[2] = mem_total() / 4096; sf[3] = sf[4] = (mem_total() - mem_used()) / 4096; }   /* (Chromium checks /dev/shm has room) */
        sf[5] = 1 << 20; sf[6] = 1 << 19;                       /* f_files, f_ffree */
        sf[8] = 255; sf[9] = 4096;                              /* f_namelen, f_frsize */
        return 0;
    }
    case 21: case 269: {                                       /* access, faccessat */
        char p[200], full[200];
        if (!ustr(p, n == 21 ? a : b, sizeof p)) return E(EFAULT_);
        int64_t aj = at_join(lx, n == 269 ? (int)a : -100, p, full, sizeof full);
        if (aj) return (uint64_t)aj;
        struct dunc_stat st; char pp[200];
        if (proc_norm(full, pp, sizeof pp)) { struct task *who; int what, nl; return proc_node(pp, &who, &what, &nl) ? 0 : E(ENOENT_); }
        return dev_of(full) || vfs_stat(full, &st) == 0 ? 0 : E(ENOENT_);
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
        #define MAXDENTS 4096                                   /* (a folder of shared libraries can be big) */
        static struct vfs_dirent *ents;                        /* full names, up to 255 characters */
        if (!ents) ents = kmalloc(sizeof *ents * MAXDENTS);
        if (!ents) return E(ENOMEM_);
        char pp[200]; int ne;
        if (proc_norm(fd->path, pp, sizeof pp)) ne = proc_list(lx, pp, ents, MAXDENTS);
        else ne = vfs_list_full(fd->path, ents, MAXDENTS);
        uint64_t pos = 0;
        while ((int)fd->off < ne) {
            const struct vfs_dirent *de = &ents[fd->off];
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
        if (a < NFD && lx->fd[a].kind == FD_INOTIFY && b == 0x541B && uok(c, 4, true)) { *(int32_t *)c = (int32_t)((struct inot *)lx->fd[a].obj)->qlen; return 0; }   /* FIONREAD */
        if (a < NFD && lx->fd[a].kind == FD_WIN) return axwin_ioctl(lx, &lx->fd[a], (uint32_t)b, c);
        if (a < NFD && lx->fd[a].kind == FD_SND) { long r = snd_ioctl(lx->fd[a].obj, (uint32_t)b, c, lx->fd[a].flags & 04000); return r < 0 ? E(-r) : (uint64_t)r; }
        if (a < NFD && lx->fd[a].kind == FD_INET) {
            struct lfd *fd = &lx->fd[a]; struct isock *so = fd->obj;
            if (b == 0x541B && uok(c, 4, true)) {                /* FIONREAD */
                struct tcp_sock_info i; uint32_t full = 0; uint8_t one;
                if (so->type == 1) *(int32_t *)c = so->h >= 0 && !tcp_sock_info(so->h, so, &i) ? (int32_t)i.rlen : 0;
                else *(int32_t *)c = so->h >= 0 && udp_sock_recv(so->h, so, &one, 1, 0, 0, true, &full) >= 0 ? (int32_t)full : 0;
                return 0;
            }
            if (b == 0x5421 && uok(c, 4, false)) { if (*(int32_t *)c) fd->flags |= 04000; else fd->flags &= ~04000; return 0; }   /* FIONBIO */
            return E(ENOTTY_);
        }
        if (a < NFD && lx->fd[a].kind == FD_CHAN) {
            struct lfd *fd = &lx->fd[a];
            if (b == 0x541B && uok(c, 4, true)) { *(int32_t *)c = fd->rx ? (int32_t)(fd->rx->packet && fd->rx->mcount ? fd->rx->msg[fd->rx->mhead] : fd->rx->len) : 0; return 0; }   /* FIONREAD */
            if (b == 0x5421 && uok(c, 4, false)) { if (*(int32_t *)c) fd->flags |= 04000; else fd->flags &= ~04000; return 0; }   /* FIONBIO */
        }
        return E(ENOTTY_);
    case 72: {                                                 /* fcntl */
        if (a >= NFD || !lx->fd[a].kind) return E(EBADF_);
        struct lfd *fd = &lx->fd[a];
        switch (b) {
        case 0: case 1030: {                                   /* F_DUPFD, F_DUPFD_CLOEXEC */
            int k = c < NFD ? fd_new(lx, (int)c) : -1; if (k < 0) return E(EMFILE_);
            fd_copy(&lx->fd[k], fd); return k;
        }
        case 3: return (uint64_t)(fd->flags & ~0300 & ~01000);  /* F_GETFL (without O_CREAT, O_EXCL, O_TRUNC) */
        case 1033: case 1034: {                                /* F_ADD_SEALS, F_GET_SEALS: memfds only */
            if (fd->kind != FD_SHM) return E(EINVAL_);
            struct shm *o = fd->obj;
            if (b == 1034) return (uint64_t)o->seals;
            if (c & ~(uint64_t)63) return E(EINVAL_);
            if (!(fd->flags & 3)) return E(EPERM_);
            if (o->seals & SEAL_SEAL_) return E(EPERM_);
            if ((c & SEAL_WRITE_) && !(o->seals & SEAL_WRITE_))  /* not while it's mapped shared and writable */
                for (int i = 0; i < lx->nmaps; i++)
                    if (lx->maps[i].obj == o && (mm_prot_at(t->mm, lx->maps[i].s) & DPROT_WRITE)) return E(EBUSY_);
            o->seals |= (int)c;
            return 0;
        }
        case 4: fd->flags = (fd->flags & ~(04000 | 02000)) | ((int)c & (04000 | 02000)); return 0;   /* F_SETFL: O_NONBLOCK, O_APPEND */
        case 5: case 36:                                       /* F_GETLK, F_OFD_GETLK: nobody else holds a lock (one program per */
            if (!uok(c, 32, true)) return E(EFAULT_);          /* file: locks are always granted), so say F_UNLCK */
            *(int16_t *)c = 2; return 0;
        case 1: return lx->fd[a].cloexec ? 1 : 0;              /* F_GETFD: FD_CLOEXEC */
        case 2: lx->fd[a].cloexec = (c & 1) != 0; return 0;    /* F_SETFD */
        default: return 0;                                     /* locks (one user) */
        }
    }
    case 32: { if (a >= NFD || !lx->fd[a].kind) return E(EBADF_); int k = fd_new(lx, 0); if (k < 0) return E(EMFILE_); fd_copy(&lx->fd[k], &lx->fd[a]); return k; }   /* dup */
    case 33: case 292:                                         /* dup2, dup3 */
        if (a >= NFD || !lx->fd[a].kind || b >= NFD) return E(EBADF_);
        if (n == 292 && a == b) return E(EINVAL_);
        fd_copy(&lx->fd[b], &lx->fd[a]);
        return b;

    /* ---- pipes, socket pairs, poll ---- */
    case 22: case 293: {                                       /* pipe, pipe2 */
        if (n == 293 && (b & ~(uint64_t)(04000 | 02000000 | 040000))) return E(EINVAL_);   /* O_NONBLOCK, O_CLOEXEC, O_DIRECT */
        if (!uok(a, 8, true)) return E(EFAULT_);
        struct chan *ch = chan_new(false); if (!ch) return E(ENOMEM_);
        int fds[2];
        if (chan_fds(lx, ch, 0, 0, ch, false, n == 293 ? (int)b : 0, fds) < 0) { kfree(ch->data); kfree(ch); return E(EMFILE_); }
        lx->fd[fds[0]].flags &= ~3; lx->fd[fds[1]].flags = (lx->fd[fds[1]].flags & ~3) | 1;   /* O_RDONLY / O_WRONLY */
        ((int32_t *)a)[0] = fds[0]; ((int32_t *)a)[1] = fds[1];
        return 0;
    }
    case 73:                                                   /* flock: one user, so every lock is granted */
        if (a >= NFD || !lx->fd[a].kind) return E(EBADF_);
        return 0;
    case 53: {                                                 /* socketpair */
        int type = (int)b & 0xF;
        if (a != 1) return E(EAFNOSUPPORT_);                   /* AF_UNIX only */
        if (type != 1 && type != 2 && type != 5) return E(EINVAL_);   /* SOCK_STREAM, SOCK_DGRAM, SOCK_SEQPACKET */
        if ((b & ~(uint64_t)0xF) & ~(uint64_t)(04000 | 02000000)) return E(EINVAL_);   /* SOCK_NONBLOCK, SOCK_CLOEXEC */
        if (!uok(d, 8, true)) return E(EFAULT_);
        struct chan *x = chan_new(type != 1), *y = chan_new(type != 1);
        int fds[2];
        if (!x || !y || chan_fds(lx, x, y, y, x, true, (int)b, fds) < 0) {
            if (x) { kfree(x->data); kfree(x); }
            if (y) { kfree(y->data); kfree(y); }
            return x && y ? E(EMFILE_) : E(ENOMEM_);
        }
        lx->fd[fds[0]].stype = lx->fd[fds[1]].stype = (uint8_t)type;
        ((int32_t *)d)[0] = fds[0]; ((int32_t *)d)[1] = fds[1];
        return 0;
    }
    case 54: case 55: {                                        /* setsockopt, getsockopt: socket pairs */
        if (a >= NFD || !lx->fd[a].kind) return E(EBADF_);
        struct lfd *fd = &lx->fd[a];
        if (fd->kind == FD_INET) {                             /* options are taken and mostly ignored (net.c has fixed buffers) */
            struct isock *so = fd->obj;
            if (n == 54) return uok(d, e < 4 ? 4 : e, false) ? 0 : E(EFAULT_);
            if (!uok(e, 4, true) || *(uint32_t *)e < 4 || !uok(d, *(uint32_t *)e, true)) return E(EFAULT_);
            uint32_t v = 0;
            if (b == 1) switch (c) {                           /* SOL_SOCKET */
                case 3: v = (uint32_t)so->type; break;         /* SO_TYPE */
                case 4: v = (uint32_t)isock_soerror(so); break;   /* SO_ERROR */
                case 7: case 8: v = 65536; break;              /* SO_SNDBUF, SO_RCVBUF */
                case 39: v = (uint32_t)so->fam; break;         /* SO_DOMAIN */
                case 38: v = so->type == 1 ? 6 : 17; break;    /* SO_PROTOCOL */
                default: v = 0;
            } else if (b == 6 && c == 2) v = 1460;             /* TCP_MAXSEG */
            *(uint32_t *)d = v; *(uint32_t *)e = 4;
            return 0;
        }
        if (fd->kind != FD_CHAN || !fd->sock) return E(ENOTSOCK_);
        if (n == 54) {
            if (!uok(d, e < 4 ? 4 : e, false)) return E(EFAULT_);
            if (b == 1) return 0;                              /* SOL_SOCKET: buffer sizes, SO_PASSCRED, priority... all fine as they are */
            return E(ENOPROTOOPT_);
        }
        if (!uok(e, 4, true) || !uok(d, *(uint32_t *)e, true) || *(uint32_t *)e < 4) return E(EFAULT_);
        if (b != 1) return E(ENOPROTOOPT_);
        uint32_t v;
        switch (c) {
        case 3: v = fd->stype; break;                          /* SO_TYPE */
        case 4: v = 0; break;                                  /* SO_ERROR */
        case 7: case 8: v = CHAN_CAP; break;                   /* SO_SNDBUF, SO_RCVBUF */
        case 39: v = 1; break;                                 /* SO_DOMAIN: AF_UNIX */
        case 16: case 12: v = 0; break;                        /* SO_PASSCRED, SO_PRIORITY */
        case 30: v = fd->lis ? 1 : 0; break;                   /* SO_ACCEPTCONN */
        case 17:                                               /* SO_PEERCRED: the other end's program (a pair: this one) */
            if (*(uint32_t *)e < 12) return E(EINVAL_);
            ((uint32_t *)d)[0] = (uint32_t)(fd->peer ? fd->peer : t->proc->pid); ((uint32_t *)d)[1] = 1000; ((uint32_t *)d)[2] = 1000;
            *(uint32_t *)e = 12; return 0;
        default: return E(ENOPROTOOPT_);
        }
        *(uint32_t *)d = v; *(uint32_t *)e = 4;
        return 0;
    }
    case 41: {                                                 /* socket: internet sockets (AF_UNIX: use socketpair) */
        int type = (int)(b & 0xF);
        if (a != 2 && a != 10) return a == 1 ? E(ENOSYS_) : E(EAFNOSUPPORT_);
        if ((b & ~(uint64_t)0xF) & ~(uint64_t)(04000 | 02000000)) return E(EINVAL_);   /* SOCK_NONBLOCK, SOCK_CLOEXEC */
        if (type != 1 && type != 2) return E(EPROTONOSUPPORT_);
        if (c && c != (type == 1 ? 6u : 17u)) return E(EPROTONOSUPPORT_);
        return isock_new(lx, (int)a, type, (int)b);
    }
    case 42: case 49: {                                        /* connect, bind */
        if (a >= NFD || !lx->fd[a].kind) return E(EBADF_);
        struct lfd *fd = &lx->fd[a];
        if (fd->kind != FD_INET) return E(ENOTSOCK_);
        struct isock *so = fd->obj;
        struct ipaddr ip; uint16_t port = 0;
        int64_t r = sa_get(b, c, &ip, &port);
        if (r < 0) return (uint64_t)r;
        if (n == 49) {                                         /* bind: only the port matters (one address per machine here) */
            if (so->h >= 0 || so->bport) return E(EINVAL_);
            so->bport = port;
            if (so->type == 2) { int h = udp_sock_open(so, port); if (h < 0) return h == NET_EINVAL ? E(EADDRINUSE_) : neterr(h); so->h = h; so->bport = (uint16_t)udp_sock_port(h, so); }
            return 0;
        }
        if (r == 1) { if (so->type == 2) so->peer_set = false; return 0; }   /* AF_UNSPEC: forget the peer (UDP) */
        return (uint64_t)isock_connect(so, &ip, port, fd->flags & 04000);
    }
    case 50: case 43: case 288:                                /* listen, accept, accept4: no servers yet */
        if (a >= NFD || !lx->fd[a].kind) return E(EBADF_);
        return lx->fd[a].kind == FD_INET ? E(EOPNOTSUPP_) : E(ENOTSOCK_);
    case 51: case 52: {                                        /* getsockname, getpeername */
        if (a >= NFD || !lx->fd[a].kind) return E(EBADF_);
        struct lfd *fd = &lx->fd[a];
        if (fd->kind == FD_CHAN && fd->sock) {                 /* a socket pair: an unnamed AF_UNIX address */
            if (!uok(c, 4, true)) return E(EFAULT_);
            if (*(uint32_t *)c >= 2 && uok(b, 2, true)) *(uint16_t *)b = 1;
            *(uint32_t *)c = 2; return 0;
        }
        if (fd->kind != FD_INET) return E(ENOTSOCK_);
        struct isock *so = fd->obj;
        struct ipaddr ip; memset(&ip, 0, sizeof ip); uint16_t port = 0;
        if (n == 52) {
            if (!so->peer_set) return E(ENOTCONN_);
            if (so->type == 1) { struct tcp_sock_info i; if (so->h < 0 || tcp_sock_info(so->h, so, &i) || i.state == 0) return E(ENOTCONN_); }
            ip = so->peer; port = so->pport;
        } else if (so->type == 1) {
            struct tcp_sock_info i;
            if (so->h >= 0 && !tcp_sock_info(so->h, so, &i)) { ip = i.lip; port = i.lport; } else port = so->bport;
        } else {
            if (so->peer_set) net_local_addr(&so->peer, &ip);
            port = so->h >= 0 ? (uint16_t)udp_sock_port(so->h, so) : so->bport;
        }
        if (so->fam == 10 && !ip.v6 && !(ip.b[0] | ip.b[1] | ip.b[2] | ip.b[3])) ip.v6 = 0;
        sa_put(so, b, c, &ip, port);
        return 0;
    }
    case 307: case 299: {                                      /* sendmmsg, recvmmsg: several sendmsg / recvmsg */
        if (c > 1024) c = 1024;
        if (!uok(b, c * 64, true)) return E(EFAULT_);
        uint64_t k = 0;
        for (; k < c; k++) {
            struct frame g = *f; g.rax = n == 307 ? 46 : 47; g.rdi = a; g.rsi = b + k * 64;
            g.rdx = (d & ~(uint64_t)0x10000) | (k && n == 299 ? 0x40 : 0);   /* (MSG_WAITFORONE: wait for the first only) */
            uint64_t r = linux_syscall_(&g);
            if ((int64_t)r < 0) { if (k) break; return r; }
            *(uint32_t *)(b + k * 64 + 56) = (uint32_t)r;
        }
        return k;
    }
    case 48: {                                                 /* shutdown */
        if (a >= NFD || !lx->fd[a].kind) return E(EBADF_);
        struct lfd *fd = &lx->fd[a];
        if (fd->kind == FD_INET) {
            struct isock *so = fd->obj;
            if (b > 2) return E(EINVAL_);
            if (so->type == 1 && so->h < 0) return E(ENOTCONN_);
            if (b != 1) so->rd_shut = true;
            if (b != 0) { so->wr_shut = true; if (so->type == 1) tcp_sock_shutdown(so->h, so); }
            chan_wake();
            return 0;
        }
        if (fd->kind != FD_CHAN || !fd->sock) return E(ENOTSOCK_);
        if (b > 2) return E(EINVAL_);
        if (!fd->obj) fd->obj = (void *)fd_id(fd);             /* the same open file to epoll after this (Mojo shuts a socket down, then stops watching it) */
        if (b != 1 && fd->rx) { chan_drop(fd->rx, true); fd->rx = 0; }    /* SHUT_RD, SHUT_RDWR */
        if (b != 0 && fd->tx) { chan_drop(fd->tx, false); fd->tx = 0; }   /* SHUT_WR, SHUT_RDWR */
        return 0;
    }
    case 319: {                                                /* memfd_create (the name is only for /proc, which isn't there) */
        char nm[250];
        if (!ustr(nm, a, sizeof nm)) return uok(a, 1, false) ? E(EINVAL_) : E(EFAULT_);   /* (names are at most 249 bytes) */
        if (b & ~(uint64_t)(1 | 2 | 8 | 16)) return E(EINVAL_);   /* MFD_CLOEXEC, ALLOW_SEALING, NOEXEC_SEAL, EXEC (no HUGETLB); Chromium checks this */
        if ((b & 8) && (b & 16)) return E(EINVAL_);
        struct shm *o = kmalloc(sizeof *o); if (!o) return E(ENOMEM_);
        int k = fd_new(lx, 0); if (k < 0) { kfree(o); return E(EMFILE_); }
        memset(o, 0, sizeof *o); o->refs = 1;
        o->seals = (b & 2) ? 0 : SEAL_SEAL_;                   /* without MFD_ALLOW_SEALING no seals can be added */
        if (b & 8) o->seals |= SEAL_EXEC_;                      /* MFD_NOEXEC_SEAL */
        struct lfd *fd = &lx->fd[k]; memset(fd, 0, sizeof *fd);
        fd->kind = FD_SHM; fd->obj = o; fd->flags = 2;
        return k;
    }
    case 77: case 285: {                                       /* ftruncate, fallocate */
        if (a >= NFD || !lx->fd[a].kind) return E(EBADF_);
        struct lfd *fd = &lx->fd[a];
        uint64_t want = n == 77 ? b : c + d;                   /* (fallocate: offset + length, if that's bigger) */
        if ((int64_t)want < 0) return E(EINVAL_);
        if (fd->kind == FD_SHM) {
            struct shm *o = fd->obj;
            if (n == 285 && want <= o->size) return 0;
            if ((want < o->size && (o->seals & SEAL_SHRINK_)) || (want > o->size && (o->seals & SEAL_GROW_))) return E(EPERM_);
            if (!(fd->flags & 3)) return E(EINVAL_);
            return shm_resize(o, want) ? 0 : E(ENOMEM_);
        }
        if (fd->kind != FD_FILE || !(fd->flags & 3)) return E(EINVAL_);
        struct dunc_stat st; if (vfs_stat(fd->path, &st) != 0) return E(ENOENT_);
        if (n == 285 && want <= st.size) return 0;
        if (want == st.size) return 0;
        uint8_t *buf = kmalloc(want ? want : 1); if (!buf) return E(ENOMEM_);   /* (files on AXIOM's volumes: read, resize, write) */
        memset(buf, 0, want); vfs_read(fd->path, buf, 0, st.size < want ? st.size : want);
        long r = vfs_write(fd->path, buf, want); kfree(buf);
        return r < 0 ? E(EINVAL_) : 0;
    }
    case 221:                                                  /* fadvise64: only advice */
        return a < NFD && lx->fd[a].kind ? 0 : E(EBADF_);
    case 74: case 75:                                          /* fsync, fdatasync: writes go straight to the volume */
        return a < NFD && lx->fd[a].kind ? 0 : E(EBADF_);
    case 283: {                                                /* timerfd_create */
        if (a != 0 && a != 1 && a != 7 && a != 8 && a != 9) return E(EINVAL_);   /* REALTIME, MONOTONIC, BOOTTIME (+_ALARM) */
        if (b & ~(uint64_t)(04000 | 02000000)) return E(EINVAL_);
        struct tfd *x = kmalloc(sizeof *x); if (!x) return E(ENOMEM_);
        int k = fd_new(lx, 0); if (k < 0) { kfree(x); return E(EMFILE_); }
        memset(x, 0, sizeof *x); x->refs = 1; x->clock = (int)a;
        struct lfd *fd = &lx->fd[k]; memset(fd, 0, sizeof *fd);
        fd->kind = FD_TIMER; fd->obj = x; fd->flags = 2 | ((int)b & 04000);
        return k;
    }
    case 286: case 287: {                                      /* timerfd_settime, timerfd_gettime */
        if (a >= NFD || lx->fd[a].kind != FD_TIMER) return a < NFD && lx->fd[a].kind ? E(EINVAL_) : E(EBADF_);
        struct tfd *x = lx->fd[a].obj;
        uint64_t old_spec = n == 286 ? d : b;                   /* where the old setting goes */
        tfd_update(x);
        if (old_spec) {
            if (!uok(old_spec, 32, true)) return E(EFAULT_);
            uint64_t now = uptime_ns(), left = x->next > now ? x->next - now : 0;
            uint64_t *o = (uint64_t *)old_spec;
            o[0] = x->interval / 1000000000ull; o[1] = x->interval % 1000000000ull;
            o[2] = left / 1000000000ull; o[3] = left % 1000000000ull;
            if (x->next && !left) o[3] = 1;
        }
        if (n == 287) return 0;
        if (b & ~(uint64_t)3) return E(EINVAL_);                /* TFD_TIMER_ABSTIME, CANCEL_ON_SET */
        if (!uok(c, 32, false)) return E(EFAULT_);
        const int64_t *v = (const int64_t *)c;
        if (v[0] < 0 || v[2] < 0 || (uint64_t)v[1] >= 1000000000ull || (uint64_t)v[3] >= 1000000000ull) return E(EINVAL_);
        uint64_t interval = (uint64_t)v[0] * 1000000000ull + (uint64_t)v[1], val = (uint64_t)v[2] * 1000000000ull + (uint64_t)v[3];
        x->ticks = 0; x->interval = interval;
        if (!val) x->next = 0;
        else if (b & 1) {                                       /* absolute, on the timer's clock */
            uint64_t up = uptime_ns(), clk = (x->clock == 0 || x->clock == 8) ? linux_now_ns() : up;
            x->next = val > clk ? up + (val - clk) : up;
            if (!x->next) x->next = 1;
        } else x->next = uptime_ns() + val;
        chan_wake();                                            /* (pollers look again, for the new deadline) */
        return 0;
    }
    case 253: case 294: {                                      /* inotify_init, inotify_init1 */
        if (n == 294 && (a & ~(uint64_t)(04000 | 02000000))) return E(EINVAL_);   /* IN_NONBLOCK, IN_CLOEXEC */
        struct inot *in = kmalloc(sizeof *in); if (!in) return E(ENOMEM_);
        int k = fd_new(lx, 0); if (k < 0) { kfree(in); return E(EMFILE_); }
        memset(in, 0, sizeof *in - INOT_QUEUE); in->refs = 1; in->next_wd = 1;
        in->next = inots; inots = in; vfs_watch = linux_vfs_watch;
        struct lfd *fd = &lx->fd[k]; memset(fd, 0, sizeof *fd);
        fd->kind = FD_INOTIFY; fd->obj = in; fd->flags = n == 294 ? (int)a & 04000 : 0;
        if (n == 294 && (a & 02000000)) fd->cloexec = true;
        return k;
    }
    case 254: {                                                /* inotify_add_watch(fd, path, mask) */
        if (a >= NFD || lx->fd[a].kind != FD_INOTIFY) return a < NFD && lx->fd[a].kind ? E(EINVAL_) : E(EBADF_);
        struct inot *in = lx->fd[a].obj; uint32_t mask = (uint32_t)c;
        if (!(mask & IN_ALL_EVENTS_) || ((mask & IN_MASK_ADD_) && (mask & IN_MASK_CREATE_))) return E(EINVAL_);
        char p[200], full[200]; struct dunc_stat st;
        if (!ustr(p, b, sizeof p)) return E(EFAULT_);
        path_join(lx->cwd, p, full, sizeof full);
        if (vfs_stat(full, &st) != FS_OK) return E(ENOENT_);
        if ((mask & IN_ONLYDIR_) && !st.is_dir) return E(ENOTDIR_);
        for (int i = 0; i < in->nw; i++)
            if (!strcmp(in->w[i].path, full)) {
                if (mask & IN_MASK_CREATE_) return E(EEXIST_);
                in->w[i].mask = (mask & IN_MASK_ADD_ ? in->w[i].mask : 0) | mask;
                return (uint64_t)in->w[i].wd;
            }
        if (in->nw >= INOT_WATCHES) return E(ENOSPC_);
        struct iwatch *w = &in->w[in->nw++];
        w->wd = in->next_wd++; w->mask = mask; strncpy0(w->path, full, sizeof w->path);
        return (uint64_t)w->wd;
    }
    case 255: {                                                /* inotify_rm_watch(fd, wd) */
        if (a >= NFD || lx->fd[a].kind != FD_INOTIFY) return a < NFD && lx->fd[a].kind ? E(EINVAL_) : E(EBADF_);
        struct inot *in = lx->fd[a].obj;
        for (int i = 0; i < in->nw; i++) if (in->w[i].wd == (int)b) { inot_drop(in, i); return 0; }
        return E(EINVAL_);
    }
    case 284: case 290: {                                      /* eventfd, eventfd2 */
        if (n == 290 && (b & ~(uint64_t)(1 | 04000 | 02000000))) return E(EINVAL_);   /* EFD_SEMAPHORE, NONBLOCK, CLOEXEC (Chromium checks) */
        struct evfd *ev = kmalloc(sizeof *ev); if (!ev) return E(ENOMEM_);
        int k = fd_new(lx, 0); if (k < 0) { kfree(ev); return E(EMFILE_); }
        ev->refs = 1; ev->count = (uint32_t)a; ev->sem = n == 290 && (b & 1);   /* EFD_SEMAPHORE */
        struct lfd *fd = &lx->fd[k]; memset(fd, 0, sizeof *fd);
        fd->kind = FD_EVENT; fd->obj = ev; fd->flags = 2 | (n == 290 ? (int)b & 04000 : 0);
        return k;
    }
    case 213: case 291: {                                      /* epoll_create, epoll_create1 */
        if (n == 213 && (int)a <= 0) return E(EINVAL_);
        if (n == 291 && (a & ~(uint64_t)02000000)) return E(EINVAL_);   /* EPOLL_CLOEXEC only */
        struct epoll *ep = kmalloc(sizeof *ep); if (!ep) return E(ENOMEM_);
        int k = fd_new(lx, 0); if (k < 0) { kfree(ep); return E(EMFILE_); }
        memset(ep, 0, sizeof *ep); ep->refs = 1;
        struct lfd *fd = &lx->fd[k]; memset(fd, 0, sizeof *fd);
        fd->kind = FD_EPOLL; fd->obj = ep; fd->flags = 2;
        return k;
    }
    case 233: {                                                /* epoll_ctl */
        if (a >= NFD || !lx->fd[a].kind || c >= NFD || !lx->fd[c].kind) return E(EBADF_);
        if (lx->fd[a].kind != FD_EPOLL || a == c) return E(EINVAL_);
        struct lfd *tf = &lx->fd[c];
        if (tf->kind == FD_FILE || tf->kind == FD_DIR) return E(EPERM_);   /* (as Linux: files are always ready) */
        struct epoll *ep = lx->fd[a].obj;
        int i = 0; const void *id = fd_id(tf);
        while (i < ep->n && !(ep->it[i].fd == (int)c && ep->it[i].id == id)) i++;
        if (b == 2) {                                          /* EPOLL_CTL_DEL */
            if (i == ep->n) return E(ENOENT_);
            ep->it[i] = ep->it[--ep->n]; return 0;
        }
        if (b != 1 && b != 3) return E(EINVAL_);
        if (!uok(d, 12, false)) return E(EFAULT_);
        if (b == 1 && i < ep->n) return E(EEXIST_);             /* EPOLL_CTL_ADD */
        if (b == 3 && i == ep->n) return E(ENOENT_);            /* EPOLL_CTL_MOD */
        if (b == 1) { if (ep->n == NFD) return E(ENOMEM_); ep->n++; }
        struct epitem *it = &ep->it[i];
        it->fd = (int)c; it->id = id; it->off = false;
        it->events = *(uint32_t *)d; it->data = *(uint64_t *)(d + 4);
        chan_wake();                                           /* (a waiter may now have something to report) */
        return 0;
    }
    case 232: case 281:                                        /* epoll_wait, epoll_pwait (no signals: the mask doesn't matter) */
        return do_epoll_wait(lx, a, b, (int)c, (int64_t)(int32_t)d);
    case 441: {                                                /* epoll_pwait2 (a timespec) */
        bool forever; uint64_t ms = ts_ms(d, &forever);
        return do_epoll_wait(lx, a, b, (int)c, forever ? -1 : (int64_t)ms);
    }
    case 7: return do_poll(lx, a, b, (int64_t)(int32_t)c);    /* poll (timeout in ms, negative: none) */
    case 271: {                                                /* ppoll (no signals, so the mask doesn't matter) */
        bool forever; uint64_t ms = ts_ms(c, &forever);
        return do_poll(lx, a, b, forever ? -1 : (int64_t)ms);
    }
    case 44: case 45: {                                        /* sendto, recvfrom: on a socket pair (no addresses) */
        if (a >= NFD || !lx->fd[a].kind) return E(EBADF_);
        struct lfd *fd = &lx->fd[a];
        if (fd->kind == FD_INET) {
            struct isock *so = fd->obj;
            bool nb = (fd->flags & 04000) || (d & 0x40);       /* MSG_DONTWAIT */
            if (!uok(b, c, n == 45)) return E(EFAULT_);
            if (n == 44) {
                struct ipaddr to; uint16_t tp = 0;
                if (e && so->type == 2) { int64_t r = sa_get(e, f->r9, &to, &tp); if (r < 0) return (uint64_t)r; if (r == 1) e = 0; }
                else e = 0;
                return (uint64_t)isock_send(so, (const uint8_t *)b, c, e ? &to : 0, tp, nb);
            }
            struct ipaddr from; uint16_t fp = 0;
            int64_t r = isock_recv(so, (uint8_t *)b, c, nb, d & 2, &from, &fp, 0);   /* MSG_PEEK */
            if (r >= 0 && e) sa_put(so, e, f->r9, &from, fp);
            return (uint64_t)r;
        }
        if (fd->kind != FD_CHAN || !fd->sock) return E(ENOTSOCK_);
        bool nb = (fd->flags & 04000) || (d & 0x40);           /* MSG_DONTWAIT */
        if (!uok(b, c, n == 45)) return E(EFAULT_);
        if (n == 44) return (uint64_t)chan_write(fd, (const uint8_t *)b, c, true, nb, 0);
        if (e && uok(f->r9, 4, true)) *(uint32_t *)f->r9 = 0;   /* (no sender address) */
        return (uint64_t)chan_read(fd, (uint8_t *)b, c, true, nb, d & 2, 0, 0);   /* MSG_PEEK */
    }
    case 46: case 47: {                                        /* sendmsg, recvmsg: gathered / scattered, one message */
        if (a >= NFD || !lx->fd[a].kind) return E(EBADF_);
        struct lfd *fd = &lx->fd[a];
        if (fd->kind == FD_INET) {
            struct isock *so = fd->obj;
            if (!uok(b, 56, n == 47)) return E(EFAULT_);
            uint8_t *mh = (uint8_t *)b;
            uint64_t name = *(uint64_t *)mh, iov = *(uint64_t *)(mh + 16), niov = *(uint64_t *)(mh + 24);
            uint32_t namelen = *(uint32_t *)(mh + 8);
            if (niov > 1024 || !uok(iov, niov * 16, false)) return E(EFAULT_);
            uint64_t total = 0, cap = so->type == 2 ? 65536 : 1u << 20;
            for (uint64_t i = 0; i < niov; i++) total += ((uint64_t *)iov)[i * 2 + 1];
            if (total > cap) total = cap;
            uint8_t *kb = kmalloc(total ? total : 1); if (!kb) return E(ENOMEM_);
            bool nb = (fd->flags & 04000) || (c & 0x40);
            int64_t r;
            if (n == 46) {
                uint64_t at = 0;
                for (uint64_t i = 0; i < niov && at < total; i++) {
                    uint64_t base = ((uint64_t *)iov)[i * 2], len = ((uint64_t *)iov)[i * 2 + 1];
                    if (len > total - at) len = total - at;
                    if (!uok(base, len, false)) { kfree(kb); return E(EFAULT_); }
                    memcpy(kb + at, (const void *)base, len); at += len;
                }
                struct ipaddr to; uint16_t tp = 0; bool has = false;
                if (name && namelen && so->type == 2) { r = sa_get(name, namelen, &to, &tp); if (r < 0) { kfree(kb); return (uint64_t)r; } has = r == 0; }
                r = isock_send(so, kb, total, has ? &to : 0, tp, nb);
            } else {
                struct ipaddr from; uint16_t fp = 0; uint32_t full = 0;
                r = isock_recv(so, kb, total, nb, c & 2, &from, &fp, &full);
                if (r >= 0) {
                    uint64_t at = 0;
                    for (uint64_t i = 0; i < niov && at < (uint64_t)r; i++) {
                        uint64_t base = ((uint64_t *)iov)[i * 2], len = ((uint64_t *)iov)[i * 2 + 1];
                        if (len > (uint64_t)r - at) len = (uint64_t)r - at;
                        if (!uok(base, len, true)) { kfree(kb); return E(EFAULT_); }
                        memcpy((void *)base, kb + at, len); at += len;
                    }
                    if (name) sa_put(so, name, b + 8, &from, fp);
                    *(uint64_t *)(mh + 40) = 0;                /* no control messages */
                    *(int32_t *)(mh + 48) = so->type == 2 && full > (uint64_t)r ? 0x20 : 0;   /* MSG_TRUNC */
                }
            }
            kfree(kb);
            return (uint64_t)r;
        }
        if (fd->kind != FD_CHAN || !fd->sock) return E(ENOTSOCK_);
        if (!uok(b, 56, n == 47)) return E(EFAULT_);
        uint8_t *mh = (uint8_t *)b;
        uint64_t iov = *(uint64_t *)(mh + 16), niov = *(uint64_t *)(mh + 24), ctl = *(uint64_t *)(mh + 32), ctllen = *(uint64_t *)(mh + 40);
        if (niov > 1024 || !uok(iov, niov * 16, false)) return E(EFAULT_);
        struct anc *anc = 0;
        if (n == 46 && ctl && ctllen) {                        /* control messages: files to pass (SCM_RIGHTS) */
            if (ctllen > 4096 || !uok(ctl, ctllen, false)) return E(EFAULT_);
            for (uint64_t off = 0; off + 16 <= ctllen;) {
                uint64_t len = *(uint64_t *)(ctl + off); int lvl = *(int32_t *)(ctl + off + 8), typ = *(int32_t *)(ctl + off + 12);
                if (len < 16 || off + len > ctllen) { anc_free(anc); return E(EINVAL_); }
                if (lvl == 1 && typ == 1) {                    /* SOL_SOCKET, SCM_RIGHTS */
                    if (!anc) { anc = kmalloc(sizeof *anc); if (!anc) return E(ENOMEM_); memset(anc, 0, sizeof *anc); }
                    for (uint64_t i = 0; i < (len - 16) / 4; i++) {
                        uint32_t sfd = *(uint32_t *)(ctl + off + 16 + i * 4);
                        if (sfd >= NFD || !lx->fd[sfd].kind) { anc_free(anc); return E(EBADF_); }
                        if (anc->n == ANC_MAX) { anc_free(anc); return E(EINVAL_); }
                        fd_copy(&anc->fd[anc->n++], &lx->fd[sfd]);
                    }
                }                                              /* (SCM_CREDENTIALS: the receiver can't ask for them here) */
                off += (len + 7) & ~7ULL;
            }
        }
        uint64_t total = 0;
        for (uint64_t i = 0; i < niov; i++) total += ((uint64_t *)iov)[i * 2 + 1];
        if (total > CHAN_CAP) total = CHAN_CAP;
        uint8_t *kb = kmalloc(total ? total : 1); if (!kb) { anc_free(anc); return E(ENOMEM_); }
        bool nb = (fd->flags & 04000) || (c & 0x40);
        int64_t r;
        if (n == 46) {
            uint64_t at = 0;
            for (uint64_t i = 0; i < niov && at < total; i++) {
                uint64_t base = ((uint64_t *)iov)[i * 2], len = ((uint64_t *)iov)[i * 2 + 1];
                if (len > total - at) len = total - at;
                if (!uok(base, len, false)) { kfree(kb); anc_free(anc); return E(EFAULT_); }
                memcpy(kb + at, (const void *)base, len); at += len;
            }
            r = chan_write(fd, kb, total, false, nb, &anc);
            anc_free(anc);                                     /* (not sent: nothing was written) */
        } else {
            bool trunc; struct anc *got = 0;
            r = chan_read(fd, kb, total, false, nb, c & 2, &trunc, &got);
            if (r >= 0) {
                uint64_t at = 0;
                for (uint64_t i = 0; i < niov && at < (uint64_t)r; i++) {
                    uint64_t base = ((uint64_t *)iov)[i * 2], len = ((uint64_t *)iov)[i * 2 + 1];
                    if (len > (uint64_t)r - at) len = (uint64_t)r - at;
                    if (!uok(base, len, true)) { kfree(kb); anc_free(got); return E(EFAULT_); }
                    memcpy((void *)base, kb + at, len); at += len;
                }
                int fl = trunc ? 0x20 : 0;                       /* MSG_TRUNC */
                uint64_t used = 0;
                if (got) {                                       /* the files: new numbers in this program */
                    int m = 0;
                    if (ctl && ctllen >= 20 && uok(ctl, ctllen, true)) {
                        int room = (int)((ctllen - 16) / 4);
                        for (; m < got->n && m < room; m++) {
                            int k = fd_new(lx, 0);
                            if (k < 0) break;
                            lx->fd[k] = got->fd[m]; memset(&got->fd[m], 0, sizeof got->fd[m]);   /* (moved: its references go too) */
                            *(int32_t *)(ctl + 16 + m * 4) = k;
                        }
                        if (m) { *(uint64_t *)ctl = 16 + 4 * (uint64_t)m; *(int32_t *)(ctl + 8) = 1; *(int32_t *)(ctl + 12) = 1; used = (16 + 4 * (uint64_t)m + 7) & ~7ULL; }
                    }
                    if (m < got->n) fl |= 0x8;                   /* MSG_CTRUNC: the rest are closed */
                    anc_free(got);
                }
                *(uint32_t *)(mh + 8) = 0;                       /* msg_namelen: no sender address */
                *(uint64_t *)(mh + 40) = used;                   /* msg_controllen */
                *(int32_t *)(mh + 48) = fl;
            }
        }
        kfree(kb);
        return (uint64_t)r;
    }
    case 89: case 267: {                                       /* readlink(at): only /proc/self/exe is a link */
        char p[200];
        if (!ustr(p, n == 89 ? a : b, sizeof p)) return E(EFAULT_);
        uint64_t ub = n == 89 ? b : c, ul = n == 89 ? c : d;
        char full[200], pp[200];
        int64_t aj = at_join(lx, n == 267 ? (int)a : -100, p, full, sizeof full);
        if (aj) return (uint64_t)aj;
        if (!proc_norm(full, pp, sizeof pp)) { struct dunc_stat st; return vfs_stat(full, &st) == 0 || dev_of(full) ? E(EINVAL_) : E(ENOENT_); }
        struct task *who; int what, nl;
        if (!proc_node(pp, &who, &what, &nl)) return E(ENOENT_);
        char *sl = pp + 6; while (*sl && *sl != '/') sl++;
        if (who != current->proc || strcmp(sl, "/exe")) return E(EINVAL_);
        uint64_t l = strlen(lx->exe); if (l > ul) l = ul;
        if (!uok(ub, l, true)) return E(EFAULT_);
        memcpy((void *)ub, lx->exe, l); return l;
    }
    case 83: case 258: {                                       /* mkdir, mkdirat */
        char p[200], full[200];
        if (!ustr(p, n == 83 ? a : b, sizeof p)) return E(EFAULT_);
        path_join(lx->cwd, p, full, sizeof full);
        if (vfs_mkdir(full) == 0) return 0;
        /* why not, as Linux says it (GLib's mkdir -p goes on to make the parents only after ENOENT) */
        struct dunc_stat st;
        if (vfs_stat(full, &st) == FS_OK) return E(EEXIST_);
        char par[200]; strncpy0(par, full, sizeof par);
        char *sl = 0; for (char *q = par; *q; q++) if (*q == '/') sl = q;
        if (sl && sl != par) { *sl = 0; if (vfs_stat(par, &st) != FS_OK) return E(ENOENT_); if (!st.is_dir) return E(ENOTDIR_); }
        return E(EACCES_);
    }
    case 87: case 263: case 84: {                              /* unlink, unlinkat, rmdir */
        char p[200], full[200];
        if (!ustr(p, n == 263 ? b : a, sizeof p)) return E(EFAULT_);
        path_join(lx->cwd, p, full, sizeof full);
        pc_file_written(full);
        return vfs_delete(full) == 0 ? 0 : E(ENOENT_);
    }
    case 86: case 265: {                                       /* link, linkat: FAT has no hard links, so a copy (same
                                                                  contents now; later writes to one don't show in the other) */
        char p[200], q[200], f1[200], f2[200]; struct dunc_stat st;
        if (!ustr(p, n == 86 ? a : b, sizeof p) || !ustr(q, n == 86 ? b : d, sizeof q)) return E(EFAULT_);
        path_join(lx->cwd, p, f1, sizeof f1); path_join(lx->cwd, q, f2, sizeof f2);
        if (vfs_stat(f1, &st) != 0) return E(ENOENT_);
        if (st.is_dir) return E(EPERM_);
        if (vfs_stat(f2, &st) == 0) return E(EEXIST_);
        vfs_stat(f1, &st);
        uint8_t *buf = kmalloc(st.size ? st.size : 1); if (!buf) return E(ENOMEM_);
        long r = vfs_read(f1, buf, 0, st.size);
        if (r >= 0) r = vfs_write(f2, buf, (uint64_t)r);
        kfree(buf);
        return r < 0 ? E(ENOENT_) : 0;
    }
    case 280: case 235: case 132: {                            /* utimensat, utimes, utime: the modified time (FAT keeps that one) */
        char p[200], full[200]; uint64_t up = n == 280 ? b : a, tv = n == 280 ? c : b;
        if (n == 280 && !up) {                                 /* futimens: the file of dirfd */
            if (a >= NFD || !lx->fd[a].kind || !lx->fd[a].path[0]) return E(EBADF_);
            strncpy0(full, lx->fd[a].path, sizeof full);
        } else {
            if (!ustr(p, up, sizeof p)) return E(EFAULT_);
            if (n == 280 && p[0] != '/' && (int)a != -100) {  /* relative to a directory file */
                if (a >= NFD || lx->fd[a].kind != FD_DIR) return E(EBADF_);
                char tmp[200]; ksnprintf(tmp, sizeof tmp, "%s/%s", lx->fd[a].path, p); strncpy0(full, tmp, sizeof full);
            } else path_join(lx->cwd, p, full, sizeof full);
        }
        int64_t when = (int64_t)(linux_now_ns() / 1000000000ull);
        if (tv) {
            if (!uok(tv, n == 132 ? 16 : 32, false)) return E(EFAULT_);
            if (n == 280) {                                    /* struct timespec[2]: [1] is the modified time */
                int64_t ns = ((int64_t *)tv)[3];
                if (ns == (1 << 30) - 2) return 0;             /* UTIME_OMIT */
                if (ns != (1 << 30) - 1) when = ((int64_t *)tv)[2];   /* (UTIME_NOW: now) */
            } else when = n == 235 ? ((int64_t *)tv)[2] : ((int64_t *)tv)[1];   /* timeval[2] / utimbuf */
        }
        struct dunc_stat st;
        if (vfs_stat(full, &st) != FS_OK) return E(ENOENT_);
        int r = vfs_set_mtime(full, when);
        return r == FS_OK ? 0 : E(EACCES_);
    }
    case 82: case 264: case 316: {                             /* rename, renameat(2) */
        char p[200], q[200], f1[200], f2[200];
        if (!ustr(p, n == 82 ? a : b, sizeof p) || !ustr(q, n == 82 ? b : d, sizeof q)) return E(EFAULT_);
        path_join(lx->cwd, p, f1, sizeof f1); path_join(lx->cwd, q, f2, sizeof f2);
        pc_file_written(f1); pc_file_written(f2);
        if (n == 316 && (e & 1)) {                             /* RENAME_NOREPLACE */
            struct dunc_stat st; if (vfs_stat(f2, &st) == FS_OK) return E(EEXIST_);
        }
        int r = vfs_rename(f1, f2);
        if (r == FS_EEXIST && strcmp(f1, f2)) {                /* Linux replaces a file that's there (how programs save) */
            struct dunc_stat s1, s2;
            if (vfs_stat(f1, &s1) != FS_OK) return E(ENOENT_);
            if (vfs_stat(f2, &s2) == FS_OK && s2.is_dir) return s1.is_dir ? E(ENOTEMPTY_) : E(EISDIR_);
            if (s1.is_dir) return E(ENOTDIR_);
            if (vfs_delete(f2) != FS_OK) return E(EACCES_);
            r = vfs_rename(f1, f2);
        }
        if (r == FS_OK || (r == FS_EEXIST && !strcmp(f1, f2))) return 0;
        static int said; if (said++ < 10) kprintf("[linux] rename %s -> %s: %d\n", f1, f2, r);
        return r == FS_ENOENT ? E(ENOENT_) : r == FS_ENOTDIR ? E(ENOTDIR_) : r == FS_ENOSPC ? E(ENOSPC_) : r == FS_EINVAL ? E(EINVAL_) : E(EACCES_);
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
        if (!(fl & 0x20) && fd >= 0 && fd < NFD && lx->fd[fd].kind == FD_DEV) {
            if (lx->fd[fd].dev != DEV_ZERO) return E(ENODEV_);
            fl |= 0x20;                                        /* /dev/zero: fresh zeroed memory */
        }
        if (dfl) { shm_unmapped(lx, a & ~4095ULL, (a + b + 4095) & ~4095ULL); fmap_cut(lx, a & ~4095ULL, (a + b + 4095) & ~4095ULL); }   /* (whatever was there goes) */
        if (!(fl & 0x20) && fd >= 0 && fd < NFD && lx->fd[fd].kind == FD_WIN && !((struct axwin *)lx->fd[fd].obj)->pix) return E(ENODEV_);   /* (no window yet) */
        if (!(fl & 0x20) && fd >= 0 && fd < NFD && lx->fd[fd].kind == FD_SND && !snd_mmap_obj(lx->fd[fd].obj, off)) return E(ENODEV_);   /* (status/control pages: SYNC_PTR instead) */
        if (!(fl & 0x20) && fd >= 0 && fd < NFD && (lx->fd[fd].kind == FD_SHM || lx->fd[fd].kind == FD_WIN || lx->fd[fd].kind == FD_SND)) {   /* shared memory, a window's pixels, a sound ring */
            struct shm *o = lx->fd[fd].kind == FD_WIN ? ((struct axwin *)lx->fd[fd].obj)->pix : lx->fd[fd].kind == FD_SND ? snd_mmap_obj(lx->fd[fd].obj, off) : lx->fd[fd].obj;
            if (lx->fd[fd].kind == FD_SND) off = 0;
            if (off & 4095) return E(EINVAL_);
            if ((lx->fd[fd].flags & 3) == 1) return E(EACCES_);   /* opened only for writing */
            if ((fl & 3) != 2 && (prot & DPROT_WRITE) && (lx->fd[fd].flags & 3) == 0) return E(EACCES_);   /* shared and writable, from a read-only file */
            if ((fl & 3) != 2 && (prot & DPROT_WRITE) && (o->seals & (SEAL_WRITE_ | SEAL_FUTURE_WRITE_))) return E(EPERM_);
            if ((fl & 3) == 2) {                               /* MAP_PRIVATE: a copy */
                long r = mm_mmap(t->mm, a, b, DPROT_READ | DPROT_WRITE, dfl, &t->proc->pages);
                if (r < 0) return E(ENOMEM_);
                if (off < o->size) { uint64_t k = o->size - off < b ? o->size - off : b; if (uok(r, k, true)) shm_rw(o, off, (uint8_t *)r, k, false); }
                if (prot != (DPROT_READ | DPROT_WRITE)) mm_mprotect(t->mm, r, b, prot);
                return r;
            }
            if (lx->nmaps == lx->capmaps) {                    /* room to remember it */
                int nc = lx->capmaps ? lx->capmaps * 2 : 64;
                struct shmmap *nm = kmalloc(nc * sizeof *nm);
                if (!nm) return E(ENOMEM_);
                if (lx->maps) memcpy(nm, lx->maps, lx->nmaps * sizeof *nm);
                kfree(lx->maps); lx->maps = nm; lx->capmaps = nc;
            }
            long r = mm_mmap(t->mm, a, b, prot, dfl, &t->proc->pages);
            if (r < 0) return E(ENOMEM_);
            uint64_t len = (b + 4095) & ~4095ULL;
            shm_map_pages(t->mm, o, (uint64_t)r, len, off / 4096, prot);
            lx->maps[lx->nmaps++] = (struct shmmap){ (uint64_t)r, (uint64_t)r + len, o, off / 4096 };
            o->refs++;
            return (uint64_t)r;
        }
        if (!(fl & 0x20)) {                                    /* a file: its pages are read when first touched */
            if (fd < 0 || fd >= NFD || lx->fd[fd].kind != FD_FILE) return E(EBADF_);
            if (off & 4095) return E(EINVAL_);
            struct dunc_stat fst;
            if (vfs_stat(lx->fd[fd].path, &fst) == 0 && !fst.is_dir) {
                long r = mm_mmap(t->mm, a, b, prot ? prot : DPROT_READ, dfl, &t->proc->pages);
                if (r < 0) return E(ENOMEM_);
                if (!prot) mm_mprotect(t->mm, r, b, 0);
                uint64_t end = ((uint64_t)r + b + 4095) & ~4095ULL;
                uint64_t data = fst.size > off ? fst.size - off : 0;
                fmap_add_lazy(lx, (uint64_t)r, end, off, lx->fd[fd].path, (uint64_t)r + (data < b ? data : b));
                return r;
            }
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
            fmap_add(lx, (uint64_t)r, ((uint64_t)r + b + 4095) & ~4095ULL, off, lx->fd[fd].path);
            return r;
        }
        long r = mm_mmap(t->mm, a, b, prot, dfl, &t->proc->pages);
        return r < 0 ? (uint64_t)r : (uint64_t)r;
    }
    case 11: {                                                 /* munmap */
        long r = mm_munmap(t->mm, a, b, &t->proc->pages);
        if (r >= 0) { shm_unmapped(lx, a, (a + b + 4095) & ~4095ULL); fmap_cut(lx, a, (a + b + 4095) & ~4095ULL); }
        return r < 0 ? (uint64_t)r : 0;
    }
    case 10: { long r = mm_mprotect(t->mm, a, b, (int)c & 7); return r < 0 ? (uint64_t)r : 0; }       /* mprotect */
    case 28:                                                   /* madvise: DONTNEED frees the pages (they come back zeroed) */
        if (c == 4) {
            mm_dontneed(t->mm, a, b, &t->proc->pages);
            for (int i = 0; i < lx->nmaps; i++) {               /* shared memory keeps its contents: put its pages back */
                struct shmmap *m = &lx->maps[i];
                uint64_t s = a > m->s ? a : m->s, e = a + b < m->e ? a + b : m->e;
                s &= ~4095ULL;
                if (s >= e) continue;
                shm_map_pages(t->mm, m->obj, s, (e - s + 4095) & ~4095ULL, m->pgoff + (s - m->s) / 4096, mm_prot_at(t->mm, s));
            }
        }
        return 0;
    case 25: return E(ENOMEM_);                                /* mremap: callers fall back */
    case 26:                                                   /* msync: shared mappings are the memfd's own pages,
                                                                  and file mappings aren't written back: nothing to do */
        if (a & 4095) return E(EINVAL_);
        return 0;
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
    case 57: return lx_clone_process(f, 17, 0, 0, 0);         /* fork */
    case 58: return lx_clone_process(f, 0x4111, 0, 0, 0);     /* vfork: CLONE_VM | CLONE_VFORK | SIGCHLD */
    case 59: return lx_execve(f, (const char *)a, b, c);       /* execve */
    case 322: {                                                /* execveat: a path relative to AT_FDCWD (or absolute) */
        if ((int)a != -100 && (!uok(b, 1, false) || *(char *)b != '/')) return E(ENOSYS_);
        return lx_execve(f, (const char *)b, c, d);
    }
    case 61: {                                                 /* wait4(pid, status, options, rusage) */
        int st = 0, wp = 0;
        int64_t r = lx_wait((int)a, (c & 1) != 0, &st, &wp);
        if (r <= 0) return (uint64_t)r;
        if (b && uok(b, 4, true)) *(int32_t *)b = st;
        if (d && uok(d, 144, true)) memset((void *)d, 0, 144);
        return (uint64_t)wp;
    }
    case 247: {                                                /* waitid(idtype, id, siginfo, options, rusage) */
        int st = 0, wp = 0;
        int64_t r = lx_wait(a == 1 ? (int)b : -1, (d & 1) != 0, &st, &wp);
        if (r < 0) return (uint64_t)r;
        if (c && uok(c, 128, true)) {
            memset((void *)c, 0, 128);
            if (r > 0) {
                int32_t *si = (int32_t *)c; si[0] = 17; si[2] = (st & 0x7f) ? 2 : 1;   /* SIGCHLD; CLD_KILLED / CLD_EXITED */
                si[4] = wp; si[5] = 1000; si[6] = (st & 0x7f) ? (st & 0x7f) : (st >> 8) & 0xff;
            }
        }
        return 0;
    }
    case 56: {                                                 /* clone: threads, or a new process */
        uint64_t flags = a;
        if (!(flags & 0x10000) && !((flags & 0x100) && !(flags & 0x4000) && (flags & 0x800)))   /* not CLONE_THREAD (or CLONE_VM|SIGHAND): a process */
            return lx_clone_process(f, flags, b, c, d);
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
    case 110: return lx->ppid ? (uint64_t)lx->ppid : 1;       /* getppid */
    case 109: return 0;                                        /* setpgid (one session) */
    case 121: case 111: return (uint64_t)t->proc->pid;        /* getpgid, getpgrp */
    case 102: case 104: case 107: case 108: return 1000;       /* getuid, getgid, geteuid, getegid */
    case 118: case 120:                                        /* getresuid, getresgid */
        if (!uok(a, 4, true) || !uok(b, 4, true) || !uok(c, 4, true)) return E(EFAULT_);
        *(uint32_t *)a = *(uint32_t *)b = *(uint32_t *)c = 1000; return 0;
    case 112: return t->proc->pid;                             /* setsid */
    case 62: case 200: case 234: {                             /* kill, tkill, tgkill */
        int sig = (int)(n == 62 || n == 200 ? b : c);
        if (sig < 0 || sig > 64) return E(EINVAL_);
        struct task *to = 0;
        if (n == 62 && (int)a > 0 && (int)a != t->proc->pid) {   /* another program (a child, say) */
            to = proc_task((int)a);
            if (!to) return E(ESRCH_);
        } else if (n == 62) {                                  /* this one (0, -1: its group, which is just it) */
            for (int i = 0; i < MAX_TASKS; i++)                /* the first of its threads that doesn't block it */
                if (tasks[i].state && tasks[i].state != PS_ZOMBIE && tasks[i].lx == lx && (!to || !(tasks[i].sig_blocked & SIGBIT(sig ? sig : 1)))) {
                    if (!to || (to->sig_blocked & SIGBIT(sig ? sig : 1))) to = &tasks[i];
                }
        } else {
            int tid = (int)(n == 200 ? a : b);
            for (int i = 0; i < MAX_TASKS; i++) if (tasks[i].state && tasks[i].state != PS_ZOMBIE && tasks[i].pid == tid && tasks[i].lx == lx) to = &tasks[i];
        }
        if (!to) return E(ESRCH_);
        if (!sig) return 0;                                    /* (only asking whether it's there) */
        if ((sig == 9 || ((sig == 6 || sig == 15) && !lx->sa[sig].handler)) && to == t) { if (sig == 6) lx_abort_trace(lx, f); t->proc->exit_signal = sig; task_exit(128 + sig); }
        if (sig == 9 || (to->lx && !to->lx->sa[sig].handler && !sig_default_ignored(sig) && to->lx != lx)) {   /* another program, ending */
            if (to->proc) to->proc->exit_signal = sig;
            task_kill(to->proc ? to->proc->pid : to->pid); return 0;
        }
        sig_post(to, sig, t->proc->pid);
        return 0;
    }
    case 13: {                                                 /* rt_sigaction */
        int sig = (int)a;
        if (sig < 1 || sig > 64 || d != 8) return E(EINVAL_);
        if (c) {
            if (!uok(c, 32, true)) return E(EFAULT_);
            uint64_t *o = (uint64_t *)c; o[0] = lx->sa[sig].handler; o[1] = lx->sa[sig].flags; o[2] = lx->sa[sig].restorer; o[3] = lx->sa[sig].mask;
        }
        if (b) {
            if (sig == 9 || sig == 19) return E(EINVAL_);
            if (!uok(b, 32, false)) return E(EFAULT_);
            uint64_t *v = (uint64_t *)b;
            if (v[0] > 1 && !(v[1] & SA_RESTORER_)) return E(EINVAL_);   /* (glibc always gives a restorer) */
            lx->sa[sig].handler = v[0]; lx->sa[sig].flags = v[1]; lx->sa[sig].restorer = v[2]; lx->sa[sig].mask = v[3];
            if (v[0] == 1) for (int i = 0; i < MAX_TASKS; i++) if (tasks[i].lx == lx) tasks[i].sig_pending &= ~SIGBIT(sig);
        }
        return 0;
    }
    case 14: {                                                 /* rt_sigprocmask */
        if (d != 8) return E(EINVAL_);
        if (c) { if (!uok(c, 8, true)) return E(EFAULT_); *(uint64_t *)c = t->sig_blocked; }
        if (b) {
            if (!uok(b, 8, false)) return E(EFAULT_);
            uint64_t m = *(uint64_t *)b;
            if (a == 0) t->sig_blocked |= m; else if (a == 1) t->sig_blocked &= ~m; else if (a == 2) t->sig_blocked = m; else return E(EINVAL_);
            t->sig_blocked &= ~SIG_UNBLOCKABLE;
        }
        return 0;
    }
    case 127:                                                  /* rt_sigpending */
        if (!uok(a, 8, true)) return E(EFAULT_);
        *(uint64_t *)a = t->sig_pending & t->sig_blocked; return 0;
    case 130: {                                                /* rt_sigsuspend: wait for a signal with this mask */
        if (b != 8 || !uok(a, 8, false)) return E(EINVAL_);
        t->sig_saved = t->sig_blocked; t->sig_restore = true;
        t->sig_blocked = *(uint64_t *)a & ~SIG_UNBLOCKABLE;
        while (!sig_now()) chan_wait(-1);
        return E(EINTR_);                                      /* (its handler runs on the way back; then the old mask) */
    }
    case 128: {                                                /* rt_sigtimedwait: take one of these signals, no handler */
        if (d != 8 || !uok(a, 8, false)) return E(EINVAL_);
        uint64_t set = *(uint64_t *)a; bool forever; uint64_t ms = ts_ms(c, &forever);
        uint64_t end = uptime_ms() + ms;
        for (;;) {
            uint64_t got = t->sig_pending & set;
            if (got) {
                int sn = __builtin_ctzll(got) + 1; t->sig_pending &= ~SIGBIT(sn);
                if (b && uok(b, 128, true)) { memset((void *)b, 0, 128); *(int32_t *)b = sn; *(int32_t *)(b + 8) = -6; *(int32_t *)(b + 16) = t->sig_from; }
                return (uint64_t)sn;
            }
            if (sig_now()) return E(EINTR_);
            if (!forever && uptime_ms() >= end) return E(EAGAIN_);
            chan_wait(forever ? -1 : (int64_t)(end - uptime_ms()));
        }
    }
    case 131: {                                                /* sigaltstack: handlers run on the thread's own stack */
        if (b && uok(b, 24, true)) { memset((void *)b, 0, 24); *(int32_t *)(b + 8) = 2; }   /* SS_DISABLE */
        return 0;
    }
    case 15: return sig_return(f);                             /* rt_sigreturn */
    case 24: schedule(); return 0;                             /* sched_yield */
    case 204: {                                                /* sched_getaffinity */
        if (b < 8 || !uok(c, 8, true)) return E(EINVAL_);
        uint64_t mask = ncpus_on >= 64 ? ~0ULL : (1ULL << ncpus_on) - 1;
        memset((void *)c, 0, b < 128 ? b : 128); *(uint64_t *)c = mask; return 8;
    }
    case 203: return 0;                                        /* sched_setaffinity */
    case 302: {                                                /* prlimit64 */
        if (d && !uok(d, 16, true)) return E(EFAULT_);          /* (Chromium checks this, with a read-only page) */
        if (c && !uok(c, 16, false)) return E(EFAULT_);
        if (d) {
            uint64_t cur = ~0ULL, max = ~0ULL;
            if (b == 3) cur = STACK_SIZE;                      /* RLIMIT_STACK */
            if (b == 7) cur = max = NFD;                       /* RLIMIT_NOFILE */
            ((uint64_t *)d)[0] = cur; ((uint64_t *)d)[1] = max;
        }
        return 0;
    }
    case 97: if (!uok(b, 16, true)) return E(EFAULT_); { ((uint64_t *)b)[0] = a == 3 ? STACK_SIZE : a == 7 ? NFD : ~0ULL; ((uint64_t *)b)[1] = a == 7 ? NFD : ~0ULL; } return 0;   /* getrlimit */
    case 98: {                                                 /* getrusage: CPU time (its threads, or this one); the rest 0 */
        if (!uok(b, 144, true)) return E(EFAULT_);
        memset((void *)b, 0, 144);
        uint64_t us = 0;
        if ((int)a == 1) us = t->ticks;                        /* RUSAGE_THREAD */
        else for (int i = 0; i < MAX_TASKS; i++) if (tasks[i].state && tasks[i].lx == lx) us += tasks[i].ticks;
        ((int64_t *)b)[0] = (int64_t)(us / 1000000); ((int64_t *)b)[1] = (int64_t)(us % 1000000);   /* ru_utime */
        ((int64_t *)b)[4] = (int64_t)(t->proc ? t->proc->pages : t->pages) * 4;                       /* ru_maxrss (KiB) */
        return 0;
    }
    case 85: return do_open(lx, -100, a, 01 | 0100 | 01000);   /* creat: O_WRONLY | O_CREAT | O_TRUNC */
    case 27: {                                                 /* mincore: say every page is in memory (AXIOM doesn't page out) */
        if (a & 4095) return E(EINVAL_);
        uint64_t np = (b + 4095) / 4096;
        if (!uok(c, np, true)) return E(EFAULT_);
        memset((void *)c, 1, np);
        return 0;
    }
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
    case 90: case 91: case 268: case 92: case 93: case 94: case 260:   /* chmod, fchmod, fchmodat, chown, fchown, lchown, fchownat: */
        return 0;                                              /* AXIOM's files have no owners or modes (everything is the user's) */
    case 95: return 022;                                       /* umask: the usual one */
    case 143:                                                  /* sched_getparam: priority 0 (SCHED_OTHER) */
        if (!uok(b, 4, true)) return E(EFAULT_);
        *(int32_t *)b = 0; return 0;
    case 145: return 0;                                        /* sched_getscheduler: SCHED_OTHER */
    case 144: return E(EPERM_);                                /* sched_setscheduler: realtime needs privileges (as for a user on Linux) */
    case 334: case 324: case 273: case 332: return E(ENOSYS_); /* rseq, membarrier, set_robust_list, statx */
    case 444: return E(EOPNOTSUPP_);                           /* landlock_create_ruleset: no Landlock (as a kernel without it) */
    case 157: return 0;                                        /* prctl */
    case 140: return 20;                                       /* getpriority (as 20 - nice: nice 0) */
    case 141: return 0;                                        /* setpriority: everything runs at one priority */

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
        if (sig_now()) return E(EINTR_);
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
            if (r == -DERR_TIMEDOUT && sig_now()) return E(EINTR_);
            return r == -DERR_AGAIN ? E(EAGAIN_) : r == -DERR_TIMEDOUT ? E(ETIMEDOUT_) : r == -DERR_FAULT ? E(EFAULT_) : 0;
        }
        if (op == 1 || op == 10) return futex_wake(t->mm, a, (int)c);   /* WAKE, WAKE_BITSET */
        if (op == 5) {                                         /* WAKE_OP (Qt's semaphores, glibc's older condition variables) */
            uint64_t a2 = e; uint32_t v3 = (uint32_t)f->r9;
            if (!uok(a, 4, false) || !uok(a2, 4, true) || (a & 3) || (a2 & 3)) return E(EFAULT_);
            int sop = (int)(v3 >> 28) & 7, cmp = (int)(v3 >> 24) & 15;
            int32_t oparg = (int32_t)(v3 << 8) >> 20, cmparg = (int32_t)(v3 << 20) >> 20;   /* (12-bit, signed) */
            if (v3 >> 31) oparg = oparg >= 0 && oparg < 32 ? (int32_t)(1u << oparg) : 0;    /* FUTEX_OP_OPARG_SHIFT */
            if (sop > 4 || cmp > 5) return E(ENOSYS_);
            uint32_t *w = (uint32_t *)a2, old = __atomic_load_n(w, __ATOMIC_SEQ_CST), nv;
            do {                                               /* *uaddr2 = old OP oparg, atomically (other cores run user code) */
                nv = sop == 0 ? (uint32_t)oparg : sop == 1 ? old + (uint32_t)oparg : sop == 2 ? old | (uint32_t)oparg :
                     sop == 3 ? old & ~(uint32_t)oparg : old ^ (uint32_t)oparg;
            } while (!__atomic_compare_exchange_n(w, &old, nv, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
            long k = futex_wake(t->mm, a, (int)c);
            int32_t o = (int32_t)old;
            bool hit = cmp == 0 ? o == cmparg : cmp == 1 ? o != cmparg : cmp == 2 ? o < cmparg : cmp == 3 ? o <= cmparg : cmp == 4 ? o > cmparg : o >= cmparg;
            if (hit) k += futex_wake(t->mm, a2, (int)(uint32_t)d);   /* (val2 is in the timeout argument) */
            return (uint64_t)k;
        }
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
