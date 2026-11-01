// libkern cache control on Linux (see ../README.md)
#pragma once
#include <stddef.h>
static inline void sys_icache_invalidate(void *p, size_t n) { __builtin___clear_cache((char *)p, (char *)p + n); }
static inline void sys_dcache_flush(void *p, size_t n) {
    for (size_t o = 0; o < n; o += 64) __asm__ volatile("dc cvau, %0" :: "r"((char *)p + o) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
}
