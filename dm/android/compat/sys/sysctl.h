// Darwin's sysctlbyname on Linux (see ../README.md): no names are known, the callers' defaults apply
#pragma once
#include <stddef.h>
static inline int sysctlbyname(const char *name, void *old, size_t *oldlen, void *nw, size_t newlen) { (void)name; (void)old; (void)oldlen; (void)nw; (void)newlen; return -1; }
