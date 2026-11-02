// Apple API compatibility on Linux / bionic (see README.md)
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include "dm_mach.h"
#include "pthread/qos.h"
#include "dispatch/dispatch.h"
#include "malloc/malloc.h"
#include "CommonCrypto/CommonDigest.h"

vm_size_t vm_page_size = 4096;
mach_port_t mach_task_self(void) { return 1; }
mach_port_t mach_thread_self(void) { return (mach_port_t)gettid(); }
mach_port_t pthread_mach_thread_np(pthread_t t) { return (mach_port_t)pthread_gettid_np(t); }
kern_return_t mach_port_deallocate(mach_port_t task, mach_port_t name) { (void)task; (void)name; return KERN_SUCCESS; }

uint64_t mach_absolute_time(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
kern_return_t mach_timebase_info(mach_timebase_info_t i) { i->numer = i->denom = 1; return KERN_SUCCESS; }
kern_return_t mach_wait_until(uint64_t deadline) {
    struct timespec t = {(time_t)(deadline / 1000000000ull), (long)(deadline % 1000000000ull)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL) == EINTR) {}
    return KERN_SUCCESS;
}

kern_return_t vm_read_overwrite(mach_port_t task, vm_address_t addr, vm_size_t size, vm_address_t data, vm_size_t *out) {
    (void)task;
    struct iovec l = {(void *)data, size}, r = {(void *)addr, size};
    ssize_t n = process_vm_readv(getpid(), &l, 1, &r, 1, 0);
    if (n != (ssize_t)size) return KERN_FAILURE;
    if (out) *out = size;
    return KERN_SUCCESS;
}
kern_return_t vm_write(mach_port_t task, vm_address_t addr, vm_offset_t data, mach_msg_type_number_t cnt) {
    (void)task;
    struct iovec l = {(void *)data, cnt}, r = {(void *)addr, cnt};
    return process_vm_writev(getpid(), &l, 1, &r, 1, 0) == (ssize_t)cnt ? KERN_SUCCESS : KERN_FAILURE;
}
kern_return_t vm_protect(mach_port_t task, vm_address_t addr, vm_size_t size, boolean_t set_max, vm_prot_t prot) {
    (void)task; (void)set_max;
    return mprotect((void *)addr, size, (prot & VM_PROT_READ ? PROT_READ : 0) | (prot & VM_PROT_WRITE ? PROT_WRITE : 0) | (prot & VM_PROT_EXECUTE ? PROT_EXEC : 0)) ? KERN_FAILURE : KERN_SUCCESS;
}
kern_return_t vm_allocate(mach_port_t task, vm_address_t *addr, vm_size_t size, int flags) {
    (void)task;
    void *p = mmap(flags & VM_FLAGS_ANYWHERE ? NULL : (void *)*addr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | (flags & VM_FLAGS_ANYWHERE ? 0 : MAP_FIXED), -1, 0);
    if (p == MAP_FAILED) return KERN_FAILURE;
    *addr = (vm_address_t)p; return KERN_SUCCESS;
}
kern_return_t vm_deallocate(mach_port_t task, vm_address_t addr, vm_size_t size) { (void)task; return munmap((void *)addr, size) ? KERN_FAILURE : KERN_SUCCESS; }
// Anonymous private memory cannot be mapped a second time on Linux
kern_return_t vm_remap(mach_port_t task, vm_address_t *to, vm_size_t size, vm_address_t mask, int flags, mach_port_t src_task, vm_address_t from, boolean_t copy, vm_prot_t *cur, vm_prot_t *max, int inherit) {
    (void)task; (void)to; (void)size; (void)mask; (void)flags; (void)src_task; (void)from; (void)copy; (void)cur; (void)max; (void)inherit;
    return KERN_FAILURE;
}

