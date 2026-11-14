#pragma once
#include <stddef.h>
#include <stdarg.h>
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strncpy0(char *d, const char *s, size_t n);   /* always NUL-terminates */
int kvsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
long utf8_to_cp1252(char *s, long n);
long cp1252_to_utf8(const char *in, long n, char *out, long max);   /* in place, if s is UTF-8 */
int  cp1252_from_unicode(unsigned int u);   /* -1 if it has no Windows-1252 character */
int ksnprintf(char *buf, size_t n, const char *fmt, ...);
long katoi(const char *s);
