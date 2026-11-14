/* Prints UTC offsets from src/tz.c for every zone, hourly over several years, for test_tz.py to compare with zoneinfo. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
int tz_offset_zone(const char *z, int64_t utc);
int tz_list(char *out, int n);
int main(void) {
    static char list[8192]; tz_list(list, sizeof list);
    for (char *p = strtok(list, ";"); p; p = strtok(0, ";")) {
        char id[64]; sscanf(p, "%63[^=]", id);
        for (int64_t t = 1704067200; t < 1704067200 + 7LL * 365 * 86400; t += 3600) printf("%s %lld %d\n", id, (long long)t, tz_offset_zone(id, t));
    }
}