// A thread's state and CPU time from /proc/self/task/<tid>/stat (the port is the tid): run state R is running (the host's same-core preemption, hle_thread.c thr_running, waits on it), utime / stime in clock ticks
kern_return_t thread_info(mach_port_t th, thread_flavor_t f, thread_info_t out, mach_msg_type_number_t *cnt) {
    if (f != THREAD_BASIC_INFO || !cnt || *cnt < THREAD_BASIC_INFO_COUNT) return KERN_FAILURE;
    char path[64], buf[512]; snprintf(path, sizeof path, "/proc/self/task/%u/stat", th);
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return KERN_FAILURE;
    ssize_t n = read(fd, buf, sizeof buf - 1); close(fd);
    if (n <= 0) return KERN_FAILURE;
    buf[n] = 0; char *p = strrchr(buf, ')'); if (!p || p[1] != ' ') return KERN_FAILURE;
    char st = p[2]; unsigned long long ut = 0, sy = 0;   // fields after the name: state (3), ..., utime (14), stime (15)
    if (sscanf(p + 4, "%*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &sy) != 2) return KERN_FAILURE;
    static long hz; if (!hz) { hz = sysconf(_SC_CLK_TCK); if (hz <= 0) hz = 100; }
    thread_basic_info_data_t *bi = (thread_basic_info_data_t *)out; memset(bi, 0, sizeof *bi);
    bi->user_time.seconds = (int)(ut / hz); bi->user_time.microseconds = (int)(ut % hz * 1000000 / hz);
    bi->system_time.seconds = (int)(sy / hz); bi->system_time.microseconds = (int)(sy % hz * 1000000 / hz);
    bi->run_state = st == 'R' ? TH_STATE_RUNNING : TH_STATE_WAITING;
    *cnt = THREAD_BASIC_INFO_COUNT; return KERN_SUCCESS;
}
kern_return_t thread_suspend(mach_port_t th) { (void)th; return KERN_FAILURE; }
kern_return_t thread_resume(mach_port_t th) { (void)th; return KERN_FAILURE; }
kern_return_t thread_get_state(mach_port_t th, int f, thread_state_t st, mach_msg_type_number_t *cnt) { (void)th; (void)f; (void)st; (void)cnt; return KERN_FAILURE; }
kern_return_t task_threads(mach_port_t task, thread_act_array_t *l, mach_msg_type_number_t *n) { (void)task; (void)l; *n = 0; return KERN_FAILURE; }
kern_return_t task_info(mach_port_t task, task_flavor_t f, task_info_t out, mach_msg_type_number_t *cnt) { (void)task; (void)f; (void)out; (void)cnt; return KERN_FAILURE; }
kern_return_t thread_policy_set(mach_port_t th, thread_policy_flavor_t f, thread_policy_t p, mach_msg_type_number_t cnt) { (void)th; (void)f; (void)p; (void)cnt; return KERN_FAILURE; }

static int qos_nice(qos_class_t q) { return q >= QOS_CLASS_USER_INTERACTIVE ? -4 : q >= QOS_CLASS_USER_INITIATED ? -2 : q >= QOS_CLASS_DEFAULT || !q ? 0 : q >= QOS_CLASS_UTILITY ? 5 : 10; }
static __thread qos_class_t g_qos;
int pthread_set_qos_class_self_np(qos_class_t q, int rel) { (void)rel; g_qos = q; setpriority(PRIO_PROCESS, (id_t)gettid(), qos_nice(q)); return 0; }
int pthread_get_qos_class_np(pthread_t t, qos_class_t *q, int *rel) { (void)t; if (q) *q = g_qos; if (rel) *rel = 0; return 0; }
int pthread_attr_set_qos_class_np(pthread_attr_t *a, qos_class_t q, int rel) { (void)a; (void)q; (void)rel; return 0; }
pthread_override_t pthread_override_qos_class_start_np(pthread_t t, qos_class_t q, int rel) { (void)t; (void)q; (void)rel; return NULL; }
int pthread_override_qos_class_end_np(pthread_override_t o) { (void)o; return 0; }

struct dm_dispatch_sem { sem_t s; };
dispatch_time_t dispatch_time(dispatch_time_t when, int64_t delta) { return (when ? when : mach_absolute_time()) + (uint64_t)delta; }
dispatch_semaphore_t dispatch_semaphore_create(long v) { dispatch_semaphore_t s = calloc(1, sizeof *s); if (s) sem_init(&s->s, 0, (unsigned)v); return s; }
long dispatch_semaphore_wait(dispatch_semaphore_t s, dispatch_time_t timeout) {
    if (timeout == DISPATCH_TIME_FOREVER) { while (sem_wait(&s->s) && errno == EINTR) {} return 0; }
    struct timespec t = {(time_t)(timeout / 1000000000ull), (long)(timeout % 1000000000ull)};
    for (;;) { if (!sem_clockwait(&s->s, CLOCK_MONOTONIC, &t)) return 0; if (errno != EINTR) return 1; }
}
long dispatch_semaphore_signal(dispatch_semaphore_t s) { sem_post(&s->s); return 0; }
void dispatch_release(void *o) { struct dm_dispatch_sem *s = o; if (s) { sem_destroy(&s->s); free(s); } }

malloc_zone_t *malloc_create_zone(size_t start, unsigned flags) { (void)start; (void)flags; static malloc_zone_t z; return &z; }

// SHA-256 (FIPS 180-4)
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))
static void sha_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) w[i] = w[i - 16] + (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ w[i - 15] >> 3) + w[i - 7] + (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ w[i - 2] >> 10);
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i], t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}
int CC_SHA256_Init(CC_SHA256_CTX *c) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof iv); c->n = 0; return 1;
}
int CC_SHA256_Update(CC_SHA256_CTX *c, const void *data, CC_LONG len) {
    const uint8_t *p = data;
    while (len) {
        unsigned used = (unsigned)(c->n & 63), take = 64 - used < len ? 64 - used : len;
        memcpy(c->buf + used, p, take); c->n += take; p += take; len -= take;
        if (!(c->n & 63)) sha_block(c->h, c->buf);
    }
    return 1;
}
int CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *c) {
    uint64_t bits = c->n * 8; uint8_t pad = 0x80, z = 0, l[8];
    CC_SHA256_Update(c, &pad, 1);
    while ((c->n & 63) != 56) CC_SHA256_Update(c, &z, 1);
    for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    CC_SHA256_Update(c, l, 8);
    for (int i = 0; i < 8; i++) { md[4 * i] = (uint8_t)(c->h[i] >> 24); md[4 * i + 1] = (uint8_t)(c->h[i] >> 16); md[4 * i + 2] = (uint8_t)(c->h[i] >> 8); md[4 * i + 3] = (uint8_t)c->h[i]; }
    return 1;
}
unsigned char *CC_SHA256(const void *data, CC_LONG len, unsigned char *md) { CC_SHA256_CTX c; CC_SHA256_Init(&c); CC_SHA256_Update(&c, data, len); CC_SHA256_Final(md, &c); return md; }

