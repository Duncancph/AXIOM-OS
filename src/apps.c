/* App registry (built-in apps + apps installed with clan) and the clan package manager.
 * Packages live in /system/repo (index.txt + <id>.elf); installed apps in /apps. */
#include "kernel.h"
#include "vfs.h"

#define MAX_APPS 24
static struct app_entry apps[MAX_APPS];
static int napps, nhidden;

static const char *builtin_icon(const char *id) {
    static const char *map[][2] = { {"files", "folder"}, {"terminal", "terminal"}, {"monitor", "monitor"}, {"bluetooth", "bluetooth"},
                                    {"calc", "calc"}, {"sandbox", "shield"}, {"about", "crown"}, {"network", "network"}, {"settings", "gear"}, {"viewer", "photo"}, {"web", "arc"}, {"notes", "note"}, {"calendar", "calendar"}, {"music", "music"}, {"video", "video"} };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) if (!strcmp(map[i][0], id)) return map[i][1];
    return "gear";
}

/* "id|Name|version|icon|description" */
static int split(char *line, char **f, int max) {
    int n = 0;
    f[n++] = line;
    for (char *p = line; *p && n < max; p++) if (*p == '|') { *p = 0; f[n++] = p + 1; }
    for (char *p = f[n - 1]; *p; p++) if (*p == '\n' || *p == '\r') *p = 0;
    return n;
}

void apps_refresh(void) {
    napps = 0;
    for (int i = 0; i < app_count && napps < MAX_APPS; i++) {
        struct app_entry *a = &apps[napps++];
        memset(a, 0, sizeof *a);
        strncpy0(a->id, app_images[i].id, sizeof a->id);
        strncpy0(a->name, app_images[i].title, sizeof a->name);
        strncpy0(a->icon, builtin_icon(a->id), sizeof a->icon);
        strncpy0(a->version, OS_VERSION, sizeof a->version);
        a->builtin = &app_images[i];
    }
    struct dunc_dirent ents[32];
    int n = vfs_list("/apps", ents, 32);
    for (int i = 0; i < n && napps < MAX_APPS; i++) {
        int l = strlen(ents[i].name);
        if (l < 6 || strcmp(ents[i].name + l - 5, ".info")) continue;
        char path[96], line[256]; ksnprintf(path, sizeof path, "/apps/%s", ents[i].name);
        long r = vfs_read(path, line, 0, sizeof line - 1);
        if (r <= 0) continue;
        line[r] = 0;
        char *f[5];
        if (split(line, f, 5) < 4) continue;
        struct app_entry *a = &apps[napps++];
        memset(a, 0, sizeof *a);
        strncpy0(a->id, f[0], sizeof a->id); strncpy0(a->name, f[1], sizeof a->name);
        strncpy0(a->version, f[2], sizeof a->version); strncpy0(a->icon, f[3], sizeof a->icon);
        a->installed = true;
    }
    nhidden = 0;                                              /* apps not shown in the dock or menu go last */
    for (int i = 0; i < napps - nhidden; i++)
        if (!strcmp(apps[i].id, "sandbox")) {
            struct app_entry t = apps[i];
            for (int k = i; k + 1 < napps; k++) apps[k] = apps[k + 1];
            apps[napps - 1] = t; nhidden++; i--;
        }
}

int apps_count(void) { return napps; }
int apps_visible(void) { return napps - nhidden; }
const struct app_entry *apps_get(int i) { return i >= 0 && i < napps ? &apps[i] : 0; }
const struct app_entry *apps_find(const char *id) {
    if (id && !strcmp(id, "arc")) id = "web";             /* Arc is the Web app's name now */
    for (int i = 0; i < napps; i++) if (!strcmp(apps[i].id, id)) return &apps[i];
    return 0;
}

/* ---------------- clan ---------------- */
struct pkg { char id[24], name[32], version[12], icon[16], desc[96]; };

