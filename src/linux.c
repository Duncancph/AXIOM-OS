/* Linux compatibility: programs built for Linux (x86-64, static or position-independent) run on AXIOM. They
 * make Linux system calls with the syscall instruction; this turns the ones ordinary programs use into
 * AXIOM's own: memory maps, threads (clone), futexes, time, randomness, files on AXIOM's volumes and
 * output to the terminal that started them. */
#include "kernel.h"
#include "vfs.h"
#include "axiom_window.h"

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
enum { FD_NONE, FD_TTY_IN, FD_TTY_OUT, FD_FILE, FD_DIR, FD_CHAN, FD_EVENT, FD_EPOLL, FD_SHM, FD_DEV, FD_WIN, FD_SND };
/* sound devices (linux_snd.c): /dev/snd/controlC0 and pcmC0D0p */
void *snd_open(int kind); void snd_put(void *o); long snd_ioctl(void *o, uint32_t cmd, uint64_t arg, bool nonblock);
long snd_write(void *o, uint64_t ubuf, uint64_t bytes, bool nonblock); int snd_ready(void *o, bool *waiting);
void *snd_mmap_obj(void *o, uint64_t off);
static bool snd_waiting;                                      /* a poll found a sound device not ready: look again soon */
struct chan;
/* FD_CHAN: a pipe end or a socket-pair end. It reads from rx and writes to tx (either may be 0: a pipe's
 * other end, or after shutdown) and holds a reader count on rx and a writer count on tx. */
struct lfd { int kind; int flags; uint64_t off; char path[200]; struct chan *rx, *tx; bool sock; uint8_t stype;
             uint8_t dev;    /* FD_DEV: which of devs[] */
             void *obj; };   /* FD_EVENT: a struct evfd, FD_EPOLL: a struct epoll, FD_SHM: a struct shm,
                                FD_WIN: a struct axwin (shared by duplicates); FD_CHAN after shutdown: its
                                identity from before (compared only, never used) */
struct shmmap;
#define NFD 128
struct lxproc {
    struct lfd fd[NFD];
    char cwd[200];
    char exe[200];                 /* the program, as Linux paths see it (/proc/self/exe) */
    struct task *out;              /* the terminal task that started it (its output goes there), or 0 */
    int out_pid;
    char line[256]; int linelen;   /* output waiting for its end of line (for the log) */
    struct shmmap *maps; int nmaps, capmaps;   /* where shared memory (memfd) is mapped */
    struct fmap *fmaps; int nfmaps, capfmaps;  /* which parts of memory came from which file (/proc/self/maps) */
};
/* a range of a program's memory read from a file (its own segments, its loader's, mmap'd libraries): only
 * for /proc/self/maps, which Chromium (and debuggers) read to find code and print stack traces */
struct fmap { uint64_t s, e, off; char path[120]; };
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
    f->s = s; f->e = e; f->off = off;
    strncpy0(f->path, !strncmp(path, "/system/linux/", 14) ? path + 13 : path, sizeof f->path);   /* (as Linux paths see it) */
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
       EMSGSIZE_ = 90, ENOPROTOOPT_ = 92, EOPNOTSUPP_ = 95, EAFNOSUPPORT_ = 97, ETIMEDOUT_ = 110 };

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

