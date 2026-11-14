/* Minimal JSON helpers (user/lib/json.c). Values are pointers into the original text, bounded by `end`. */
#pragma once
const char *json_get(const char *obj, const char *end, const char *path);   /* member value ("a.b"), or 0 */
const char *json_skip(const char *v, const char *end);                       /* end of the value at v, or 0 */
long json_str(const char *v, const char *end, char *out, long max);          /* decode a string (UTF-8); length or -1 */
long json_escape(const char *in, long n, char *out, long max);               /* escape for a JSON string; length needed */
bool json_is(const char *v, const char *end, const char *s);                 /* the value is exactly the string s */
long json_int(const char *v, const char *end);
