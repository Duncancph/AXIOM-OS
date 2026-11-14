/* Reads addresses on stdin, prints the canonical form or "invalid" (driven by test_inet.py). */
#include <stdio.h>
#include <string.h>
#include "../common/inet.h"
int main(void) {
    char line[256], out[IP_STRLEN];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        struct ipaddr a;
        if (ip_parse(line, &a)) { ip_format(&a, out); puts(out); } else puts("invalid");
    }
}