int linux_spawn(const char *path, int argc, char **argv, struct task *out) {
    struct mm *m = mm_new();
    struct lxproc *lx = kmalloc(sizeof *lx);
    if (!m || !lx) { if (m) mm_put(m); kfree(lx); return -1; }
    memset(lx, 0, sizeof *lx);
    uint64_t entry, phdr; int phnum; char interp[128]; bool fixed;
    if (!load_elf(m, lx, path, MAIN_BASE, &entry, &phdr, &phnum, interp, sizeof interp, &fixed)) {
        struct dunc_stat st;
        if (vfs_stat(path, &st) != 0) kprintf("run: can't find %s\n", path);
        else kprintf("run: %s isn't a program AXIOM can run%s\n", path, fixed ? " (built at a fixed address: build it position-independent, e.g. -static-pie)" : "");
        mm_put(m); kfree(lx->fmaps); kfree(lx); return -1;
    }
    uint64_t start = entry, ibase = 0;
    if (interp[0]) {                                           /* a dynamically linked program: its loader first */
        char ip[200]; path_join("/", interp, ip, sizeof ip);
        uint64_t ie, iph; int ipn;
        if (!load_elf(m, lx, ip, INTERP_BASE, &ie, &iph, &ipn, 0, 0, 0)) { kprintf("run: needs %s, which isn't there\n", interp); mm_put(m); kfree(lx->fmaps); kfree(lx); return -1; }
        start = ie; ibase = INTERP_BASE;
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
    strncpy0(lx->exe, !strncmp(path, "/system/linux/", 14) ? path + 13 : path, sizeof lx->exe);
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
static void fd_close(struct lfd *f);
static void shm_forget_all(struct lxproc *lx);
void linux_task_exit(struct task *t) {
    struct lxproc *lx = t->lx;
    if (!lx) return;
    if (lx->linelen) { lx->line[lx->linelen] = 0; kprintf("[%s] %s\n", t->name, lx->line); }
    for (int i = 0; i < NFD; i++) if (lx->fd[i].kind) fd_close(&lx->fd[i]);   /* (the other ends see end of file) */
    shm_forget_all(lx);
    kfree(lx->fmaps);
    for (int i = 0; i < MAX_TASKS; i++) if (tasks[i].lx == lx) tasks[i].lx = 0;
    kfree(lx);
}

/* ---------------- the system calls ---------------- */
struct lstat { uint64_t dev, ino, nlink; uint32_t mode, uid, gid, pad; uint64_t rdev; int64_t size, blksize, blocks; uint64_t at[2], mt[2], ct[2]; int64_t r[3]; };
static uint64_t path_ino(const char *p) { uint64_t h = 1469598103934665603ull; for (; p && *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull; return h >> 1 | 1; }
static void fill_stat(struct lstat *st, bool dir, uint64_t size, bool tty, const char *path) {
    memset(st, 0, sizeof *st);
    st->dev = 1; st->ino = path_ino(path);   /* (loaders tell files apart by these) */ st->nlink = 1; st->uid = st->gid = 1000;
    st->mode = tty ? 0020620 : dir ? 0040755 : 0100644;
    st->size = size; st->blksize = 4096; st->blocks = (size + 511) / 512;
    uint64_t now = (uint64_t)(linux_now_ns() / 1000000000ull);
    st->at[0] = st->mt[0] = st->ct[0] = now;
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
struct evfd { int refs; uint64_t count; bool sem; };
struct shm;
static void shm_put(struct shm *o);
#define AXQ 256
struct axwin {
    int refs;                         /* (first: fd_close/fd_copy count duplicates like the others) */
    struct task *owner; int id; bool resizable;
    struct shm *pix;                  /* the window holds one reference; each mapping one more */
    struct axwin_event q[AXQ]; int qh, qt;
    struct axwin *next;
};
static void axwin_put(struct axwin *x);
struct epitem { int fd; uint32_t events; uint64_t data; const void *id; bool off; };
struct epoll { int refs, n, next; struct epitem it[NFD]; };
static void fd_close(struct lfd *f) {
    if (f->kind == FD_CHAN) { chan_drop(f->rx, true); chan_drop(f->tx, false); }
    if ((f->kind == FD_EVENT || f->kind == FD_EPOLL) && f->obj && --*(int *)f->obj <= 0) { kfree(f->obj); chan_wake(); }   /* (refs come first in both) */
    if (f->kind == FD_SHM) shm_put(f->obj);
    if (f->kind == FD_WIN) axwin_put(f->obj);
    if (f->kind == FD_SND) snd_put(f->obj);
    memset(f, 0, sizeof *f);
}
static void fd_copy(struct lfd *dst, const struct lfd *src) {      /* dup: one more reference to the same ends */
    if (dst == src) return;
    fd_close(dst);
    *dst = *src;
    if (dst->kind == FD_CHAN) { if (dst->rx) dst->rx->readers++; if (dst->tx) dst->tx->writers++; }
    if ((dst->kind == FD_EVENT || dst->kind == FD_EPOLL || dst->kind == FD_SHM || dst->kind == FD_WIN || dst->kind == FD_SND) && dst->obj) (*(int *)dst->obj)++;
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
        chan_wait(-1);
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
            chan_wait(-1);
            if (!c->readers) return (int64_t)E(EPIPE_);
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
            chan_wait(-1);
            if (!c->readers) return done ? (int64_t)done : (int64_t)E(EPIPE_);
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
        if (f->rx) { if (f->rx->len || !f->rx->writers) r |= POLLIN_; if (!f->rx->writers) r |= f->sock ? POLLRDHUP_ | (f->tx ? 0 : POLLHUP_) : POLLHUP_; }
        else if (f->sock) r |= POLLIN_ | POLLRDHUP_;              /* shut down for reading: reads give end of file */
        if (f->tx) { if (!f->tx->readers) r |= f->sock ? POLLOUT_ | POLLHUP_ : POLLERR_; else if (f->tx->len < CHAN_CAP && f->tx->mcount < CHAN_MSGS) r |= POLLOUT_; }
        return r;
    }
    case FD_EVENT: { struct evfd *ev = f->obj; return (ev->count ? POLLIN_ : 0) | (ev->count < ~0ULL - 1 ? POLLOUT_ : 0); }
    case FD_EPOLL: return epoll_scan(lx, f->obj, 0, 1) ? POLLIN_ : 0;
    case FD_WIN: { struct axwin *x = f->obj; return (x->qh != x->qt ? POLLIN_ : 0) | POLLOUT_; }
    case FD_SND: return snd_ready(f->obj, &snd_waiting);
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
        if (snd_waiting) { snd_waiting = false; if (left < 0 || left > 5) left = 5; }   /* (sound plays without waking anyone) */
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
    if (f->kind == FD_EVENT || f->kind == FD_EPOLL || f->kind == FD_SHM || f->kind == FD_WIN) return f->obj;
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
        if (snd_waiting) { snd_waiting = false; if (left < 0 || left > 5) left = 5; }
        chan_wait(left);
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
        struct window *w = wm_create(P, c->w, c->h, title, &buf, &pages, 0, (c->flags & AXWIN_RESIZABLE) ? WIN_RESIZABLE : 0);
        if (!w) return E(EINVAL_);                               /* (too big or small for the screen, or too many) */
        x->owner = P; x->id = wm_id(w); x->resizable = c->flags & AXWIN_RESIZABLE;
        if (!(x->pix = axwin_pixels(P, x->id))) { wm_close(P, x->id); x->id = 0; return E(ENOMEM_); }
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
    case AXWIN_RESIZE: {
        if (!x->id) return E(ENOENT_);
        if (!x->resizable) return E(EINVAL_);
        if (x->pix->refs > 1) return E(EBUSY_);                 /* still mapped: munmap first (its pages are about to go) */
        int nw, nh;
        if (!wm_resize(x->owner, x->id, &nw, &nh)) return E(ENOMEM_);
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

static uint64_t do_open(struct lxproc *lx, int dirfd, uint64_t upath, int flags) {
    char p[200], full[200];
    if (!ustr(p, upath, sizeof p)) return E(EFAULT_);
    (void)dirfd;
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
    path_join(lx->cwd, p, full, sizeof full);
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
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (!t->state || !t->lx || !t->user || !t->kstack) continue;
        struct frame *f = (struct frame *)(t->kstack_top - sizeof *f);
        kprintf("[dump] pid %d '%s' state %d: %s %lu (%lx, %lx, %lx) futex %lx rip %lx\n", t->pid, t->name, t->state,
                f->vec == 0x81 ? "system call" : "interrupt", f->vec == 0x81 ? f->rax : f->vec, f->rdi, f->rsi, f->rdx,
                t->futex_addr, f->rip);
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
static uint64_t linux_syscall_(struct frame *f);
/* Linux reads int arguments (file numbers, flags) from the low 32 bits of their registers; programs that make
 * system calls themselves may leave anything in the upper half, so those are cut off here */
uint64_t linux_syscall(struct frame *f) {
    switch (f->rax) {
    case 0: case 1: case 3: case 5: case 8: case 16: case 17: case 18: case 19: case 20: case 32: case 33: case 72:
    case 77: case 138: case 217: case 285: case 292: case 44: case 45: case 46: case 47: case 48: case 232: case 281: case 54: case 55:
    case 441: case 74: case 75: case 81:
        f->rdi = (uint32_t)f->rdi; break;                        /* the first argument is a file */
    case 233: f->rdi = (uint32_t)f->rdi; f->rdx = (uint32_t)f->rdx; break;   /* epoll_ctl: two */
    case 9: f->r8 = (uint64_t)(int64_t)(int32_t)f->r8; f->rdx = (uint32_t)f->rdx; f->r10 = (uint32_t)f->r10; break;   /* mmap: fd (-1 for none), prot, flags */
    case 257: case 262: case 269: case 267: case 258: case 263: case 264: case 316: case 265:
        f->rdi = (uint64_t)(int64_t)(int32_t)f->rdi; break;      /* *at: a directory file or AT_FDCWD (-100) */
    }
    chan_woke = false;
    uint64_t r = linux_syscall_(f);
    if (chan_woke) { chan_woke = false; task_yield(); }
    return r;
}
static uint64_t linux_syscall_(struct frame *f) {
    uint64_t n = f->rax, a = f->rdi, b = f->rsi, c = f->rdx, d = f->r10, e = f->r8;
    struct task *t = current;
    struct lxproc *lx = t->lx;
    if (!lx) return E(ENOSYS_);
    switch (n) {
    case 0: case 17: {                                         /* read, pread64 */
        if (a >= NFD || !lx->fd[a].kind) return E(EBADF_);
        struct lfd *fd = &lx->fd[a];
        if (fd->kind == FD_TTY_IN) return 0;                     /* (no input yet: end of file) */
        if (fd->kind == FD_WIN) {                               /* window events: whole records */
            struct axwin *x = fd->obj;
            if (n == 17) return E(ESPIPE_);
            if (c < sizeof(struct axwin_event)) return E(EINVAL_);
            while (x->qh == x->qt) { if (fd->flags & 04000) return E(EAGAIN_); chan_wait(-1); }
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
        if (fd->kind == FD_EVENT) {
            struct evfd *ev = fd->obj;
            if (n == 17) return E(ESPIPE_);
            if (c < 8) return E(EINVAL_);
            while (!ev->count) { if (fd->flags & 04000) return E(EAGAIN_); chan_wait(-1); }
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
        if (fd->kind == FD_DEV) return fd->dev == DEV_FULL ? E(ENOSPC_) : c;   /* (written to random: mixed in on Linux; here, dropped) */
        if (fd->kind == FD_EVENT) {
            struct evfd *ev = fd->obj;
            if (n == 18) return E(ESPIPE_);
            if (c < 8) return E(EINVAL_);
            uint64_t v = *(uint64_t *)b;
            if (v == ~0ULL) return E(EINVAL_);
            while (ev->count + v < ev->count || ev->count + v == ~0ULL) { if (fd->flags & 04000) return E(EAGAIN_); chan_wait(-1); }
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
        path_join(lx->cwd, p, full, sizeof full);
        if (dev_of(full)) { dev_stat((struct lstat *)us, dev_of(full)); return 0; }
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
        path_join(lx->cwd, p, full, sizeof full);
        struct dunc_stat st;
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
        int ne = vfs_list_full(fd->path, ents, MAXDENTS);
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
        if (a < NFD && lx->fd[a].kind == FD_WIN) return axwin_ioctl(lx, &lx->fd[a], (uint32_t)b, c);
        if (a < NFD && lx->fd[a].kind == FD_SND) { long r = snd_ioctl(lx->fd[a].obj, (uint32_t)b, c, lx->fd[a].flags & 04000); return r < 0 ? E(-r) : (uint64_t)r; }
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
        default: return 0;                                     /* F_GETFD / F_SETFD (no exec, so close-on-exec doesn't matter), locks */
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
        case 17:                                               /* SO_PEERCRED: this program (both ends are its own) */
            if (*(uint32_t *)e < 12) return E(EINVAL_);
            ((uint32_t *)d)[0] = (uint32_t)t->proc->pid; ((uint32_t *)d)[1] = 1000; ((uint32_t *)d)[2] = 1000;
            *(uint32_t *)e = 12; return 0;
        default: return E(ENOPROTOOPT_);
        }
        *(uint32_t *)d = v; *(uint32_t *)e = 4;
        return 0;
    }
    case 48: {                                                 /* shutdown */
        if (a >= NFD || !lx->fd[a].kind) return E(EBADF_);
        struct lfd *fd = &lx->fd[a];
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
    case 74: case 75:                                          /* fsync, fdatasync: writes go straight to the volume */
        return a < NFD && lx->fd[a].kind ? 0 : E(EBADF_);
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
        if (strcmp(p, "/proc/self/exe")) return E(EINVAL_);
        uint64_t l = strlen(lx->exe); if (l > ul) l = ul;
        if (!uok(ub, l, true)) return E(EFAULT_);
        memcpy((void *)ub, lx->exe, l); return l;
    }
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
    case 118: case 120:                                        /* getresuid, getresgid */
        if (!uok(a, 4, true) || !uok(b, 4, true) || !uok(c, 4, true)) return E(EFAULT_);
        *(uint32_t *)a = *(uint32_t *)b = *(uint32_t *)c = 1000; return 0;
    case 112: return t->proc->pid;                             /* setsid */
    case 62: case 200: case 234:                               /* kill, tkill, tgkill: ending itself */
        if (n == 62 && a == (uint64_t)t->proc->pid && (b == 6 || b == 9 || b == 15)) { if (b == 6) lx_abort_trace(lx, f); task_exit(128 + (int)b); }
        if (n != 62 && (c == 6 || c == 9)) { if (c == 6) lx_abort_trace(lx, f); task_exit(128 + (int)c); }
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
    case 97: if (!uok(b, 16, true)) return E(EFAULT_); { ((uint64_t *)b)[0] = a == 3 ? STACK_SIZE : ~0ULL; ((uint64_t *)b)[1] = ~0ULL; } return 0;   /* getrlimit */
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