// clonefile: a copy of the tree (the save system clones its work directory at mount and commit; save data is small)
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include "sys/clonefile.h"
static int copy_xattrs(const char *src, const char *dst) {
    char names[4096]; ssize_t n = llistxattr(src, names, sizeof names);
    for (ssize_t o = 0; n > 0 && o < n; o += (ssize_t)strlen(names + o) + 1) {
        if (strncmp(names + o, "user.", 5)) continue;
        char v[4096]; ssize_t k = lgetxattr(src, names + o, v, sizeof v);
        if (k >= 0 && lsetxattr(dst, names + o, v, (size_t)k, 0)) return -1;
    }
    return 0;
}
static int copy_file(const char *src, const char *dst, mode_t mode) {
    int in = open(src, O_RDONLY | O_CLOEXEC), out = in < 0 ? -1 : open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode & 0777);
    int r = in < 0 || out < 0 ? -1 : 0; char b[65536]; ssize_t k;
    while (!r && (k = read(in, b, sizeof b)) != 0) {
        if (k < 0) { if (errno == EINTR) continue; r = -1; break; }
        for (ssize_t w = 0; w < k;) { ssize_t x = write(out, b + w, (size_t)(k - w)); if (x < 0) { if (errno == EINTR) continue; r = -1; break; } w += x; }
    }
    int e = errno;
    if (in >= 0) close(in);
    if (out >= 0 && close(out)) r = -1;
    errno = e; return r;
}
int clonefile(const char *src, const char *dst, uint32_t flags) {
    (void)flags;
    struct stat st; if (lstat(src, &st)) return -1;
    if (S_ISLNK(st.st_mode)) { char t[4096]; ssize_t k = readlink(src, t, sizeof t - 1); if (k < 0) return -1; t[k] = 0; return symlink(t, dst); }
    if (!S_ISDIR(st.st_mode)) return copy_file(src, dst, st.st_mode) || copy_xattrs(src, dst) ? -1 : 0;
    if (mkdir(dst, st.st_mode & 0777) || copy_xattrs(src, dst)) return -1;
    DIR *d = opendir(src); if (!d) return -1;
    struct dirent *e; int r = 0;
    while (!r && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[4096], b[4096]; snprintf(a, sizeof a, "%s/%s", src, e->d_name); snprintf(b, sizeof b, "%s/%s", dst, e->d_name);
        r = clonefile(a, b, flags);
    }
    int er = errno; closedir(d); errno = er; return r;
}
int renamex_np(const char *from, const char *to, unsigned flags) {
    if (flags & RENAME_SWAP) return renameat2(AT_FDCWD, from, AT_FDCWD, to, 2 /* RENAME_EXCHANGE */);
    if (flags & RENAME_EXCL) return renameat2(AT_FDCWD, from, AT_FDCWD, to, 1 /* RENAME_NOREPLACE */);
    return rename(from, to);
}

