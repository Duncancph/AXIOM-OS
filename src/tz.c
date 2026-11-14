/* Time zones with their daylight-saving rules (EU, US/Canada, Australia, New Zealand). */
#ifdef TZ_TEST
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#define ksnprintf snprintf
#else
#include "kernel.h"
#endif

/* ---- time zones ---- */
enum { R_NONE, R_EU, R_US, R_AU, R_NZ };
static const struct { const char *id, *name; int off; int rule; } zones[] = {
    { "UTC", "UTC (no daylight saving)", 0, R_NONE },
    { "Europe/London", "London, Edinburgh, Cardiff", 0, R_EU }, { "Europe/Dublin", "Dublin", 0, R_EU },
    { "Europe/Lisbon", "Lisbon", 0, R_EU },
    { "Europe/Paris", "Paris, Brussels, Amsterdam", 60, R_EU }, { "Europe/Berlin", "Berlin, Vienna, Zurich", 60, R_EU },
    { "Europe/Madrid", "Madrid", 60, R_EU }, { "Europe/Rome", "Rome", 60, R_EU }, { "Europe/Stockholm", "Stockholm, Oslo, Copenhagen", 60, R_EU },
    { "Europe/Warsaw", "Warsaw, Prague", 60, R_EU },
    { "Europe/Athens", "Athens, Bucharest", 120, R_EU }, { "Europe/Helsinki", "Helsinki, Kyiv", 120, R_EU },
    { "Europe/Istanbul", "Istanbul", 180, R_NONE }, { "Europe/Moscow", "Moscow", 180, R_NONE },
    { "Africa/Johannesburg", "Johannesburg", 120, R_NONE }, { "Africa/Lagos", "Lagos", 60, R_NONE }, { "Africa/Nairobi", "Nairobi", 180, R_NONE },
    { "Asia/Dubai", "Dubai", 240, R_NONE }, { "Asia/Karachi", "Karachi", 300, R_NONE }, { "Asia/Kolkata", "India", 330, R_NONE },
    { "Asia/Bangkok", "Bangkok, Jakarta", 420, R_NONE }, { "Asia/Shanghai", "Beijing, Hong Kong, Singapore", 480, R_NONE },
    { "Asia/Tokyo", "Tokyo, Seoul", 540, R_NONE },
    { "Australia/Perth", "Perth", 480, R_NONE }, { "Australia/Brisbane", "Brisbane", 600, R_NONE },
    { "Australia/Adelaide", "Adelaide", 570, R_AU }, { "Australia/Sydney", "Sydney, Melbourne, Canberra", 600, R_AU },
    { "Pacific/Auckland", "Auckland, Wellington", 720, R_NZ },
    { "America/St_Johns", "Newfoundland", -210, R_US }, { "America/Halifax", "Atlantic (Halifax)", -240, R_US },
    { "America/New_York", "Eastern (New York, Toronto)", -300, R_US }, { "America/Chicago", "Central (Chicago)", -360, R_US },
    { "America/Denver", "Mountain (Denver)", -420, R_US }, { "America/Phoenix", "Arizona", -420, R_NONE },
    { "America/Los_Angeles", "Pacific (Los Angeles, Vancouver)", -480, R_US }, { "America/Anchorage", "Alaska", -540, R_US },
    { "Pacific/Honolulu", "Hawaii", -600, R_NONE }, { "America/Mexico_City", "Mexico City", -360, R_NONE },
    { "America/Bogota", "Bogota, Lima", -300, R_NONE }, { "America/Sao_Paulo", "Sao Paulo, Buenos Aires", -180, R_NONE },
};
#define NZONES (int)(sizeof zones / sizeof zones[0])

int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
void civil_from_days(int64_t z, int *y, int *m, int *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yy + (*m <= 2));
}
/* the n-th Sunday of a month (n = -1: the last one), as a day number */
static int64_t sunday(int y, int m, int n) {
    if (n < 0) return m == 12 ? sunday(y + 1, 1, 1) - 7 : sunday(y, m + 1, 1) - 7;   /* last: a week before next month's first */
    int64_t first = days_from_civil(y, m, 1);
    int wd = (int)((first + 4) % 7); if (wd < 0) wd += 7;
    return first + (7 - wd) % 7 + 7 * (n - 1);
}

static int zone_index(const char *z) {
    for (int i = 0; i < NZONES; i++) if (!strcmp(zones[i].id, z)) return i;
    return 0;
}

/* minutes to add to UTC for local time in zone z at this UTC moment */
int tz_offset_zone(const char *z, int64_t utc) {
    int zi = zone_index(z), off = zones[zi].off, rule = zones[zi].rule;
    if (rule == R_NONE) return off;
    int y, m, d; civil_from_days(utc / 86400, &y, &m, &d);
    int64_t start, end;                                          /* in UTC seconds */
    switch (rule) {
    case R_EU: start = sunday(y, 3, -1) * 86400 + 3600; end = sunday(y, 10, -1) * 86400 + 3600; break;
    case R_US: start = sunday(y, 3, 2) * 86400 + 7200 - off * 60; end = sunday(y, 11, 1) * 86400 + 7200 - (off + 60) * 60; break;
    case R_AU: start = sunday(y, 10, 1) * 86400 + 7200 - off * 60; end = sunday(y, 4, 1) * 86400 + 10800 - (off + 60) * 60; break;
    default:   start = sunday(y, 9, -1) * 86400 + 7200 - off * 60; end = sunday(y, 4, 1) * 86400 + 10800 - (off + 60) * 60; break;
    }
    bool dst = start < end ? (utc >= start && utc < end) : (utc >= start || utc < end);   /* southern: across new year */
    return off + (dst ? 60 : 0);
}

/* "id=Name;..." */
int tz_list(char *out, int n) {
    int k = 0;
    for (int i = 0; i < NZONES && k < n - 1; i++) {
        int o = zones[i].off; char sign = o < 0 ? '-' : '+'; if (o < 0) o = -o;
        k += ksnprintf(out + k, n - k, "%s%s=(UTC%c%d:%02d) %s", i ? ";" : "", zones[i].id, sign, o / 60, o % 60, zones[i].name);
    }
    return k;
}

