// malloc zones on Linux (see ../README.md): one zone, bionic's malloc
#pragma once
#include <malloc.h>
#include <stddef.h>
typedef struct { const char *zone_name; } malloc_zone_t;
static inline size_t malloc_size(const void *p) { return p ? malloc_usable_size((void *)p) : 0; }
static inline size_t malloc_good_size(size_t n) { return (n + 15) & ~(size_t)15; }
malloc_zone_t *malloc_create_zone(size_t start, unsigned flags);
static inline void malloc_set_zone_name(malloc_zone_t *z, const char *n) { if (z) z->zone_name = n; }
static inline void *malloc_zone_malloc(malloc_zone_t *z, size_t n) { (void)z; return malloc(n); }
static inline void *malloc_zone_realloc(malloc_zone_t *z, void *p, size_t n) { (void)z; return realloc(p, n); }
static inline void *malloc_zone_memalign(malloc_zone_t *z, size_t a, size_t n) { (void)z; void *p = NULL; return posix_memalign(&p, a < sizeof(void *) ? sizeof(void *) : a, n) ? NULL : p; }
static inline void malloc_zone_free(malloc_zone_t *z, void *p) { (void)z; free(p); }
