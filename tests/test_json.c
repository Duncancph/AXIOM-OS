/* Reads lines of ["<json document>", "<expected>"], decodes delta.text with user/lib/json.c,
 * re-escapes it and prints it as a JSON string (driven by test_json.py). */
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "../user/lib/json.h"
static char line[1 << 20], doc[1 << 20], text[1 << 20], esc[1 << 22];
int main(void) {
    while (fgets(line, sizeof line, stdin)) {
        const char *e = line + strlen(line);
        const char *d = json_get(line, e, "") ;                          /* not used: line is an array */
        (void)d;
        const char *first = line + 1;                                     /* the document is the first array element */
        long dl = json_str(first, e, doc, sizeof doc);
        if (dl < 0) { puts("\"<bad>\""); continue; }
        const char *v = json_get(doc, doc + dl, "delta.text");
        long tl = v ? json_str(v, doc + dl, text, sizeof text) : -1;
        if (tl < 0) { puts("\"<missing>\""); continue; }
        json_escape(text, tl, esc, sizeof esc);
        printf("\"%s\"\n", esc);
    }
}
