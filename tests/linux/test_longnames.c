/* gcc -O1 -static-pie -o longls tests/linux/test_longnames.c; on AXIOM: run longls <folder> (after installing the demo package, see
 * ports/chromium/PUBLISHING.md: it has an 81-character folder name) */
/* lists a folder with getdents (as glibc's readdir does) and prints each name with its length */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    DIR *d = opendir(argc > 1 ? argv[1] : ".");
    if (!d) { printf("opendir failed\n"); return 1; }
    struct dirent *e; int n = 0, longest = 0;
    while ((e = readdir(d))) { int l = (int)strlen(e->d_name); if (l > longest) longest = l; if (l > 60) printf("long: %s (%d)\n", e->d_name, l); n++; }
    printf("%d entries, longest name %d characters\n", n, longest);
    FILE *f = fopen("/system/linux/opt/demo/data/a-folder-with-a-rather-long-name-to-need-gnu-long-name-records-in-the-tar-archive/readme-with-a-long-name-too.txt", "r");
    char b[64] = ""; if (f) { fgets(b, sizeof b, f); fclose(f); }
    printf("read through the long path: %s", b[0] ? b : "(failed)\n");
    return 0;
}