// CFDictionary with no callbacks: an open-addressing pointer map (deleted entries are tombstones, the table doubles at half full)
#include "CoreFoundation/CoreFoundation.h"
#define CF_TOMB ((const void *)1)
struct dm_cfdict { const void **k, **v; size_t cap, n, used; };
static size_t cf_slot(const void *k, size_t mask) { uint64_t h = (uint64_t)(uintptr_t)k * 0x9E3779B97F4A7C15ull; return (size_t)(h >> 20) & mask; }
CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef a, CFIndex cap, const void *kcb, const void *vcb) {
    (void)a; (void)cap; (void)kcb; (void)vcb;
    CFMutableDictionaryRef d = calloc(1, sizeof *d); d->cap = 64; d->k = calloc(d->cap, sizeof *d->k); d->v = calloc(d->cap, sizeof *d->v); return d;
}
static size_t cf_find(CFMutableDictionaryRef d, const void *k) {
    for (size_t i = cf_slot(k, d->cap - 1);; i = (i + 1) & (d->cap - 1)) if (!d->k[i] || d->k[i] == k) return i;
}
const void *CFDictionaryGetValue(CFMutableDictionaryRef d, const void *k) { size_t i = cf_find(d, k); return d->k[i] == k ? d->v[i] : NULL; }
void CFDictionarySetValue(CFMutableDictionaryRef d, const void *k, const void *v) {
    size_t i = cf_find(d, k);
    if (d->k[i] == k) { d->v[i] = v; return; }
    if ((d->used + 1) * 2 > d->cap) {
        const void **ok = d->k, **ov = d->v; size_t oc = d->cap;
        d->cap *= 2; d->k = calloc(d->cap, sizeof *d->k); d->v = calloc(d->cap, sizeof *d->v); d->used = d->n = 0;
        for (size_t j = 0; j < oc; j++) if (ok[j] && ok[j] != CF_TOMB) CFDictionarySetValue(d, ok[j], ov[j]);
        free(ok); free(ov); i = cf_find(d, k);
    }
    d->k[i] = k; d->v[i] = v; d->n++; d->used++;
}
void CFDictionaryRemoveValue(CFMutableDictionaryRef d, const void *k) { size_t i = cf_find(d, k); if (d->k[i] == k) { d->k[i] = CF_TOMB; d->v[i] = NULL; d->n--; } }
CFIndex CFDictionaryGetCount(CFMutableDictionaryRef d) { return (CFIndex)d->n; }
void CFDictionaryGetKeysAndValues(CFMutableDictionaryRef d, const void **keys, const void **values) {   // in table order, as CoreFoundation gives no order either
    size_t n = 0; for (size_t i = 0; i < d->cap; i++) if (d->k[i] && d->k[i] != CF_TOMB) { if (keys) keys[n] = d->k[i]; if (values) values[n] = d->v[i]; n++; }
}
// The mapping that holds *addr, else the first one after it, from /proc/self/maps: its start, length and protection (VM_REGION_BASIC_INFO_64 only). Neighbouring mappings
// with the same protection count as one region, as one malloc'd block may span two (hle_uemalloc.c dm_ue_heap_owns asks whether a thread stack lies inside one region).
// The pointer's top byte is a tag on Android (heap pointer tagging), not part of the address
kern_return_t vm_region_64(mach_port_t task, vm_address_t *addr, vm_size_t *size, int flavor, vm_region_info_t info, mach_msg_type_number_t *cnt, mach_port_t *obj) {
    (void)task; (void)flavor;
    if (!addr || !size || !info || !cnt || *cnt < VM_REGION_BASIC_INFO_COUNT_64) return KERN_FAILURE;
    uint64_t want = (uint64_t)*addr & 0x00FFFFFFFFFFFFFFull, lo = 0, hi = 0; int prot = -1;
    FILE *f = fopen("/proc/self/maps", "re"); if (!f) return KERN_FAILURE;
    char ln[512];
    while (fgets(ln, sizeof ln, f)) {
        unsigned long long s, e; char perm[8];
        if (sscanf(ln, "%llx-%llx %7s", &s, &e, perm) != 3) continue;
        int p = (perm[0] == 'r' ? VM_PROT_READ : 0) | (perm[1] == 'w' ? VM_PROT_WRITE : 0) | (perm[2] == 'x' ? VM_PROT_EXECUTE : 0);
        if (prot >= 0) { if (s == hi && p == prot) { hi = e; continue; } break; }   // found: extend it over equal neighbours
        if (e > want) { lo = s; hi = e; prot = p; }
    }
    fclose(f);
    if (prot < 0) return KERN_FAILURE;
    vm_region_basic_info_data_64_t *bi = (vm_region_basic_info_data_64_t *)info; memset(bi, 0, sizeof *bi);
    bi->protection = prot; bi->max_protection = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE;
    *addr = (vm_address_t)lo; *size = (vm_size_t)(hi - lo); *cnt = VM_REGION_BASIC_INFO_COUNT_64; if (obj) *obj = 0;
    return KERN_SUCCESS;
}
kern_return_t thread_policy_get(mach_port_t th, thread_policy_flavor_t f, thread_policy_t p, mach_msg_type_number_t *cnt, boolean_t *def) { (void)th; (void)f; (void)p; (void)cnt; (void)def; return KERN_FAILURE; }
// Darwin's per-thread performance counters (libsyscall): not available, the caller's zeros stay
int thread_selfcounts(int type, void *buf, size_t nbytes) { (void)type; (void)buf; (void)nbytes; return -1; }
// BSD's mergesort: a stable sort (bionic has none); 0, or -1 with errno set when the buffer cannot be allocated
int mergesort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    if (n < 2) return 0;
    uint8_t *a = base, *t = malloc(n * size); if (!t) return -1;
    for (size_t w = 1; w < n; w *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n, i = lo, j = mid, k = lo;
            while (i < mid && j < hi) { if (cmp(a + j * size, a + i * size) < 0) memcpy(t + k++ * size, a + j++ * size, size); else memcpy(t + k++ * size, a + i++ * size, size); }
            memcpy(t + k * size, a + i * size, (mid - i) * size); k += mid - i;
            memcpy(t + k * size, a + j * size, (hi - j) * size);
        }
        memcpy(a, t, n * size);
    }
    free(t); return 0;
}
