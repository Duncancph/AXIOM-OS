/* AXIOM media library: port layer. The only external functions used are the ones below.
 * With -DMEDIA_HOST_TEST the libc headers are used instead (host tests). */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef MEDIA_HOST_TEST
#include <stdlib.h>
#include <string.h>
#else
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *malloc(size_t n);
void *calloc(size_t n, size_t m);
void *realloc(void *p, size_t n);
void free(void *p);
#endif