static int read_index(struct pkg *out, int max) {
    uint64_t len;
    char *idx = vfs_read_all("/system/repo/index.txt", &len);
    if (!idx) return 0;
    char *buf = kmalloc(len + 1);
    memcpy(buf, idx, len); buf[len] = 0; kfree(idx);
    int n = 0;
    for (char *line = buf; *line && n < max;) {
        char *nl = line; while (*nl && *nl != '\n') nl++;
        char save = *nl; *nl = 0;
        if (*line && *line != '#') {
            char *f[5];
            if (split(line, f, 5) == 5) {
                strncpy0(out[n].id, f[0], 24); strncpy0(out[n].name, f[1], 32); strncpy0(out[n].version, f[2], 12);
                strncpy0(out[n].icon, f[3], 16); strncpy0(out[n].desc, f[4], 96); n++;
            }
        }
        if (!save) break;
        line = nl + 1;
    }
    kfree(buf);
    return n;
}

static void install(struct pkg *p) {
    char src[96], dst[96], info[256];
    ksnprintf(src, sizeof src, "/system/repo/%s.elf", p->id);
    ksnprintf(dst, sizeof dst, "/apps/%s.elf", p->id);
    kprintf("resolving... ");
    uint64_t len; void *elf = vfs_read_all(src, &len);
    if (!elf) { kprintf("failed - package file missing\n"); return; }
    long r = vfs_write(dst, elf, len);
    kfree(elf);
    if (r < 0) { kprintf("failed - %s\n", fs_error((int)r)); return; }
    ksnprintf(info, sizeof info, "%s|%s|%s|%s|%s\n", p->id, p->name, p->version, p->icon, p->desc);
    ksnprintf(dst, sizeof dst, "/apps/%s.info", p->id);
    vfs_write(dst, info, strlen(info));
    apps_refresh();
    kprintf("done - installed %s %s\n", p->id, p->version);
}

void clan_command(int argc, char **argv) {
    struct pkg pk[16];
    int n = read_index(pk, 16);
    const char *sub = argc > 1 ? argv[1] : "help";
    if (!strcmp(sub, "list") || !strcmp(sub, "search")) {
        const char *term = argc > 2 ? argv[2] : "";
        kprintf("Packages in the AXIOM repository:\n");
        for (int i = 0; i < n; i++) {
            bool match = !*term;
            for (const char *s = pk[i].id; *s && !match; s++) if (!strncmp(s, term, strlen(term))) match = true;
            for (const char *s = pk[i].desc; *s && !match; s++) if (!strncmp(s, term, strlen(term))) match = true;
            if (!match) continue;
            const struct app_entry *a = apps_find(pk[i].id);
            kprintf("  %-10s %-6s %s%s\n", pk[i].id, pk[i].version, pk[i].desc, a && a->installed ? "  [installed]" : "");
        }
    } else if (!strcmp(sub, "install") || !strcmp(sub, "remove") || !strcmp(sub, "info")) {
        if (argc < 3) { kprintf("usage: clan %s <package>\n", sub); return; }
        struct pkg *p = 0;
        for (int i = 0; i < n; i++) if (!strcmp(pk[i].id, argv[2])) p = &pk[i];
        const struct app_entry *a = apps_find(argv[2]);
        if (!strcmp(sub, "info")) {
            if (!p) { kprintf("clan: no package called '%s'\n", argv[2]); return; }
            kprintf("%s %s - %s\n%s\n", p->name, p->version, a && a->installed ? "installed" : "not installed", p->desc);
        } else if (!strcmp(sub, "install")) {
            if (!p) { kprintf("clan: no package called '%s'. Try 'clan list'.\n", argv[2]); return; }
            if (a) { kprintf("%s is already installed.\n", p->id); return; }
            install(p);
        } else {
            if (!a || !a->installed) { kprintf("clan: '%s' isn't installed (built-in apps can't be removed).\n", argv[2]); return; }
            char path[96];
            ksnprintf(path, sizeof path, "/apps/%s.elf", argv[2]); vfs_delete(path);
            ksnprintf(path, sizeof path, "/apps/%s.info", argv[2]); vfs_delete(path);
            apps_refresh();
            kprintf("removed %s\n", argv[2]);
        }
    } else {
        kprintf("clan - the AXIOM package manager\n");
        kprintf("  clan list              show available packages\n");
        kprintf("  clan search <word>     find packages\n");
        kprintf("  clan info <package>    describe a package\n");
        kprintf("  clan install <package> install an app (it appears in the dock)\n");
        kprintf("  clan remove <package>  uninstall an app\n");
    }
}
