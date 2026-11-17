/* SQLite on AXIOM the way Chromium uses it (its profile's databases): a file database read through mmap while
 * written with pwrite, rollback journal and WAL (with exclusive locking, as Chromium sets), reopened and checked.
 * Uses the C library's libsqlite3.so.0 (on the AXIOM disk for Chromium's NSS).
 *   gcc -O1 -o test_sqlite tests/linux/test_sqlite.c -l:libsqlite3.so.0
 * Run on AXIOM ('run test_sqlite', in /home); it also runs on Linux itself, which must print the same. */
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static int exec(sqlite3 *db, const char *sql) {
    char *err = 0;
    int r = sqlite3_exec(db, sql, 0, 0, &err);
    if (r != SQLITE_OK) printf("  %s: %s\n", sql, err ? err : "?");
    sqlite3_free(err);
    return r;
}
static long long value(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st; long long v = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, 0) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}
static const char *text(sqlite3 *db, const char *sql, char *buf, int n) {
    sqlite3_stmt *st; buf[0] = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &st, 0) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        snprintf(buf, n, "%s", (const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return buf;
}

/* fill a database in several transactions, close, reopen, check */
static void run(const char *name, const char *mode, int mmap_mb) {
    char path[64], q[200], buf[64];
    snprintf(path, sizeof path, "sqlt_%s.db", name);
    unlink(path);
    for (const char *sfx = "-journal"; sfx; sfx = sfx[1] == 'j' ? "-wal" : 0) { char p[80]; snprintf(p, sizeof p, "%s%s", path, sfx); unlink(p); }
    sqlite3 *db;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK);
    snprintf(q, sizeof q, "PRAGMA mmap_size=%d", mmap_mb << 20); exec(db, q);
    exec(db, "PRAGMA locking_mode=EXCLUSIVE");
    snprintf(q, sizeof q, "PRAGMA journal_mode=%s", mode);
    CHECK(!strcasecmp(text(db, q, buf, sizeof buf), mode));
    CHECK(exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, s TEXT, b BLOB)") == SQLITE_OK);
    for (int k = 0; k < 20; k++) {
        exec(db, "BEGIN");
        for (int i = 0; i < 100; i++) {
            snprintf(q, sizeof q, "INSERT INTO t(s, b) VALUES('row %d of batch %d', randomblob(300))", i, k);
            if (exec(db, q) != SQLITE_OK) break;
        }
        exec(db, "COMMIT");
        CHECK(value(db, "SELECT count(*) FROM t") == (k + 1) * 100);   /* (read back while it grows) */
    }
    CHECK(exec(db, "DELETE FROM t WHERE id % 3 = 0") == SQLITE_OK);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    CHECK(sqlite3_open(path, &db) == SQLITE_OK);
    snprintf(q, sizeof q, "PRAGMA mmap_size=%d", mmap_mb << 20); exec(db, q);
    CHECK(!strcmp(text(db, "PRAGMA integrity_check", buf, sizeof buf), "ok"));
    long long n = value(db, "SELECT count(*) FROM t");
    CHECK(n == 2000 - 666);
    CHECK(exec(db, "VACUUM") == SQLITE_OK);
    CHECK(!strcmp(text(db, "PRAGMA integrity_check", buf, sizeof buf), "ok"));
    printf("%-14s journal %-8s mmap %3d MB: %lld rows, %s\n", name, mode, mmap_mb, n, buf);
    sqlite3_close(db);
}

int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);
    printf("SQLite %s\n", sqlite3_libversion());
    run("plain", "delete", 0);
    run("mmap", "delete", 256);
    run("truncate_mmap", "truncate", 256);
    run("wal", "wal", 0);
    run("wal_mmap", "wal", 256);
    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails != 0;
}
