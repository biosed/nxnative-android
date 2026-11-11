// Included first in every Android host source (-include): Darwin libc extensions the host uses (see README.md)
#pragma once
#ifndef __ASSEMBLER__
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
#define CLOCK_MONOTONIC_RAW_APPROX CLOCK_MONOTONIC
static inline uint64_t clock_gettime_nsec_np(clockid_t c) { struct timespec t; if (clock_gettime(c, &t)) return 0; return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static inline int getdtablesize(void) { return (int)sysconf(_SC_OPEN_MAX); }
#include <pthread.h>
// Darwin's stack bounds of a thread (the address is the top: stacks grow down). Bionic counts the guard pages at the bottom of a created thread's stack in it (the PROT_NONE
// region pthread_create puts there); Darwin does not, and a collector scanning the reported range (Unity's, through nn::os stack info) faulted there: they are left out
static inline size_t pthread_get_stacksize_np(pthread_t t) {
    pthread_attr_t a; size_t sz = 0, g = 0; void *lo;
    if (!pthread_getattr_np(t, &a)) { pthread_attr_getstack(&a, &lo, &sz); pthread_attr_getguardsize(&a, &g); pthread_attr_destroy(&a); }
    return sz > g ? sz - g : sz;
}
static inline void *pthread_get_stackaddr_np(pthread_t t) { pthread_attr_t a; size_t sz = 0; void *lo = 0; if (!pthread_getattr_np(t, &a)) { pthread_attr_getstack(&a, &lo, &sz); pthread_attr_destroy(&a); } return (char *)lo + sz; }
// Darwin's renamex_np: RENAME_SWAP is Linux's renameat2 RENAME_EXCHANGE
#define RENAME_SWAP 2
#define RENAME_EXCL 4
int renamex_np(const char *from, const char *to, unsigned flags);
// Darwin names the calling thread only (Linux names any thread, up to 15 characters)
#define pthread_setname_np(name) pthread_setname_np(pthread_self(), name)
// Darwin's wait with a relative timeout
#include <time.h>
static inline int pthread_cond_timedwait_relative_np(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *rel) {
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += rel->tv_sec; t.tv_nsec += rel->tv_nsec; if (t.tv_nsec >= 1000000000) { t.tv_sec++; t.tv_nsec -= 1000000000; }
    return pthread_cond_timedwait(c, m, &t);
}
// BSD's stable sort (hle_hid.c orders the DM_KEYS steps with it); compat.c
int mergesort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
// Darwin's name of struct stat's modification time (save_sync.c)
#define st_mtimespec st_mtim
#endif
