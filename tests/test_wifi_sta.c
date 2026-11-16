/* Host driver for common/wifi_sta.c: commands on stdin, and for each one line out: the state, the reason,
 * then every frame the station sent ("S:hex") and Ethernet frame it delivered ("D:hex").
 * tests/test_wifi_sta.py plays the access point. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "../common/wifi_sta.h"

static char out[200000]; static int on;
static void put(const char *tag, const uint8_t *b, int n) { on += sprintf(out + on, " %s:", tag); for (int i = 0; i < n; i++) on += sprintf(out + on, "%02x", b[i]); }
static void tx(void *c, const uint8_t *f, int n) { (void)c; put("S", f, n); }
static void dl(void *c, const uint8_t *e, int n) { (void)c; put("D", e, n); }
static size_t unh(const char *h, uint8_t *o) { size_t n = strlen(h) / 2; for (size_t i = 0; i < n; i++) sscanf(h + 2 * i, "%2hhx", &o[i]); return n; }
static struct wifi_sta sta;

int main(void) {
    static char line[20000], op[16], a[9000], b[9000], c[200], d[200], e[200], f[64];
    static uint8_t A[4500], B[4500], C[100], D[100], E[100];
    setvbuf(stdout, 0, _IOLBF, 0);
    sta.send = tx; sta.deliver = dl;
    while (fgets(line, sizeof line, stdin)) {
        a[0] = b[0] = c[0] = d[0] = e[0] = f[0] = 0; on = 0; out[0] = 0;
        sscanf(line, "%15s %8999s %8999s %199s %199s %199s %63s", op, a, b, c, d, e, f);
        if (!strcmp(op, "join")) {               /* me bssid ssid(hex) pmk|- snonce now */
            unh(a, A); unh(b, B); size_t sl = unh(c, C); C[sl] = 0; unh(e, E);
            uint8_t pmk[32]; bool sec = strcmp(d, "-"); if (sec) unh(d, pmk);
            wsta_join(&sta, A, B, (char *)C, sec ? pmk : 0, E, strtoull(f, 0, 10));
        } else if (!strcmp(op, "rx")) { int n = (int)unh(a, A); wsta_rx(&sta, A, n, strtoull(b, 0, 10)); }
        else if (!strcmp(op, "tick")) wsta_tick(&sta, strtoull(a, 0, 10));
        else if (!strcmp(op, "eth")) { int n = (int)unh(a, A); int r = wsta_send_eth(&sta, A, n); if (r) on += sprintf(out + on, " refused"); }
        else if (!strcmp(op, "leave")) wsta_leave(&sta);
        printf("%d|%s|%s\n", sta.state, sta.why[0] ? sta.why : "-", out);
    }
}
