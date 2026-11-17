/* Processes on AXIOM's Linux layer: fork (memory copied), exit status, execve with a pipe, close-on-exec,
 * posix_spawn (vfork) with arguments and environment, kill, #! scripts, SIGCHLD. Run it by its full path.
 * Prints PASS/FAIL. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
extern char **environ;
static int fails;
#define CHECK(c, ...) do { if (c) printf("ok   " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); fails++; } printf("\n"); fflush(stdout); } while (0)
static volatile int chld;
static void on_chld(int s) { chld++; }
static char self[256];
static int read_all(int fd, char *b, int n) { int k = 0; long r; while (k < n - 1 && (r = read(fd, b + k, n - 1 - k)) > 0) k += r; b[k] = 0; return k; }
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--child")) {             /* run by the tests below */
        printf("child args=%d %s %s env=%s", argc, argc > 2 ? argv[2] : "-", argc > 3 ? argv[3] : "-", getenv("PROCTEST") ? getenv("PROCTEST") : "-");
        if (argc > 2 && !strcmp(argv[2], "cloexec")) printf(" fd%s=%s", argv[3], fcntl(atoi(argv[3]), F_GETFD) < 0 && errno == EBADF ? "closed" : "open");
        printf("\n"); return 3;
    }
    if (argc > 1 && !strcmp(argv[1], "--script")) { printf("script %s\n", argc > 2 ? argv[2] : "-"); return 4; }
    if (argc > 1 && !strcmp(argv[1], "--sleep")) { for (;;) pause(); }
    strncpy(self, argv[0], sizeof self - 1);
    signal(SIGCHLD, on_chld);
    /* fork: exit status, and the child's memory is its own */
    static int shared_var = 1;
    pid_t p = fork();
    if (p == 0) { shared_var = 99; _exit(7); }
    int st = 0; pid_t w = waitpid(p, &st, 0);
    CHECK(p > 0 && w == p && WIFEXITED(st) && WEXITSTATUS(st) == 7, "fork + wait: exit status 7 (pid %d, status %x)", p, st);
    CHECK(shared_var == 1, "fork: the child's change stays in the child (%d)", shared_var);
    CHECK(getpid() != p, "the child has its own pid");
    /* fork + execve, its output through a pipe */
    int pf[2]; pipe(pf);
    p = fork();
    if (p == 0) { dup2(pf[1], 1); close(pf[0]); close(pf[1]); char *a[] = { self, "--child", "one", "two", 0 }; char *e[] = { "PROCTEST=yes", 0 }; execve(self, a, e); _exit(127); }
    close(pf[1]); char out[256]; read_all(pf[0], out, sizeof out); close(pf[0]);
    waitpid(p, &st, 0);
    CHECK(strstr(out, "child args=4 one two env=yes") && WEXITSTATUS(st) == 3, "fork + execve + pipe: \"%.*s\" (status %d)", (int)strcspn(out, "\n"), out, WEXITSTATUS(st));
    /* close-on-exec */
    int keep = open(self, O_RDONLY), gone = open(self, O_RDONLY | O_CLOEXEC);
    pipe(pf);
    p = fork();
    if (p == 0) { dup2(pf[1], 1); char n1[8], n2[8]; snprintf(n1, 8, "%d", keep); snprintf(n2, 8, "%d", gone);
        if (fork() == 0) { char *a[] = { self, "--child", "cloexec", n1, 0 }; execv(self, a); _exit(127); }
        wait(0); char *a[] = { self, "--child", "cloexec", n2, 0 }; execv(self, a); _exit(127); }
    close(pf[1]); read_all(pf[0], out, sizeof out); close(pf[0]); waitpid(p, &st, 0);
    char want1[32], want2[32]; snprintf(want1, 32, "fd%d=open", keep); snprintf(want2, 32, "fd%d=closed", gone);
    CHECK(strstr(out, want1) && strstr(out, want2), "close-on-exec: a plain file stays open, an O_CLOEXEC one closes");
    /* posix_spawn (vfork) */
    pipe(pf);
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pf[1], 1); posix_spawn_file_actions_addclose(&fa, pf[0]);
    char *a2[] = { self, "--child", "spawned", "x", 0 }; char *e2[] = { "PROCTEST=spawn", 0 };
    int sr = posix_spawn(&p, self, &fa, 0, a2, e2);
    close(pf[1]); read_all(pf[0], out, sizeof out); close(pf[0]); waitpid(p, &st, 0);
    CHECK(sr == 0 && strstr(out, "spawned x env=spawn") && WEXITSTATUS(st) == 3, "posix_spawn: \"%.*s\" (%d)", (int)strcspn(out, "\n"), out, sr);
    /* kill */
    p = fork();
    if (p == 0) { execl(self, self, "--sleep", (char *)0); _exit(127); }
    struct timespec ts = { 0, 300000000 }; nanosleep(&ts, 0);
    kill(p, SIGTERM); waitpid(p, &st, 0);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM, "kill: the child ended by SIGTERM (status %x)", st);
    /* a child that crashes: ended by SIGSEGV (not SIGKILL), as Chromium tells a crashed tab from a killed one */
    p = fork();
    if (p == 0) { *(volatile int *)0 = 1; _exit(0); }
    waitpid(p, &st, 0);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, "a null pointer: the child ended by SIGSEGV (status %x)", st);
    /* #! script */
    char script[300]; snprintf(script, sizeof script, "/home/proctest-script.sh");
    FILE *sf = fopen(script, "w"); fprintf(sf, "#!%s --script\n", self); fclose(sf); chmod(script, 0755);
    pipe(pf);
    p = fork();
    if (p == 0) { dup2(pf[1], 1); execl(script, script, (char *)0); _exit(127); }
    close(pf[1]); read_all(pf[0], out, sizeof out); close(pf[0]); waitpid(p, &st, 0);
    CHECK(strstr(out, "script /home/proctest-script.sh") && WEXITSTATUS(st) == 4, "#! script: \"%.*s\"", (int)strcspn(out, "\n"), out);
    CHECK(waitpid(-1, &st, WNOHANG) < 0 && errno == ECHILD, "no children left: ECHILD");
    CHECK(chld >= 5, "SIGCHLD came (%d times)", chld);
    printf("%s\n", fails ? "SOME FAILED" : "PASS"); return fails != 0;
}
