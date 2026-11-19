/* axiom-open: open files with AXIOM's own app for them (Video, Music, Viewer, Editor...), from a Linux program.
 * The .desktop files AXIOM puts in /usr/share/applications run it, so Thunar and other Linux apps can open an MP4
 * with AXIOM's Video app, a picture with its Viewer, and so on. It asks the kernel through AXIOM's own Linux system
 * call 1000 (the path, as this program sees it), which starts the app that AXIOM uses for that kind of file.
 *   gcc -O2 -static-pie -o axiom-open axiom-open.c */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: axiom-open FILE...\n"); return 2; }
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        const char *p = argv[i];
        if (!strncmp(p, "file://", 7)) p += 7;                  /* (a URI, as some apps pass) */
        if (syscall(1000, p) < 0) { fprintf(stderr, "axiom-open: %s: %s\n", p, errno == ENOENT ? "no such file" : "AXIOM has no app for this kind of file"); bad = 1; }
    }
    return bad;
}
